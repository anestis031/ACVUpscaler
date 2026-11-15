// Per-device and per-command-list state, known-pass detection with per-frame statistics, and the
// one-frame capture written to acvupscaler_frame_NNN.log.
#pragma once

#include "common.hpp"
#include "descriptors.hpp"
#include "pipelines.hpp"

#include <cstdio>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace acv
{
	// Layout of the TAA draws (docs/TECHNICAL.md, sections 2 and 8)
	inline constexpr uint32_t kTaaViewParam = 1;   // Root parameter with the view constants at descriptor +0
	inline constexpr uint32_t kTaaPassParam = 2;   // Root parameter with: +0 TAA constants, +3.. t0..t6
	inline constexpr uint32_t kTaaConstantRows = 7;
	inline constexpr uint32_t kTaaFirstInput = 3;  // Descriptor index of t0 in the pass table
	inline constexpr uint32_t kTaaInputCount = 7;  // t0..t6
	inline constexpr uint32_t kFsrDebugTextureCount = 4; // The add-on textures a snapshot also dumps while FSR is active

	inline constexpr uint32_t kViewProjectionRow = 26; // Rows 26..29 of the view constants
	inline constexpr uint32_t kViewRenderSizeRow = 52;
	// Rows of the view constants with which the TAA of the game reprojects the sky by the camera alone
	// (disassembly of its pixel shader): inverse view 6..8, previous view-projection 38, 39 and 41, the depth of
	// the far plane in 54.y, inverse projection 69..72. The velocity texture holds nothing usable in the sky.
	inline constexpr uint32_t kViewSkyRowCount = 11;
	inline constexpr uint32_t kViewSkyRows[kViewSkyRowCount] = { 6, 7, 8, 38, 39, 41, 54, 69, 70, 71, 72 };
	inline constexpr uint32_t kViewRowsRead = 73;

	// State of every texture the game samples: NON_PIXEL_SHADER_RESOURCE | PIXEL_SHADER_RESOURCE. Verified for
	// all seven TAA inputs of capture B, it is the only whole-texture read state in both captures
	// (captures/{A,B}/trace/barriers.txt), and the snapshot copies ran with it in the real game. A wrong
	// "before" state in a barrier would be undefined behaviour.
	inline constexpr api::resource_usage kGameReadState = api::resource_usage::shader_resource;

	// Descriptor tables bound to one bind point (graphics or compute) of a command list
	inline constexpr uint32_t kMaxRootParams = 32;
	struct RootTables
	{
		uint64_t layout = 0; // Root signature
		uint32_t used = 0;   // Highest bound root parameter index + 1
		uint64_t table[kMaxRootParams] = {};

		void clear() { layout = 0; used = 0; }
		void set(uint32_t index, uint64_t handle)
		{
			if (index >= kMaxRootParams)
				return;
			for (; used <= index; ++used)
				table[used] = 0;
			table[index] = handle;
		}
	};

	struct TargetInfo
	{
		uint64_t view = 0;
		uint64_t resource = 0;
		bool known = false; // 'info' is valid
		ResourceInfo info;
	};

	// Everything resolved for one draw or dispatch of a known pass, at the time it was recorded.
	struct PassRecord
	{
		bool valid = false;
		Kind kind = Kind::None;
		uint64_t pipeline = 0;
		TargetInfo rt0;                 // Graphics passes only
		std::vector<BoundTable> tables; // Every bound descriptor table of the bind point

		// TAA passes only
		ReadResult taa_constants_result = ReadResult::NotRead;
		float taa_constants[kTaaConstantRows][4] = {};
		ReadResult view_constants_result = ReadResult::NotRead;
		float view_projection[4][4] = {};
		float view_render_size[4] = {};
		float view_sky[kViewSkyRowCount][4] = {};

		const BoundTable *find_table(uint32_t param) const
		{
			for (const BoundTable &table : tables)
				if (table.param == param)
					return &table;
			return nullptr;
		}
	};

	struct FrameStats
	{
		uint64_t frame = 0;
		uint32_t counts[kKindCount] = {};
		uint64_t descriptors_written = 0, descriptors_copied = 0; // CBV/SRV/UAV descriptors, all threads, approximate
		PassRecord taa;  // The TaaFull draw if there was one, otherwise another TAA variant
		PassRecord easu;
		PassRecord rcas;
	};

	class Fsr;
	enum class ReplaceMode : uint32_t { GameTaa, Bypass, Fsr };

	struct JitterSample
	{
		uint64_t frame = 0;
		bool valid = false;                // A TAA draw with readable constants was recorded in that frame
		float x = 0.0f, y = 0.0f;          // Pixels (TAA constants, row 3)
		float width = 0.0f, height = 0.0f; // TAA input size (row 2)
	};
	inline constexpr uint32_t kJitterHistory = 128;
	inline constexpr uint32_t kFrameTimeRing = 120;

	// One-shot snapshot of the TAA inputs (snapshot.cpp): request -> the textures are copied into readback
	// memory right before the next TAA draw -> a few presents later they are written to files.
	struct Snapshot
	{
		enum Stage : uint32_t { Idle, Armed, Copying, Copied };
		std::atomic<uint32_t> stage { Idle };
		uint64_t armed_frame = 0; // Presenting thread only

		// Owned by the recording thread in stage 'Copying' and by the presenting thread in stage 'Copied'
		struct Texture
		{
			api::resource readback = { 0 };
			uint32_t width = 0, height = 0;
			api::format format = api::format::unknown;
		};
		Texture textures[kTaaInputCount + kFsrDebugTextureCount];
		PassRecord record;
		uint64_t record_frame = 0;
		std::string settings; // Replacement mode and FSR switches when the copy was recorded

		// Preview images for the overlay: [0] motion vectors, [1] depth
		std::mutex preview_mutex;
		api::resource preview_resource[2] = {};
		api::resource_view preview_view[2] = {};
		uint32_t preview_width = 0, preview_height = 0;
		uint32_t done = 0; // Snapshots written so far
	};

	// Automatic FSR test (autotest.cpp). Presenting thread only, apart from 'config'.
	struct AutoTest
	{
		std::atomic<uint32_t> config { 0 }; // 0 = idle, otherwise the running configuration (1-based)
		bool snapping = false;
		uint64_t wait_until = 0;
		uint32_t snapshots_before = 0;
		uint32_t saved_mode = 0;
		float saved_reactive_gain = 0.0f;
	};

	struct DeviceData
	{
		DeviceData();
		DeviceData(const DeviceData &) = delete;
		DeviceData &operator=(const DeviceData &) = delete;
		~DeviceData();

		PipelineRegistry pipelines;
		ResourceTable resources;
		DescriptorShadow descriptors;

		// Swap chain (set in 'init_swapchain'; back buffers are not announced through 'init_resource')
		std::mutex swapchain_mutex;
		std::vector<std::pair<api::swapchain *, uint64_t>> back_buffers;
		std::atomic<uint32_t> swap_width { 0 }, swap_height { 0 }, swap_format { 0 };

		// Statistics of the frame being recorded, and of the last finished frame
		std::atomic<uint32_t> counts[kKindCount] = {};
		std::mutex record_mutex;
		PassRecord current_taa, current_easu, current_rcas;
		std::atomic<uint64_t> frame_index { 0 };
		uint64_t written_total_at_present = 0, copied_total_at_present = 0; // Presenting thread only
		std::mutex published_mutex;
		FrameStats published;

		// Jitter of the last frames, slot = frame % kJitterHistory (presenting thread only)
		JitterSample jitter_history[kJitterHistory];

		Snapshot snapshot;

		// Replacement of the TAA resolve (replace.cpp). Set from the overlay, read by the recording threads.
		std::atomic<uint32_t> replace_mode { 0 }; // ReplaceMode
		std::atomic<uint64_t> replace_applied_frame { 0 };
		std::atomic<bool> fsr_flip_jitter_x { false }, fsr_flip_jitter_y { false }, fsr_flip_motion { false };
		std::atomic<float> fsr_reactive_gain { 0.0f }; // Reactive mask = t3 * gain, zero = none. From the settings.
		std::atomic<bool> game_fsr1_seen { false }; // EASU and RCAS were recorded in the last finished frame
		std::atomic<bool> rcas_replace_failed { false }; // GameSharpening=0 did not work once: FSR runs at EASU again
		std::atomic<float> saved_render_scale { 0.0f }; // Last render scale written to the settings file

		// Frame time, averaged over the last frames (written by the presenting thread)
		int64_t last_present_time = 0;
		float frame_ms_ring[kFrameTimeRing] = {};
		std::atomic<float> average_frame_ms { 0.0f };
		std::unique_ptr<Fsr> fsr; // Created with the device data, never replaced
		AutoTest autotest;

		// One-frame capture. 'capture_gen' is non-zero from the request until the file is closed:
		// request (armed, command lists start buffering) -> next present (file opened, executed lists
		// are written) -> following present (footer, closed).
		std::atomic<uint32_t> capture_gen { 0 };
		std::atomic<bool> capture_writing { false };
		std::mutex capture_mutex; // Guards everything below
		std::FILE *capture_file = nullptr;
		uint32_t capture_next_gen = 0;
		uint32_t capture_lists = 0, capture_partial_lists = 0, capture_events = 0;
		uint32_t capture_kind_counts[kKindCount] = {};
		uint32_t captures_done = 0;

		FrameStats published_copy();
	};

	// Private data attached to the ReShade device object.
	struct __declspec(uuid("5d0b7a52-93e4-4c0a-8f1e-6a2f0c3b9d11")) DeviceHolder
	{
		std::shared_ptr<DeviceData> data;
	};

	inline DeviceData *device_data(api::device *device)
	{
		const DeviceHolder *const holder = device != nullptr ? device->get_private_data<DeviceHolder>() : nullptr;
		return holder != nullptr ? holder->data.get() : nullptr;
	}

	namespace frame
	{
		// Registers the command list, queue and present events.
		void register_events();
		void unregister_events();

		// Arms a capture of the next full frame. Returns false when one is already in progress.
		bool request_capture(DeviceData &data);
		// "idle", "armed" or "writing"
		const char *capture_state(const DeviceData &data);

		// Text of a pass record as it appears in the frame log (bound tables, TAA and view constants)
		void append_pass_text(std::string &out, const PassRecord &record);
		// Jitter of the frames up to 'last_frame', oldest first. Presenting thread only.
		void append_jitter_history(std::string &out, const DeviceData &data, uint64_t last_frame);
	}

	// "game FSR 1 on" (TAA, EASU and RCAS seen), "native" (TAA only) or "TAA not found"
	const char *detect_mode(const FrameStats &stats);

	const char *format_name(api::format format, char (&buffer)[16]);
}
