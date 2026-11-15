#include "common.hpp"

namespace acv
{
	bool settings::file_enabled()
	{
		return GetEnvironmentVariableW(L"ACV_TEST_NO_SETTINGS", nullptr, 0) == 0;
	}

	std::wstring settings::path()
	{
		return log::directory() + L"ACVUpscaler.ini";
	}

	const settings::Values &settings::get()
	{
		static const Values s_values = []() {
			Values values;
			// The test harness drives the developer tools with key presses, with or without the settings file
			const bool test_dev_mode = GetEnvironmentVariableW(L"ACV_TEST_DEV_MODE", nullptr, 0) != 0;
			if (file_enabled())
			{
				const std::wstring file = path();
				const auto flag = [&file](const wchar_t *key, bool fallback) {
					return GetPrivateProfileIntW(L"ACVUpscaler", key, fallback ? 1 : 0, file.c_str()) != 0;
				};
				values.dev_mode = flag(L"DevMode", values.dev_mode);
				values.lod_bias = flag(L"LodBias", values.lod_bias);
				values.keep_contexts = flag(L"KeepContexts", values.keep_contexts);
				values.game_sharpening = flag(L"GameSharpening", values.game_sharpening);

				wchar_t text[32] = {};
				if (GetPrivateProfileStringW(L"ACVUpscaler", L"ReactiveGain", L"", text, 32, file.c_str()) != 0)
				{
					const float gain = static_cast<float>(_wtof(text));
					if (gain >= 0.0f && gain <= 64.0f)
						values.reactive_gain = gain;
				}
			}
			values.dev_mode = values.dev_mode || test_dev_mode || !file_enabled();
			if (GetEnvironmentVariableW(L"ACV_TEST_NO_SHARPENING", nullptr, 0) != 0) // Test harness only
				values.game_sharpening = false;

			log::write("settings: developer tools %s, reactive gain %.1f, texture LOD bias %s, contexts %s, sharpening of the game %s", values.dev_mode ? "on" : "off",
				static_cast<double>(values.reactive_gain), values.lod_bias ? "on" : "off", values.keep_contexts ? "kept (KeepContexts=1)" : "destroyed when replaced",
				values.game_sharpening ? "on" : "off (GameSharpening=0)");
			return values;
		}();
		return s_values;
	}
}
