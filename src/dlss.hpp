// DLSS through NVIDIA's NGX SDK, as a second upscaler behind the inputs FSR 3.1 gets (fsr.cpp). NGX loads
// nvngx_dlss.dll from the add-on directory; the file is NVIDIA's and is not part of this project.
// Built without the SDK (CMake option ACV_WITH_DLSS off), every function reports DLSS as unavailable.
// None of this is thread-safe: 'Fsr' calls it with its mutex held.
#pragma once

#include "common.hpp"

#include <d3d12.h>

namespace acv::dlss
{
	struct Feature; // One DLSS instance for a render and an output size

	// Starts NGX for the device at the first call and asks whether DLSS can run on this system. The answer is
	// kept until 'shutdown'. False: 'reason' says why (no NVIDIA card, driver too old, nvngx_dlss.dll missing).
	bool available(ID3D12Device *device, std::string &reason);

	// 'list' is an open command list of a direct queue: NGX records into it and leaves its state changed.
	// 'preset' is the model as NGX numbers it (settings::Values::dlss_preset). Null: 'error' says why.
	Feature *create(ID3D12GraphicsCommandList *list, uint32_t width, uint32_t height, uint32_t out_width, uint32_t out_height, uint32_t preset, std::string &error);

	struct Eval
	{
		ID3D12Resource *colour = nullptr; // Render size, in a read state that includes NON_PIXEL_SHADER_RESOURCE
		ID3D12Resource *depth = nullptr;  // Reversed Z, render size, NON_PIXEL_SHADER_RESOURCE
		ID3D12Resource *motion = nullptr; // UV units, from the current to the previous position, render size, NON_PIXEL_SHADER_RESOURCE
		ID3D12Resource *output = nullptr; // Output size, UNORDERED_ACCESS
		uint32_t width = 0, height = 0;   // Render size
		float jitter_x = 0.0f, jitter_y = 0.0f; // Pixels
		float frame_ms = 0.0f;
		bool reset = false;
	};
	// Records the upscale into 'list' and leaves the state of the list changed
	bool evaluate(ID3D12GraphicsCommandList *list, Feature *feature, const Eval &eval, std::string &error);

	// Only once the GPU has run every list the feature was evaluated on
	void release(Feature *&feature);
	// Ends NGX for the device. Every feature has to be released before.
	void shutdown(ID3D12Device *device);
}
