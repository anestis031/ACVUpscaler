// One-shot snapshot of the TAA inputs: the textures bound to the first TAA draw after the request are
// copied into readback memory, written as raw files next to the log (acvupscaler_snap_NNN_*.bin plus
// acvupscaler_snap_NNN.txt), and motion vectors and depth are turned into preview images for the overlay.
#pragma once

#include "frame.hpp"

namespace acv::snapshot
{
	// Arms a snapshot of the next TAA draw. Returns false when one is already in progress.
	bool request(DeviceData &data);
	const char *state(const DeviceData &data);

	// Recording thread, at the first TAA draw after the request, before the draw itself is recorded
	void copy_inputs(api::command_list *cmd_list, api::device *device, DeviceData &data, const PassRecord &record);
	// Presenting thread, once per frame
	void on_present(api::command_queue *queue, api::device *device, DeviceData &data, uint64_t frame);
	// Device destruction: frees the readback and preview textures
	void release(api::device *device, DeviceData &data);
}
