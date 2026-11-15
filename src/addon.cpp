// Add-on entry point: exports, the log file, device lifetime and the device-level events that feed
// the pipeline registry, the resource table and the descriptor shadow.
#include "frame.hpp"
#include "fsr.hpp"
#include "overlay.hpp"
#include "replace.hpp"
#include "snapshot.hpp"
#include "record.hpp"

#include <cstdio>
#include <d3d12.h>
#include <share.h>

namespace acv
{
	// ---- Log ------------------------------------------------------------------------------------

	namespace
	{
		std::mutex s_log_mutex;
		std::FILE *s_log_file = nullptr;
		std::atomic<bool> s_log_dirty { false };
		std::wstring s_directory;
		std::string s_directory_utf8;

		void write_line(const char *prefix, const char *format, va_list args, bool flush) noexcept
		{
			try
			{
				const std::lock_guard<std::mutex> lock(s_log_mutex);
				if (s_log_file == nullptr)
					return;

				SYSTEMTIME time;
				GetLocalTime(&time);
				std::fprintf(s_log_file, "%02u:%02u:%02u.%03u [%5lu] %s", time.wHour, time.wMinute, time.wSecond, time.wMilliseconds, GetCurrentThreadId(), prefix);
				std::vfprintf(s_log_file, format, args);
				std::fputc('\n', s_log_file);

				if (flush)
					std::fflush(s_log_file);
				else
					s_log_dirty.store(true, std::memory_order_relaxed);
			}
			catch (...)
			{
			}
		}
	}

	void log::open(HMODULE addon_module)
	{
		std::wstring path(1024, L'\0');
		for (;;)
		{
			const DWORD length = GetModuleFileNameW(addon_module, path.data(), static_cast<DWORD>(path.size()));
			if (length == 0)
				return;
			if (length < path.size())
			{
				path.resize(length);
				break;
			}
			if (path.size() >= 32768)
				return;
			path.resize(path.size() * 2);
		}

		const size_t separator = path.find_last_of(L"\\/");
		s_directory = separator != std::wstring::npos ? path.substr(0, separator + 1) : std::wstring();

		const int utf8_length = WideCharToMultiByte(CP_UTF8, 0, s_directory.data(), static_cast<int>(s_directory.size()), nullptr, 0, nullptr, nullptr);
		s_directory_utf8.resize(utf8_length > 0 ? utf8_length : 0);
		if (utf8_length > 0)
			WideCharToMultiByte(CP_UTF8, 0, s_directory.data(), static_cast<int>(s_directory.size()), s_directory_utf8.data(), utf8_length, nullptr, nullptr);

		const std::lock_guard<std::mutex> lock(s_log_mutex);
		if (s_log_file == nullptr)
			s_log_file = _wfsopen((s_directory + L"acvupscaler.log").c_str(), L"w", _SH_DENYWR); // Truncates; readable while the game runs
	}
	void log::close()
	{
		const std::lock_guard<std::mutex> lock(s_log_mutex);
		if (s_log_file != nullptr)
			std::fclose(s_log_file);
		s_log_file = nullptr;
	}

	void log::write(const char *format, ...)
	{
		va_list args;
		va_start(args, format);
		write_line("", format, args, false);
		va_end(args);
	}
	void log::warn(const char *format, ...)
	{
		va_list args;
		va_start(args, format);
		write_line("WARNING: ", format, args, true);
		va_end(args);
	}
	void log::flush_if_dirty()
	{
		if (!s_log_dirty.exchange(false, std::memory_order_relaxed))
			return;
		try
		{
			const std::lock_guard<std::mutex> lock(s_log_mutex);
			if (s_log_file != nullptr)
				std::fflush(s_log_file);
		}
		catch (...)
		{
		}
	}

	const std::wstring &log::directory() { return s_directory; }
	const std::string &log::directory_utf8() { return s_directory_utf8; }

	void append_format(std::string &out, const char *format, ...)
	{
		char stack[512];

		va_list args, args_copy;
		va_start(args, format);
		va_copy(args_copy, args);
		const int length = std::vsnprintf(stack, sizeof(stack), format, args);
		va_end(args);

		if (length > 0 && static_cast<size_t>(length) < sizeof(stack))
		{
			out.append(stack, static_cast<size_t>(length));
		}
		else if (length > 0)
		{
			const size_t offset = out.size();
			out.resize(offset + static_cast<size_t>(length) + 1);
			std::vsnprintf(out.data() + offset, static_cast<size_t>(length) + 1, format, args_copy);
			out.resize(offset + static_cast<size_t>(length));
		}
		va_end(args_copy);
	}

	// ---- Device-level events --------------------------------------------------------------------

	namespace
	{
		void on_init_device(api::device *device)
		{
			guarded("init_device", [&]() {
				if (device->get_api() != api::device_api::d3d12)
				{
					log::write("ignoring device 0x%p: graphics API 0x%X is not Direct3D 12", static_cast<void *>(device), static_cast<unsigned int>(device->get_api()));
					return;
				}
				if (device->get_private_data<DeviceHolder>() != nullptr)
					return;

				device->create_private_data<DeviceHolder>()->data = std::make_shared<DeviceData>();
				log::write("attached to Direct3D 12 device 0x%p (ID3D12Device 0x%llX)", static_cast<void *>(device), device->get_native());
				log::flush_if_dirty();
			});
		}
		void on_destroy_device(api::device *device)
		{
			guarded("destroy_device", [&]() {
				const DeviceHolder *const holder = device->get_private_data<DeviceHolder>();
				if (holder == nullptr)
					return;

				if (holder->data != nullptr)
				{
					const PipelineRegistry::Counts counts = holder->data->pipelines.counts();
					log::write("detached from Direct3D 12 device 0x%p: %llu pipelines seen, %llu frames presented",
						static_cast<void *>(device), counts.created, holder->data->frame_index.load(std::memory_order_relaxed));
				}
				if (holder->data != nullptr)
				{
					snapshot::release(device, *holder->data);
					record::release(device);
				}
				// Command lists that outlive the device keep the data alive through their own reference
				device->destroy_private_data<DeviceHolder>();
				log::flush_if_dirty();
			});
		}

		void on_init_swapchain(api::swapchain *swapchain, bool)
		{
			guarded("init_swapchain", [&]() {
				api::device *const device = swapchain->get_device();
				DeviceData *const data = device_data(device);
				if (data == nullptr)
					return;

				// Back buffers are not announced through 'init_resource'
				const uint32_t count = swapchain->get_back_buffer_count();
				for (uint32_t i = 0; i < count; ++i)
				{
					const api::resource buffer = swapchain->get_back_buffer(i);
					if (buffer.handle == 0)
						continue;

					const api::resource_desc desc = device->get_resource_desc(buffer);
					data->resources.add(buffer.handle, desc);
					{
						const std::lock_guard<std::mutex> lock(data->swapchain_mutex);
						data->back_buffers.emplace_back(swapchain, buffer.handle);
					}

					if (i == 0)
					{
						data->swap_width.store(desc.texture.width, std::memory_order_relaxed);
						data->swap_height.store(desc.texture.height, std::memory_order_relaxed);
						data->swap_format.store(static_cast<uint32_t>(desc.texture.format), std::memory_order_relaxed);

						char format[16];
						log::write("swap chain 0x%p: %u buffers, %ux%u %s", static_cast<void *>(swapchain), count, desc.texture.width, desc.texture.height, format_name(desc.texture.format, format));
					}
				}
			});
		}
		void on_destroy_effect_runtime(api::effect_runtime *runtime)
		{
			// Swap chain destruction or resize. The FidelityFX context goes here, while the device and the queue
			// are still fully alive; FSR creates a new one at the next dispatch.
			guarded("destroy_effect_runtime", [&]() {
				DeviceData *const data = device_data(runtime->get_device());
				if (data == nullptr)
					return;
				if (api::command_queue *const queue = runtime->get_command_queue())
					queue->wait_idle();
				data->fsr->shutdown();
			});
		}

		void on_destroy_swapchain(api::swapchain *swapchain, bool)
		{
			guarded("destroy_swapchain", [&]() {
				DeviceData *const data = device_data(swapchain->get_device());
				if (data == nullptr)
					return;

				const std::lock_guard<std::mutex> lock(data->swapchain_mutex);
				for (auto it = data->back_buffers.begin(); it != data->back_buffers.end();)
				{
					if (it->first == swapchain)
					{
						data->resources.remove(it->second);
						it = data->back_buffers.erase(it);
					}
					else
					{
						++it;
					}
				}
			});
		}

		void on_init_pipeline(api::device *device, api::pipeline_layout layout, uint32_t subobject_count, const api::pipeline_subobject *subobjects, api::pipeline pipeline)
		{
			if (DeviceData *const data = device_data(device))
				guarded("init_pipeline", [&]() { data->pipelines.add(pipeline.handle, layout.handle, subobject_count, subobjects); });
		}
		void on_destroy_pipeline(api::device *device, api::pipeline pipeline)
		{
			if (DeviceData *const data = device_data(device))
				guarded("destroy_pipeline", [&]() { data->pipelines.remove(pipeline.handle); });
		}

		// The game renders below the output resolution without any texture LOD bias (all its samplers have bias 0),
		// so textures come out as blurry as the render resolution. Anisotropic samplers are the material samplers;
		// the others (post-processing, shadows, UI) are left alone.
		// This cannot be done in 'create_sampler': ReShade raises that event for the static samplers of root
		// signatures too, and a change there makes it rebuild the root signature, after which the game's pipelines
		// can no longer be created (seen in the test harness). 'init_sampler' is raised for heap samplers only, with
		// the CPU descriptor handle the game wrote to: the sampler is simply written again with the bias added.
		void apply_mip_bias(api::device *device, const api::sampler_desc &desc, api::sampler sampler)
		{
			const uint32_t filter = static_cast<uint32_t>(desc.filter);
			if ((filter & 0x40) == 0 || (filter & 0x80) != 0 || sampler.handle == 0) // D3D12_FILTER_ANISOTROPIC family, no comparison
				return;
			const float bias = replace::session_mip_bias();
			if (bias == 0.0f)
				return;

			// Same conversion as ReShade's own (d3d12_impl_type_convert.cpp)
			D3D12_SAMPLER_DESC native = {};
			native.Filter = static_cast<D3D12_FILTER>(filter);
			native.AddressU = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(desc.address_u);
			native.AddressV = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(desc.address_v);
			native.AddressW = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(desc.address_w);
			native.MipLODBias = desc.mip_lod_bias + bias;
			native.MaxAnisotropy = static_cast<UINT>(desc.max_anisotropy);
			native.ComparisonFunc = static_cast<D3D12_COMPARISON_FUNC>(static_cast<uint32_t>(desc.compare_op) + 1);
			std::memcpy(native.BorderColor, desc.border_color, sizeof(native.BorderColor));
			native.MinLOD = desc.min_lod;
			native.MaxLOD = desc.max_lod;
			reinterpret_cast<ID3D12Device *>(static_cast<uintptr_t>(device->get_native()))->CreateSampler(&native, { static_cast<SIZE_T>(sampler.handle) });

			static std::atomic<uint32_t> s_count { 0 };
			if (s_count.fetch_add(1, std::memory_order_relaxed) == 0)
				log::write("texture LOD bias %+.2f written to the first anisotropic sampler (filter 0x%X, anisotropy %g)", static_cast<double>(bias), filter, static_cast<double>(desc.max_anisotropy));
		}

		// Which texture filters and mip biases the game uses, and when it creates them (a mip bias that follows
		// the game's upscaling setting would show up here as new combinations after the setting changes)
		void on_init_sampler(api::device *device, const api::sampler_desc &desc, api::sampler sampler)
		{
			if (device_data(device) == nullptr)
				return;
			guarded("init_sampler", [&]() {
				apply_mip_bias(device, desc, sampler);

				struct Combination { uint32_t filter; float bias, anisotropy; uint64_t count; };
				static std::mutex s_mutex;
				static std::vector<Combination> s_seen;

				const std::lock_guard<std::mutex> lock(s_mutex);
				for (Combination &seen : s_seen)
				{
					if (seen.filter == static_cast<uint32_t>(desc.filter) && seen.bias == desc.mip_lod_bias && seen.anisotropy == desc.max_anisotropy)
					{
						++seen.count;
						return;
					}
				}
				if (s_seen.size() < 64)
				{
					s_seen.push_back({ static_cast<uint32_t>(desc.filter), desc.mip_lod_bias, desc.max_anisotropy, 1 });
					log::write("sampler combination #%u: filter 0x%X, mip bias %+.3f, anisotropy %g", static_cast<unsigned int>(s_seen.size()),
						static_cast<uint32_t>(desc.filter), static_cast<double>(desc.mip_lod_bias), static_cast<double>(desc.max_anisotropy));
				}
			});
		}

		void on_init_resource(api::device *device, const api::resource_desc &desc, const api::subresource_data *, api::resource_usage, api::resource resource)
		{
			if (DeviceData *const data = device_data(device))
				guarded("init_resource", [&]() { data->resources.add(resource.handle, desc); });
		}
		void on_destroy_resource(api::device *device, api::resource resource)
		{
			if (DeviceData *const data = device_data(device))
				guarded("destroy_resource", [&]() { data->resources.remove(resource.handle); });
		}

		// Both descriptor events return false: the game's call always goes through unchanged
		bool on_update_descriptor_tables(api::device *device, uint32_t count, const api::descriptor_table_update *updates)
		{
			if (DeviceData *const data = updates != nullptr ? device_data(device) : nullptr)
			{
				guarded("update_descriptor_tables", [&]() {
					for (uint32_t i = 0; i < count; ++i)
						data->descriptors.on_update(device, updates[i]);
				});
			}
			return false;
		}
		bool on_copy_descriptor_tables(api::device *device, uint32_t count, const api::descriptor_table_copy *copies)
		{
			if (DeviceData *const data = copies != nullptr ? device_data(device) : nullptr)
			{
				guarded("copy_descriptor_tables", [&]() {
					for (uint32_t i = 0; i < count; ++i)
						data->descriptors.on_copy(device, copies[i]);
				});
			}
			return false;
		}

		void register_events()
		{
			reshade::register_event<reshade::addon_event::init_device>(on_init_device);
			reshade::register_event<reshade::addon_event::destroy_device>(on_destroy_device);
			reshade::register_event<reshade::addon_event::init_swapchain>(on_init_swapchain);
			reshade::register_event<reshade::addon_event::destroy_swapchain>(on_destroy_swapchain);
			reshade::register_event<reshade::addon_event::destroy_effect_runtime>(on_destroy_effect_runtime);
			reshade::register_event<reshade::addon_event::init_pipeline>(on_init_pipeline);
			reshade::register_event<reshade::addon_event::destroy_pipeline>(on_destroy_pipeline);
			reshade::register_event<reshade::addon_event::init_resource>(on_init_resource);
			reshade::register_event<reshade::addon_event::init_sampler>(on_init_sampler);
			reshade::register_event<reshade::addon_event::destroy_resource>(on_destroy_resource);
			reshade::register_event<reshade::addon_event::update_descriptor_tables>(on_update_descriptor_tables);
			reshade::register_event<reshade::addon_event::copy_descriptor_tables>(on_copy_descriptor_tables);
			reshade::register_event<reshade::addon_event::reshade_present>(overlay::on_reshade_present);
			frame::register_events();
			reshade::register_overlay(nullptr, overlay::draw);
		}
		void unregister_events()
		{
			reshade::unregister_overlay(nullptr, overlay::draw);
			frame::unregister_events();
			reshade::unregister_event<reshade::addon_event::reshade_present>(overlay::on_reshade_present);
			reshade::unregister_event<reshade::addon_event::copy_descriptor_tables>(on_copy_descriptor_tables);
			reshade::unregister_event<reshade::addon_event::update_descriptor_tables>(on_update_descriptor_tables);
			reshade::unregister_event<reshade::addon_event::destroy_resource>(on_destroy_resource);
			reshade::unregister_event<reshade::addon_event::init_sampler>(on_init_sampler);
			reshade::unregister_event<reshade::addon_event::init_resource>(on_init_resource);
			reshade::unregister_event<reshade::addon_event::destroy_pipeline>(on_destroy_pipeline);
			reshade::unregister_event<reshade::addon_event::init_pipeline>(on_init_pipeline);
						reshade::unregister_event<reshade::addon_event::destroy_effect_runtime>(on_destroy_effect_runtime);
reshade::unregister_event<reshade::addon_event::destroy_swapchain>(on_destroy_swapchain);
			reshade::unregister_event<reshade::addon_event::init_swapchain>(on_init_swapchain);
			reshade::unregister_event<reshade::addon_event::destroy_device>(on_destroy_device);
			reshade::unregister_event<reshade::addon_event::init_device>(on_init_device);
		}
	}
}

extern "C" __declspec(dllexport) const char *NAME = "ACVUpscaler";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "Assassin's Creed Valhalla temporal upscaler mod. This build can replace the game TAA and FSR 1 with FSR 3.1 (F7). With the replacement off it only detects passes and logs diagnostics.";

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved)
{
	switch (reason)
	{
	case DLL_PROCESS_ATTACH:
		// Fails (and the DLL is simply not loaded) outside of a ReShade build with a matching add-on API
		if (!reshade::register_addon(module))
			return FALSE;

		acv::guarded("attach", [module]() {
			acv::log::open(module);
			acv::log::write("ACVUpscaler " ACV_VERSION ", built for ReShade add-on API %d", RESHADE_API_VERSION);
			acv::log::write("log directory: %s", acv::log::directory_utf8().c_str());
			acv::log::flush_if_dirty();
		});

		acv::register_events();
		break;
	case DLL_PROCESS_DETACH:
		// On process termination other threads are already gone, possibly while holding one of our locks: touch nothing
		if (reserved != nullptr)
			break;

		acv::unregister_events();
		reshade::unregister_addon(module);

		acv::log::write("add-on unloaded");
		acv::log::close();
		break;
	}
	return TRUE;
}
