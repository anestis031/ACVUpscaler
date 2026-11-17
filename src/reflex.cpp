#include "reflex.hpp"

#ifdef ACV_WITH_REFLEX

#include <dxgi1_4.h> // nvapi.h names DXGI types without including them
#include <nvapi.h>

namespace acv::reflex
{
	namespace
	{
		constexpr uint32_t kReportFrames = 300; // Frames between two reports in the log (developer tools)

		struct State
		{
			bool tried = false, available = false;
			std::string reason;             // Why not available
			ID3D12Device *device = nullptr; // The device the driver was last told a mode for
			bool mode_on = false;           // That mode
			bool driver_confirms = false;   // The driver's own word on it, asked at every report
			bool refused = false;           // The driver did not take the mode for this device: not asked again
			bool sleep_failed = false;      // Logged once
			uint64_t frame_id = 0;          // Of the frame being recorded; zero before the first marked frame
			uint32_t frames = 0;            // Since the last report
		};
		State s_state;

		bool start()
		{
			if (!s_state.tried)
			{
				s_state.tried = true;
				const NvAPI_Status status = NvAPI_Initialize();
				s_state.available = status == NVAPI_OK;
				if (!s_state.available)
				{
					append_format(s_state.reason, "NVAPI did not start (status %d): no NVIDIA driver?", static_cast<int>(status));
					log::write("Reflex: not available: %s", s_state.reason.c_str());
				}
			}
			return s_state.available;
		}

		void mark(ID3D12Device *device, NV_LATENCY_MARKER_TYPE type)
		{
			NV_LATENCY_MARKER_PARAMS params = {};
			params.version = NV_LATENCY_MARKER_PARAMS_VER;
			params.frameID = s_state.frame_id;
			params.markerType = type;
			NvAPI_D3D_SetLatencyMarker(device, &params);
		}

		void set_mode(ID3D12Device *device, bool on)
		{
			NV_SET_SLEEP_MODE_PARAMS params = {};
			params.version = NV_SET_SLEEP_MODE_PARAMS_VER;
			params.bLowLatencyMode = on;
			const NvAPI_Status status = NvAPI_D3D_SetSleepMode(device, &params);
			s_state.mode_on = on && status == NVAPI_OK;
			s_state.refused = on && status != NVAPI_OK;
			s_state.driver_confirms = false;
			if (status == NVAPI_OK)
				log::write("Reflex: low latency mode switched %s", on ? "on" : "off");
			else
				log::warn("Reflex: the driver did not take the low latency mode (status %d)", static_cast<int>(status));
		}

		// Averages of the driver's latency report over its valid frames. Times are in microseconds.
		void report(ID3D12Device *device)
		{
			NV_GET_SLEEP_STATUS_PARAMS sleep = {};
			sleep.version = NV_GET_SLEEP_STATUS_PARAMS_VER;
			if (NvAPI_D3D_GetSleepStatus(device, &sleep) == NVAPI_OK)
				s_state.driver_confirms = sleep.bLowLatencyMode != 0;
			if (!settings::get().dev_mode)
				return;

			static NV_LATENCY_RESULT_PARAMS result; // 16 KB
			result = {};
			result.version = NV_LATENCY_RESULT_PARAMS_VER;
			if (NvAPI_D3D_GetLatency(device, &result) != NVAPI_OK)
				return;
			double total = 0.0, queue = 0.0, gpu = 0.0, present = 0.0, frame_time = 0.0;
			uint32_t count = 0;
			for (const auto &frame : result.frameReport)
			{
				if (frame.frameID == 0 || frame.simStartTime == 0 || frame.gpuRenderEndTime <= frame.simStartTime)
					continue;
				++count;
				total += static_cast<double>(frame.gpuRenderEndTime - frame.simStartTime);
				queue += static_cast<double>(frame.osRenderQueueEndTime - frame.osRenderQueueStartTime);
				gpu += static_cast<double>(frame.gpuRenderEndTime - frame.gpuRenderStartTime);
				present += static_cast<double>(frame.presentEndTime - frame.presentStartTime);
				frame_time += static_cast<double>(frame.gpuFrameTimeUs);
			}
			if (count == 0)
				return;
			const double scale = 0.001 / count;
			log::write("Reflex %s (driver says low latency %s, sleep call %s): %u frames: start of frame to end of GPU work %.2f ms, OS render queue span %.2f ms, GPU work span %.2f ms, Present call %.2f ms, frame time %.2f ms",
				s_state.mode_on ? "on" : "off", sleep.bLowLatencyMode ? "on" : "off", sleep.bUseGameSleep ? "seen" : "not seen", count,
				total * scale, queue * scale, gpu * scale, present * scale, frame_time * scale);
		}
	}

	void before_present(ID3D12Device *device)
	{
		if (!s_state.available || s_state.frame_id == 0 || !settings::get().dev_mode)
			return;
		mark(device, RENDERSUBMIT_END);
		mark(device, PRESENT_START);
	}

	void after_present(ID3D12Device *device)
	{
		const bool wanted = settings::reflex(), dev = settings::get().dev_mode;
		// Off and never switched on: the driver hears nothing from the add-on
		if (!wanted && !s_state.mode_on && !dev)
			return;
		if (!start())
			return;

		if (dev && s_state.frame_id != 0)
			mark(device, PRESENT_END);

		if (device != s_state.device)
		{
			s_state.device = device;
			s_state.mode_on = s_state.refused = s_state.sleep_failed = false;
		}
		if (wanted != s_state.mode_on && !(wanted && s_state.refused))
			set_mode(device, wanted);
		// The one sleep call of the frame. The driver waits here as long as the GPU still has frames to work on.
		if (s_state.mode_on && NvAPI_D3D_Sleep(device) != NVAPI_OK && !s_state.sleep_failed)
		{
			s_state.sleep_failed = true;
			log::warn("Reflex: the sleep call failed");
		}

		if (dev)
		{
			// The frame starts here for the add-on. What the game calls simulation ran earlier, on its own threads.
			++s_state.frame_id;
			mark(device, SIMULATION_START);
			mark(device, SIMULATION_END);
			mark(device, RENDERSUBMIT_START);
		}
		if ((dev || s_state.mode_on) && ++s_state.frames >= kReportFrames)
		{
			s_state.frames = 0;
			report(device);
		}
	}

	std::string status()
	{
		if (!settings::reflex())
			return "off";
		if (s_state.tried && !s_state.available)
			return "not available: " + s_state.reason;
		if (!s_state.mode_on)
			return s_state.refused ? "not taken by the driver (see the log)" : "starting";
		return s_state.driver_confirms ? "on (confirmed by the driver)" : "on";
	}
}

#else

namespace acv::reflex
{
	void before_present(ID3D12Device *)
	{
	}
	void after_present(ID3D12Device *)
	{
	}
	std::string status()
	{
		return settings::reflex() ? "not in this build of the add-on (built without NVAPI)" : "off";
	}
}

#endif
