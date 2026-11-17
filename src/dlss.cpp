#include "dlss.hpp"

#ifdef ACV_WITH_DLSS

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>

#include <cstring>

namespace acv::dlss
{
	struct Feature
	{
		NVSDK_NGX_Handle *handle = nullptr;
		NVSDK_NGX_Parameter *params = nullptr; // The parameter block of this feature, filled again for every frame
	};

	namespace
	{
		// NGX asks engines it does not know for a project id in GUID form. This one is the add-on's own, the same
		// in every version.
		const char kProjectId[] = "7d3f5c1e-0b9a-4f62-8e47-a1c9d25b6f30";

		struct State
		{
			bool tried = false, ok = false;
			std::string reason;
			ID3D12Device *device = nullptr; // Non-null while NGX is started for it
		};
		State s_state;

		std::string result_text(NVSDK_NGX_Result result)
		{
			const char *name = "";
			switch (result)
			{
			case NVSDK_NGX_Result_FAIL_FeatureNotSupported: name = ", feature not supported"; break;
			case NVSDK_NGX_Result_FAIL_PlatformError: name = ", platform error"; break;
			case NVSDK_NGX_Result_FAIL_InvalidParameter: name = ", invalid parameter"; break;
			case NVSDK_NGX_Result_FAIL_NotInitialized: name = ", not initialized"; break;
			case NVSDK_NGX_Result_FAIL_UnsupportedInputFormat: name = ", unsupported input format"; break;
			case NVSDK_NGX_Result_FAIL_RWFlagMissing: name = ", output without unordered access"; break;
			case NVSDK_NGX_Result_FAIL_MissingInput: name = ", missing input"; break;
			case NVSDK_NGX_Result_FAIL_UnableToInitializeFeature: name = ", feature could not be initialized"; break;
			case NVSDK_NGX_Result_FAIL_OutOfDate: name = ", driver out of date"; break;
			case NVSDK_NGX_Result_FAIL_OutOfGPUMemory: name = ", out of GPU memory"; break;
			case NVSDK_NGX_Result_FAIL_UnableToWriteToAppDataPath: name = ", data folder not writable"; break;
			case NVSDK_NGX_Result_FAIL_Denied: name = ", denied for this application"; break;
			default: break;
			}
			std::string text;
			append_format(text, "NGX code 0x%08X%s", static_cast<unsigned int>(result), name);
			return text;
		}

		// Test harness only: ACV_TEST_DLSS_FAIL=init, create or evaluate makes that step fail
		bool test_fail(const char *step)
		{
			static const std::string s_step = []() {
				char text[16] = {};
				const DWORD length = GetEnvironmentVariableA("ACV_TEST_DLSS_FAIL", text, sizeof(text));
				return std::string(length != 0 && length < sizeof(text) ? text : "");
			}();
			return s_step == step;
		}

		void stop()
		{
			if (s_state.device != nullptr)
				NVSDK_NGX_D3D12_Shutdown1(s_state.device);
			s_state.device = nullptr;
		}

		// The quality level DLSS names for a scale; with the sizes given it only selects defaults inside DLSS.
		// The game's four FSR 1 levels are 1.30, 1.50, 1.69 and 2.00.
		NVSDK_NGX_PerfQuality_Value quality_for(uint32_t width, uint32_t out_width)
		{
			const float scale = static_cast<float>(out_width) / static_cast<float>(width);
			return scale < 1.1f ? NVSDK_NGX_PerfQuality_Value_DLAA : scale < 1.6f ? NVSDK_NGX_PerfQuality_Value_MaxQuality :
				scale < 1.85f ? NVSDK_NGX_PerfQuality_Value_Balanced : scale < 2.5f ? NVSDK_NGX_PerfQuality_Value_MaxPerf : NVSDK_NGX_PerfQuality_Value_UltraPerformance;
		}
	}

	bool available(ID3D12Device *device, std::string &reason)
	{
		if (s_state.tried)
		{
			reason = s_state.reason;
			return s_state.ok;
		}
		s_state.tried = true;
		const auto unavailable = [&reason](std::string text) {
			log::warn("DLSS: not available: %s", text.c_str());
			reason = s_state.reason = std::move(text);
			return false;
		};
		if (test_fail("init"))
			return unavailable("switched off for the test (ACV_TEST_DLSS_FAIL)");

		// nvngx_dlss.dll is looked for in the add-on directory only.
		// Test harness only: ACV_TEST_DLSS_DIR names another directory.
		std::wstring directory = log::directory();
		wchar_t test_directory[MAX_PATH] = {};
		const DWORD test_length = GetEnvironmentVariableW(L"ACV_TEST_DLSS_DIR", test_directory, MAX_PATH);
		if (test_length != 0 && test_length < MAX_PATH)
			directory = test_directory;
		while (!directory.empty() && directory.back() == L'\\')
			directory.pop_back();
		if (GetFileAttributesW((directory + L"\\nvngx_dlss.dll").c_str()) == INVALID_FILE_ATTRIBUTES)
			return unavailable("nvngx_dlss.dll is not in the add-on folder");

		// NGX writes its own log files into the data folder; only with the developer tools on
		std::wstring data_directory = log::directory();
		while (!data_directory.empty() && data_directory.back() == L'\\')
			data_directory.pop_back();
		const wchar_t *const paths[1] = { directory.c_str() };
		NVSDK_NGX_FeatureCommonInfo info = {};
		info.PathListInfo.Path = paths;
		info.PathListInfo.Length = 1;
		info.LoggingInfo.MinimumLoggingLevel = settings::get().dev_mode ? NVSDK_NGX_LOGGING_LEVEL_ON : NVSDK_NGX_LOGGING_LEVEL_OFF;

		NVSDK_NGX_Result result = NVSDK_NGX_D3D12_Init_with_ProjectID(kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, ACV_VERSION, data_directory.c_str(), device, &info);
		if (NVSDK_NGX_FAILED(result))
			return unavailable("NGX did not start, which needs an NVIDIA RTX card and a current driver (" + result_text(result) + ")");
		s_state.device = device;

		NVSDK_NGX_Parameter *capabilities = nullptr;
		result = NVSDK_NGX_D3D12_GetCapabilityParameters(&capabilities);
		if (NVSDK_NGX_FAILED(result) || capabilities == nullptr)
		{
			stop();
			return unavailable("NGX does not report what it can do (" + result_text(result) + ")");
		}
		int supported = 0, needs_driver = 0, init_result = 0;
		unsigned int driver_major = 0, driver_minor = 0;
		capabilities->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &supported);
		capabilities->Get(NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needs_driver);
		capabilities->Get(NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &driver_major);
		capabilities->Get(NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &driver_minor);
		capabilities->Get(NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &init_result);
		NVSDK_NGX_D3D12_DestroyParameters(capabilities);
		if (supported == 0)
		{
			stop();
			std::string text;
			if (needs_driver != 0)
				append_format(text, "the NVIDIA driver is too old, DLSS needs version %u.%u or newer", driver_major, driver_minor);
			else
				text = "DLSS cannot run on this system (" + result_text(static_cast<NVSDK_NGX_Result>(init_result)) + ")";
			return unavailable(std::move(text));
		}

		log::write("DLSS: NGX started, nvngx_dlss.dll from %ls", directory.c_str());
		s_state.ok = true;
		return true;
	}

	Feature *create(ID3D12GraphicsCommandList *list, uint32_t width, uint32_t height, uint32_t out_width, uint32_t out_height, uint32_t preset, std::string &error)
	{
		if (!s_state.ok || test_fail("create"))
		{
			error = s_state.ok ? "creation failed for the test (ACV_TEST_DLSS_FAIL)" : "NGX is not started";
			return nullptr;
		}

		NVSDK_NGX_Parameter *params = nullptr;
		NVSDK_NGX_Result result = NVSDK_NGX_D3D12_AllocateParameters(&params);
		if (NVSDK_NGX_FAILED(result) || params == nullptr)
		{
			error = "no parameter block (" + result_text(result) + ")";
			return nullptr;
		}

		// The same model for every quality level; zero leaves the choice to DLSS
		if (preset != 0)
			for (const char *const name : { NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
					NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance })
				params->Set(name, preset);

		// Colour is the game's tonemapped 8-bit picture (no HDR flag), motion vectors have the render size and
		// are not jittered, depth is reversed. No exposure texture and no automatic exposure: nothing to expose.
		NVSDK_NGX_DLSS_Create_Params desc = {};
		desc.Feature.InWidth = width;
		desc.Feature.InHeight = height;
		desc.Feature.InTargetWidth = out_width;
		desc.Feature.InTargetHeight = out_height;
		desc.Feature.InPerfQualityValue = quality_for(width, out_width);
		desc.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;

		NVSDK_NGX_Handle *handle = nullptr;
		result = NGX_D3D12_CREATE_DLSS_EXT(list, 1, 1, &handle, params, &desc);
		if (NVSDK_NGX_FAILED(result) || handle == nullptr)
		{
			NVSDK_NGX_D3D12_DestroyParameters(params);
			error = "the DLSS feature could not be created (" + result_text(result) + ")";
			return nullptr;
		}
		return new Feature { handle, params };
	}

	bool evaluate(ID3D12GraphicsCommandList *list, Feature *feature, const Eval &eval, std::string &error)
	{
		if (test_fail("evaluate"))
		{
			error = "evaluation failed for the test (ACV_TEST_DLSS_FAIL)";
			return false;
		}

		NVSDK_NGX_D3D12_DLSS_Eval_Params desc = {};
		desc.Feature.pInColor = eval.colour;
		desc.Feature.pInOutput = eval.output;
		desc.pInDepth = eval.depth;
		desc.pInMotionVectors = eval.motion;
		desc.InJitterOffsetX = eval.jitter_x;
		desc.InJitterOffsetY = eval.jitter_y;
		desc.InRenderSubrectDimensions = { eval.width, eval.height };
		desc.InReset = eval.reset ? 1 : 0;
		// The vectors are in UV units: times the render size gives the pixels DLSS wants
		desc.InMVScaleX = static_cast<float>(eval.width);
		desc.InMVScaleY = static_cast<float>(eval.height);
		desc.InPreExposure = 1.0f;
		desc.InExposureScale = 1.0f;
		desc.InFrameTimeDeltaInMsec = eval.frame_ms;

		const NVSDK_NGX_Result result = NGX_D3D12_EVALUATE_DLSS_EXT(list, feature->handle, feature->params, &desc);
		if (NVSDK_NGX_FAILED(result))
		{
			error = "DLSS evaluation failed (" + result_text(result) + ")";
			return false;
		}
		return true;
	}

	void release(Feature *&feature)
	{
		if (feature == nullptr)
			return;
		NVSDK_NGX_D3D12_ReleaseFeature(feature->handle);
		NVSDK_NGX_D3D12_DestroyParameters(feature->params);
		delete feature;
		feature = nullptr;
	}

	void shutdown(ID3D12Device *device)
	{
		if (s_state.device != nullptr && s_state.device == device)
		{
			stop();
			log::write("DLSS: NGX shut down");
		}
		// A new swap chain is a new try
		if (s_state.device == nullptr)
			s_state = State();
	}
}

#else

namespace acv::dlss
{
	struct Feature
	{
	};

	bool available(ID3D12Device *, std::string &reason)
	{
		reason = "this build of the add-on has no DLSS (built without the NVIDIA DLSS SDK)";
		return false;
	}
	Feature *create(ID3D12GraphicsCommandList *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, std::string &error)
	{
		error = "no DLSS in this build";
		return nullptr;
	}
	bool evaluate(ID3D12GraphicsCommandList *, Feature *, const Eval &, std::string &error)
	{
		error = "no DLSS in this build";
		return false;
	}
	void release(Feature *&feature)
	{
		feature = nullptr;
	}
	void shutdown(ID3D12Device *)
	{
	}
}

#endif
