// Automatic FSR test: with the camera standing still, runs FSR 3.1 with four strengths of the reactive
// mask for about a second each and takes a snapshot of every result. The snapshots show how much rain or
// fire still smears at each strength, without the human having to judge it on screen.
#pragma once

#include "frame.hpp"

namespace acv::autotest
{
	// Presenting thread. Returns false when a test is already running.
	bool start(DeviceData &data);
	void on_present(DeviceData &data, uint64_t frame);
	// Text for the overlay, e.g. "idle" or "configuration 2 of 4"
	std::string state(const DeviceData &data);
}
