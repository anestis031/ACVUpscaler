#include "snapshot.hpp"
#include "fsr.hpp"
#include "replace.hpp"

#include <cmath>
#include <share.h>

namespace acv
{
	namespace
	{
		constexpr uint64_t kDelayFrames = 4;    // Presents between the copy and the readback
		constexpr uint64_t kArmedTimeout = 600; // Presents to wait for a TAA draw before giving up
		constexpr uint32_t kPreviewDivisor = 2;

		constexpr uint32_t kTextureCount = kTaaInputCount + kFsrDebugTextureCount;
		const char *const kInputNames[kTextureCount] = { "t0_colour", "t1_velocity", "t2_lineardepth", "t3_mask", "t4_history", "t5_maskhistory", "t6_dedithered", "fsr_motion", "fsr_depth", "fsr_output", "fsr_reactive" };
		constexpr uint32_t kVelocityInput = 1, kDepthInput = 2;

		bool write_rows(const std::wstring &path, const api::subresource_data &mapped, uint32_t row_size, uint32_t height)
		{
			std::FILE *const file = _wfsopen(path.c_str(), L"wb", _SH_DENYWR);
			if (file == nullptr)
				return false;
			std::setvbuf(file, nullptr, _IOFBF, 1 << 20);

			bool ok = true;
			for (uint32_t y = 0; y < height && ok; ++y)
				ok = std::fwrite(static_cast<const uint8_t *>(mapped.data) + static_cast<size_t>(y) * mapped.row_pitch, 1, row_size, file) == row_size;
			return std::fclose(file) == 0 && ok;
		}

		// Stored 12-bit value -> motion in UV units: e = q / 2047 - 1, m = sign(e) * 0.2 * e^4 (docs/TECHNICAL.md)
		float decode_velocity_axis(uint32_t q)
		{
			const float e = static_cast<float>(q) * (2.0f / 4094.0f) - 1.0f;
			const float e2 = e * e;
			return std::copysign(0.2f * e2 * e2, e);
		}
		// Motion in pixels -> 0..255 around 128, on a square root scale that saturates at 64 pixels
		uint8_t motion_to_byte(float pixels)
		{
			const float magnitude = std::sqrt(std::fmin(std::fabs(pixels) / 64.0f, 1.0f));
			return static_cast<uint8_t>(127.5f + std::copysign(127.5f * magnitude, pixels));
		}

		// Red = horizontal motion, green = vertical motion, grey = none
		void build_velocity_preview(const api::subresource_data &mapped, uint32_t width, uint32_t height, std::vector<uint8_t> &out)
		{
			const uint32_t preview_width = width / kPreviewDivisor, preview_height = height / kPreviewDivisor;
			out.resize(static_cast<size_t>(preview_width) * preview_height * 4);
			uint8_t *dest = out.data();
			for (uint32_t y = 0; y < preview_height; ++y)
			{
				const uint8_t *const row = static_cast<const uint8_t *>(mapped.data) + static_cast<size_t>(y) * kPreviewDivisor * mapped.row_pitch;
				for (uint32_t x = 0; x < preview_width; ++x, dest += 4)
				{
					// R = x >> 4, G = (x & 15) << 4 | y >> 8, B = y & 255
					const uint8_t *const p = row + static_cast<size_t>(x) * kPreviewDivisor * 4;
					const uint32_t qx = p[0] * 16u + (p[1] >> 4), qy = (p[1] & 15u) * 256u + p[2];
					dest[0] = motion_to_byte(decode_velocity_axis(qx) * static_cast<float>(width));
					dest[1] = motion_to_byte(decode_velocity_axis(qy) * static_cast<float>(height));
					dest[2] = 128;
					dest[3] = 255;
				}
			}
		}

		// View-space distance on a logarithmic grey scale: 0.1 (near plane) = black, 60000 = white
		void build_depth_preview(const api::subresource_data &mapped, uint32_t width, uint32_t height, std::vector<uint8_t> &out)
		{
			const uint32_t preview_width = width / kPreviewDivisor, preview_height = height / kPreviewDivisor;
			out.resize(static_cast<size_t>(preview_width) * preview_height * 4);
			const float scale = 1.0f / std::log(600000.0f);
			uint8_t *dest = out.data();
			for (uint32_t y = 0; y < preview_height; ++y)
			{
				const uint8_t *const row = static_cast<const uint8_t *>(mapped.data) + static_cast<size_t>(y) * kPreviewDivisor * mapped.row_pitch;
				for (uint32_t x = 0; x < preview_width; ++x, dest += 4)
				{
					float distance;
					std::memcpy(&distance, row + static_cast<size_t>(x) * kPreviewDivisor * 4, sizeof(distance));
					if (std::isfinite(distance) && distance > 0.0f)
					{
						const float grey = std::fmin(std::fmax(std::log(distance * 10.0f) * scale, 0.0f), 1.0f);
						dest[0] = dest[1] = dest[2] = static_cast<uint8_t>(grey * 255.0f);
					}
					else
					{
						dest[0] = 255, dest[1] = 0, dest[2] = 255; // Not a distance
					}
					dest[3] = 255;
				}
			}
		}

		bool create_preview(api::device *device, const std::vector<uint8_t> &pixels, uint32_t width, uint32_t height, api::resource &resource, api::resource_view &view)
		{
			// ReShade uploads the pixels through its immediate command list. That is safe inside 'present',
			// where ReShade holds the lock of the command queue.
			const api::resource_desc desc(width, height, 1, 1, api::format::r8g8b8a8_unorm, 1, api::memory_heap::default_, api::resource_usage::shader_resource | api::resource_usage::copy_dest);
			const api::subresource_data initial = { const_cast<uint8_t *>(pixels.data()), width * 4, width * height * 4 };
			if (!device->create_resource(desc, &initial, api::resource_usage::shader_resource, &resource))
				return false;
			if (!device->create_resource_view(resource, api::resource_usage::shader_resource, api::resource_view_desc(api::format::r8g8b8a8_unorm), &view))
			{
				device->destroy_resource(resource);
				resource = { 0 };
				return false;
			}
			return true;
		}

		void destroy_previews(api::device *device, Snapshot &snap)
		{
			for (uint32_t i = 0; i < 2; ++i)
			{
				if (snap.preview_view[i] != 0)
					device->destroy_resource_view(snap.preview_view[i]);
				if (snap.preview_resource[i] != 0)
					device->destroy_resource(snap.preview_resource[i]);
				snap.preview_view[i] = { 0 };
				snap.preview_resource[i] = { 0 };
			}
			snap.preview_width = snap.preview_height = 0;
		}

		void finish(api::command_queue *queue, api::device *device, DeviceData &data, uint64_t frame)
		{
			Snapshot &snap = data.snapshot;

			// The copies went to the GPU with the game's own command lists several presents ago. Waiting here
			// makes sure they have run, and that nothing still uses the previous preview textures.
			queue->wait_idle();

			static unsigned int s_snapshot_index = 0; // Presenting thread only
			const unsigned int index = ++s_snapshot_index;

			std::string info;
			append_format(info, "ACVUpscaler TAA input snapshot #%u, add-on version " ACV_VERSION "\n", index);
			append_format(info, "copied right before the first TAA draw recorded in frame %llu, read back at frame %llu\n", snap.record_frame, frame);
			info += snap.settings + "\n";
			info += "Raw files: rows from top to bottom without padding, pixels as the format says.\n\n";

			std::vector<uint8_t> preview_pixels[2];
			uint32_t preview_source_width = 0, preview_source_height = 0;
			uint32_t written = 0;

			for (uint32_t i = 0; i < kTextureCount; ++i)
			{
				Snapshot::Texture &texture = snap.textures[i];
				if (texture.readback == 0)
				{
					append_format(info, "%s: not copied\n", kInputNames[i]);
					continue;
				}

				char format[16];
				const char *const format_text = format_name(texture.format, format);
				const uint32_t row_size = api::format_row_pitch(texture.format, texture.width);

				api::subresource_data mapped = {};
				if (row_size != 0 && device->map_texture_region(texture.readback, 0, nullptr, api::map_access::read_only, &mapped) && mapped.data != nullptr)
				{
					wchar_t name[128];
					swprintf_s(name, L"acvupscaler_snap_%03u_%hs_%ux%u_%hs.bin", index, kInputNames[i], texture.width, texture.height, format_text);
					const bool ok = write_rows(log::directory() + name, mapped, row_size, texture.height);
					append_format(info, "%s: %ux%u %s, %u bytes per row -> %ls%s\n", kInputNames[i], texture.width, texture.height, format_text, row_size, name, ok ? "" : " (WRITE FAILED)");
					written += ok ? 1 : 0;

					const api::format typed = api::format_to_default_typed(texture.format);
					if (i == kVelocityInput && typed == api::format::r8g8b8a8_unorm)
					{
						build_velocity_preview(mapped, texture.width, texture.height, preview_pixels[0]);
						preview_source_width = texture.width, preview_source_height = texture.height;
					}
					if (i == kDepthInput && typed == api::format::r32_float)
					{
						build_depth_preview(mapped, texture.width, texture.height, preview_pixels[1]);
						preview_source_width = texture.width, preview_source_height = texture.height;
					}

					device->unmap_texture_region(texture.readback, 0);
				}
				else
				{
					append_format(info, "%s: %ux%u %s, could not be read back\n", kInputNames[i], texture.width, texture.height, format_text);
				}

				device->destroy_resource(texture.readback);
				texture = Snapshot::Texture();
			}

			info += "\nBindings and constants of that TAA draw:\n";
			frame::append_pass_text(info, snap.record);
			info += "\n";
			frame::append_jitter_history(info, data, frame);
			snap.record = PassRecord();

			wchar_t info_name[48];
			swprintf_s(info_name, L"acvupscaler_snap_%03u.txt", index);
			if (std::FILE *const file = _wfsopen((log::directory() + info_name).c_str(), L"w", _SH_DENYWR))
			{
				std::fwrite(info.data(), 1, info.size(), file);
				std::fclose(file);
			}

			{
				const std::lock_guard<std::mutex> lock(snap.preview_mutex);
				destroy_previews(device, snap);
				const uint32_t preview_width = preview_source_width / kPreviewDivisor, preview_height = preview_source_height / kPreviewDivisor;
				for (uint32_t i = 0; i < 2; ++i)
				{
					if (preview_pixels[i].empty() || preview_pixels[i].size() != static_cast<size_t>(preview_width) * preview_height * 4)
						continue;
					if (create_preview(device, preview_pixels[i], preview_width, preview_height, snap.preview_resource[i], snap.preview_view[i]))
						snap.preview_width = preview_width, snap.preview_height = preview_height;
				}
				++snap.done;
			}

			log::write("TAA input snapshot #%u: %u textures written (acvupscaler_snap_%03u_*.bin), copied in frame %llu", index, written, index, snap.record_frame);
			snap.stage.store(Snapshot::Idle, std::memory_order_release);
		}
	}

	bool snapshot::request(DeviceData &data)
	{
		uint32_t expected = Snapshot::Idle;
		if (!data.snapshot.stage.compare_exchange_strong(expected, Snapshot::Copying))
			return false;
		// Nothing else touches the snapshot between these two stores ('Copying' keeps every other thread out)
		data.snapshot.armed_frame = data.frame_index.load(std::memory_order_relaxed);
		data.snapshot.stage.store(Snapshot::Armed, std::memory_order_release);
		log::write("TAA input snapshot requested");
		return true;
	}

	const char *snapshot::state(const DeviceData &data)
	{
		switch (data.snapshot.stage.load(std::memory_order_relaxed))
		{
		case Snapshot::Idle:
			return "idle";
		case Snapshot::Armed:
			return "waiting for a TAA draw";
		default:
			return "reading back";
		}
	}

	void snapshot::copy_inputs(api::command_list *cmd_list, api::device *device, DeviceData &data, const PassRecord &record)
	{
		Snapshot &snap = data.snapshot;

		uint32_t expected = Snapshot::Armed;
		if (!snap.stage.compare_exchange_strong(expected, Snapshot::Copying))
			return; // Another thread got here first

		uint32_t copied = 0;
		if (const BoundTable *const table = record.find_table(kTaaPassParam))
		{
			for (uint32_t i = 0; i < kTaaInputCount; ++i)
			{
				const BoundDescriptor &bound = table->descriptors[kTaaFirstInput + i];
				Snapshot::Texture &texture = snap.textures[i];
				texture = Snapshot::Texture();

				// Only textures that are known to be alive (the draw being recorded keeps them alive from here on)
				if (bound.status != SlotStatus::Ok || bound.desc.type != DescType::Srv || !bound.resource_known || bound.info.type != api::resource_type::texture_2d)
					continue;

				const api::resource source = { bound.desc.resource };
				const api::resource_desc source_desc = device->get_resource_desc(source);
				if (source_desc.type != api::resource_type::texture_2d || source_desc.texture.samples != 1 || source_desc.heap != api::memory_heap::default_)
					continue;

				// ReShade creates a texture in the readback heap as a buffer and remembers its footprint
				const api::resource_desc readback_desc(source_desc.texture.width, source_desc.texture.height, 1, 1, source_desc.texture.format, 1, api::memory_heap::readback, api::resource_usage::copy_dest);
				if (!device->create_resource(readback_desc, nullptr, api::resource_usage::copy_dest, &texture.readback))
					continue;

				cmd_list->barrier(source, kGameReadState, api::resource_usage::copy_source);
				cmd_list->copy_texture_region(source, 0, nullptr, texture.readback, 0, nullptr);
				cmd_list->barrier(source, api::resource_usage::copy_source, kGameReadState);

				texture.width = source_desc.texture.width;
				texture.height = source_desc.texture.height;
				texture.format = source_desc.texture.format;
				++copied;
			}
		}


		// While FSR replaces the TAA: what the add-on fed it and what came out. This frame's when FSR ran at the
		// TAA draw (the replacement is recorded before the snapshot), the previous frame's when it runs at EASU.
		for (uint32_t i = 0; i < kFsrDebugTextureCount; ++i)
			snap.textures[kTaaInputCount + i] = Snapshot::Texture();
		if (data.replace_mode.load(std::memory_order_relaxed) == static_cast<uint32_t>(ReplaceMode::Fsr) &&
			data.replace_applied_frame.load(std::memory_order_relaxed) == data.frame_index.load(std::memory_order_relaxed) + 1)
		{
			Fsr::DebugTexture debug[kFsrDebugTextureCount];
			const uint32_t debug_count = data.fsr->debug_textures(debug);
			for (uint32_t i = 0; i < debug_count; ++i)
			{
				Snapshot::Texture &texture = snap.textures[kTaaInputCount + i];
				const api::resource source = { reinterpret_cast<uintptr_t>(debug[i].resource) };
				const api::resource_desc source_desc = device->get_resource_desc(source);
				const api::resource_desc readback_desc(source_desc.texture.width, source_desc.texture.height, 1, 1, source_desc.texture.format, 1, api::memory_heap::readback, api::resource_usage::copy_dest);
				if (!device->create_resource(readback_desc, nullptr, api::resource_usage::copy_dest, &texture.readback))
					continue;

				const api::resource_usage rest_state = static_cast<api::resource_usage>(debug[i].state); // Same values as D3D12_RESOURCE_STATES
				if (rest_state != api::resource_usage::copy_source)
					cmd_list->barrier(source, rest_state, api::resource_usage::copy_source);
				cmd_list->copy_texture_region(source, 0, nullptr, texture.readback, 0, nullptr);
				if (rest_state != api::resource_usage::copy_source)
					cmd_list->barrier(source, api::resource_usage::copy_source, rest_state);

				texture.width = source_desc.texture.width;
				texture.height = source_desc.texture.height;
				texture.format = source_desc.texture.format;
				++copied;
			}
		}
		if (copied == 0)
		{
			log::warn("TAA input snapshot cancelled: none of the inputs could be copied");
			snap.stage.store(Snapshot::Idle, std::memory_order_release);
			return;
		}

		snap.record = record;
		snap.settings.clear();
		append_format(snap.settings, "TAA replacement: %s; FSR switches: flip jitter x %d, flip jitter y %d, flip motion %d, reactive gain %.0f; average frame time %.2f ms",
			replace::mode_name(static_cast<ReplaceMode>(data.replace_mode.load(std::memory_order_relaxed))),
			data.fsr_flip_jitter_x.load(std::memory_order_relaxed) ? 1 : 0, data.fsr_flip_jitter_y.load(std::memory_order_relaxed) ? 1 : 0,
			data.fsr_flip_motion.load(std::memory_order_relaxed) ? 1 : 0, static_cast<double>(data.fsr_reactive_gain.load(std::memory_order_relaxed)),
			static_cast<double>(data.average_frame_ms.load(std::memory_order_relaxed)));
		snap.record_frame = data.frame_index.load(std::memory_order_relaxed) + 1;
		snap.stage.store(Snapshot::Copied, std::memory_order_release);
	}

	void snapshot::on_present(api::command_queue *queue, api::device *device, DeviceData &data, uint64_t frame)
	{
		Snapshot &snap = data.snapshot;
		switch (snap.stage.load(std::memory_order_acquire))
		{
		case Snapshot::Armed:
			if (frame > snap.armed_frame + kArmedTimeout)
			{
				uint32_t expected = Snapshot::Armed;
				if (snap.stage.compare_exchange_strong(expected, Snapshot::Idle))
					log::warn("TAA input snapshot cancelled: no TAA draw within %llu frames", kArmedTimeout);
			}
			break;
		case Snapshot::Copied:
			if (queue != nullptr && device != nullptr && frame >= snap.record_frame + kDelayFrames)
				finish(queue, device, data, frame);
			break;
		default:
			break;
		}
	}

	void snapshot::release(api::device *device, DeviceData &data)
	{
		Snapshot &snap = data.snapshot;
		for (Snapshot::Texture &texture : snap.textures)
		{
			if (texture.readback != 0)
				device->destroy_resource(texture.readback);
			texture = Snapshot::Texture();
		}

		const std::lock_guard<std::mutex> lock(snap.preview_mutex);
		destroy_previews(device, snap);
	}
}
