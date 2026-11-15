#include "replace.hpp"
#include "fsr.hpp"

#include <cmath>

namespace acv
{
	namespace
	{
		// TAA inputs (index into t0..t6). t6, the de-dithered colour, is only written in the tiles that need it.
		constexpr uint32_t kColourInput = 0, kVelocityInput = 1, kDepthInput = 2, kMaskInput = 3;
		constexpr uint32_t kTargetCount = 2; // RT0 = TAA output, RT1 = history for the next frame

		bool fail(const char *reason)
		{
			static std::atomic<uint32_t> s_reported { 0 };
			if (s_reported.fetch_add(1, std::memory_order_relaxed) < 8)
				log::warn("TAA replacement not applied: %s", reason);
			return false;
		}

		// A TAA input that is a live 2D texture, or zero
		api::resource input_texture(const PassRecord &record, uint32_t index)
		{
			const BoundTable *const table = record.find_table(kTaaPassParam);
			if (table == nullptr)
				return { 0 };
			const BoundDescriptor &input = table->descriptors[kTaaFirstInput + index];
			if (input.status != SlotStatus::Ok || input.desc.type != DescType::Srv || !input.resource_known || input.info.type != api::resource_type::texture_2d)
				return { 0 };
			return { input.desc.resource };
		}

		bool same_layout(const api::resource_desc &a, const api::resource_desc &b)
		{
			return a.type == api::resource_type::texture_2d && a.texture.samples == 1 &&
				a.texture.width == b.texture.width && a.texture.height == b.texture.height &&
				api::format_to_typeless(a.texture.format) == api::format_to_typeless(b.texture.format);
		}

		// The render targets of the TAA draw that can take a copy of a texture like 'like'.
		// RT1 is written as well: it is the history the game samples in the next frame.
		uint32_t collect_targets(api::device *device, const DeviceData &data, const replace::ListState &list, const api::resource_desc &like, api::resource exclude, api::resource (&targets)[kTargetCount])
		{
			uint32_t count = 0;
			for (uint32_t i = 0; i < kTargetCount && i < list.rtv_count; ++i)
			{
				const api::resource target = list.rtvs[i] != 0 ? device->get_resource_from_view(list.rtvs[i]) : api::resource { 0 };
				ResourceInfo info;
				if (target == 0 || target == exclude || !data.resources.find(target.handle, info) || !same_layout(device->get_resource_desc(target), like))
					break;
				targets[count++] = target;
			}
			return count;
		}

		// 'source' has to be in COPY_SOURCE state, the targets in 'target_state'
		void copy_to_targets(api::command_list *cmd_list, api::resource source, const api::resource *targets, uint32_t count, api::resource_usage target_state)
		{
			for (uint32_t i = 0; i < count; ++i)
			{
				cmd_list->barrier(targets[i], target_state, api::resource_usage::copy_dest);
				cmd_list->copy_texture_region(source, 0, nullptr, targets[i], 0, nullptr);
				cmd_list->barrier(targets[i], api::resource_usage::copy_dest, target_state);
			}
		}

		void mark_applied(DeviceData &data)
		{
			data.replace_applied_frame.store(data.frame_index.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
		}

		// Copies the current colour (t0) into the TAA render targets, which are bound as render targets (docs/TECHNICAL.md):
		// the TAA output and, with 'history', the history of the next frame
		bool passthrough(api::command_list *cmd_list, api::device *device, DeviceData &data, const PassRecord &taa, const replace::ListState &list, bool history)
		{
			const api::resource source = input_texture(taa, kColourInput);
			if (source == 0)
				return fail("the current colour (t0) could not be resolved");

			api::resource targets[kTargetCount] = {};
			uint32_t target_count = collect_targets(device, data, list, device->get_resource_desc(source), source, targets);
			if (target_count == 0)
				return fail("render target 0 of the TAA draw does not match the current colour in size or format");
			if (!history)
				target_count = 1;

			cmd_list->barrier(source, kGameReadState, api::resource_usage::copy_source);
			copy_to_targets(cmd_list, source, targets, target_count, api::resource_usage::render_target);
			cmd_list->barrier(source, api::resource_usage::copy_source, kGameReadState);

			static std::atomic<bool> s_logged { false };
			if (!s_logged.exchange(true, std::memory_order_relaxed))
				log::write("TAA output written by the add-on for the first time: t0 copied to %u render target(s), TAA draws skipped", target_count);
			mark_applied(data);
			return true;
		}

		// Puts back what the decode pass and FSR changed on the list: descriptor heaps, compute root signature
		// and pipeline. The root tables are set again as well, since they refer to the heaps.
		// 'bound' is the record of the call being replaced; its tables tell which heaps the game has set.
		void restore_list(ID3D12GraphicsCommandList *native, const PassRecord &bound, const replace::ListState &list)
		{
			ID3D12DescriptorHeap *heaps[2] = {};
			uint32_t heap_count = 0;
			for (const BoundTable &table : bound.tables)
			{
				ID3D12DescriptorHeap *const heap = reinterpret_cast<ID3D12DescriptorHeap *>(static_cast<uintptr_t>(table.heap));
				if (heap == nullptr || (heap_count != 0 && heaps[0] == heap) || heap_count == 2)
					continue;
				heaps[heap_count++] = heap;
			}
			if (heap_count != 0)
				native->SetDescriptorHeaps(heap_count, heaps);

			if (list.compute->layout != 0)
			{
				native->SetComputeRootSignature(reinterpret_cast<ID3D12RootSignature *>(static_cast<uintptr_t>(list.compute->layout)));
				for (uint32_t i = 0; i < list.compute->used; ++i)
					if (list.compute->table[i] != 0)
						native->SetComputeRootDescriptorTable(i, { list.compute->table[i] });
			}
			// The graphics root signature was not touched
			for (uint32_t i = 0; i < list.graphics->used; ++i)
				if (list.graphics->table[i] != 0)
					native->SetGraphicsRootDescriptorTable(i, { list.graphics->table[i] });

			if (list.pipeline != 0)
				native->SetPipelineState(reinterpret_cast<ID3D12PipelineState *>(static_cast<uintptr_t>(list.pipeline)));
		}

		// Runs FSR on the inputs of the TAA draw 'taa' and puts the result into 'targets', which all have the
		// size of the result and are in 'target_state'. 'bound' and 'list' describe the call being replaced.
		bool run_fsr(api::command_list *cmd_list, api::device *device, DeviceData &data, const PassRecord &taa, const PassRecord &bound, const replace::ListState &list,
			const api::resource *targets, uint32_t target_count, api::resource_usage target_state)
		{
			const api::resource colour = input_texture(taa, kColourInput), velocity = input_texture(taa, kVelocityInput), depth = input_texture(taa, kDepthInput);
			if (colour == 0 || velocity == 0 || depth == 0)
				return fail("colour, velocity or depth (t0..t2) could not be resolved");
			if (taa.taa_constants_result != ReadResult::Ok || taa.view_constants_result != ReadResult::Ok)
				return fail("the TAA or view constants could not be read");

			// Projection rows as the game stores them (docs/TECHNICAL.md): [1][1] = cot(fov / 2), row 2 = (0, 0, a, b)
			// with reversed-Z depth = b / distance - a
			const float cot_half_fov = taa.view_projection[1][1];
			const float depth_a = taa.view_projection[2][2], depth_b = taa.view_projection[2][3];
			if (!(cot_half_fov > 0.01f) || !(depth_b > 0.0f) || !(depth_a >= 0.0f))
				return fail("the projection in the view constants does not look like the known layout");

			const api::resource_desc colour_desc = device->get_resource_desc(colour), target_desc = device->get_resource_desc(targets[0]);

			Fsr::Inputs inputs;
			inputs.colour = reinterpret_cast<ID3D12Resource *>(static_cast<uintptr_t>(colour.handle));
			inputs.velocity = reinterpret_cast<ID3D12Resource *>(static_cast<uintptr_t>(velocity.handle));
			inputs.linear_depth = reinterpret_cast<ID3D12Resource *>(static_cast<uintptr_t>(depth.handle));
			inputs.mask = reinterpret_cast<ID3D12Resource *>(static_cast<uintptr_t>(input_texture(taa, kMaskInput).handle));
			inputs.reactive_gain = data.fsr_reactive_gain.load(std::memory_order_relaxed);
			inputs.width = colour_desc.texture.width;
			inputs.height = colour_desc.texture.height;
			inputs.out_width = target_desc.texture.width;
			inputs.out_height = target_desc.texture.height;
			// The game's jitter in pixels (TAA constants row 3) has the convention FSR documents: the projection is
			// shifted by (+2 jx / W, -2 jy / H). Confirmed by the automatic test (docs/TECHNICAL.md); the switches stay for tests.
			inputs.jitter_x = taa.taa_constants[3][0] * (data.fsr_flip_jitter_x.load(std::memory_order_relaxed) ? -1.0f : 1.0f);
			inputs.jitter_y = taa.taa_constants[3][1] * (data.fsr_flip_jitter_y.load(std::memory_order_relaxed) ? -1.0f : 1.0f);
			inputs.motion_sign = data.fsr_flip_motion.load(std::memory_order_relaxed) ? -1.0f : 1.0f;
			inputs.depth_a = depth_a;
			inputs.depth_b = depth_b;
			inputs.fov_y = 2.0f * std::atan(1.0f / cot_half_fov);
			// Far-plane test of the game's TAA: distance >= (c6.x - c6.y) / (c6.z - c6.w), about 50000.
			// Without the matrices (all zero) the sky keeps what the velocity texture says.
			{
				const float *const c6 = taa.taa_constants[6];
				const float denominator = c6[2] - c6[3];
				bool matrices = false;
				for (uint32_t i = 0; i < 4; ++i)
					matrices = matrices || taa.view_sky[kViewSkyRowCount - 1][i] != 0.0f;
				if (matrices && denominator > 1e-9f && c6[0] - c6[1] > 0.0f)
				{
					std::memcpy(inputs.sky_rows, taa.view_sky, sizeof(inputs.sky_rows));
					inputs.sky_distance = (c6[0] - c6[1]) / denominator;
				}
			}
			inputs.frame = data.frame_index.load(std::memory_order_relaxed) + 1;

			ID3D12Device *const native_device = reinterpret_cast<ID3D12Device *>(static_cast<uintptr_t>(device->get_native()));
			ID3D12GraphicsCommandList *const native_list = reinterpret_cast<ID3D12GraphicsCommandList *>(static_cast<uintptr_t>(cmd_list->get_native()));

			// A single target in unordered access state (an output of the game's FSR 1) is written by FSR itself.
			// Render targets get a copy of the add-on's own output.
			// Test harness only: ACV_TEST_COPY_OUTPUT keeps the copy, to measure what it costs
			static const bool s_test_copy = GetEnvironmentVariableW(L"ACV_TEST_COPY_OUTPUT", nullptr, 0) != 0;
			const bool direct = target_count == 1 && target_state == api::resource_usage::unordered_access && !s_test_copy;
			if (direct)
				inputs.output = reinterpret_cast<ID3D12Resource *>(static_cast<uintptr_t>(targets[0].handle));

			const bool ok = data.fsr->dispatch(native_device, native_list, inputs);
			const api::resource own_output = { reinterpret_cast<uintptr_t>(data.fsr->output()) };
			if (ok && !direct)
				copy_to_targets(cmd_list, own_output, targets, target_count, target_state);
			data.fsr->end_timer(native_list);
			if (ok && direct && settings::get().dev_mode)
			{
				// Snapshots and recordings read the add-on's own output
				cmd_list->barrier(targets[0], target_state, api::resource_usage::copy_source);
				copy_to_targets(cmd_list, targets[0], &own_output, 1, api::resource_usage::copy_source);
				cmd_list->barrier(targets[0], api::resource_usage::copy_source, target_state);
			}
			// Also after a failure: the list state may have been changed before it
			restore_list(native_list, bound, list);

			if (!ok)
				return fail("FSR could not be dispatched (see the FSR lines in the log)");
			replace::save_render_scale(data, inputs.width, inputs.out_width);
			mark_applied(data);
			return true;
		}

		bool fsr_native(api::command_list *cmd_list, api::device *device, DeviceData &data, const PassRecord &taa, const replace::ListState &list)
		{
			const api::resource colour = input_texture(taa, kColourInput);
			if (colour == 0)
				return fail("the current colour (t0) could not be resolved");

			api::resource targets[kTargetCount] = {};
			const uint32_t target_count = collect_targets(device, data, list, device->get_resource_desc(colour), colour, targets);
			if (target_count == 0)
				return fail("render target 0 of the TAA draw does not match the current colour in size or format");

			return run_fsr(cmd_list, device, data, taa, taa, list, targets, target_count, api::resource_usage::render_target);
		}
	}

	replace::Result replace::at_taa(ReplaceMode mode, api::command_list *cmd_list, api::device *device, DeviceData &data, const PassRecord &taa, const ListState &list)
	{
		switch (mode)
		{
		case ReplaceMode::Bypass:
			return passthrough(cmd_list, device, data, taa, list, true) ? Result::Applied : Result::NotApplied;
		case ReplaceMode::Fsr:
			// With the game's FSR 1 on (seen in the previous frame), FSR 3.1 runs at the EASU or RCAS dispatch instead.
			// The TAA output still gets the current colour, for whatever else reads it. The history does not:
			// only the TAA draws read it (every pass of the captured frame checked, docs/TECHNICAL.md), and they are skipped.
			if (data.game_fsr1_seen.load(std::memory_order_relaxed))
				return passthrough(cmd_list, device, data, taa, list, false) ? Result::Deferred : Result::NotApplied;
			return fsr_native(cmd_list, device, data, taa, list) ? Result::Applied : Result::NotApplied;
		default:
			return Result::NotApplied;
		}
	}

	bool replace::at_fsr1(api::command_list *cmd_list, api::device *device, DeviceData &data, const PassRecord &taa, const PassRecord &pass, const ListState &list)
	{
		// The output of EASU or RCAS: the one texture bound for unordered access. It is in that state for the dispatch.
		api::resource output = { 0 };
		for (const BoundTable &table : pass.tables)
			for (const BoundDescriptor &bound : table.descriptors)
				if (output == 0 && bound.status == SlotStatus::Ok && bound.desc.type == DescType::Uav && bound.resource_known && bound.info.type == api::resource_type::texture_2d)
					output = { bound.desc.resource };
		if (output == 0)
			return fail("the output of the FSR 1 dispatch could not be resolved");

		const api::resource_desc output_desc = device->get_resource_desc(output);
		if (output_desc.type != api::resource_type::texture_2d || output_desc.texture.samples != 1 ||
			api::format_to_typeless(output_desc.texture.format) != api::format_to_typeless(api::format::r8g8b8a8_unorm))
			return fail("the output of the FSR 1 dispatch is not an 8-bit RGBA texture");

		return run_fsr(cmd_list, device, data, taa, pass, list, &output, 1, api::resource_usage::unordered_access);
	}

	namespace
	{
		const wchar_t *const kModeNames[3] = { L"GameTAA", L"Bypass", L"FSR31" }; // Indexed by ReplaceMode

		bool settings_enabled()
		{
			return settings::file_enabled();
		}
		std::wstring settings_path()
		{
			return settings::path();
		}
	}

	ReplaceMode replace::load_mode()
	{
		if (!settings_enabled())
			return ReplaceMode::GameTaa;
		wchar_t text[32] = {};
		GetPrivateProfileStringW(L"ACVUpscaler", L"Mode", kModeNames[0], text, 32, settings_path().c_str());
		for (uint32_t i = 0; i < 3; ++i)
			if (_wcsicmp(text, kModeNames[i]) == 0)
				return static_cast<ReplaceMode>(i);
		return ReplaceMode::GameTaa;
	}

	void replace::set_mode(DeviceData &data, ReplaceMode mode, const char *source)
	{
		data.replace_mode.store(static_cast<uint32_t>(mode), std::memory_order_relaxed);
		log::write("%s: replacement switched to \"%s\" (frame %llu, %.2f ms per frame before the switch)", source, mode_name(mode),
			data.frame_index.load(std::memory_order_relaxed), static_cast<double>(data.average_frame_ms.load(std::memory_order_relaxed)));
		if (settings_enabled())
			WritePrivateProfileStringW(L"ACVUpscaler", L"Mode", kModeNames[static_cast<uint32_t>(mode)], settings_path().c_str());
	}

	float replace::session_mip_bias()
	{
		static const float s_bias = []() {
			if (!settings_enabled() || !settings::get().lod_bias || load_mode() != ReplaceMode::Fsr)
				return 0.0f;
			wchar_t text[32] = {};
			GetPrivateProfileStringW(L"ACVUpscaler", L"RenderScale", L"1", text, 32, settings_path().c_str());
			const float scale = static_cast<float>(_wtof(text));
			if (!(scale >= 0.3f && scale < 0.99f))
				return 0.0f;
			const float bias = std::log2(scale);
			log::write("texture LOD bias %+.2f for this session (render scale %.3f in the last session with FSR 3.1)", static_cast<double>(bias), static_cast<double>(scale));
			return bias;
		}();
		return s_bias;
	}

	void replace::save_render_scale(DeviceData &data, uint32_t render_width, uint32_t output_width)
	{
		if (output_width == 0 || !settings_enabled())
			return;
		// The title screen is drawn at the display resolution whatever the FSR setting of the game is. A scale of
		// one therefore counts only after a minute of frames without a single upscaled one in this session.
		static std::atomic<bool> s_upscaled { false };
		static std::atomic<uint32_t> s_native_frames { 0 };
		if (render_width < output_width)
			s_upscaled.store(true, std::memory_order_relaxed);
		else if (s_upscaled.load(std::memory_order_relaxed) || s_native_frames.fetch_add(1, std::memory_order_relaxed) < 3600)
			return;
		const float scale = static_cast<float>(render_width) / static_cast<float>(output_width);
		if (data.saved_render_scale.exchange(scale, std::memory_order_relaxed) == scale)
			return;
		wchar_t text[32];
		swprintf_s(text, L"%.4f", static_cast<double>(scale));
		WritePrivateProfileStringW(L"ACVUpscaler", L"RenderScale", text, settings_path().c_str());
		log::write("render scale %.4f saved for the texture LOD bias of the next start", static_cast<double>(scale));
	}

	const char *replace::state(const DeviceData &data)
	{
		if (data.replace_mode.load(std::memory_order_relaxed) == static_cast<uint32_t>(ReplaceMode::GameTaa))
			return "off";
		// Applied in the frame being recorded or the one before
		return data.replace_applied_frame.load(std::memory_order_relaxed) + 2 > data.frame_index.load(std::memory_order_relaxed) ? "active" : "requested, but not applied (see the log)";
	}

	const char *replace::mode_name(ReplaceMode mode)
	{
		switch (mode)
		{
		case ReplaceMode::Bypass:
			return "TAA bypass (no anti-aliasing, test)";
		case ReplaceMode::Fsr:
			return "FSR 3.1 (replaces the game TAA, and its FSR 1 upscale when that is on)";
		default:
			return "Game TAA (add-on passive)";
		}
	}
}
