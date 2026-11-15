#include "fsr.hpp"

#include <d3dcompiler.h>
#include <dx12/ffx_api_dx12.h>
#include <ffx_upscale.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <share.h>

using Microsoft::WRL::ComPtr;

namespace acv
{
	namespace
	{
		// ---- FidelityFX API, loaded from the add-on directory ----------------------------------------

		struct FfxApi
		{
			bool loaded = false;
			PfnFfxCreateContext create = nullptr;
			PfnFfxDestroyContext destroy = nullptr;
			PfnFfxQuery query = nullptr;
			PfnFfxDispatch dispatch = nullptr;
		};

		const FfxApi &ffx_api()
		{
			static FfxApi s_api;
			static std::once_flag s_once;
			std::call_once(s_once, []() {
				// The loader finds the upscaler by name, so it has to be in the process already
				const std::wstring upscaler_path = log::directory() + L"amd_fidelityfx_upscaler_dx12.dll";
				const std::wstring loader_path = log::directory() + L"amd_fidelityfx_loader_dx12.dll";
				if (LoadLibraryExW(upscaler_path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH) == nullptr)
				{
					log::warn("FSR: could not load amd_fidelityfx_upscaler_dx12.dll from the add-on directory (error %lu)", GetLastError());
					return;
				}
				const HMODULE loader = LoadLibraryExW(loader_path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
				if (loader == nullptr)
				{
					log::warn("FSR: could not load amd_fidelityfx_loader_dx12.dll from the add-on directory (error %lu)", GetLastError());
					return;
				}

				s_api.create = reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(loader, "ffxCreateContext"));
				s_api.destroy = reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(loader, "ffxDestroyContext"));
				s_api.query = reinterpret_cast<PfnFfxQuery>(GetProcAddress(loader, "ffxQuery"));
				s_api.dispatch = reinterpret_cast<PfnFfxDispatch>(GetProcAddress(loader, "ffxDispatch"));
				s_api.loaded = s_api.create != nullptr && s_api.destroy != nullptr && s_api.query != nullptr && s_api.dispatch != nullptr;
				if (s_api.loaded)
					log::write("FSR: FidelityFX API loaded from the add-on directory");
				else
					log::warn("FSR: amd_fidelityfx_loader_dx12.dll does not export the FidelityFX API functions");
			});
			return s_api;
		}

		void on_ffx_message(uint32_t type, const wchar_t *message)
		{
			static std::atomic<uint32_t> s_reported { 0 };
			if (s_reported.fetch_add(1, std::memory_order_relaxed) < 32)
				log::warn("FSR %s: %ls", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", message != nullptr ? message : L"(null)");
		}

		// ---- Decode pass ------------------------------------------------------------------------------

		// Velocity: 12 bits per axis packed into RGB, fourth-root encoded, m = uvCurrent - uvPrevious
		// (docs/TECHNICAL.md, verified on snapshots). RGB 0 means "no vector".
		// Sky: nothing writes the velocity texture there. With a still camera it reads as "no vector", with a
		// moving one it holds stale values of up to hundreds of pixels (recorded benchmark), which DLSS preset K
		// shows as blocks. The TAA of the game ignores the texture at the far plane and reprojects with the
		// camera matrices; the same is done here, instruction for instruction.
		// FSR wants the motion from the current to the previous position, hence the minus.
		// Depth: the game hands its TAA the view-space distance; FSR wants the depth buffer value.
		// Mask: t3 holds what rain, fire and similar effects cover (they have no motion vectors and no depth).
		// As a reactive mask it keeps FSR from smearing them over several frames. Never fully reactive: such
		// pixels would stay aliased.
		const char kDecodeShader[] = R"(
Texture2D<float4> velocity : register(t0);
Texture2D<float> linear_depth : register(t1);
Texture2D<float> mask : register(t2);
RWTexture2D<float2> out_motion : register(u0);
RWTexture2D<float> out_depth : register(u1);
RWTexture2D<float> out_reactive : register(u2);
cbuffer constants : register(b0)
{
	float depth_a; float depth_b; float motion_sign; float reactive_gain;
	float4 inverse_view[3];   // View constants rows 6..8
	float4 previous_x, previous_y, previous_w; // Rows 38, 39, 41 of the previous view-projection
	float4 far_plane;         // Row 54: .y is the depth of the far plane
	float4 inverse_projection[4]; // Rows 69..72
	float sky_distance; float2 inverse_size; float unused;
};

bool motion_at(int2 p, out float2 m)
{
	m = 0.0;
	const uint3 code = uint3(velocity[p].rgb * 255.0 + 0.5);
	if (!any(code != 0))
		return false;
	const uint2 q = uint2(code.r * 16 + (code.g >> 4), (code.g & 15) * 256 + code.b);
	const float2 e = float2(q) * (2.0 / 4094.0) - 1.0;
	m = sign(e) * 0.2 * (e * e) * (e * e);
	return true;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	const int2 p = int2(id.xy);
	float2 m = 0.0;
	if (linear_depth[p] >= sky_distance)
	{
		const float2 uv = (float2(p) + 0.5) * inverse_size;
		const float4 ndc = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), far_plane.y, 1.0);
		const float4 view_h = float4(dot(ndc, inverse_projection[0]), dot(ndc, inverse_projection[1]), dot(ndc, inverse_projection[2]), dot(ndc, inverse_projection[3]));
		const float4 view = float4(view_h.xyz / view_h.w, 1.0);
		const float4 world = float4(dot(view, inverse_view[0]), dot(view, inverse_view[1]), dot(view, inverse_view[2]), 1.0);
		const float previous_clip_w = dot(world, previous_w);
		const float2 previous_uv = float2(dot(world, previous_x), dot(world, previous_y)) / previous_clip_w * float2(0.5, -0.5) + 0.5;
		// A point that was behind the camera or far off the screen a frame ago has no usable history anyway
		if (view_h.w != 0.0 && previous_clip_w > 0.0)
			m = clamp(uv - previous_uv, -1.0, 1.0);
	}
	else
	{
		motion_at(p, m);
	}
	out_motion[id.xy] = -motion_sign * m;

	const float distance = linear_depth[id.xy];
	out_depth[id.xy] = saturate(depth_b / max(distance, 1e-6) - depth_a);

	out_reactive[id.xy] = min(mask[id.xy] * reactive_gain, 0.9);
}
)";

		D3D12_RESOURCE_BARRIER transition(ID3D12Resource *resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
		{
			D3D12_RESOURCE_BARRIER barrier = {};
			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barrier.Transition.pResource = resource;
			barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			barrier.Transition.StateBefore = before;
			barrier.Transition.StateAfter = after;
			return barrier;
		}

		bool create_texture(ID3D12Device *device, uint32_t width, uint32_t height, DXGI_FORMAT format, D3D12_RESOURCE_STATES state, ComPtr<ID3D12Resource> &texture)
		{
			D3D12_HEAP_PROPERTIES heap = {};
			heap.Type = D3D12_HEAP_TYPE_DEFAULT;
			D3D12_RESOURCE_DESC desc = {};
			desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			desc.Width = width;
			desc.Height = height;
			desc.DepthOrArraySize = 1;
			desc.MipLevels = 1;
			desc.Format = format;
			desc.SampleDesc.Count = 1;
			desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
			return SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&texture)));
		}

		bool format_is(ID3D12Resource *resource, DXGI_FORMAT typed, DXGI_FORMAT typeless, uint32_t width, uint32_t height)
		{
			const D3D12_RESOURCE_DESC desc = resource->GetDesc();
			return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.Width == width && desc.Height == height &&
				desc.SampleDesc.Count == 1 && (desc.Format == typed || desc.Format == typeless);
		}
	}

	Fsr::~Fsr()
	{
		// Deliberately no 'shutdown': this can run after the device is gone, and a context that is still alive
		// then is better leaked than destroyed (see 'shutdown' in fsr.hpp)
	}

	bool Fsr::create_decode_pass(ID3D12Device *device)
	{
		ComPtr<ID3DBlob> shader, error;
		if (FAILED(D3DCompile(kDecodeShader, sizeof(kDecodeShader) - 1, "acv_decode", nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shader, &error)))
		{
			log::warn("FSR: decode shader did not compile: %s", error != nullptr ? static_cast<const char *>(error->GetBufferPointer()) : "no message");
			return false;
		}

		D3D12_DESCRIPTOR_RANGE ranges[2] = {};
		ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		ranges[0].NumDescriptors = 3;
		ranges[0].OffsetInDescriptorsFromTableStart = 0;
		ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
		ranges[1].NumDescriptors = 3;
		ranges[1].OffsetInDescriptorsFromTableStart = 3;
		D3D12_ROOT_PARAMETER params[2] = {};
		params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		params[0].Constants.Num32BitValues = kDecodeConstants;
		params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[1].DescriptorTable.NumDescriptorRanges = 2;
		params[1].DescriptorTable.pDescriptorRanges = ranges;
		D3D12_ROOT_SIGNATURE_DESC root_desc = {};
		root_desc.NumParameters = 2;
		root_desc.pParameters = params;

		ComPtr<ID3DBlob> root_blob;
		if (FAILED(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &root_blob, &error)) ||
			FAILED(device->CreateRootSignature(0, root_blob->GetBufferPointer(), root_blob->GetBufferSize(), IID_PPV_ARGS(&_root_signature))))
		{
			log::warn("FSR: decode root signature could not be created");
			return false;
		}

		D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_desc = {};
		pipeline_desc.pRootSignature = _root_signature.Get();
		pipeline_desc.CS = { shader->GetBufferPointer(), shader->GetBufferSize() };
		if (FAILED(device->CreateComputePipelineState(&pipeline_desc, IID_PPV_ARGS(&_pipeline))))
		{
			log::warn("FSR: decode pipeline could not be created");
			return false;
		}

		D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
		heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		heap_desc.NumDescriptors = kRing * kDecodeDescriptors;
		heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		if (FAILED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&_heap))))
		{
			log::warn("FSR: decode descriptor heap could not be created");
			return false;
		}
		_heap_increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		return true;
	}

	bool Fsr::create_set(ID3D12Device *device, const Inputs &inputs, Set &set)
	{
		const uint32_t width = inputs.width, height = inputs.height;
		const FfxApi &api = ffx_api();
		if (!api.loaded)
		{
			_error = "FidelityFX DLLs not loaded";
			return false;
		}

		set.width = width;
		set.height = height;
		set.out_width = inputs.out_width;
		set.out_height = inputs.out_height;
		if (!create_texture(device, width, height, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, set.motion) ||
			!create_texture(device, width, height, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, set.depth) ||
			!create_texture(device, width, height, DXGI_FORMAT_R8_UNORM, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, set.reactive) ||
			!create_texture(device, inputs.out_width, inputs.out_height, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_COPY_SOURCE, set.output))
		{
			_error = "textures could not be created";
			return false;
		}

		// Colour is the game's tonemapped 8-bit image. Render size = output size means native anti-aliasing.
		ffxCreateContextDescUpscaleVersion version = {};
		version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
		version.version = FFX_UPSCALER_VERSION;
		ffxCreateBackendDX12Desc backend = {};
		backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
		backend.header.pNext = &version.header;
		backend.device = device;
		ffxCreateContextDescUpscale desc = {};
		desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
		desc.header.pNext = &backend.header;
		desc.flags = FFX_UPSCALE_ENABLE_DEPTH_INVERTED | FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
		desc.maxRenderSize = { width, height };
		desc.maxUpscaleSize = { inputs.out_width, inputs.out_height };
		desc.fpMessage = &on_ffx_message;

		const ffxReturnCode_t result = api.create(&set.context, &desc.header, nullptr);
		if (result != FFX_API_RETURN_OK || set.context == nullptr)
		{
			set.context = nullptr;
			_error = "ffxCreateContext failed with code " + std::to_string(result);
			return false;
		}

		ffxQueryGetProviderVersion provider = {};
		provider.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
		if (api.query(&set.context, &provider.header) == FFX_API_RETURN_OK && provider.versionName != nullptr)
			_provider = provider.versionName;
		else
			_provider = "unknown version";

		log::write("FSR: context created for %ux%u -> %ux%u, provider %s", width, height, inputs.out_width, inputs.out_height, _provider.c_str());
		return true;
	}

	// ACVUpscaler.ini, KeepContexts=1: contexts are never destroyed, only forgotten. For OptiScaler without its
	// swap chain hooks (installed as winmm.dll behind ReShade): there the game ended right after OptiScaler
	// released the features of our replaced contexts (docs/TECHNICAL.md, M5 step 3). Costs the memory of every old context.
	static bool keep_contexts()
	{
		return settings::get().keep_contexts;
	}

	void Fsr::destroy_set(Set &set)
	{
		if (set.context != nullptr)
		{
			const FfxApi &api = ffx_api();
			if (api.loaded && !keep_contexts())
				api.destroy(&set.context, nullptr);
			set.context = nullptr;
		}
		set = Set();
	}

	void Fsr::begin_timer(ID3D12Device *device, ID3D12GraphicsCommandList *list)
	{
		if (!settings::get().dev_mode || _timer_failed)
			return;

		if (_timer_heap == nullptr)
		{
			D3D12_QUERY_HEAP_DESC heap_desc = {};
			heap_desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
			heap_desc.Count = kRing * 2;
			D3D12_HEAP_PROPERTIES heap = {};
			heap.Type = D3D12_HEAP_TYPE_READBACK;
			D3D12_RESOURCE_DESC desc = {};
			desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			desc.Width = kRing * 2 * sizeof(uint64_t);
			desc.Height = 1;
			desc.DepthOrArraySize = 1;
			desc.MipLevels = 1;
			desc.SampleDesc.Count = 1;
			desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			void *mapped = nullptr;
			if (FAILED(device->CreateQueryHeap(&heap_desc, IID_PPV_ARGS(&_timer_heap))) ||
				FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&_timer_readback))) ||
				FAILED(_timer_readback->Map(0, nullptr, &mapped)))
			{
				log::warn("FSR: GPU timer could not be created");
				_timer_heap.Reset();
				_timer_readback.Reset();
				_timer_failed = true;
				return;
			}
			_timer_data = static_cast<const uint64_t *>(mapped);
		}

		// What this slot got kRing dispatches ago has long been run by the GPU
		_timer_slot = _timer_next++ % kRing;
		const uint64_t begin = _timer_data[_timer_slot * 2], end = _timer_data[_timer_slot * 2 + 1];
		if (_timer_frequency != 0 && end > begin)
		{
			const double ms = static_cast<double>(end - begin) * 1000.0 / static_cast<double>(_timer_frequency);
			_timer_sum_ms += ms;
			_timer_max_ms = std::fmax(_timer_max_ms, ms);
			if (++_timer_count == kTimerWindow)
			{
				_timer_ms = _timer_sum_ms / kTimerWindow;
				log::write("FSR: GPU time of the add-on's passes: %.2f ms on average over %u frames, longest %.2f ms", _timer_ms, kTimerWindow, _timer_max_ms);
				// The log starts empty with every start of the game; this file keeps the runs of a comparison
				if (FILE *const file = _wfsopen((log::directory() + L"acvupscaler_perf.log").c_str(), L"a", _SH_DENYNO))
				{
					SYSTEMTIME time;
					GetLocalTime(&time);
					std::fprintf(file, "%04u-%02u-%02u %02u:%02u:%02u  %s  %ux%u -> %ux%u  game sharpening %s  %.2f ms average, %.2f ms longest, %u frames\n",
						time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond, _provider.c_str(), _set.width, _set.height, _set.out_width, _set.out_height,
						settings::get().game_sharpening ? "on" : "off", _timer_ms, _timer_max_ms, kTimerWindow);
					std::fclose(file);
				}
				_timer_count = 0;
				_timer_sum_ms = _timer_max_ms = 0.0;
			}
		}

		list->EndQuery(_timer_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, _timer_slot * 2);
		_timer_open = true;
	}

	void Fsr::end_timer(ID3D12GraphicsCommandList *list)
	{
		const std::lock_guard<std::mutex> lock(_mutex);
		if (!_timer_open)
			return;
		_timer_open = false;
		list->EndQuery(_timer_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, _timer_slot * 2 + 1);
		list->ResolveQueryData(_timer_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, _timer_slot * 2, 2, _timer_readback.Get(), _timer_slot * 2 * sizeof(uint64_t));
	}

	bool Fsr::dispatch(ID3D12Device *device, ID3D12GraphicsCommandList *list, const Inputs &inputs)
	{
		const std::lock_guard<std::mutex> lock(_mutex);

		if (_decode_failed || (inputs.width == _failed_width && inputs.height == _failed_height && inputs.out_width == _failed_out_width && inputs.out_height == _failed_out_height))
			return false;

		if (!format_is(inputs.colour, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_TYPELESS, inputs.width, inputs.height) ||
			!format_is(inputs.velocity, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_TYPELESS, inputs.width, inputs.height) ||
			!format_is(inputs.linear_depth, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_TYPELESS, inputs.width, inputs.height))
		{
			_error = "unexpected input formats";
			return false;
		}
		if (inputs.output != nullptr && !format_is(inputs.output, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_TYPELESS, inputs.out_width, inputs.out_height))
		{
			_error = "unexpected output format";
			return false;
		}

		if (_pipeline == nullptr && !create_decode_pass(device))
		{
			_decode_failed = true;
			_error = "decode pass could not be created (see the log)";
			return false;
		}

		// Test harness only: ACV_TEST_RETIRE_AT=N replaces the context at dispatch N as a size change would
		static const uint64_t s_test_retire_at = []() {
			char text[24];
			const DWORD length = GetEnvironmentVariableA("ACV_TEST_RETIRE_AT", text, sizeof(text));
			return length != 0 && length < sizeof(text) ? std::strtoull(text, nullptr, 10) : 0ull;
		}();
		const bool test_retire = s_test_retire_at != 0 && _dispatches == s_test_retire_at;

		if (_set.context == nullptr || _set.width != inputs.width || _set.height != inputs.height || _set.out_width != inputs.out_width || _set.out_height != inputs.out_height || test_retire)
		{
			// The GPU may still be working with the old textures: they are freed in 'on_present'
			if (_set.context != nullptr || _set.output != nullptr)
			{
				_set.retired_frame = inputs.frame;
				_retired.push_back(std::move(_set));
				_set = Set();
			}
			if (!create_set(device, inputs, _set))
			{
				log::warn("FSR: could not set up %ux%u -> %ux%u: %s", inputs.width, inputs.height, inputs.out_width, inputs.out_height, _error.c_str());
				_set.retired_frame = inputs.frame; // Holds whatever was created before the failure
				_retired.push_back(std::move(_set));
				_set = Set();
				_failed_width = inputs.width;
				_failed_height = inputs.height;
				_failed_out_width = inputs.out_width;
				_failed_out_height = inputs.out_height;
				return false;
			}
			_reset = true;
		}

		// Without a usable mask the shader reads zero from a null descriptor and FSR gets no reactive mask
		const bool use_mask = inputs.mask != nullptr && inputs.reactive_gain > 0.0f &&
			format_is(inputs.mask, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_TYPELESS, inputs.width, inputs.height);

		begin_timer(device, list);

		// ---- Decode pass: velocity, distance and mask -> motion vectors, depth and reactive mask ----
		const uint32_t base = (_ring_next++ % kRing) * kDecodeDescriptors;
		D3D12_CPU_DESCRIPTOR_HANDLE cpu = _heap->GetCPUDescriptorHandleForHeapStart();
		cpu.ptr += static_cast<SIZE_T>(base) * _heap_increment;
		D3D12_GPU_DESCRIPTOR_HANDLE gpu = _heap->GetGPUDescriptorHandleForHeapStart();
		gpu.ptr += static_cast<UINT64>(base) * _heap_increment;

		const auto write_srv = [&](ID3D12Resource *resource, DXGI_FORMAT format) {
			D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
			desc.Format = format;
			desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			desc.Texture2D.MipLevels = 1;
			device->CreateShaderResourceView(resource, &desc, cpu);
			cpu.ptr += _heap_increment;
		};
		const auto write_uav = [&](ID3D12Resource *resource, DXGI_FORMAT format) {
			D3D12_UNORDERED_ACCESS_VIEW_DESC desc = {};
			desc.Format = format;
			desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
			device->CreateUnorderedAccessView(resource, nullptr, &desc, cpu);
			cpu.ptr += _heap_increment;
		};
		write_srv(inputs.velocity, DXGI_FORMAT_R8G8B8A8_UNORM);
		write_srv(inputs.linear_depth, DXGI_FORMAT_R32_FLOAT);
		write_srv(use_mask ? inputs.mask : nullptr, DXGI_FORMAT_R8_UNORM);
		write_uav(_set.motion.Get(), DXGI_FORMAT_R16G16_FLOAT);
		write_uav(_set.depth.Get(), DXGI_FORMAT_R32_FLOAT);
		write_uav(_set.reactive.Get(), DXGI_FORMAT_R8_UNORM);

		ID3D12DescriptorHeap *const heaps[1] = { _heap.Get() };
		float constants[kDecodeConstants] = { inputs.depth_a, inputs.depth_b, inputs.motion_sign, use_mask ? inputs.reactive_gain : 0.0f };
		std::memcpy(constants + 4, inputs.sky_rows, sizeof(inputs.sky_rows));
		constants[48] = inputs.sky_distance;
		constants[49] = 1.0f / static_cast<float>(inputs.width);
		constants[50] = 1.0f / static_cast<float>(inputs.height);
		list->SetDescriptorHeaps(1, heaps);
		list->SetComputeRootSignature(_root_signature.Get());
		list->SetPipelineState(_pipeline.Get());
		list->SetComputeRoot32BitConstants(0, kDecodeConstants, constants, 0);
		list->SetComputeRootDescriptorTable(1, gpu);
		list->Dispatch((inputs.width + 7) / 8, (inputs.height + 7) / 8, 1);

		// The add-on's own output is the last barrier: an output of the caller is in the right state already
		const UINT barrier_count = inputs.output != nullptr ? 3 : 4;
		const D3D12_RESOURCE_BARRIER to_read[4] = {
			transition(_set.motion.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
			transition(_set.depth.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
			transition(_set.reactive.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
			transition(_set.output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS) };
		list->ResourceBarrier(barrier_count, to_read);

		// ---- FSR ----
		LARGE_INTEGER now, frequency;
		QueryPerformanceCounter(&now);
		QueryPerformanceFrequency(&frequency);
		float delta_ms = _last_time != 0 ? static_cast<float>(static_cast<double>(now.QuadPart - _last_time) * 1000.0 / static_cast<double>(frequency.QuadPart)) : 16.7f;
		delta_ms = std::fmin(std::fmax(delta_ms, 1.0f), 200.0f);
		// Test harness only: ACV_TEST_FRAME_MS reports the frame time of a recorded sequence instead of the real one
		static const float s_test_frame_ms = []() {
			char text[24];
			const DWORD length = GetEnvironmentVariableA("ACV_TEST_FRAME_MS", text, sizeof(text));
			return length != 0 && length < sizeof(text) ? std::strtof(text, nullptr) : 0.0f;
		}();
		if (s_test_frame_ms > 0.0f)
			delta_ms = s_test_frame_ms;
		_last_time = now.QuadPart;

		// A gap in the frames (the replacement was off, or the game drew no scene) invalidates the history
		// Test harness only: ACV_TEST_RESET_AT=N drops the history at dispatch N (start of a replayed sequence)
		static const uint64_t s_test_reset_at = []() {
			char text[24];
			const DWORD length = GetEnvironmentVariableA("ACV_TEST_RESET_AT", text, sizeof(text));
			return length != 0 && length < sizeof(text) ? std::strtoull(text, nullptr, 10) : 0ull;
		}();
		const bool reset = _reset || inputs.frame > _last_frame + 1 || (s_test_reset_at != 0 && _dispatches == s_test_reset_at);
		_last_frame = inputs.frame;

		const float width = static_cast<float>(inputs.width), height = static_cast<float>(inputs.height);
		ffxDispatchDescUpscale desc = {};
		desc.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
		desc.commandList = list;
		desc.color = ffxApiGetResourceDX12(inputs.colour, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
		desc.depth = ffxApiGetResourceDX12(_set.depth.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
		desc.motionVectors = ffxApiGetResourceDX12(_set.motion.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
		if (use_mask)
			desc.reactive = ffxApiGetResourceDX12(_set.reactive.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
		desc.output = ffxApiGetResourceDX12(inputs.output != nullptr ? inputs.output : _set.output.Get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
		desc.jitterOffset = { inputs.jitter_x, inputs.jitter_y };
		desc.motionVectorScale = { width, height }; // The decoded vectors are in UV units
		desc.renderSize = { inputs.width, inputs.height };
		desc.upscaleSize = { inputs.out_width, inputs.out_height };
		desc.enableSharpening = false;
		desc.frameTimeDelta = delta_ms;
		desc.preExposure = 1.0f;
		desc.reset = reset;
		// Finite reversed Z: depth = b / distance - a, so near = b and far = b / a + b
		desc.cameraNear = inputs.depth_b;
		desc.cameraFar = inputs.depth_a > 0.0f ? inputs.depth_b / inputs.depth_a + inputs.depth_b : 100000.0f;
		desc.cameraFovAngleVertical = inputs.fov_y;
		desc.viewSpaceToMetersFactor = 1.0f;
		desc.flags = FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;

		const ffxReturnCode_t result = ffx_api().dispatch(&_set.context, &desc.header);

		const D3D12_RESOURCE_BARRIER to_rest[4] = {
			transition(_set.motion.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
			transition(_set.depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
			transition(_set.reactive.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
			transition(_set.output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE) };
		list->ResourceBarrier(barrier_count, to_rest);

		if (result != FFX_API_RETURN_OK)
		{
			if (_error.empty() || _dispatches != 0)
				log::warn("FSR: ffxDispatch failed with code %u", result);
			_error = "ffxDispatch failed with code " + std::to_string(result);
			_dispatches = 0;
			_reset = true;
			return false;
		}

		if (_dispatches++ == 0)
			log::write("FSR: first dispatch recorded (render %ux%u, jitter %+.4f %+.4f, near %g far %g, fov %.2f deg, %.1f ms, reactive mask %s, output %s, DXGI format %u)", inputs.width, inputs.height,
				static_cast<double>(inputs.jitter_x), static_cast<double>(inputs.jitter_y), static_cast<double>(desc.cameraNear), static_cast<double>(desc.cameraFar),
				static_cast<double>(inputs.fov_y) * 57.29578, static_cast<double>(delta_ms), use_mask ? "from t3" : "not used",
				inputs.output != nullptr ? "written straight into the game's texture" : "the add-on's own texture",
				static_cast<unsigned int>((inputs.output != nullptr ? inputs.output : _set.output.Get())->GetDesc().Format));
		_reset = false;
		_error.clear();
		return true;
	}

	ID3D12Resource *Fsr::output()
	{
		const std::lock_guard<std::mutex> lock(_mutex);
		return _set.output.Get();
	}

	void Fsr::reset_history()
	{
		const std::lock_guard<std::mutex> lock(_mutex);
		_reset = true;
	}

	uint32_t Fsr::debug_textures(DebugTexture (&out)[4])
	{
		const std::lock_guard<std::mutex> lock(_mutex);
		if (_set.output == nullptr)
			return 0;
		out[0] = { _set.motion.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS };
		out[1] = { _set.depth.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS };
		out[2] = { _set.output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE };
		out[3] = { _set.reactive.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS };
		return 4;
	}

	void Fsr::on_present(api::command_queue *queue, uint64_t frame)
	{
		const std::lock_guard<std::mutex> lock(_mutex);
		if (_timer_frequency == 0 && _timer_heap != nullptr && queue != nullptr)
			reinterpret_cast<ID3D12CommandQueue *>(static_cast<uintptr_t>(queue->get_native()))->GetTimestampFrequency(&_timer_frequency);
		if (_retired.empty() || queue == nullptr || frame < _retired.back().retired_frame + 8)
			return;

		// Every list that used the retired objects was submitted frames ago; make sure the GPU has run them
		queue->wait_idle();
		log::write("FSR: %u replaced context(s) %s", static_cast<unsigned int>(_retired.size()), keep_contexts() ? "dropped without destroying them" : "destroyed");
		for (Set &set : _retired)
			destroy_set(set);
		_retired.clear();
	}

	void Fsr::shutdown()
	{
		const std::lock_guard<std::mutex> lock(_mutex);
		if (_set.context != nullptr)
			log::write("FSR: context for %ux%u -> %ux%u destroyed after %llu frames", _set.width, _set.height, _set.out_width, _set.out_height, _dispatches);
		for (Set &set : _retired)
			destroy_set(set);
		_retired.clear();
		destroy_set(_set);
		_reset = true;
		_dispatches = 0;
		_pipeline.Reset();
		_root_signature.Reset();
		_heap.Reset();
		_timer_open = false;
		_timer_data = nullptr;
		_timer_readback.Reset();
		_timer_heap.Reset();
	}

	std::string Fsr::status()
	{
		const std::lock_guard<std::mutex> lock(_mutex);
		std::string text;
		if (!_error.empty())
			append_format(text, "error: %s", _error.c_str());
		else if (_set.context != nullptr)
		{
			append_format(text, "%s, %ux%u -> %ux%u, %llu frames", _provider.c_str(), _set.width, _set.height, _set.out_width, _set.out_height, _dispatches);
			if (_timer_ms > 0.0)
				append_format(text, ", GPU %.2f ms", _timer_ms);
		}
		else
			text = "not started";
		return text;
	}
}
