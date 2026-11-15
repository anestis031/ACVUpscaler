#pragma once

#include "common.hpp"

namespace acv::overlay
{
	// 'reshade::register_overlay' callback
	void draw(api::effect_runtime *runtime);
	// 'reshade_present' callback: polls the capture hotkey (F9)
	void on_reshade_present(api::effect_runtime *runtime);
}
