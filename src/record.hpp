// Burst recorder: while it is on, every few seconds the TAA inputs t0..t3 of a run of consecutive frames are
// copied and written to disk together with the jitter and projection of each frame, plus the FSR 3.1 result
// of the last frame. One run of the game's benchmark then gives real frame sequences of many scenes, which
// the test harness can feed to FSR again with other settings, without starting the game.
#pragma once

#include "frame.hpp"

namespace acv::record
{
	// Presenting thread (F10 or the overlay): starts recording, or stops it after the burst in progress
	void toggle(DeviceData &data);
	// Recording thread, at every TAA draw before the draw itself is recorded
	void copy_inputs(api::command_list *cmd_list, api::device *device, DeviceData &data, const PassRecord &record);
	// Presenting thread, once per frame
	void on_present(api::command_queue *queue, api::device *device, DeviceData &data, uint64_t frame);
	// Device destruction: frees the readback buffers
	void release(api::device *device);
	// Text for the overlay
	std::string state();
}
