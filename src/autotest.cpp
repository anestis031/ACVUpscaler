#include "autotest.hpp"
#include "snapshot.hpp"
#include "fsr.hpp"

namespace acv
{
	namespace
	{
		constexpr uint32_t kConfigCount = 4;
		constexpr float kReactiveGains[kConfigCount] = { 0.0f, 4.0f, 16.0f, 64.0f };
		constexpr uint64_t kSettleFrames = 90;    // Frames FSR accumulates before the snapshot
		constexpr uint64_t kSnapshotTimeout = 900;

		uint32_t snapshots_done(DeviceData &data)
		{
			const std::lock_guard<std::mutex> lock(data.snapshot.preview_mutex);
			return data.snapshot.done;
		}

		void apply_config(DeviceData &data, uint32_t config, uint64_t frame)
		{
			AutoTest &test = data.autotest;
			data.fsr_reactive_gain.store(kReactiveGains[config], std::memory_order_relaxed);
			test.wait_until = frame + kSettleFrames;
			test.snapping = false;
			data.fsr->reset_history();
			log::write("auto test: configuration %u of %u (reactive gain %.0f) from frame %llu", config + 1, kConfigCount, static_cast<double>(kReactiveGains[config]), frame);
		}

		void finish(DeviceData &data, const char *reason)
		{
			AutoTest &test = data.autotest;
			data.replace_mode.store(test.saved_mode, std::memory_order_relaxed);
			data.fsr_reactive_gain.store(test.saved_reactive_gain, std::memory_order_relaxed);
			test.config.store(0, std::memory_order_relaxed);
			log::write("auto test: %s, previous settings restored", reason);
		}
	}

	bool autotest::start(DeviceData &data)
	{
		AutoTest &test = data.autotest;
		if (test.config.load(std::memory_order_relaxed) != 0)
			return false;

		test.saved_mode = data.replace_mode.load(std::memory_order_relaxed);
		test.saved_reactive_gain = data.fsr_reactive_gain.load(std::memory_order_relaxed);

		data.replace_mode.store(static_cast<uint32_t>(ReplaceMode::Fsr), std::memory_order_relaxed);
		test.config.store(1, std::memory_order_relaxed);
		apply_config(data, 0, data.frame_index.load(std::memory_order_relaxed));
		return true;
	}

	void autotest::on_present(DeviceData &data, uint64_t frame)
	{
		AutoTest &test = data.autotest;
		const uint32_t config = test.config.load(std::memory_order_relaxed);
		if (config == 0)
			return;

		if (!test.snapping)
		{
			if (frame < test.wait_until)
				return;
			test.snapshots_before = snapshots_done(data);
			if (!snapshot::request(data))
				return; // A manual snapshot is in progress: try again in the next frame
			test.snapping = true;
			test.wait_until = frame + kSnapshotTimeout;
			return;
		}

		if (snapshots_done(data) == test.snapshots_before)
		{
			if (frame > test.wait_until)
				finish(data, "stopped, the snapshot did not complete");
			return;
		}

		if (config == kConfigCount)
		{
			finish(data, "done");
			return;
		}
		test.config.store(config + 1, std::memory_order_relaxed);
		apply_config(data, config, frame);
	}

	std::string autotest::state(const DeviceData &data)
	{
		const uint32_t config = data.autotest.config.load(std::memory_order_relaxed);
		if (config == 0)
			return "idle";
		std::string text;
		append_format(text, "configuration %u of %u, keep the camera still", config, kConfigCount);
		return text;
	}
}
