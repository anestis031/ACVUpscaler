#include "frame.hpp"
#include "autotest.hpp"
#include "fsr.hpp"
#include "replace.hpp"
#include "snapshot.hpp"
#include "record.hpp"

#include <share.h>

namespace acv
{
	namespace
	{
		constexpr uint32_t kMaxRenderTargets = 8;
		constexpr size_t kMaxListTextSize = 8u << 20;  // Safety limits for one capture
		constexpr uint32_t kMaxCapturedLists = 4096;

		// Only one device can write frame logs at a time
		std::atomic<DeviceData *> s_capture_owner { nullptr };

		// Private data attached to every Direct3D 12 command list. Only touched by the thread that records
		// the list, and by the thread that submits it once recording has finished.
		struct __declspec(uuid("5d0b7a52-93e4-4c0a-8f1e-6a2f0c3b9d12")) CmdState
		{
			explicit CmdState(std::shared_ptr<DeviceData> data) : dev(std::move(data)) {}

			const std::shared_ptr<DeviceData> dev; // Keeps the device data alive for as long as the list exists

			// Direct3D 12 has a single pipeline state slot ('SetPipelineState'); whether it is the graphics or the
			// compute pipeline follows from the pipeline itself. 'kind' is resolved when the pipeline is bound.
			uint64_t pipeline = 0;
			Kind kind = Kind::None;

			uint32_t rtv_count = 0;
			api::resource_view rtvs[kMaxRenderTargets] = {};
			api::resource_view dsv = { 0 };
			bool viewport_valid = false;
			float viewport_width = 0.0f, viewport_height = 0.0f;

			RootTables graphics, compute;

			// The TAA output of this list was written by replace.cpp, so its TAA draws are skipped
			bool taa_replaced = false;
			// A TAA draw of this list could not be replaced: the game's own TAA and FSR 1 run for all of it
			bool taa_declined = false;
			// FSR 3.1 is to run at the EASU dispatch of this list, with the inputs of this TAA draw
			bool taa_pending_valid = false;
			PassRecord taa_pending;

			// Text of the events recorded for the capture with generation 'buffer_gen' (zero: nothing buffered)
			uint32_t buffer_gen = 0;
			bool buffer_complete = false; // The list was reset after the capture was armed, so nothing is missing
			bool buffer_truncated = false;
			uint32_t buffer_events = 0;
			uint16_t buffer_kinds[kKindCount] = {};
			std::string buffer;

			void reset_bindings()
			{
				pipeline = 0;
				kind = Kind::None;
				rtv_count = 0;
				dsv = { 0 };
				viewport_valid = false;
				taa_replaced = false;
				taa_declined = false;
				taa_pending_valid = false;
				graphics.clear();
				compute.clear();
			}
			void restart_buffer(uint32_t gen, bool complete)
			{
				if (gen == 0)
					std::string().swap(buffer); // Leave nothing behind once a capture is over
				else
					buffer.clear();
				buffer_gen = gen;
				buffer_complete = complete;
				buffer_truncated = false;
				buffer_events = 0;
				std::memset(buffer_kinds, 0, sizeof(buffer_kinds));
			}
		};

		inline CmdState *state_of(api::command_list *cmd_list)
		{
			return cmd_list->get_private_data<CmdState>();
		}

		enum class Ev : uint8_t { Draw, DrawIndexed, Dispatch, Indirect };

		bool kind_applies(Kind kind, Ev ev)
		{
			if (kind == Kind::None)
				return false;
			switch (ev)
			{
			case Ev::Draw:
			case Ev::DrawIndexed:
				return !kind_is_compute(kind);
			case Ev::Dispatch:
				return kind_is_compute(kind);
			default: // 'ExecuteIndirect' can be either
				return true;
			}
		}
		bool kind_needs_record(Kind kind)
		{
			return kind_is_taa(kind) || kind == Kind::Fsr1Easu || kind == Kind::Fsr1Rcas;
		}

		// ---- Resolution ---------------------------------------------------------------------------

		TargetInfo target_info(api::device *device, const DeviceData &data, api::resource_view view)
		{
			TargetInfo target;
			target.view = view.handle;
			if (view.handle != 0)
			{
				// RTV and DSV handles are tracked by ReShade itself, including copies between RTV/DSV heaps
				target.resource = device->get_resource_from_view(view).handle;
				target.known = data.resources.find(target.resource, target.info);
			}
			return target;
		}

		ReadResult read_constant_rows(const DeviceData &data, const PassRecord &record, uint32_t param, void *dest, uint32_t row_count)
		{
			const BoundTable *const table = record.find_table(param);
			if (table == nullptr)
				return ReadResult::NotCbv;
			const BoundDescriptor &cb = table->descriptors[0];
			if (cb.status != SlotStatus::Ok || cb.desc.type != DescType::Cbv)
				return ReadResult::NotCbv;
			const size_t size = static_cast<size_t>(row_count) * 16;
			if (cb.desc.size < size)
				return ReadResult::OutOfBounds;
			return data.resources.read_buffer(cb.desc.resource, cb.desc.offset, dest, size);
		}

		void build_record(api::device *device, const DeviceData &data, const CmdState &state, Kind kind, PassRecord &record)
		{
			record.valid = true;
			record.kind = kind;
			record.pipeline = state.pipeline;

			const bool compute = kind_is_compute(kind);
			if (!compute && state.rtv_count != 0)
				record.rt0 = target_info(device, data, state.rtvs[0]);

			const RootTables &root = compute ? state.compute : state.graphics;
			for (uint32_t param = 0; param < root.used; ++param)
			{
				if (root.table[param] == 0)
					continue;
				record.tables.emplace_back();
				resolve_table(device, data.descriptors, data.resources, param, root.table[param], record.tables.back());
			}

			if (kind_is_taa(kind))
			{
				record.taa_constants_result = read_constant_rows(data, record, kTaaPassParam, record.taa_constants, kTaaConstantRows);

				float view_rows[kViewRowsRead][4] = {};
				record.view_constants_result = read_constant_rows(data, record, kTaaViewParam, view_rows, kViewRowsRead);
				if (record.view_constants_result == ReadResult::Ok)
				{
					std::memcpy(record.view_projection, view_rows[kViewProjectionRow], sizeof(record.view_projection));
					std::memcpy(record.view_render_size, view_rows[kViewRenderSizeRow], sizeof(record.view_render_size));
					for (uint32_t i = 0; i < kViewSkyRowCount; ++i)
						std::memcpy(record.view_sky[i], view_rows[kViewSkyRows[i]], sizeof(record.view_sky[i]));
				}
			}
		}

		// ---- Text for the frame log ---------------------------------------------------------------

		void append_resource(std::string &out, uint64_t resource, bool known, const ResourceInfo &info)
		{
			if (resource == 0)
			{
				out += "null";
				return;
			}
			append_format(out, "0x%llX", resource);
			if (!known)
			{
				out += "(not tracked)";
			}
			else if (info.type == api::resource_type::buffer)
			{
				append_format(out, "(buffer %llu bytes)", info.size);
			}
			else
			{
				char name[16];
				append_format(out, "(%ux%u %s)", info.width, info.height, format_name(info.format, name));
			}
		}

		const char *taa_slot_label(Kind kind, uint32_t param, uint32_t index)
		{
			if (!kind_is_taa(kind))
				return nullptr;
			if (param == kTaaViewParam)
				return index == 0 ? "view constants" : nullptr;
			if (param != kTaaPassParam)
				return nullptr;
			switch (index)
			{
			case 0: return "TAA constants";
			case 3: return "t0 current colour";
			case 4: return "t1 motion vectors";
			case 5: return "t2 linear depth";
			case 6: return "t3 reactive mask";
			case 7: return "t4 history colour";
			case 8: return "t5 mask history";
			case 9: return "t6 de-dithered colour";
			default: return nullptr;
			}
		}

		void append_rows(std::string &out, const float (*rows)[4], uint32_t first_row, uint32_t row_count)
		{
			for (uint32_t i = 0; i < row_count; ++i)
			{
				uint32_t bits[4];
				std::memcpy(bits, rows[i], sizeof(bits));
				append_format(out, "          [%2u] %.9g %.9g %.9g %.9g   (0x%08X 0x%08X 0x%08X 0x%08X)\n", first_row + i,
					static_cast<double>(rows[i][0]), static_cast<double>(rows[i][1]), static_cast<double>(rows[i][2]), static_cast<double>(rows[i][3]),
					bits[0], bits[1], bits[2], bits[3]);
			}
		}

		void append_record(std::string &out, const PassRecord &record)
		{
			if (record.tables.empty())
				out += "        no descriptor tables bound\n";

			for (const BoundTable &table : record.tables)
			{
				if (table.heap == 0)
				{
					append_format(out, "        root param %u: table 0x%llX -> descriptor heap unknown to ReShade\n", table.param, table.table);
					continue;
				}
				append_format(out, "        root param %u: table 0x%llX -> heap 0x%llX index %u\n", table.param, table.table, table.heap, table.base);

				for (uint32_t i = 0; i < kTableDumpCount; ++i)
				{
					const BoundDescriptor &bound = table.descriptors[i];
					if (bound.status == SlotStatus::HeapNotTracked)
					{
						out += "          contents not tracked (sampler heap, or no descriptor write was seen for this heap)\n";
						break;
					}
					if (bound.status == SlotStatus::OutOfRange)
					{
						append_format(out, "          +%-2u end of heap\n", i);
						break;
					}

					append_format(out, "          +%-2u ", i);
					switch (bound.desc.type)
					{
					case DescType::Empty:
						out += "empty";
						break;
					case DescType::Cbv:
						out += "cbv buffer ";
						append_resource(out, bound.desc.resource, bound.resource_known, bound.info);
						append_format(out, " offset %llu size %u", bound.desc.offset, bound.desc.size);
						break;
					case DescType::Srv:
					case DescType::Uav:
						out += bound.desc.type == DescType::Srv ? "srv " : "uav ";
						append_resource(out, bound.desc.resource, bound.resource_known, bound.info);
						break;
					case DescType::Other:
						out += "other";
						break;
					}
					if (const char *const label = taa_slot_label(record.kind, table.param, i))
						append_format(out, "   <- %s", label);
					out += '\n';
				}
			}

			if (kind_is_taa(record.kind))
			{
				append_format(out, "        TAA constants (root param %u, +0), read: %s\n", kTaaPassParam, read_result_name(record.taa_constants_result));
				if (record.taa_constants_result == ReadResult::Ok)
					append_rows(out, record.taa_constants, 0, kTaaConstantRows);

				append_format(out, "        view constants (root param %u, +0), read: %s\n", kTaaViewParam, read_result_name(record.view_constants_result));
				if (record.view_constants_result == ReadResult::Ok)
				{
					append_rows(out, record.view_projection, kViewProjectionRow, 4);
					append_rows(out, &record.view_render_size, kViewRenderSizeRow, 1);
				}
			}
		}

		void append_event(api::device *device, const DeviceData &data, CmdState &state, Ev ev, uint32_t a, uint32_t b, uint32_t c, Kind kind, const PassRecord *record)
		{
			if (state.buffer.size() > kMaxListTextSize)
			{
				state.buffer_truncated = true;
				return;
			}

			std::string &out = state.buffer;

			PipelineInfo pipeline;
			const bool pipeline_known = state.pipeline != 0 && data.pipelines.find(state.pipeline, pipeline);

			bool compute = ev == Ev::Dispatch;
			bool bind_point_known = ev != Ev::Indirect;
			if (ev == Ev::Indirect && (kind != Kind::None || pipeline_known))
			{
				compute = kind != Kind::None ? kind_is_compute(kind) : pipeline.is_compute();
				bind_point_known = true;
			}

			static constexpr const char *event_names[] = { "draw", "draw_indexed", "dispatch", "indirect" };
			append_format(out, "  #%-4u %-12s %s pso=0x%llX", state.buffer_events, event_names[static_cast<size_t>(ev)],
				!bind_point_known ? "???" : compute ? "cmp" : "gfx", state.pipeline);

			if (pipeline_known)
			{
				const ShaderId &shader = pipeline.stage(compute ? Stage::Compute : Stage::Pixel);
				const char *const stage = compute ? "cs" : "ps";
				if (shader.dxbc)
					append_format(out, " %s=%02x%02x%02x%02x", stage, shader.checksum[0], shader.checksum[1], shader.checksum[2], shader.checksum[3]);
				else
					append_format(out, " %s=%s", stage, shader.present ? "not-dxbc" : "none");
			}
			else
			{
				out += state.pipeline != 0 ? " shader=unknown-pipeline" : " shader=no-pipeline";
			}

			if (kind != Kind::None)
				append_format(out, " [%s]", kind_name(kind));

			switch (ev)
			{
			case Ev::Draw:
				append_format(out, " vertices=%u instances=%u", a, b);
				break;
			case Ev::DrawIndexed:
				append_format(out, " indices=%u instances=%u", a, b);
				break;
			case Ev::Dispatch:
				append_format(out, " groups=(%u, %u, %u)", a, b, c);
				break;
			case Ev::Indirect:
				append_format(out, " INDIRECT (ExecuteIndirect) max_commands=%u", a);
				break;
			}

			if (!compute || !bind_point_known)
			{
				out += " |";
				for (uint32_t i = 0; i < state.rtv_count; ++i)
				{
					if (state.rtvs[i].handle == 0)
						continue;
					const TargetInfo target = target_info(device, data, state.rtvs[i]);
					append_format(out, " rt%u=", i);
					append_resource(out, target.resource, target.known, target.info);
				}
				out += " ds=";
				if (state.dsv.handle != 0)
				{
					const TargetInfo target = target_info(device, data, state.dsv);
					append_resource(out, target.resource, target.known, target.info);
				}
				else
				{
					out += "none";
				}
				if (state.viewport_valid)
					append_format(out, " vp=%gx%g", static_cast<double>(state.viewport_width), static_cast<double>(state.viewport_height));
				else
					out += " vp=none";
			}
			out += '\n';

			if (record != nullptr)
				append_record(out, *record);

			++state.buffer_events;
			if (kind != Kind::None && state.buffer_kinds[static_cast<size_t>(kind)] != UINT16_MAX)
				++state.buffer_kinds[static_cast<size_t>(kind)];
		}

		void append_kind_counts(std::string &out, const uint32_t (&counts)[kKindCount])
		{
			bool any = false;
			for (size_t i = 1; i < kKindCount; ++i)
			{
				if (counts[i] == 0)
					continue;
				append_format(out, "%s%s %u", any ? ", " : "", kind_name(static_cast<Kind>(i)), counts[i]);
				any = true;
			}
			if (!any)
				out += "none";
		}

		// ---- Draw and dispatch --------------------------------------------------------------------

		replace::ListState list_state(const CmdState &state)
		{
			replace::ListState list;
			list.rtvs = state.rtvs;
			list.rtv_count = state.rtv_count;
			list.pipeline = state.pipeline;
			list.graphics = &state.graphics;
			list.compute = &state.compute;
			return list;
		}

		// Returns true when the call must not reach the driver
		bool handle_event(api::command_list *cmd_list, CmdState &state, Ev ev, uint32_t a, uint32_t b, uint32_t c)
		{
			DeviceData &data = *state.dev;

			const uint32_t gen = data.capture_gen.load(std::memory_order_acquire);
			if (state.buffer_gen != gen)
				state.restart_buffer(gen, false); // Capture armed in the middle of this list, or over

			const Kind kind = kind_applies(state.kind, ev) ? state.kind : Kind::None;
			if (kind == Kind::None && gen == 0)
				return false;

			api::device *const device = cmd_list->get_device();
			if (device == nullptr)
				return false;

			bool skip = false;
			PassRecord record;
			if (kind != Kind::None)
			{
				data.counts[static_cast<size_t>(kind)].fetch_add(1, std::memory_order_relaxed);

				if (kind_needs_record(kind) || gen != 0)
					build_record(device, data, state, kind, record);

				if (record.valid && kind_is_taa(kind))
				{
					const ReplaceMode mode = static_cast<ReplaceMode>(data.replace_mode.load(std::memory_order_relaxed));
					if (mode != ReplaceMode::GameTaa)
					{
						if (!state.taa_replaced && !state.taa_declined)
						{
							const replace::Result result = replace::at_taa(mode, cmd_list, device, data, record, list_state(state));
							state.taa_replaced = result != replace::Result::NotApplied;
							state.taa_declined = !state.taa_replaced;
							if (state.taa_declined)
								data.declined_lists.fetch_add(1, std::memory_order_relaxed);
							if (result == replace::Result::Deferred)
							{
								state.taa_pending = record;
								state.taa_pending_valid = true;
							}
						}
						skip = state.taa_replaced;
					}

					// Before the draw itself is recorded: its inputs are in their final state here
					if (data.snapshot.stage.load(std::memory_order_relaxed) == Snapshot::Armed)
						snapshot::copy_inputs(cmd_list, device, data, record);
					record::copy_inputs(cmd_list, device, data, record);
				}
				else if (record.valid && (kind == Kind::Fsr1Easu || kind == Kind::Fsr1Rcas) && state.taa_pending_valid)
				{
					// FSR 3.1 takes the place of EASU, with the inputs of the TAA draws skipped earlier in this list.
					// Without the game's sharpening it takes the place of RCAS, and EASU is only skipped.
					if (kind == Kind::Fsr1Easu && !settings::game_sharpening() && !data.rcas_replace_failed.load(std::memory_order_relaxed))
					{
						skip = true;
					}
					else
					{
						state.taa_pending_valid = false;
						skip = replace::at_fsr1(cmd_list, device, data, state.taa_pending, record, list_state(state));
						// RCAS now sharpens an EASU output nobody wrote: one stale frame, then back to the EASU dispatch
						if (!skip && kind == Kind::Fsr1Rcas)
							data.rcas_replace_failed.store(true, std::memory_order_relaxed);
					}
				}
			}

			if (gen != 0)
			{
				append_event(device, data, state, ev, a, b, c, kind, record.valid ? &record : nullptr);
				if (skip)
					state.buffer += "        ^ SKIPPED by the add-on (TAA replacement)\n";
			}

			if (record.valid && kind_needs_record(kind))
			{
				const std::lock_guard<std::mutex> lock(data.record_mutex);
				if (kind == Kind::Fsr1Easu)
					data.current_easu = std::move(record);
				else if (kind == Kind::Fsr1Rcas)
					data.current_rcas = std::move(record);
				else if (kind == Kind::TaaFull || !data.current_taa.valid)
					data.current_taa = std::move(record);
			}
			return skip;
		}

		// The common case (unknown pipeline, no capture) ends after the private data lookup and three compares.
		inline bool on_event(api::command_list *cmd_list, Ev ev, uint32_t a, uint32_t b, uint32_t c) noexcept
		{
			CmdState *const state = state_of(cmd_list);
			if (state == nullptr ||
				(state->kind == Kind::None && state->buffer_gen == 0 && state->dev->capture_gen.load(std::memory_order_relaxed) == 0))
				return false;

			bool skip = false;
			guarded("draw/dispatch", [&]() { skip = handle_event(cmd_list, *state, ev, a, b, c); });
			return skip;
		}

		bool on_draw(api::command_list *cmd_list, uint32_t vertex_count, uint32_t instance_count, uint32_t, uint32_t)
		{
			return on_event(cmd_list, Ev::Draw, vertex_count, instance_count, 0);
		}
		bool on_draw_indexed(api::command_list *cmd_list, uint32_t index_count, uint32_t instance_count, uint32_t, int32_t, uint32_t)
		{
			return on_event(cmd_list, Ev::DrawIndexed, index_count, instance_count, 0);
		}
		bool on_dispatch(api::command_list *cmd_list, uint32_t group_count_x, uint32_t group_count_y, uint32_t group_count_z)
		{
			return on_event(cmd_list, Ev::Dispatch, group_count_x, group_count_y, group_count_z);
		}
		bool on_draw_or_dispatch_indirect(api::command_list *cmd_list, api::indirect_command, api::resource, uint64_t, uint32_t draw_count, uint32_t)
		{
			// Direct3D 12 'ExecuteIndirect' arrives here (type 'unknown', 'draw_count' = MaxCommandCount)
			return on_event(cmd_list, Ev::Indirect, draw_count, 0, 0);
		}

		// ---- Command list state -------------------------------------------------------------------

		void on_init_command_list(api::command_list *cmd_list)
		{
			guarded("init_command_list", [&]() {
				api::device *const device = cmd_list->get_device();
				const DeviceHolder *const holder = device != nullptr ? device->get_private_data<DeviceHolder>() : nullptr;
				if (holder == nullptr || holder->data == nullptr)
					return; // Not a Direct3D 12 device
				if (state_of(cmd_list) == nullptr)
					cmd_list->create_private_data<CmdState>(holder->data);
			});
		}
		void on_destroy_command_list(api::command_list *cmd_list)
		{
			guarded("destroy_command_list", [&]() {
				if (state_of(cmd_list) != nullptr)
					cmd_list->destroy_private_data<CmdState>();
			});
		}

		void on_reset_command_list(api::command_list *cmd_list)
		{
			CmdState *const state = state_of(cmd_list);
			if (state == nullptr)
				return;

			state->reset_bindings();

			const uint32_t gen = state->dev->capture_gen.load(std::memory_order_acquire);
			if (gen != 0 || state->buffer_gen != 0)
				guarded("reset_command_list", [&]() { state->restart_buffer(gen, true); });
		}

		void on_bind_pipeline(api::command_list *cmd_list, api::pipeline_stage stages, api::pipeline pipeline)
		{
			// 'SetPipelineState', 'Reset' and 'ClearState' report 'all'; ray tracing state objects are not of interest
			if (stages != api::pipeline_stage::all)
				return;
			CmdState *const state = state_of(cmd_list);
			if (state == nullptr)
				return;

			state->pipeline = pipeline.handle;
			state->kind = Kind::None;
			if (pipeline.handle != 0)
				guarded("bind_pipeline", [&]() { state->kind = state->dev->pipelines.kind_of(pipeline.handle); });
		}

		void on_bind_render_targets_and_depth_stencil(api::command_list *cmd_list, uint32_t count, const api::resource_view *rtvs, api::resource_view dsv)
		{
			CmdState *const state = state_of(cmd_list);
			if (state == nullptr)
				return;

			state->rtv_count = rtvs != nullptr ? (count < kMaxRenderTargets ? count : kMaxRenderTargets) : 0;
			for (uint32_t i = 0; i < state->rtv_count; ++i)
				state->rtvs[i] = rtvs[i];
			state->dsv = dsv;
		}

		void on_bind_viewports(api::command_list *cmd_list, uint32_t first, uint32_t count, const api::viewport *viewports)
		{
			if (first != 0)
				return;
			CmdState *const state = state_of(cmd_list);
			if (state == nullptr)
				return;

			state->viewport_valid = count != 0 && viewports != nullptr;
			if (state->viewport_valid)
			{
				state->viewport_width = viewports[0].width;
				state->viewport_height = viewports[0].height;
			}
		}

		void on_bind_descriptor_tables(api::command_list *cmd_list, api::shader_stage stages, api::pipeline_layout layout, uint32_t first, uint32_t count, const api::descriptor_table *tables, uint32_t, const uint32_t *)
		{
			CmdState *const state = state_of(cmd_list);
			if (state == nullptr)
				return;

			const auto apply = [=](RootTables &root) {
				if (count == 0 || tables == nullptr)
				{
					// 'Set*RootSignature' (or 'ClearState' with a null layout): bindings only survive when the
					// same root signature is set again
					if (layout.handle == 0 || root.layout != layout.handle)
						root.clear();
					root.layout = layout.handle;
					return;
				}

				// 'Set*RootDescriptorTable': the table handle is the GPU descriptor handle passed by the game
				root.layout = layout.handle;
				for (uint32_t i = 0; i < count && first + i < kMaxRootParams; ++i)
					root.set(first + i, tables[i].handle);
			};

			const uint32_t stage_bits = static_cast<uint32_t>(stages);
			if ((stage_bits & static_cast<uint32_t>(api::shader_stage::all_graphics)) != 0)
				apply(state->graphics);
			if ((stage_bits & static_cast<uint32_t>(api::shader_stage::all_compute)) != 0)
				apply(state->compute);
		}

		// ---- Capture file -------------------------------------------------------------------------

		void release_capture(DeviceData &data)
		{
			data.capture_writing.store(false, std::memory_order_release);
			data.capture_gen.store(0, std::memory_order_release);

			DeviceData *expected = &data;
			s_capture_owner.compare_exchange_strong(expected, nullptr);
		}

		// Present after the request: open the file. Called with 'capture_mutex' held.
		void begin_capture_file(DeviceData &data, uint64_t frame)
		{
			// Numbered per session (acvupscaler_frame_001.log, ...), so that several scenes can be captured in
			// one play session without overwriting each other. Called with 'capture_mutex' held.
			static unsigned int s_capture_file_index = 0;
			wchar_t name[48];
			swprintf_s(name, L"acvupscaler_frame_%03u.log", ++s_capture_file_index);
			const std::wstring path = log::directory() + name;

			std::FILE *const file = _wfsopen(path.c_str(), L"w", _SH_DENYWR);
			if (file == nullptr)
			{
				log::warn("could not open the frame log for writing; capture cancelled");
				release_capture(data);
				return;
			}
			std::setvbuf(file, nullptr, _IOFBF, 1 << 18);

			char format[16];
			std::fprintf(file,
				"ACVUpscaler frame capture #%u, add-on version " ACV_VERSION "\n"
				"frame %llu, swap chain %ux%u %s\n"
				"Command lists appear in the order they were passed to ExecuteCommandLists. Each event line and every\n"
				"binding below it was produced when the call was recorded, not when it executed.\n\n",
				data.capture_gen.load(std::memory_order_relaxed), frame,
				data.swap_width.load(std::memory_order_relaxed), data.swap_height.load(std::memory_order_relaxed),
				format_name(static_cast<api::format>(data.swap_format.load(std::memory_order_relaxed)), format));

			data.capture_file = file;
			data.capture_lists = data.capture_partial_lists = data.capture_events = 0;
			std::memset(data.capture_kind_counts, 0, sizeof(data.capture_kind_counts));
			data.capture_writing.store(true, std::memory_order_release);
		}

		// Second present after the request: footer and close. Called with 'capture_mutex' held.
		void finish_capture_file(DeviceData &data, const FrameStats &stats)
		{
			if (std::FILE *const file = data.capture_file)
			{
				std::string text;
				append_format(text, "\n=== end of frame %llu ===\n", stats.frame);
				append_format(text, "command lists written: %u (%u partial), events written: %u%s\n", data.capture_lists, data.capture_partial_lists, data.capture_events,
					data.capture_lists >= kMaxCapturedLists ? " (list limit reached, capture truncated)" : "");
				text += "known passes in the written lists: ";
				append_kind_counts(text, data.capture_kind_counts);
				text += "\nknown passes counted for this frame while recording: ";
				append_kind_counts(text, stats.counts);
				append_format(text, "\ndetected mode: %s\n", detect_mode(stats));
				append_format(text, "CBV/SRV/UAV descriptors written during this frame: about %llu, copied: about %llu\n", stats.descriptors_written, stats.descriptors_copied);
				frame::append_jitter_history(text, data, stats.frame);

				std::fwrite(text.data(), 1, text.size(), file);
				std::fclose(file);
				data.capture_file = nullptr;

				++data.captures_done;
				log::write("frame capture #%u written to acvupscaler_frame_NNN.log: frame %llu, %u command lists, %u events",
					data.capture_gen.load(std::memory_order_relaxed), stats.frame, data.capture_lists, data.capture_events);
			}

			release_capture(data);
		}

		void on_execute_command_list(api::command_queue *queue, api::command_list *cmd_list)
		{
			// Raised once per list, in submission order, before the lists reach the driver
			CmdState *const state = state_of(cmd_list);
			if (state == nullptr || state->buffer_gen == 0)
				return;

			guarded("execute_command_list", [&]() {
				DeviceData &data = *state->dev;

				if (data.capture_writing.load(std::memory_order_acquire))
				{
					const std::lock_guard<std::mutex> lock(data.capture_mutex);

					if (data.capture_file != nullptr && data.capture_lists < kMaxCapturedLists &&
						state->buffer_gen == data.capture_gen.load(std::memory_order_relaxed))
					{
						std::fprintf(data.capture_file, "--- command list 0x%llX on queue 0x%llX: %u events%s%s\n",
							static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(cmd_list)), static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(queue)),
							state->buffer_events,
							state->buffer_complete ? "" : " [PARTIAL: recording started before the capture was requested]",
							state->buffer_truncated ? " [TRUNCATED]" : "");
						std::fwrite(state->buffer.data(), 1, state->buffer.size(), data.capture_file);

						++data.capture_lists;
						if (!state->buffer_complete)
							++data.capture_partial_lists;
						data.capture_events += state->buffer_events;
						for (size_t i = 0; i < kKindCount; ++i)
							data.capture_kind_counts[i] += state->buffer_kinds[i];
					}
				}

				// Written or not (a list submitted before the captured frame began), the text is done with
				state->restart_buffer(0, false);
			});
		}

		void on_present(api::command_queue *queue, api::swapchain *swapchain, const api::rect *, const api::rect *, uint32_t, const api::rect *)
		{
			guarded("present", [&]() {
				DeviceData *const data = swapchain != nullptr ? device_data(swapchain->get_device()) : nullptr;
				if (data == nullptr)
					return;

				// Close the statistics of the frame that ends with this present
				FrameStats stats;
				stats.frame = data->frame_index.fetch_add(1, std::memory_order_relaxed) + 1;
				for (size_t i = 0; i < kKindCount; ++i)
					stats.counts[i] = data->counts[i].exchange(0, std::memory_order_relaxed);

				const uint64_t written_total = descriptors_written_total(), copied_total = descriptors_copied_total();
				stats.descriptors_written = written_total - data->written_total_at_present;
				stats.descriptors_copied = copied_total - data->copied_total_at_present;
				data->written_total_at_present = written_total;
				data->copied_total_at_present = copied_total;
				{
					const std::lock_guard<std::mutex> lock(data->record_mutex);
					stats.taa = std::move(data->current_taa);
					stats.easu = std::move(data->current_easu);
					stats.rcas = std::move(data->current_rcas);
					data->current_taa = PassRecord();
					data->current_easu = PassRecord();
					data->current_rcas = PassRecord();
				}

				{
					JitterSample &sample = data->jitter_history[stats.frame % kJitterHistory];
					sample = JitterSample();
					sample.frame = stats.frame;
					if (stats.taa.valid && stats.taa.taa_constants_result == ReadResult::Ok)
					{
						sample.valid = true;
						sample.x = stats.taa.taa_constants[3][0];
						sample.y = stats.taa.taa_constants[3][1];
						sample.width = stats.taa.taa_constants[2][0];
						sample.height = stats.taa.taa_constants[2][1];
					}
				}

				data->game_fsr1_seen.store(stats.counts[static_cast<size_t>(Kind::Fsr1Easu)] != 0 && stats.counts[static_cast<size_t>(Kind::Fsr1Rcas)] != 0, std::memory_order_relaxed);
				{
					LARGE_INTEGER now, frequency;
					QueryPerformanceCounter(&now);
					QueryPerformanceFrequency(&frequency);
					if (data->last_present_time != 0)
					{
						data->frame_ms_ring[stats.frame % kFrameTimeRing] = static_cast<float>(static_cast<double>(now.QuadPart - data->last_present_time) * 1000.0 / static_cast<double>(frequency.QuadPart));
						float sum = 0.0f;
						for (const float frame_ms : data->frame_ms_ring)
							sum += frame_ms;
						data->average_frame_ms.store(sum / static_cast<float>(kFrameTimeRing), std::memory_order_relaxed);
					}
					data->last_present_time = now.QuadPart;
				}

				snapshot::on_present(queue, swapchain->get_device(), *data, stats.frame);
				record::on_present(queue, swapchain->get_device(), *data, stats.frame);
				data->fsr->on_present(queue, stats.frame);
				autotest::on_present(*data, stats.frame);

				if (data->capture_gen.load(std::memory_order_acquire) != 0)
				{
					const std::lock_guard<std::mutex> lock(data->capture_mutex);
					if (data->capture_writing.load(std::memory_order_relaxed))
						finish_capture_file(*data, stats);
					else if (data->capture_gen.load(std::memory_order_relaxed) != 0)
						begin_capture_file(*data, stats.frame + 1);
				}

				{
					const std::lock_guard<std::mutex> lock(data->published_mutex);
					data->published = std::move(stats);
				}
			});

			log::flush_if_dirty();
		}
	}

	// ---- Public ---------------------------------------------------------------------------------

	DeviceData::DeviceData() : fsr(std::make_unique<Fsr>())
	{
		const ReplaceMode mode = replace::load_mode();
		replace_mode.store(static_cast<uint32_t>(mode), std::memory_order_relaxed);
		if (mode != ReplaceMode::GameTaa)
			log::write("replacement mode from ACVUpscaler.ini: \"%s\"", replace::mode_name(mode));

		fsr_reactive_gain.store(settings::get().reactive_gain, std::memory_order_relaxed);
		// Test harness only: ACV_TEST_REACTIVE_GAIN sets the strength of the reactive mask
		char text[24];
		const DWORD length = GetEnvironmentVariableA("ACV_TEST_REACTIVE_GAIN", text, sizeof(text));
		if (length != 0 && length < sizeof(text))
			fsr_reactive_gain.store(std::strtof(text, nullptr), std::memory_order_relaxed);
	}

	DeviceData::~DeviceData()
	{
		if (capture_file != nullptr)
			std::fclose(capture_file);

		DeviceData *expected = this;
		s_capture_owner.compare_exchange_strong(expected, nullptr);
	}

	FrameStats DeviceData::published_copy()
	{
		const std::lock_guard<std::mutex> lock(published_mutex);
		return published;
	}

	const char *detect_mode(const FrameStats &stats)
	{
		const bool taa =
			stats.counts[static_cast<size_t>(Kind::TaaFull)] != 0 ||
			stats.counts[static_cast<size_t>(Kind::TaaDeditherTiles)] != 0 ||
			stats.counts[static_cast<size_t>(Kind::TaaCameraOnly)] != 0;
		if (!taa)
			return "TAA not found";
		const bool easu = stats.counts[static_cast<size_t>(Kind::Fsr1Easu)] != 0;
		const bool rcas = stats.counts[static_cast<size_t>(Kind::Fsr1Rcas)] != 0;
		if (easu && rcas)
			return "game FSR 1 on";
		if (!easu && !rcas)
			return "native";
		return "TAA with only one of EASU/RCAS (unexpected)";
	}

	bool frame::request_capture(DeviceData &data)
	{
		const std::lock_guard<std::mutex> lock(data.capture_mutex);

		if (data.capture_gen.load(std::memory_order_relaxed) != 0)
			return false;

		DeviceData *expected = nullptr;
		if (!s_capture_owner.compare_exchange_strong(expected, &data))
		{
			log::warn("frame capture refused: another device is capturing");
			return false;
		}

		uint32_t gen = ++data.capture_next_gen;
		if (gen == 0)
			gen = ++data.capture_next_gen;
		data.capture_gen.store(gen, std::memory_order_release);

		log::write("frame capture #%u requested", gen);
		return true;
	}

	const char *frame::capture_state(const DeviceData &data)
	{
		if (data.capture_gen.load(std::memory_order_relaxed) == 0)
			return "idle";
		return data.capture_writing.load(std::memory_order_relaxed) ? "writing" : "armed";
	}

	void frame::append_pass_text(std::string &out, const PassRecord &record)
	{
		append_record(out, record);
	}

	void frame::append_jitter_history(std::string &out, const DeviceData &data, uint64_t last_frame)
	{
		append_format(out, "TAA jitter in pixels of the last %u frames, oldest first (frame: x y, input size):\n", kJitterHistory);
		for (uint64_t frame = last_frame >= kJitterHistory ? last_frame - kJitterHistory + 1 : 1; frame <= last_frame; ++frame)
		{
			const JitterSample &sample = data.jitter_history[frame % kJitterHistory];
			if (sample.frame != frame)
				continue;
			if (sample.valid)
				append_format(out, "  %llu: %+.6f %+.6f, %gx%g\n", frame, static_cast<double>(sample.x), static_cast<double>(sample.y), static_cast<double>(sample.width), static_cast<double>(sample.height));
			else
				append_format(out, "  %llu: no TAA draw\n", frame);
		}
	}

	void frame::register_events()
	{
		reshade::register_event<reshade::addon_event::init_command_list>(on_init_command_list);
		reshade::register_event<reshade::addon_event::destroy_command_list>(on_destroy_command_list);
		reshade::register_event<reshade::addon_event::reset_command_list>(on_reset_command_list);
		reshade::register_event<reshade::addon_event::bind_pipeline>(on_bind_pipeline);
		reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(on_bind_render_targets_and_depth_stencil);
		reshade::register_event<reshade::addon_event::bind_viewports>(on_bind_viewports);
		reshade::register_event<reshade::addon_event::bind_descriptor_tables>(on_bind_descriptor_tables);
		reshade::register_event<reshade::addon_event::draw>(on_draw);
		reshade::register_event<reshade::addon_event::draw_indexed>(on_draw_indexed);
		reshade::register_event<reshade::addon_event::dispatch>(on_dispatch);
		reshade::register_event<reshade::addon_event::draw_or_dispatch_indirect>(on_draw_or_dispatch_indirect);
		reshade::register_event<reshade::addon_event::execute_command_list>(on_execute_command_list);
		reshade::register_event<reshade::addon_event::present>(on_present);
	}
	void frame::unregister_events()
	{
		reshade::unregister_event<reshade::addon_event::init_command_list>(on_init_command_list);
		reshade::unregister_event<reshade::addon_event::destroy_command_list>(on_destroy_command_list);
		reshade::unregister_event<reshade::addon_event::reset_command_list>(on_reset_command_list);
		reshade::unregister_event<reshade::addon_event::bind_pipeline>(on_bind_pipeline);
		reshade::unregister_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(on_bind_render_targets_and_depth_stencil);
		reshade::unregister_event<reshade::addon_event::bind_viewports>(on_bind_viewports);
		reshade::unregister_event<reshade::addon_event::bind_descriptor_tables>(on_bind_descriptor_tables);
		reshade::unregister_event<reshade::addon_event::draw>(on_draw);
		reshade::unregister_event<reshade::addon_event::draw_indexed>(on_draw_indexed);
		reshade::unregister_event<reshade::addon_event::dispatch>(on_dispatch);
		reshade::unregister_event<reshade::addon_event::draw_or_dispatch_indirect>(on_draw_or_dispatch_indirect);
		reshade::unregister_event<reshade::addon_event::execute_command_list>(on_execute_command_list);
		reshade::unregister_event<reshade::addon_event::present>(on_present);
	}

	const char *format_name(api::format format, char (&buffer)[16])
	{
		// Values are identical to DXGI_FORMAT
		switch (static_cast<uint32_t>(format))
		{
		case 0: return "unknown";
		case 2: return "r32g32b32a32_float";
		case 10: return "r16g16b16a16_float";
		case 11: return "r16g16b16a16_unorm";
		case 16: return "r32g32_float";
		case 19: return "r32g8x24_typeless";
		case 20: return "d32_float_s8x24_uint";
		case 23: return "r10g10b10a2_typeless";
		case 24: return "r10g10b10a2_unorm";
		case 26: return "r11g11b10_float";
		case 27: return "r8g8b8a8_typeless";
		case 28: return "r8g8b8a8_unorm";
		case 29: return "r8g8b8a8_unorm_srgb";
		case 33: return "r16g16_typeless";
		case 34: return "r16g16_float";
		case 35: return "r16g16_unorm";
		case 39: return "r32_typeless";
		case 40: return "d32_float";
		case 41: return "r32_float";
		case 42: return "r32_uint";
		case 44: return "r24g8_typeless";
		case 45: return "d24_unorm_s8_uint";
		case 49: return "r8g8_unorm";
		case 53: return "r16_typeless";
		case 54: return "r16_float";
		case 55: return "d16_unorm";
		case 56: return "r16_unorm";
		case 57: return "r16_uint";
		case 61: return "r8_unorm";
		case 62: return "r8_uint";
		case 71: return "bc1_unorm";
		case 72: return "bc1_unorm_srgb";
		case 77: return "bc3_unorm";
		case 78: return "bc3_unorm_srgb";
		case 80: return "bc4_unorm";
		case 83: return "bc5_unorm";
		case 87: return "b8g8r8a8_unorm";
		case 91: return "b8g8r8a8_unorm_srgb";
		case 95: return "bc6h_uf16";
		case 98: return "bc7_unorm";
		case 99: return "bc7_unorm_srgb";
		default:
			std::snprintf(buffer, sizeof(buffer), "format%u", static_cast<uint32_t>(format));
			return buffer;
		}
	}
}
