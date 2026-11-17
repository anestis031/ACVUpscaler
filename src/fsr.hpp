// FSR 3.1 through AMD's FidelityFX API (amd_fidelityfx_loader_dx12.dll, next to the add-on), plus the
// compute pass that turns the game's velocity and linear depth textures into what FSR expects.
// With Upscaler=DLSS the upscale itself is done by DLSS instead (dlss.hpp), on the same inputs and with
// everything else unchanged; where DLSS does not start, FSR 3.1 does the work.
// Everything in here talks to the native Direct3D 12 objects, so ReShade raises no add-on events for it.
#pragma once

#include "common.hpp"

#include <d3d12.h>
#include <wrl/client.h>

#include <mutex>
#include <vector>

namespace acv::dlss
{
	struct Feature;
}

namespace acv
{
	class Fsr
	{
	public:
		struct Inputs
		{
			ID3D12Resource *colour = nullptr;       // t0: tonemapped colour, in the game read state
			ID3D12Resource *velocity = nullptr;     // t1: packed 12-bit motion, in the game read state
			ID3D12Resource *linear_depth = nullptr; // t2: view-space distance, in the game read state
			ID3D12Resource *mask = nullptr;         // t3: coverage of the effects drawn without motion vectors (rain, fire), in the game read state. May be null.
			float reactive_gain = 0.0f;             // Reactive mask = mask * gain. Zero hands FSR no reactive mask.
			uint32_t width = 0, height = 0;         // Render size: the size of all three
			uint32_t out_width = 0, out_height = 0; // Size of the result (equal to the render size for native anti-aliasing)
			// Where the result goes: an 8-bit RGBA texture of the output size in UNORDERED_ACCESS state.
			// Null: the add-on's own texture, 'output()'.
			ID3D12Resource *output = nullptr;
			float jitter_x = 0.0f, jitter_y = 0.0f; // Pixels, as FSR wants them
			float motion_sign = 1.0f;               // +1 or -1 (test switch)
			float depth_a = 0.0f, depth_b = 0.0f;   // Reversed-Z device depth = b / distance - a
			float fov_y = 0.0f;                     // Radians
			// Sky: pixels at least 'sky_distance' away get their motion from the camera matrices (the rows
			// kViewSkyRows of the view constants, in that order), as in the TAA of the game
			float sky_rows[11][4] = {};
			float sky_distance = 3.0e38f;
			// The add-on's own sharpening pass after the upscaler: 0 none, 1 the strongest. Only where the game's
			// sharpening pass does not follow.
			float sharpness = 0.0f;
			bool reset = false;                     // The game's TAA would not use its history in this frame (camera cut)
			uint64_t frame = 0;
		};

		Fsr() = default;
		Fsr(const Fsr &) = delete;
		Fsr &operator=(const Fsr &) = delete;
		~Fsr();

		struct Result
		{
			bool ok = false;
			ID3D12Resource *own_output = nullptr; // The add-on's own output texture of this dispatch, in COPY_SOURCE state
			uint32_t timer_slot = UINT32_MAX;     // For 'end_timer'
			bool any_state_changed = false;       // DLSS recorded into the list: it may have changed any state of it
		};

		// Recording thread. Records the decode pass and the FSR dispatch into 'list'. This replaces the descriptor
		// heaps, the compute root signature and the pipeline of the list: the caller restores them.
		// On success the result is in 'Inputs::output' or, without one, in 'Result::own_output'.
		Result dispatch(ID3D12Device *device, ID3D12GraphicsCommandList *list, const Inputs &inputs);
		// Developer tools: 'dispatch' starts measuring the GPU time, this ends it. Call it after every 'dispatch',
		// behind whatever the caller records for it (copies of the result).
		void end_timer(ID3D12GraphicsCommandList *list, uint32_t slot);
		// Makes the next dispatch start without history
		void reset_history();

		// Recording thread, before the game's TAA is skipped for a dispatch that only follows later in the list
		// (at an FSR 1 dispatch, where the output size is known). False: the upscaler could not be set up for this
		// render size and it is not time to try again, so the game's own passes have to run.
		bool ready(uint32_t width, uint32_t height, uint64_t frame);
		// Recording thread. Sets the upscaler up for these sizes ahead of the dispatch, so that a failure is known
		// before a pass of the game is skipped for it. False: it could not be set up.
		// 'list_changed': DLSS recorded into 'list', the caller has to restore the state of the list.
		bool prepare(ID3D12Device *device, ID3D12GraphicsCommandList *list, uint32_t width, uint32_t height, uint32_t out_width, uint32_t out_height, uint64_t frame, bool &list_changed);
		// Lets sizes that could not be set up be tried again (the user switched the replacement on again)
		void forget_failures();

		// The textures of the current size with the state they rest in between dispatches (for snapshots).
		// Order: motion vectors, depth, output, reactive mask. Returns how many there are.
		struct DebugTexture { ID3D12Resource *resource = nullptr; D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON; };
		uint32_t debug_textures(DebugTexture (&out)[4]);

		// Presenting thread: frees what a size change left behind, once the GPU is done with it
		void on_present(api::command_queue *queue, uint64_t frame);
		// Destroys the FSR context and the textures. Call it while the device and its command queue are still fully
		// alive and the GPU is idle ('destroy_effect_runtime'): ffxDestroyContext called from 'destroy_device'
		// never returned in the test harness.
		void shutdown();
		// Ends NGX (DLSS) for the device, after 'shutdown', when the swap chain goes for good and not for a resize
		void end_dlss(ID3D12Device *device);

		// One line for the overlay
		std::string status();

	private:
		// Everything that depends on the resolution
		struct Set
		{
			uint32_t width = 0, height = 0, out_width = 0, out_height = 0;
			void *context = nullptr; // ffxContext. Null while DLSS does the upscale.
			dlss::Feature *dlss = nullptr;
			std::string provider;    // "3.1.6" for FSR, "DLSS preset K"
			Microsoft::WRL::ComPtr<ID3D12Resource> motion, depth, reactive, output;
			Microsoft::WRL::ComPtr<ID3D12Resource> presharp; // What the upscaler writes when the sharpening pass follows. Created when first needed.
			bool sharpen_failed = false; // 'presharp' could not be created: not tried again for this set
			uint64_t retired_frame = 0;

			bool usable() const { return context != nullptr || dlss != nullptr; }
		};

		static constexpr uint32_t kRing = 8; // Frames before a descriptor slot is reused
		static constexpr uint32_t kDecodeDescriptors = 6; // Three inputs, three outputs: the table of the decode pass and of the sharpening pass
		static constexpr uint32_t kDecodeConstants = 52;  // Floats of the constant block of the decode shader
		// A size that could not be set up is tried again twice: once the textures of the size before it are freed
		// (they stay for 8 frames), and once much later. A failed try costs a frame without anti-aliasing.
		static constexpr uint64_t kRetryFrames[2] = { 16, 1800 };

		bool create_decode_pass(ID3D12Device *device);
		bool create_set(ID3D12Device *device, ID3D12GraphicsCommandList *list, uint32_t width, uint32_t height, uint32_t out_width, uint32_t out_height, Set &set);
		static void destroy_set(Set &set);
		// The decode pass and '_set' for these sizes, created or replaced as needed. 'replace' forces a new set.
		bool ensure_set(ID3D12Device *device, ID3D12GraphicsCommandList *list, uint32_t width, uint32_t height, uint32_t out_width, uint32_t out_height, uint64_t frame, bool replace);
		void retire_set(uint64_t frame);
		// These sizes could not be set up and it is not time to try them again
		bool blocked(uint32_t width, uint32_t height, uint32_t out_width, uint32_t out_height, uint64_t frame) const;
		void clear_failures();
		bool test_retire_due();
		uint32_t begin_timer(ID3D12Device *device, ID3D12GraphicsCommandList *list);

		std::mutex _mutex;
		bool _decode_failed = false;
		Microsoft::WRL::ComPtr<ID3D12RootSignature> _root_signature;
		Microsoft::WRL::ComPtr<ID3D12PipelineState> _pipeline;
		Microsoft::WRL::ComPtr<ID3D12PipelineState> _sharpen_pipeline; // Null: the sharpening pass could not be created
		float _sharpness = 0.0f; // Of the last dispatch, for the status line
		Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> _heap;
		uint32_t _heap_increment = 0;
		uint32_t _ring_next = 0;

		Set _set;
		std::vector<Set> _retired;
		// The sizes that could not be set up last (width zero: none), when, and how often they were tried again
		uint32_t _failed_width = 0, _failed_height = 0, _failed_out_width = 0, _failed_out_height = 0;
		uint64_t _failed_frame = 0;
		uint32_t _failed_retries = 0;

		bool _reset = true;
		uint64_t _last_frame = 0;
		int64_t _last_time = 0;
		uint64_t _dispatches = 0;
		uint64_t _game_resets = 0; // Frames with 'Inputs::reset'
		bool _last_game_reset = false;
		uint32_t _transitions_logged = 0;
		std::string _error;

		// Upscaler=DLSS
		std::string _dlss_reason;        // Why FSR 3.1 does the work instead, empty while DLSS runs or was not asked for
		uint32_t _dlss_failures = 0;     // Evaluations that failed in a row
		bool _dlss_given_up = false;     // Three did: FSR 3.1 until the next swap chain or mode switch
		bool _list_touched = false;      // DLSS recorded into the command list while a set was created

		// GPU time of the add-on's passes (developer tools): a pair of timestamps per dispatch, resolved into a
		// readback buffer and read when their slot comes up again, kRing dispatches later
		static constexpr uint32_t kTimerWindow = 300; // Dispatches per average
		Microsoft::WRL::ComPtr<ID3D12QueryHeap> _timer_heap;
		Microsoft::WRL::ComPtr<ID3D12Resource> _timer_readback;
		const uint64_t *_timer_data = nullptr; // The readback buffer, mapped for good
		uint64_t _timer_frequency = 0;         // Ticks per second of the presenting queue
		uint32_t _timer_next = 0, _timer_count = 0;
		bool _timer_failed = false;
		double _timer_sum_ms = 0.0, _timer_max_ms = 0.0;
		double _timer_ms = 0.0; // Average of the last full window, zero before the first
	};
}
