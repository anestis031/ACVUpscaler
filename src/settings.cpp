#include "common.hpp"

namespace acv
{
	namespace
	{
		// Set when the file is read ('settings::get'), then by the panel
		std::atomic<bool> s_game_sharpening { true };
		std::atomic<uint32_t> s_sharpness { 0 };
		std::atomic<bool> s_reflex { false };
	}

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
			bool game_sharpening = true;
			uint32_t own_sharpness = 0;
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
				game_sharpening = flag(L"GameSharpening", game_sharpening);
				const UINT percent = GetPrivateProfileIntW(L"ACVUpscaler", L"Sharpness", 0, file.c_str());
				own_sharpness = percent < 100 ? percent : 100;

				wchar_t text[32] = {};
				if (GetPrivateProfileStringW(L"ACVUpscaler", L"ReactiveGain", L"", text, 32, file.c_str()) != 0)
				{
					const float gain = static_cast<float>(_wtof(text));
					if (gain >= 0.0f && gain <= 64.0f)
						values.reactive_gain = gain;
				}
			}
			if (file_enabled())
			{
				const std::wstring file = path();
				wchar_t text[32] = {};
				GetPrivateProfileStringW(L"ACVUpscaler", L"Upscaler", L"FSR31", text, 32, file.c_str());
				values.dlss = _wcsicmp(text, L"DLSS") == 0;
				GetPrivateProfileStringW(L"ACVUpscaler", L"DlssPreset", L"K", text, 32, file.c_str());
				values.dlss_preset = _wcsicmp(text, L"M") == 0 ? 13 : _wcsicmp(text, L"Default") == 0 ? 0 : 11;
				s_reflex.store(GetPrivateProfileIntW(L"ACVUpscaler", L"Reflex", 0, file.c_str()) != 0, std::memory_order_relaxed);
			}
			if (GetEnvironmentVariableW(L"ACV_TEST_REFLEX", nullptr, 0) != 0) // Test harness only
				s_reflex.store(true, std::memory_order_relaxed);
			wchar_t test_upscaler[8] = {};
			if (GetEnvironmentVariableW(L"ACV_TEST_UPSCALER", test_upscaler, 8) != 0) // Test harness only: DLSS or DLSS-M
			{
				values.dlss = _wcsnicmp(test_upscaler, L"DLSS", 4) == 0;
				values.dlss_preset = _wcsicmp(test_upscaler, L"DLSS-M") == 0 ? 13 : 11;
			}
			values.dev_mode = values.dev_mode || test_dev_mode || !file_enabled();
			if (GetEnvironmentVariableW(L"ACV_TEST_NO_SHARPENING", nullptr, 0) != 0) // Test harness only
				game_sharpening = false;
			wchar_t test_sharpness[8] = {};
			if (GetEnvironmentVariableW(L"ACV_TEST_SHARPNESS", test_sharpness, 8) != 0) // Test harness only
				own_sharpness = static_cast<uint32_t>(_wtoi(test_sharpness)) % 101;
			s_game_sharpening.store(game_sharpening, std::memory_order_relaxed);
			s_sharpness.store(own_sharpness, std::memory_order_relaxed);

			log::write("settings: developer tools %s, reactive gain %.1f, texture LOD bias %s, contexts %s, sharpening of the game %s, own sharpening %u %%, upscaler %s, Reflex %s", values.dev_mode ? "on" : "off",
				static_cast<double>(values.reactive_gain), values.lod_bias ? "on" : "off", values.keep_contexts ? "kept (KeepContexts=1)" : "destroyed when replaced",
				game_sharpening ? "on" : "off (GameSharpening=0)", own_sharpness,
				!values.dlss ? "FSR 3.1" : values.dlss_preset == 13 ? "DLSS, preset M" : values.dlss_preset == 11 ? "DLSS, preset K" : "DLSS, default preset",
				s_reflex.load(std::memory_order_relaxed) ? "on" : "off");
			return values;
		}();
		return s_values;
	}

	bool settings::game_sharpening()
	{
		get();
		return s_game_sharpening.load(std::memory_order_relaxed);
	}
	uint32_t settings::sharpness()
	{
		get();
		return s_sharpness.load(std::memory_order_relaxed);
	}

	bool settings::reflex()
	{
		get();
		return s_reflex.load(std::memory_order_relaxed);
	}
	void settings::set_reflex(bool on)
	{
		get();
		if (s_reflex.exchange(on, std::memory_order_relaxed) == on)
			return;
		log::write("panel: Reflex %s", on ? "on" : "off");
		if (file_enabled())
			WritePrivateProfileStringW(L"ACVUpscaler", L"Reflex", on ? L"1" : L"0", path().c_str());
	}

	void settings::set_game_sharpening(bool on)
	{
		get();
		if (s_game_sharpening.exchange(on, std::memory_order_relaxed) == on)
			return;
		log::write("panel: sharpening of the game %s", on ? "on" : "off (GameSharpening=0)");
		if (file_enabled())
			WritePrivateProfileStringW(L"ACVUpscaler", L"GameSharpening", on ? L"1" : L"0", path().c_str());
	}
	void settings::set_sharpness(uint32_t percent, bool save)
	{
		get();
		percent = percent < 100 ? percent : 100;
		s_sharpness.store(percent, std::memory_order_relaxed);
		if (!save)
			return;
		log::write("panel: own sharpening %u %%", percent);
		if (file_enabled())
			WritePrivateProfileStringW(L"ACVUpscaler", L"Sharpness", std::to_wstring(percent).c_str(), path().c_str());
	}
}
