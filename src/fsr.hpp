// FSR 3.1 through AMD's FidelityFX API (amd_fidelityfx_loader_dx12.dll, next to the add-on), plus the
// compute pass that turns the game's velocity and linear depth textures into what FSR expects.
// Everything in here talks to the native Direct3D 12 objects, so ReShade raises no add-on events for it.
#pragma once

#include "common.hpp"

#include <d3d12.h>
#include <wrl/client.h>

#include <mutex>
#include <vector>

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
			uint64_t frame = 0;
		};

		Fsr() = default;
		Fsr(const Fsr &) = delete;
		Fsr &operator=(const Fsr &) = delete;
		~Fsr();

		// Recording thread. Records the decode pass and the FSR dispatch into 'list'. This replaces the descriptor
		// heaps, the compute root signature and the pipeline of the list: the caller restores them.
		// On success the result is in 'Inputs::output' or, without one, in 'output()', which is in COPY_SOURCE state.
		bool dispatch(ID3D12Device *device, ID3D12GraphicsCommandList *list, const Inputs &inputs);
		ID3D12Resource *output();
		// Developer tools: 'dispatch' starts measuring the GPU time, this ends it. Call it after every 'dispatch',
		// behind whatever the caller records for it (copies of the result).
		void end_timer(ID3D12GraphicsCommandList *list);
		// Makes the next dispatch start without history
		void reset_history();

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

		// One line for the overlay
		std::string status();

	private:
		// Everything that depends on the resolution
		struct Set
		{
			uint32_t width = 0, height = 0, out_width = 0, out_height = 0;
			void *context = nullptr; // ffxContext
			Microsoft::WRL::ComPtr<ID3D12Resource> motion, depth, reactive, output;
			uint64_t retired_frame = 0;
		};

		static constexpr uint32_t kRing = 8; // Frames before a descriptor slot is reused
		static constexpr uint32_t kDecodeDescriptors = 6; // Three inputs, three outputs
		static constexpr uint32_t kDecodeConstants = 52;  // Floats of the constant block of the decode shader

		bool create_decode_pass(ID3D12Device *device);
		bool create_set(ID3D12Device *device, const Inputs &inputs, Set &set);
		static void destroy_set(Set &set);
		void begin_timer(ID3D12Device *device, ID3D12GraphicsCommandList *list);

		std::mutex _mutex;
		bool _decode_failed = false;
		Microsoft::WRL::ComPtr<ID3D12RootSignature> _root_signature;
		Microsoft::WRL::ComPtr<ID3D12PipelineState> _pipeline;
		Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> _heap;
		uint32_t _heap_increment = 0;
		uint32_t _ring_next = 0;

		Set _set;
		std::vector<Set> _retired;
		uint32_t _failed_width = 0, _failed_height = 0, _failed_out_width = 0, _failed_out_height = 0; // Do not retry sizes that could not be created

		bool _reset = true;
		uint64_t _last_frame = 0;
		int64_t _last_time = 0;
		uint64_t _dispatches = 0;
		std::string _provider;
		std::string _error;

		// GPU time of the add-on's passes (developer tools): a pair of timestamps per dispatch, resolved into a
		// readback buffer and read when their slot comes up again, kRing dispatches later
		static constexpr uint32_t kTimerWindow = 300; // Dispatches per average
		Microsoft::WRL::ComPtr<ID3D12QueryHeap> _timer_heap;
		Microsoft::WRL::ComPtr<ID3D12Resource> _timer_readback;
		const uint64_t *_timer_data = nullptr; // The readback buffer, mapped for good
		uint64_t _timer_frequency = 0;         // Ticks per second of the presenting queue
		uint32_t _timer_next = 0, _timer_slot = 0, _timer_count = 0;
		bool _timer_failed = false, _timer_open = false;
		double _timer_sum_ms = 0.0, _timer_max_ms = 0.0;
		double _timer_ms = 0.0; // Average of the last full window, zero before the first
	};
}
