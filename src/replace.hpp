// Replacement of the game's TAA resolve, and of its FSR 1 upscale when that is on.
//  - Bypass: before the first TAA draw the current colour (t0) is copied into the TAA output and the three
//    TAA draws are skipped (test of the hook, no anti-aliasing at all)
//  - Fsr, game FSR 1 off: FSR 3.1 in native anti-aliasing mode writes the TAA output instead
//  - Fsr, game FSR 1 on: the TAA output gets a copy of t0, and at the EASU dispatch FSR 3.1 upscales to the
//    display resolution into EASU's output. EASU is skipped; the game's RCAS sharpening still runs on the result.
#pragma once

#include "frame.hpp"

namespace acv::replace
{
	// What the command list had bound when the call was recorded; needed to put it back after FSR
	struct ListState
	{
		const api::resource_view *rtvs = nullptr;
		uint32_t rtv_count = 0;
		uint64_t pipeline = 0;
		const RootTables *graphics = nullptr, *compute = nullptr;
	};

	enum class Result
	{
		NotApplied, // The game's TAA draws run as usual
		Applied,    // The TAA output was written: skip the TAA draws of this list
		Deferred    // As 'Applied', and FSR runs at an FSR 1 dispatch of this list: keep the TAA record for 'at_fsr1'
	};

	// Recording thread, before the first TAA draw of a command list is recorded
	Result at_taa(ReplaceMode mode, api::command_list *cmd_list, api::device *device, DeviceData &data, const PassRecord &taa, const ListState &list);
	// Recording thread, before the EASU dispatch 'pass' of such a list is skipped for a run at the RCAS dispatch:
	// sets the upscaler up for the output size. False: it could not be set up, EASU must not be skipped.
	bool prepare_fsr1(api::command_list *cmd_list, api::device *device, DeviceData &data, const PassRecord &taa, const PassRecord &pass, const ListState &list);
	// Recording thread, before the EASU or the RCAS dispatch 'pass' of a list whose TAA returned 'Deferred'.
	// Returns true when the output of that dispatch was written, so the dispatch has to be skipped.
	bool at_fsr1(api::command_list *cmd_list, api::device *device, DeviceData &data, const PassRecord &taa, const PassRecord &pass, const ListState &list);

	// The mode is kept in ACVUpscaler.ini next to the add-on, so that it survives a restart of the game.
	// Nothing is read or written while the environment variable ACV_TEST_NO_SETTINGS is set (test harness).
	ReplaceMode load_mode();
	void set_mode(DeviceData &data, ReplaceMode mode, const char *source);

	// Texture LOD bias for this session: log2(render width / output width) of the last session that ran FSR 3.1,
	// or zero when the saved mode is not FSR 3.1. Samplers are created once at start, so the bias cannot follow
	// a change of the game's FSR preset before the next start.
	float session_mip_bias();
	// Remembers the render scale for the next start (written only when it changes)
	void save_render_scale(DeviceData &data, uint32_t render_width, uint32_t output_width);

	// "off", "active" or "requested, but not applied (see the log)"
	const char *state(const DeviceData &data);
	const char *mode_name(ReplaceMode mode);
}
