// Shared includes, the add-on log and the exception guard used by every ReShade callback.
#pragma once

#include <Windows.h>

// imgui.h has to come before reshade.hpp in every translation unit: reshade.hpp then pulls in
// reshade_overlay.hpp and 'reshade::register_addon' fetches the ImGui function table.
// 'ImTextureID=ImU64' is defined on the compiler command line (CMakeLists.txt).
#include <imgui.h>
#include <reshade.hpp>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <exception>
#include <string>

#define ACV_VERSION "1.0.0-beta.1"

namespace acv
{
	namespace api = reshade::api;

	// acvupscaler.log, next to the add-on DLL. All functions are thread-safe and never throw.
	namespace log
	{
		void open(HMODULE addon_module); // Resolves the log directory from the module and truncates the log
		void close();

		void write(_Printf_format_string_ const char *format, ...); // One line, buffered (flushed once per frame)
		void warn(_Printf_format_string_ const char *format, ...);  // One line with a WARNING prefix, flushed immediately
		void flush_if_dirty();

		const std::wstring &directory();   // Directory of the add-on DLL, with a trailing backslash
		const std::string &directory_utf8();
	}

	// ACVUpscaler.ini next to the add-on DLL, read once at start. The setup application writes the file; the
	// add-on itself only updates Mode and RenderScale (replace.cpp).
	namespace settings
	{
		struct Values
		{
			bool dev_mode = false;      // Keys F6..F10, snapshot / record / frame log, test switches, verbose log
			// Reactive mask = t3 * gain. t3 holds small values for rain (mostly below 30 of 255), 60 and more for
			// fire. Replays of the recorded benchmark with synthetic rain (docs/TECHNICAL.md): 4 shows more of the rain than
			// no mask without disturbing the picture around it; 16 and 64 roughen flames and the surroundings.
			float reactive_gain = 4.0f;
			bool lod_bias = true;       // Negative texture LOD bias from the render scale of the last session
			bool keep_contexts = false; // Never call ffxDestroyContext (OptiScaler without its swap chain hooks)
			// The game's RCAS sharpening on top of the upscaler. Off: the upscaler writes what RCAS would have
			// written and both FSR 1 passes are skipped (softer picture, one pass less).
			bool game_sharpening = true;
		};
		const Values &get();
		// False in the test harness (ACV_TEST_NO_SETTINGS): the defaults apply there, with the developer tools on
		bool file_enabled();
		std::wstring path();
	}

	void append_format(std::string &out, _Printf_format_string_ const char *format, ...);

	// Runs a callback body and keeps every C++ exception inside the add-on.
	template <typename F>
	inline void guarded(const char *where, F &&body) noexcept
	{
		static std::atomic<uint32_t> s_reported { 0 };
		try
		{
			body();
		}
		catch (const std::exception &e)
		{
			if (s_reported.fetch_add(1, std::memory_order_relaxed) < 16)
				log::warn("exception in %s: %s", where, e.what());
		}
		catch (...)
		{
			if (s_reported.fetch_add(1, std::memory_order_relaxed) < 16)
				log::warn("unknown exception in %s", where);
		}
	}
}
