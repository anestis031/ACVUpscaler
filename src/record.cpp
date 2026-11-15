#include "record.hpp"
#include "fsr.hpp"
#include "replace.hpp"

#include <share.h>

namespace acv
{
	namespace
	{
		constexpr uint32_t kBurstFrames = 24;  // Consecutive frames per burst (three jitter cycles)
		constexpr uint32_t kMaxBursts = 30;    // Recording stops by itself after this many
		constexpr double kBurstInterval = 5.0; // Seconds from the start of one burst to the start of the next
		constexpr double kBurstTimeout = 3.0;  // A burst that takes longer (menu, loading screen) is dropped
		constexpr uint64_t kDelayFrames = 4;   // Presents between the last copy and the readback
		constexpr uint32_t kInputs = 4;        // t0..t3
		constexpr uint32_t kFsrOutput = 2;     // Index in Fsr::debug_textures
		const char *const kNames[kInputs] = { "t0_colour", "t1_velocity", "t2_lineardepth", "t3_mask" };

		enum Stage : uint32_t
		{
			Off,
			Waiting, // Between two bursts
			Burst,   // Copying one frame at every first TAA draw
			Output,  // The frame after the burst: copies the FSR result
			Flush    // Waiting for the GPU, then writing the files
		};

		struct Meta
		{
			uint64_t frame = 0;
			double time = 0.0;
			float jitter_x = 0.0f, jitter_y = 0.0f;
			float p00 = 0.0f, p11 = 0.0f, depth_a = 0.0f, depth_b = 0.0f;
			float sky[kViewSkyRowCount][4] = {}; // The rows kViewSkyRows of the view constants
			float taa_row6[4] = {};
		};

		struct Recorder
		{
			std::mutex mutex; // Guards everything below
			Stage stage = Off;
			bool stop_requested = false;
			uint32_t next_index = 1; // Number of the next burst file set (counts on over several recordings)
			uint32_t bursts_done = 0;
			uint32_t frames = 0;
			uint64_t last_frame = 0, flush_frame = 0;
			double next_burst_time = 0.0, burst_started = 0.0;

			uint32_t width = 0, height = 0;
			api::format formats[kInputs] = {};
			api::resource readback[kBurstFrames][kInputs] = {};
			Meta meta[kBurstFrames];

			api::resource output = {};
			uint32_t out_width = 0, out_height = 0;
			api::format out_format = api::format::unknown;
			bool game_fsr1 = false;
			std::string settings;
		};
		Recorder s_recorder;
		std::atomic<uint32_t> s_stage { Off }; // Copy of 'stage' for the checks without the lock

		double seconds_now()
		{
			LARGE_INTEGER now, frequency;
			QueryPerformanceCounter(&now);
			QueryPerformanceFrequency(&frequency);
			return static_cast<double>(now.QuadPart) / static_cast<double>(frequency.QuadPart);
		}

		void set_stage(Recorder &rec, Stage stage)
		{
			rec.stage = stage;
			s_stage.store(stage, std::memory_order_relaxed);
		}

		// Only while the GPU is idle or before the buffers were ever used
		void free_buffers(api::device *device, Recorder &rec)
		{
			for (auto &frame : rec.readback)
				for (api::resource &resource : frame)
				{
					if (resource != 0)
						device->destroy_resource(resource);
					resource = {};
				}
			if (rec.output != 0)
				device->destroy_resource(rec.output);
			rec.output = {};
			rec.width = rec.height = 0;
		}

		void drop_burst(Recorder &rec, const char *reason)
		{
			log::warn("burst record: burst dropped after %u frames: %s", rec.frames, reason);
			rec.frames = 0;
			set_stage(rec, Waiting);
		}

		// The textures t0..t3 of the TAA draw, when all of them can be copied
		bool resolve_inputs(api::device *device, const PassRecord &record, api::resource (&sources)[kInputs], api::resource_desc (&descs)[kInputs])
		{
			const BoundTable *const table = record.find_table(kTaaPassParam);
			if (table == nullptr)
				return false;
			for (uint32_t i = 0; i < kInputs; ++i)
			{
				const BoundDescriptor &bound = table->descriptors[kTaaFirstInput + i];
				if (bound.status != SlotStatus::Ok || bound.desc.type != DescType::Srv || !bound.resource_known || bound.info.type != api::resource_type::texture_2d)
					return false;
				sources[i] = { bound.desc.resource };
				descs[i] = device->get_resource_desc(sources[i]);
				if (descs[i].type != api::resource_type::texture_2d || descs[i].texture.samples != 1 || descs[i].heap != api::memory_heap::default_)
					return false;
				if (descs[i].texture.width != descs[0].texture.width || descs[i].texture.height != descs[0].texture.height)
					return false;
			}
			return true;
		}

		bool append_rows(std::FILE *file, const api::subresource_data &mapped, uint32_t row_size, uint32_t height)
		{
			bool ok = true;
			for (uint32_t y = 0; y < height && ok; ++y)
				ok = std::fwrite(static_cast<const uint8_t *>(mapped.data) + static_cast<size_t>(y) * mapped.row_pitch, 1, row_size, file) == row_size;
			return ok;
		}

		void write_burst(api::device *device, Recorder &rec)
		{
			const uint32_t index = rec.next_index++;
			uint32_t failed = 0;

			for (uint32_t i = 0; i < kInputs; ++i)
			{
				char format[16];
				const uint32_t row_size = api::format_row_pitch(rec.formats[i], rec.width);
				wchar_t name[128];
				swprintf_s(name, L"acvupscaler_rec_%03u_%hs_%ux%u_%hs_x%u.bin", index, kNames[i], rec.width, rec.height, format_name(rec.formats[i], format), rec.frames);
				std::FILE *const file = _wfsopen((log::directory() + name).c_str(), L"wb", _SH_DENYWR);
				if (file == nullptr || row_size == 0)
				{
					++failed;
					if (file != nullptr)
						std::fclose(file);
					continue;
				}
				std::setvbuf(file, nullptr, _IOFBF, 1 << 22);
				bool ok = true;
				for (uint32_t f = 0; f < rec.frames && ok; ++f)
				{
					api::subresource_data mapped = {};
					ok = device->map_texture_region(rec.readback[f][i], 0, nullptr, api::map_access::read_only, &mapped) && mapped.data != nullptr;
					if (ok)
					{
						ok = append_rows(file, mapped, row_size, rec.height);
						device->unmap_texture_region(rec.readback[f][i], 0);
					}
				}
				if (std::fclose(file) != 0 || !ok)
					++failed;
			}

			bool output_written = false;
			if (rec.output != 0)
			{
				char format[16];
				const uint32_t row_size = api::format_row_pitch(rec.out_format, rec.out_width);
				wchar_t name[128];
				swprintf_s(name, L"acvupscaler_rec_%03u_fsr_output_%ux%u_%hs.bin", index, rec.out_width, rec.out_height, format_name(rec.out_format, format));
				api::subresource_data mapped = {};
				if (row_size != 0 && device->map_texture_region(rec.output, 0, nullptr, api::map_access::read_only, &mapped) && mapped.data != nullptr)
				{
					if (std::FILE *const file = _wfsopen((log::directory() + name).c_str(), L"wb", _SH_DENYWR))
					{
						std::setvbuf(file, nullptr, _IOFBF, 1 << 22);
						output_written = append_rows(file, mapped, row_size, rec.out_height);
						output_written = std::fclose(file) == 0 && output_written;
					}
					device->unmap_texture_region(rec.output, 0);
				}
				device->destroy_resource(rec.output);
				rec.output = {};
			}

			// One line per frame, plain numbers: the test harness reads this file
			std::string info;
			append_format(info, "ACVUpscaler burst record #%u, add-on version " ACV_VERSION "\n", index);
			append_format(info, "%s\n", rec.settings.c_str());
			append_format(info, "frames %u render %u %u output %u %u fsr_output %d game_fsr1 %d\n", rec.frames, rec.width, rec.height, rec.out_width, rec.out_height, output_written ? 1 : 0, rec.game_fsr1 ? 1 : 0);
			info += "Per frame: F index, game frame, seconds since the first frame, jitter x, jitter y, projection [0][0], [1][1], depth a, depth b\n";
			info += "           S index, row 6 of the TAA constants, then the rows 6 7 8 38 39 41 54 69 70 71 72 of the view constants (sky reprojection)\n";
			info += "Files: all frames one after the other, rows from top to bottom without padding. fsr_output is the FSR 3.1 result\n";
			info += "of the last frame when the game's FSR 1 is on (FSR 3.1 then runs after the TAA draws), of the frame after it otherwise.\n";
			for (uint32_t f = 0; f < rec.frames; ++f)
			{
				const Meta &meta = rec.meta[f];
				append_format(info, "F %u %llu %.6f %.9g %.9g %.9g %.9g %.9g %.9g\n", f, meta.frame, meta.time - rec.meta[0].time,
					static_cast<double>(meta.jitter_x), static_cast<double>(meta.jitter_y), static_cast<double>(meta.p00), static_cast<double>(meta.p11),
					static_cast<double>(meta.depth_a), static_cast<double>(meta.depth_b));
				append_format(info, "S %u", f);
				for (uint32_t i = 0; i < 4; ++i)
					append_format(info, " %.9g", static_cast<double>(meta.taa_row6[i]));
				for (uint32_t row = 0; row < kViewSkyRowCount; ++row)
					for (uint32_t i = 0; i < 4; ++i)
						append_format(info, " %.9g", static_cast<double>(meta.sky[row][i]));
				info += "\n";
			}
			wchar_t info_name[48];
			swprintf_s(info_name, L"acvupscaler_rec_%03u.txt", index);
			if (std::FILE *const file = _wfsopen((log::directory() + info_name).c_str(), L"w", _SH_DENYWR))
			{
				std::fwrite(info.data(), 1, info.size(), file);
				std::fclose(file);
			}

			if (failed != 0)
				log::warn("burst record #%u: %u of %u files could not be written", index, failed, kInputs);
			else
				log::write("burst record #%u: %u frames of %ux%u written (acvupscaler_rec_%03u_*), FSR output %s", index, rec.frames, rec.width, rec.height, index, output_written ? "included" : "not available");
		}
	}

	void record::toggle(DeviceData &data)
	{
		Recorder &rec = s_recorder;
		const std::lock_guard<std::mutex> lock(rec.mutex);
		if (rec.stage != Off)
		{
			rec.stop_requested = true;
			log::write("burst record: stop requested");
			return;
		}
		rec.stop_requested = false;
		rec.bursts_done = 0;
		rec.frames = 0;
		rec.next_burst_time = seconds_now();
		set_stage(rec, Waiting);
		log::write("burst record: started at frame %llu (%u frames every %.0f s, at most %u bursts)", data.frame_index.load(std::memory_order_relaxed), kBurstFrames, kBurstInterval, kMaxBursts);
	}

	void record::copy_inputs(api::command_list *cmd_list, api::device *device, DeviceData &data, const PassRecord &record)
	{
		const uint32_t stage = s_stage.load(std::memory_order_relaxed);
		if (stage != Burst && stage != Output)
			return;

		Recorder &rec = s_recorder;
		const std::lock_guard<std::mutex> lock(rec.mutex);
		const uint64_t frame = data.frame_index.load(std::memory_order_relaxed) + 1;
		if (frame == rec.last_frame)
			return; // The other TAA draws of this frame

		if (rec.stage == Output)
		{
			rec.last_frame = frame;
			rec.game_fsr1 = data.game_fsr1_seen.load(std::memory_order_relaxed);
			Fsr::DebugTexture debug[kFsrDebugTextureCount];
			if (data.replace_mode.load(std::memory_order_relaxed) == static_cast<uint32_t>(ReplaceMode::Fsr) &&
				data.replace_applied_frame.load(std::memory_order_relaxed) + 1 >= frame && data.fsr->debug_textures(debug) > kFsrOutput)
			{
				const api::resource source = { reinterpret_cast<uintptr_t>(debug[kFsrOutput].resource) };
				const api::resource_desc source_desc = device->get_resource_desc(source);
				const api::resource_desc readback_desc(source_desc.texture.width, source_desc.texture.height, 1, 1, source_desc.texture.format, 1, api::memory_heap::readback, api::resource_usage::copy_dest);
				if (device->create_resource(readback_desc, nullptr, api::resource_usage::copy_dest, &rec.output))
				{
					// The FSR output rests in the copy source state
					cmd_list->copy_texture_region(source, 0, nullptr, rec.output, 0, nullptr);
					rec.out_width = source_desc.texture.width;
					rec.out_height = source_desc.texture.height;
					rec.out_format = source_desc.texture.format;
				}
			}
			rec.flush_frame = frame + kDelayFrames;
			set_stage(rec, Flush);
			return;
		}
		if (rec.stage != Burst)
			return;
		rec.last_frame = frame;

		api::resource sources[kInputs];
		api::resource_desc descs[kInputs];
		if (!resolve_inputs(device, record, sources, descs))
			return drop_burst(rec, "t0..t3 could not be resolved");
		if (record.taa_constants_result != ReadResult::Ok || record.view_constants_result != ReadResult::Ok)
			return drop_burst(rec, "the TAA or view constants could not be read");

		if (rec.frames == 0)
		{
			// The buffers of the last burst are reused as long as the render size stays the same. A burst is only
			// started after the previous one was read back, so nothing on the GPU uses them any more.
			bool same = rec.readback[0][0] != 0 && rec.width == descs[0].texture.width && rec.height == descs[0].texture.height;
			for (uint32_t i = 0; i < kInputs && same; ++i)
				same = rec.formats[i] == descs[i].texture.format;
			if (!same)
			{
				free_buffers(device, rec);
				rec.width = descs[0].texture.width;
				rec.height = descs[0].texture.height;
				for (uint32_t f = 0; f < kBurstFrames; ++f)
					for (uint32_t i = 0; i < kInputs; ++i)
					{
						rec.formats[i] = descs[i].texture.format;
						const api::resource_desc readback_desc(rec.width, rec.height, 1, 1, rec.formats[i], 1, api::memory_heap::readback, api::resource_usage::copy_dest);
						if (!device->create_resource(readback_desc, nullptr, api::resource_usage::copy_dest, &rec.readback[f][i]))
						{
							free_buffers(device, rec);
							return drop_burst(rec, "the readback buffers could not be created");
						}
					}
			}

			rec.settings.clear();
			append_format(rec.settings, "replacement: %s; reactive gain %.0f; average frame time %.2f ms",
				replace::mode_name(static_cast<ReplaceMode>(data.replace_mode.load(std::memory_order_relaxed))),
				static_cast<double>(data.fsr_reactive_gain.load(std::memory_order_relaxed)), static_cast<double>(data.average_frame_ms.load(std::memory_order_relaxed)));
		}
		else
		{
			if (rec.width != descs[0].texture.width || rec.height != descs[0].texture.height)
				return drop_burst(rec, "the render size changed");
			for (uint32_t i = 0; i < kInputs; ++i)
				if (rec.formats[i] != descs[i].texture.format)
					return drop_burst(rec, "a texture format changed");
		}

		for (uint32_t i = 0; i < kInputs; ++i)
		{
			cmd_list->barrier(sources[i], kGameReadState, api::resource_usage::copy_source);
			cmd_list->copy_texture_region(sources[i], 0, nullptr, rec.readback[rec.frames][i], 0, nullptr);
			cmd_list->barrier(sources[i], api::resource_usage::copy_source, kGameReadState);
		}

		Meta &meta = rec.meta[rec.frames];
		meta.frame = frame;
		meta.time = seconds_now();
		meta.jitter_x = record.taa_constants[3][0];
		meta.jitter_y = record.taa_constants[3][1];
		meta.p00 = record.view_projection[0][0];
		meta.p11 = record.view_projection[1][1];
		meta.depth_a = record.view_projection[2][2];
		meta.depth_b = record.view_projection[2][3];
		std::memcpy(meta.sky, record.view_sky, sizeof(meta.sky));
		std::memcpy(meta.taa_row6, record.taa_constants[6], sizeof(meta.taa_row6));

		if (++rec.frames == kBurstFrames)
			set_stage(rec, Output);
	}

	void record::on_present(api::command_queue *queue, api::device *device, DeviceData &, uint64_t frame)
	{
		if (s_stage.load(std::memory_order_relaxed) == Off)
			return;

		Recorder &rec = s_recorder;
		const std::lock_guard<std::mutex> lock(rec.mutex);
		const double now = seconds_now();
		switch (rec.stage)
		{
		case Waiting:
			if (rec.stop_requested)
			{
				// Every copy was read back before this stage was entered, but be sure before freeing
				queue->wait_idle();
				free_buffers(device, rec);
				set_stage(rec, Off);
				log::write("burst record: stopped after %u bursts", rec.bursts_done);
			}
			else if (now >= rec.next_burst_time)
			{
				rec.frames = 0;
				rec.burst_started = now;
				rec.next_burst_time = now + kBurstInterval;
				set_stage(rec, Burst);
			}
			break;
		case Burst:
		case Output:
			if (now > rec.burst_started + kBurstTimeout)
			{
				// The copies recorded so far may still be on their way to the GPU
				queue->wait_idle();
				drop_burst(rec, "no TAA draws for too long");
			}
			break;
		case Flush:
			if (frame >= rec.flush_frame)
			{
				queue->wait_idle();
				write_burst(device, rec);
				rec.frames = 0;
				if (++rec.bursts_done >= kMaxBursts)
					rec.stop_requested = true;
				set_stage(rec, Waiting);
			}
			break;
		default:
			break;
		}
	}

	void record::release(api::device *device)
	{
		Recorder &rec = s_recorder;
		const std::lock_guard<std::mutex> lock(rec.mutex);
		free_buffers(device, rec);
		set_stage(rec, Off);
	}

	std::string record::state()
	{
		Recorder &rec = s_recorder;
		const std::lock_guard<std::mutex> lock(rec.mutex);
		std::string text;
		if (rec.stage == Off)
			text = "off";
		else
			append_format(text, "%u of at most %u bursts written%s", rec.bursts_done, kMaxBursts, rec.stop_requested ? ", stopping" : "");
		return text;
	}
}
