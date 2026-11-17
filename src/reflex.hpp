// NVIDIA Reflex low latency mode, as far as an add-on can drive it: the driver is told to keep the render queue
// empty, and gets its once-per-frame sleep call right after the swap chain's Present returned. A game that
// supports Reflex itself calls that sleep before it samples the input; that point is inside the game and out of
// reach here, so this is the limited form (NvAPI_D3D_SetSleepMode in nvapi.h: "the sleep ... would happen at a
// less optimal point").
// With the developer tools on, the frames are also marked for the driver's latency report and its averages go
// to the log, with Reflex on or off, so that the two can be compared.
// Built without NVAPI (CMake option ACV_WITH_REFLEX off), or without an NVIDIA driver, nothing happens.
// All functions are for the presenting thread.
#pragma once

#include "common.hpp"

#include <d3d12.h>

namespace acv::reflex
{
	// Before the swap chain's Present
	void before_present(ID3D12Device *device);
	// Right after Present returned: the start of the next frame as far as the add-on sees it
	void after_present(ID3D12Device *device);

	// One line for the panel
	std::string status();
}
