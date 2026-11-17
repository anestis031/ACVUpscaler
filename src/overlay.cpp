// Settings panel in ReShade's Add-ons tab: a short one for players, the full diagnostic one with DevMode=1.
#include "overlay.hpp"
#include "frame.hpp"
#include "autotest.hpp"
#include "fsr.hpp"
#include "reflex.hpp"
#include "replace.hpp"
#include "snapshot.hpp"
#include "record.hpp"

#include <cmath>

namespace acv
{
	static uint32_t matched_pipelines(const PipelineRegistry::Counts &pipelines)
	{
		uint32_t matched = 0;
		for (size_t i = 1; i < kKindCount; ++i)
			matched += pipelines.matched[i];
		return matched;
	}

	// Sharpening, live. The game's pass only exists while its FSR 1 is on; the add-on's own pass runs
	// wherever the game's does not follow the upscaler.
	static void draw_sharpening(DeviceData &data)
	{
		ImGui::Separator();
		const bool game_pass_exists = data.game_fsr1_seen.load(std::memory_order_relaxed);
		bool game_sharpening = settings::game_sharpening();
		ImGui::BeginDisabled(!game_pass_exists);
		if (ImGui::Checkbox("Sharpening pass of the game on top (fixed strength)", &game_sharpening))
		{
			settings::set_game_sharpening(game_sharpening);
			data.rcas_replace_failed.store(false, std::memory_order_relaxed);
		}
		ImGui::EndDisabled();
		const bool own_runs = !game_pass_exists || !game_sharpening;
		int sharpness = static_cast<int>(settings::sharpness());
		ImGui::BeginDisabled(!own_runs);
		if (ImGui::SliderInt("Own sharpening", &sharpness, 0, 100, "%d %%", ImGuiSliderFlags_AlwaysClamp))
			settings::set_sharpness(static_cast<uint32_t>(sharpness), false);
		if (ImGui::IsItemDeactivatedAfterEdit())
			settings::set_sharpness(static_cast<uint32_t>(sharpness), true);
		ImGui::EndDisabled();
		ImGui::TextWrapped(own_runs ? "0 is no sharpening, 84 is as strong as the game's pass. Ctrl+click the slider to type a value." :
			"Untick the game's pass to set the strength yourself.");
	}

	// Reflex, live. Independent of the replacement: it also works with the add-on switched off.
	static void draw_reflex()
	{
		ImGui::Separator();
		bool on = settings::reflex();
		if (ImGui::Checkbox("NVIDIA Reflex low latency (limited form)", &on))
			settings::set_reflex(on);
		ImGui::TextWrapped("Reflex: %s. Keeps frames from waiting in a queue while the graphics card is the limit. The game does not support Reflex itself, so the wait sits at the end of the frame, not before the input is read.", reflex::status().c_str());
	}

	// What a player needs: is it working, on or off, and what to do when something is off
	static void draw_user_panel(DeviceData &data)
	{
		const PipelineRegistry::Counts pipelines = data.pipelines.counts();
		if (matched_pipelines(pipelines) == 0 && pipelines.created > 1000)
		{
			ImGui::TextWrapped("The shaders of this game version were not recognised. The add-on does nothing and the game runs with its own anti-aliasing.");
			return;
		}

		const uint32_t mode = data.replace_mode.load(std::memory_order_relaxed);
		const bool on = mode != static_cast<uint32_t>(ReplaceMode::GameTaa);
		if (ImGui::RadioButton("Off: the game's own TAA / FSR 1", !on) && on)
			replace::set_mode(data, ReplaceMode::GameTaa, "overlay");
		if (ImGui::RadioButton(settings::get().dlss ? "On: DLSS (FSR 3.1 where DLSS cannot run)" : "On: FSR 3.1", on) && !on)
			replace::set_mode(data, ReplaceMode::Fsr, "overlay");

		ImGui::Text("Status: %s", replace::state(data));
		ImGui::TextWrapped("Upscaler: %s", data.fsr->status().c_str());
		if (settings::get().dlss)
			ImGui::TextWrapped("Uses NVIDIA DLSS, through NVIDIA's DLSS SDK. The file nvngx_dlss.dll is NVIDIA's and not part of this add-on.");
		if (const uint64_t declined = data.declined_lists.load(std::memory_order_relaxed))
			ImGui::Text("Frames left to the game (camera cuts): %llu", declined);
		const float frame_ms = data.average_frame_ms.load(std::memory_order_relaxed);
		if (frame_ms > 0.0f)
			ImGui::Text("%.0f fps (%.2f ms)", 1000.0 / static_cast<double>(frame_ms), static_cast<double>(frame_ms));

		if (settings::get().lod_bias)
		{
			// The game creates its samplers once at start, so the bias follows the render scale of the last session
			const float bias = replace::session_mip_bias();
			const float scale = data.saved_render_scale.load(std::memory_order_relaxed);
			const float wanted = scale > 0.0f && scale < 0.99f ? std::log2(scale) : 0.0f;
			ImGui::Text("Texture LOD bias: %+.2f", static_cast<double>(bias));
			if (on && scale > 0.0f && std::fabs(wanted - bias) > 0.05f)
				ImGui::TextWrapped("The render resolution changed: restart the game once so that textures get the right sharpness (%+.2f).", static_cast<double>(wanted));
		}

		draw_sharpening(data);
		draw_reflex();

		ImGui::Separator();
		ImGui::TextWrapped("Other settings: ACVUpscalerSetup.exe (with the game closed). Log: %sacvupscaler.log", log::directory_utf8().c_str());
	}

	// First texture bound with the given descriptor type, in root parameter order
	static const BoundDescriptor *first_texture(const PassRecord &record, DescType type)
	{
		for (const BoundTable &table : record.tables)
			for (const BoundDescriptor &bound : table.descriptors)
				if (bound.status == SlotStatus::Ok && bound.desc.type == type && bound.resource_known && bound.info.type != api::resource_type::buffer)
					return &bound;
		return nullptr;
	}

	static void draw_scaler_line(const char *name, const PassRecord &record)
	{
		if (!record.valid)
			return;
		const BoundDescriptor *const input = first_texture(record, DescType::Srv);
		const BoundDescriptor *const output = first_texture(record, DescType::Uav);
		if (input != nullptr && output != nullptr)
			ImGui::Text("%s: %ux%u -> %ux%u", name, input->info.width, input->info.height, output->info.width, output->info.height);
		else
			ImGui::Text("%s: bindings not resolved (%u tables bound)", name, static_cast<unsigned int>(record.tables.size()));
	}

	void overlay::draw(api::effect_runtime *runtime)
	{
		guarded("overlay", [&]() {
			static std::atomic<bool> s_logged { false };
			if (!s_logged.exchange(true, std::memory_order_relaxed))
				log::write("settings panel drawn for the first time");

			ImGui::TextWrapped("%s", "ACVUpscaler " ACV_VERSION);

			DeviceData *const data = runtime != nullptr ? device_data(runtime->get_device()) : nullptr;
			if (data == nullptr)
			{
				ImGui::TextUnformatted("Inactive: this is not a Direct3D 12 device.");
				return;
			}

			ImGui::Separator();

			if (!settings::get().dev_mode)
			{
				draw_user_panel(*data);
				return;
			}

			const PipelineRegistry::Counts pipelines = data->pipelines.counts();
			ImGui::TextWrapped("Pipelines seen: %llu (%llu alive, %llu without PS/CS bytecode)", pipelines.created, pipelines.alive, pipelines.without_bytecode);

			const FrameStats stats = data->published_copy();
			ImGui::Text("Frame %llu: %s", stats.frame, detect_mode(stats));
			{
				const float frame_ms = data->average_frame_ms.load(std::memory_order_relaxed);
				if (frame_ms > 0.0f)
					ImGui::Text("%.1f fps (%.2f ms, average of the last %u frames)", 1000.0 / static_cast<double>(frame_ms), static_cast<double>(frame_ms), kFrameTimeRing);
			}
			ImGui::TextWrapped("Descriptors written last frame: about %llu, copied: about %llu", stats.descriptors_written, stats.descriptors_copied);

			if (ImGui::BeginTable("##acv_kinds", 3, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp))
			{
				ImGui::TableSetupColumn("Pass");
				ImGui::TableSetupColumn("Pipelines matched");
				ImGui::TableSetupColumn("Last frame");
				ImGui::TableHeadersRow();
				for (size_t i = 1; i < kKindCount; ++i)
				{
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(kind_name(static_cast<Kind>(i)));
					ImGui::TableNextColumn();
					ImGui::Text("%u", pipelines.matched[i]);
					ImGui::TableNextColumn();
					ImGui::Text("%u", stats.counts[i]);
				}
				ImGui::EndTable();
			}

			char format[16];
			if (stats.taa.valid && stats.taa.rt0.known)
				ImGui::TextWrapped("Render size (TAA RT0): %ux%u %s", stats.taa.rt0.info.width, stats.taa.rt0.info.height, format_name(stats.taa.rt0.info.format, format));
			else
				ImGui::TextUnformatted("Render size (TAA RT0): unknown");
			ImGui::TextWrapped("Output size (swap chain): %ux%u %s",
				data->swap_width.load(std::memory_order_relaxed), data->swap_height.load(std::memory_order_relaxed),
				format_name(static_cast<api::format>(data->swap_format.load(std::memory_order_relaxed)), format));

			if (stats.taa.valid && stats.taa.taa_constants_result == ReadResult::Ok)
			{
				const auto &rows = stats.taa.taa_constants;
				ImGui::TextWrapped("TAA constants: output %gx%g, input %gx%g",
					static_cast<double>(rows[1][0]), static_cast<double>(rows[1][1]), static_cast<double>(rows[2][0]), static_cast<double>(rows[2][1]));
				ImGui::TextWrapped("Jitter (pixels): x %+.6f  y %+.6f", static_cast<double>(rows[3][0]), static_cast<double>(rows[3][1]));
			}
			else if (stats.taa.valid)
			{
				ImGui::TextWrapped("TAA constants: %s", read_result_name(stats.taa.taa_constants_result));
			}
			else
			{
				ImGui::TextUnformatted("TAA constants: no TAA draw in the last frame");
			}

			draw_scaler_line("EASU", stats.easu);
			draw_scaler_line("RCAS", stats.rcas);

			ImGui::Separator();

			if (ImGui::Button("Log one frame (F9)"))
				frame::request_capture(*data);
			ImGui::SameLine();
			ImGui::Text("capture: %s", frame::capture_state(*data));

			ImGui::TextWrapped("Logs: %sacvupscaler.log, acvupscaler_frame_NNN.log, acvupscaler_snap_NNN*", log::directory_utf8().c_str());

			ImGui::Separator();

			ImGui::TextUnformatted("Replacement (F7 switches between the game's own TAA / FSR 1 and FSR 3.1):");
			const uint32_t mode = data->replace_mode.load(std::memory_order_relaxed);
			for (uint32_t i = 0; i < 3; ++i)
			{
				// Order on screen: game TAA, FSR 3.1, bypass
				static constexpr ReplaceMode kModes[3] = { ReplaceMode::GameTaa, ReplaceMode::Fsr, ReplaceMode::Bypass };
				if (ImGui::RadioButton(replace::mode_name(kModes[i]), mode == static_cast<uint32_t>(kModes[i])) && mode != static_cast<uint32_t>(kModes[i]))
				{
					replace::set_mode(*data, kModes[i], "overlay");
				}
			}
			ImGui::Text("replacement: %s", replace::state(*data));
			ImGui::TextWrapped("Upscaler: %s", data->fsr->status().c_str());
			ImGui::TextWrapped("Texture LOD bias this session: %+.2f (follows the render scale of the previous session: restart the game after changing its FSR quality)", static_cast<double>(replace::session_mip_bias()));
			draw_sharpening(*data);
			draw_reflex();
			ImGui::Separator();

			const auto toggle = [data](const char *label, std::atomic<bool> &value) {
				bool checked = value.load(std::memory_order_relaxed);
				if (ImGui::Checkbox(label, &checked))
				{
					value.store(checked, std::memory_order_relaxed);
					log::write("overlay: \"%s\" %s (frame %llu)", label, checked ? "on" : "off", data->frame_index.load(std::memory_order_relaxed));
				}
			};
			ImGui::TextUnformatted("FSR test switches (all off is the expected setting):");
			toggle("Flip jitter X", data->fsr_flip_jitter_x);
			ImGui::SameLine();
			toggle("Flip jitter Y", data->fsr_flip_jitter_y);
			ImGui::SameLine();
			toggle("Flip motion vectors", data->fsr_flip_motion);

			// Rain, fire and smoke have no motion vectors: t3 tells FSR where they are
			float reactive_gain = data->fsr_reactive_gain.load(std::memory_order_relaxed);
			if (ImGui::SliderFloat("Particle mask strength (0 = off)", &reactive_gain, 0.0f, 64.0f, "%.0f"))
				data->fsr_reactive_gain.store(reactive_gain, std::memory_order_relaxed);

			if (ImGui::Button("Run the particle mask test (F6)"))
				autotest::start(*data);
			ImGui::SameLine();
			ImGui::Text("test: %s", autotest::state(*data).c_str());

			ImGui::Separator();

			if (ImGui::Button("Record bursts of frames (F10 starts and stops)"))
				record::toggle(*data);
			ImGui::SameLine();
			ImGui::Text("record: %s", record::state().c_str());

			if (ImGui::Button("Snapshot TAA inputs (F8)"))
				snapshot::request(*data);
			ImGui::SameLine();
			ImGui::Text("snapshot: %s", snapshot::state(*data));

			Snapshot &snap = data->snapshot;
			const std::lock_guard<std::mutex> lock(snap.preview_mutex);
			if (snap.done != 0)
				ImGui::Text("Snapshots written: %u. Images below are from the last one.", snap.done);
			static const char *const kPreviewTitles[2] = {
				"Motion vectors (grey = still, red/green = horizontal/vertical motion)",
				"Depth (dark = near, bright = far)" };
			for (uint32_t i = 0; i < 2; ++i)
			{
				if (snap.preview_view[i] == 0 || snap.preview_width == 0)
					continue;
				ImGui::TextUnformatted(kPreviewTitles[i]);
				const float width = ImGui::GetContentRegionAvail().x;
				ImGui::Image(static_cast<ImTextureID>(snap.preview_view[i].handle), ImVec2(width, width * static_cast<float>(snap.preview_height) / static_cast<float>(snap.preview_width)), ImVec2(0, 0), ImVec2(1, 1));
			}
		});
	}

	void overlay::on_reshade_present(api::effect_runtime *runtime)
	{
		// Called on the presenting thread after ReShade drew its overlay; the key state is that of the frame just presented
		guarded("reshade_present", [&]() {
			if (runtime == nullptr)
				return;

			// Once, after a minute of frames: tell the log when this is a game version the add-on does not know
			static bool s_version_checked = false; // Presenting thread only
			if (!s_version_checked)
			{
				if (DeviceData *const data = device_data(runtime->get_device()); data != nullptr && data->frame_index.load(std::memory_order_relaxed) > 3600)
				{
					s_version_checked = true;
					if (matched_pipelines(data->pipelines.counts()) == 0)
						log::warn("none of the known shaders of the game was seen in the first 3600 frames: unsupported game version, the add-on stays passive");
				}
			}

			// The keys belong to the developer tools
			if (!settings::get().dev_mode)
				return;
			const bool capture = runtime->is_key_pressed(VK_F9), snap = runtime->is_key_pressed(VK_F8), bypass = runtime->is_key_pressed(VK_F7), test = runtime->is_key_pressed(VK_F6);
			const bool bursts = runtime->is_key_pressed(VK_F10);
			if (!capture && !snap && !bypass && !test && !bursts)
				return;
			if (DeviceData *const data = device_data(runtime->get_device()))
			{
				if (capture)
					frame::request_capture(*data);
				if (snap)
					snapshot::request(*data);
				if (test)
					autotest::start(*data);
				if (bursts)
					record::toggle(*data);
				if (bypass)
				{
					const ReplaceMode next = data->replace_mode.load(std::memory_order_relaxed) == static_cast<uint32_t>(ReplaceMode::GameTaa) ? ReplaceMode::Fsr : ReplaceMode::GameTaa;
					replace::set_mode(*data, next, "F7");
				}
			}
		});
	}
}
