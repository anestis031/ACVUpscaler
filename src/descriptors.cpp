#include "descriptors.hpp"

#include <d3d12.h>
#include <memory>

namespace acv
{
	const char *read_result_name(ReadResult result) noexcept
	{
		switch (result)
		{
		case ReadResult::NotRead: return "not read";
		case ReadResult::Ok: return "ok";
		case ReadResult::NotCbv: return "descriptor is not a tracked CBV";
		case ReadResult::NullBuffer: return "null buffer";
		case ReadResult::NotTracked: return "buffer not tracked (destroyed or never seen)";
		case ReadResult::NotBuffer: return "resource is not a buffer";
		case ReadResult::NotCpuReadable: return "buffer is not in a CPU-accessible heap";
		case ReadResult::OutOfBounds: return "range outside of the buffer or the view";
		case ReadResult::MapFailed: return "Map failed";
		}
		return "?";
	}

	// ---- ResourceTable --------------------------------------------------------------------------

	void ResourceTable::add(uint64_t resource, const api::resource_desc &desc)
	{
		if (resource == 0)
			return;

		ResourceInfo info;
		info.type = desc.type;
		info.heap = desc.heap;
		if (desc.type == api::resource_type::buffer)
		{
			info.size = desc.buffer.size;
		}
		else
		{
			info.width = desc.texture.width;
			info.height = desc.texture.height;
			info.format = desc.texture.format;
		}

		const std::unique_lock<std::shared_mutex> lock(_mutex);
		_resources.insert_or_assign(resource, info);
	}
	void ResourceTable::remove(uint64_t resource)
	{
		const std::unique_lock<std::shared_mutex> lock(_mutex);
		_resources.erase(resource);
	}
	bool ResourceTable::find(uint64_t resource, ResourceInfo &out) const
	{
		if (resource == 0)
			return false;

		const std::shared_lock<std::shared_mutex> lock(_mutex);
		const auto it = _resources.find(resource);
		if (it == _resources.end())
			return false;
		out = it->second;
		return true;
	}

	ReadResult ResourceTable::read_buffer(uint64_t resource, uint64_t offset, void *dest, size_t size) const
	{
		if (resource == 0)
			return ReadResult::NullBuffer;

		// The lock is held until the buffer is unmapped again: 'remove' runs in the destruction callback of a
		// resource, before its memory is released, so a buffer found here cannot go away while it is being read.
		// (Verified with a stress test: mapping without the lock loses the device when a stale descriptor points
		// at an address where buffers are created and destroyed at a high rate.)
		const std::shared_lock<std::shared_mutex> lock(_mutex);

		const auto it = _resources.find(resource);
		if (it == _resources.end())
			return ReadResult::NotTracked;
		const ResourceInfo &info = it->second;
		if (info.type != api::resource_type::buffer)
			return ReadResult::NotBuffer;
		if (info.heap == api::memory_heap::default_ || info.heap == api::memory_heap::unknown)
			return ReadResult::NotCpuReadable;
		if (offset > info.size || size > info.size - offset)
			return ReadResult::OutOfBounds;

		const auto native = reinterpret_cast<ID3D12Resource *>(static_cast<uintptr_t>(resource));

		// Map is reference counted, so this does not disturb a persistent mapping held by the game
		const D3D12_RANGE read_range = { static_cast<SIZE_T>(offset), static_cast<SIZE_T>(offset + size) };
		void *data = nullptr;
		if (FAILED(native->Map(0, &read_range, &data)) || data == nullptr)
			return ReadResult::MapFailed;

		std::memcpy(dest, static_cast<const uint8_t *>(data) + offset, size);

		const D3D12_RANGE written_range = { 0, 0 }; // Nothing was written
		native->Unmap(0, &written_range);

		return ReadResult::Ok;
	}

	// ---- DescriptorShadow -----------------------------------------------------------------------

	// Counting on a shared atomic for every descriptor would make the recording threads fight over one
	// cache line, so each thread collects 64 events first.
	static std::atomic<uint64_t> s_written_total { 0 }, s_copied_total { 0 };
	static thread_local uint32_t t_written_pending = 0, t_copied_pending = 0;

	static inline void count_descriptors(std::atomic<uint64_t> &total, uint32_t &pending, uint32_t count) noexcept
	{
		pending += count;
		if (pending >= 64)
		{
			total.fetch_add(pending, std::memory_order_relaxed);
			pending = 0;
		}
	}

	uint64_t descriptors_written_total() noexcept { return s_written_total.load(std::memory_order_relaxed); }
	uint64_t descriptors_copied_total() noexcept { return s_copied_total.load(std::memory_order_relaxed); }

	static constexpr uint64_t kResourceMask = (1ull << 56) - 1;
	static constexpr uint64_t kCbvOffsetLimit = 1ull << 40;
	static constexpr uint32_t kCbvSizeLimit = 1u << 24;

	static const char *desc_type_name(DescType type) noexcept
	{
		switch (type)
		{
		case DescType::Empty: return "empty";
		case DescType::Cbv: return "cbv";
		case DescType::Srv: return "srv";
		case DescType::Uav: return "uav";
		case DescType::Other: return "other";
		}
		return "?";
	}

	DescriptorShadow::~DescriptorShadow()
	{
		for (Heap *const heap : _owned)
		{
			if (heap->slots != nullptr)
				VirtualFree(heap->slots, 0, MEM_RELEASE);
			delete heap;
		}
	}

	void DescriptorShadow::store(Slot &slot, const Descriptor &desc) noexcept
	{
		DescType type = desc.type;
		uint64_t resource = desc.resource, b = 0;

		if ((resource & ~kResourceMask) != 0)
		{
			type = DescType::Other;
			resource = 0;
		}
		else if (type == DescType::Cbv)
		{
			if (desc.offset < kCbvOffsetLimit && desc.size < kCbvSizeLimit)
				b = desc.offset | (static_cast<uint64_t>(desc.size) << 40);
			else
				type = DescType::Other, resource = 0;
		}

		slot.a.store(0, std::memory_order_release);
		slot.b.store(b, std::memory_order_release);
		slot.a.store((static_cast<uint64_t>(type) << 56) | resource, std::memory_order_release);
	}
	bool DescriptorShadow::load(const Slot &slot, uint64_t &a, uint64_t &b) noexcept
	{
		for (int attempt = 0; attempt < 4; ++attempt)
		{
			a = slot.a.load(std::memory_order_acquire);
			if (a == 0)
			{
				b = 0; // Empty, or being rewritten at this very moment
				return true;
			}
			b = slot.b.load(std::memory_order_acquire);
			if (slot.a.load(std::memory_order_acquire) == a)
				return true;
		}
		a = b = 0;
		return false;
	}

	const DescriptorShadow::Heap *DescriptorShadow::find(uint64_t handle) const noexcept
	{
		const uint32_t count = _table_size.load(std::memory_order_acquire);
		for (uint32_t i = 0; i < count; ++i)
		{
			const Heap *const heap = _table[i].load(std::memory_order_acquire);
			if (heap != nullptr && heap->handle == handle)
				return heap;
		}
		return nullptr;
	}

	const DescriptorShadow::Heap *DescriptorShadow::acquire(uint64_t handle, uint64_t last_index, bool expect_tracked)
	{
		const auto fits = [last_index, expect_tracked](const Heap *heap) {
			return heap != nullptr && (heap->slots != nullptr ? last_index < heap->count : (!expect_tracked || heap->alloc_failed));
		};

		const Heap *heap = find(handle);
		if (fits(heap))
			return heap;

		// The table is full and this heap is not in it: nothing can be learned by taking the slow path again.
		// Without this, every descriptor write to such a heap would serialize on the mutex below.
		if (heap == nullptr && _table_size.load(std::memory_order_acquire) >= kMaxHeaps)
			return nullptr;

		const std::lock_guard<std::mutex> lock(_create_mutex);

		heap = find(handle);
		if (fits(heap))
			return heap;

		// The heap is new, or what is known about it contradicts the write that is happening right now
		// (a heap was destroyed and another one created at the same address). The heap is alive: the game
		// is writing to it in this very call.
		const auto native = reinterpret_cast<ID3D12DescriptorHeap *>(static_cast<uintptr_t>(handle));
		const D3D12_DESCRIPTOR_HEAP_DESC desc = native->GetDesc();
		const bool track = desc.Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV && desc.NumDescriptors != 0;

		if (heap != nullptr && heap->slots != nullptr && track && heap->count == desc.NumDescriptors)
			return heap; // Nothing changed, the index is simply out of range
		if (heap != nullptr && heap->slots == nullptr && !track)
			return heap;

		const uint32_t table_size = _table_size.load(std::memory_order_relaxed);
		uint32_t index = table_size;
		for (uint32_t i = 0; i < table_size; ++i)
		{
			if (_table[i].load(std::memory_order_relaxed)->handle == handle)
			{
				index = i;
				break;
			}
		}
		if (index >= kMaxHeaps)
		{
			static std::atomic<bool> s_warned { false };
			if (!s_warned.exchange(true))
				log::warn("more than %u descriptor heaps; further heaps are not tracked", kMaxHeaps);
			return nullptr;
		}

		auto fresh = std::make_unique<Heap>();
		fresh->handle = handle;
		_owned.reserve(_owned.size() + 1); // So that nothing below can fail once the memory is allocated
		if (track)
		{
			// Zero-initialized and only backed by physical memory once touched
			fresh->slots = static_cast<Slot *>(VirtualAlloc(nullptr, static_cast<SIZE_T>(desc.NumDescriptors) * sizeof(Slot), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
			if (fresh->slots != nullptr)
				fresh->count = desc.NumDescriptors;
			else
			{
				fresh->alloc_failed = true;
				log::warn("could not allocate the shadow for descriptor heap 0x%llX (%u descriptors); its contents will not be resolved", handle, desc.NumDescriptors);
			}
		}

		_owned.push_back(fresh.get());
		Heap *const result = fresh.release();

		// A replaced shadow stays in '_owned' until the device goes away, since another thread may still be using it
		_table[index].store(result, std::memory_order_release);
		if (index == table_size)
			_table_size.store(table_size + 1, std::memory_order_release);

		log::write("descriptor heap 0x%llX: type %d, %u descriptors, flags 0x%X -> %s%s",
			handle, static_cast<int>(desc.Type), desc.NumDescriptors, static_cast<unsigned int>(desc.Flags),
			result->slots != nullptr ? "tracked" : "not tracked",
			heap != nullptr ? " (replaces an earlier heap at the same address)" : "");

		return result;
	}

	void DescriptorShadow::on_update(api::device *device, const api::descriptor_table_update &update)
	{
		if (update.count == 0 || update.descriptors == nullptr || update.table.handle == 0)
			return;

		Descriptor desc;
		switch (update.type)
		{
		case api::descriptor_type::constant_buffer:
			desc.type = DescType::Cbv;
			break;
		case api::descriptor_type::texture_shader_resource_view:
		case api::descriptor_type::buffer_shader_resource_view:
			desc.type = DescType::Srv;
			break;
		case api::descriptor_type::texture_unordered_access_view:
		case api::descriptor_type::buffer_unordered_access_view:
			desc.type = DescType::Uav;
			break;
		case api::descriptor_type::acceleration_structure:
			desc.type = DescType::Other;
			break;
		default: // Samplers live in their own heaps and are of no interest
			return;
		}

		count_descriptors(s_written_total, t_written_pending, update.count);

		api::descriptor_heap heap_handle = { 0 };
		uint32_t base = 0;
		device->get_descriptor_heap_offset(update.table, update.binding, 0, &heap_handle, &base);
		if (heap_handle.handle == 0)
			return;

		const Heap *const heap = acquire(heap_handle.handle, static_cast<uint64_t>(base) + update.count - 1, true);
		if (heap == nullptr || heap->slots == nullptr)
			return;

		for (uint32_t i = 0; i < update.count && static_cast<uint64_t>(base) + i < heap->count; ++i)
		{
			desc.resource = 0;
			desc.offset = 0;
			desc.size = 0;

			if (desc.type == DescType::Cbv)
			{
				const api::buffer_range &range = static_cast<const api::buffer_range *>(update.descriptors)[i];
				desc.resource = range.buffer.handle;
				desc.offset = range.offset;
				desc.size = range.size > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(range.size);
			}
			else if (desc.type != DescType::Other)
			{
				// ReShade registered the view for this very CPU handle just before raising the event
				const api::resource_view view = static_cast<const api::resource_view *>(update.descriptors)[i];
				if (view.handle != 0)
					desc.resource = device->get_resource_from_view(view).handle;
			}

			store(heap->slots[base + i], desc);

			if (_update_diag.load(std::memory_order_relaxed) < kDiagLines)
			{
				if (const uint32_t n = _update_diag.fetch_add(1, std::memory_order_relaxed); n < kDiagLines)
					log::write("descriptor write #%u: heap 0x%llX index %u = %s resource 0x%llX offset %llu size %u",
						n, heap->handle, base + i, desc_type_name(desc.type), desc.resource, desc.offset, desc.size);
			}
		}
	}

	void DescriptorShadow::on_copy(api::device *device, const api::descriptor_table_copy &copy)
	{
		if (copy.count == 0 || copy.dest_table.handle == 0 || copy.source_table.handle == 0)
			return;

		api::descriptor_heap dest_handle = { 0 }, source_handle = { 0 };
		uint32_t dest_base = 0, source_base = 0;
		device->get_descriptor_heap_offset(copy.dest_table, copy.dest_binding, 0, &dest_handle, &dest_base);
		if (dest_handle.handle == 0)
			return;
		device->get_descriptor_heap_offset(copy.source_table, copy.source_binding, 0, &source_handle, &source_base);

		// The heap type is not part of the event, so a sampler heap gets an untracked placeholder on first sight
		const Heap *const dest = acquire(dest_handle.handle, static_cast<uint64_t>(dest_base) + copy.count - 1, false);
		if (dest == nullptr || dest->slots == nullptr)
			return;
		const Heap *const source = source_handle.handle != 0 ? acquire(source_handle.handle, static_cast<uint64_t>(source_base) + copy.count - 1, false) : nullptr;

		count_descriptors(s_copied_total, t_copied_pending, copy.count);

		for (uint32_t i = 0; i < copy.count && static_cast<uint64_t>(dest_base) + i < dest->count; ++i)
		{
			uint64_t a = 0, b = 0; // An unknown source leaves the destination marked as empty
			if (source != nullptr && source->slots != nullptr && static_cast<uint64_t>(source_base) + i < source->count)
				load(source->slots[source_base + i], a, b);

			Slot &slot = dest->slots[dest_base + i];
			slot.a.store(0, std::memory_order_release);
			slot.b.store(b, std::memory_order_release);
			slot.a.store(a, std::memory_order_release);
		}

		if (_copy_diag.load(std::memory_order_relaxed) < kDiagLines)
		{
			if (const uint32_t n = _copy_diag.fetch_add(1, std::memory_order_relaxed); n < kDiagLines)
				log::write("descriptor copy #%u: heap 0x%llX index %u <- heap 0x%llX index %u, %u descriptors%s",
					n, dest_handle.handle, dest_base, source_handle.handle, source_base, copy.count,
					source != nullptr && source->slots != nullptr ? "" : " (source heap not tracked)");
		}
	}

	SlotStatus DescriptorShadow::read(uint64_t heap_handle, uint32_t index, Descriptor &out) const noexcept
	{
		out = Descriptor();

		const Heap *const heap = find(heap_handle);
		if (heap == nullptr || heap->slots == nullptr)
			return SlotStatus::HeapNotTracked;
		if (index >= heap->count)
			return SlotStatus::OutOfRange;

		uint64_t a = 0, b = 0;
		load(heap->slots[index], a, b);

		out.type = static_cast<DescType>(a >> 56);
		out.resource = a & kResourceMask;
		if (out.type == DescType::Cbv)
		{
			out.offset = b & (kCbvOffsetLimit - 1);
			out.size = static_cast<uint32_t>(b >> 40);
		}
		return SlotStatus::Ok;
	}

	// ---- Resolution of a bound table ------------------------------------------------------------

	void resolve_table(api::device *device, const DescriptorShadow &shadow, const ResourceTable &resources, uint32_t param, uint64_t table, BoundTable &out)
	{
		out = BoundTable();
		out.param = param;
		out.table = table;
		if (table == 0)
			return;

		// GPU handle -> heap and index: ReShade keeps the GPU address range of every descriptor heap
		api::descriptor_heap heap = { 0 };
		uint32_t base = 0;
		device->get_descriptor_heap_offset(api::descriptor_table { table }, 0, 0, &heap, &base);
		if (heap.handle == 0)
			return;
		out.heap = heap.handle;
		out.base = base;

		for (uint32_t i = 0; i < kTableDumpCount; ++i)
		{
			BoundDescriptor &bound = out.descriptors[i];
			if (base > UINT32_MAX - i)
			{
				bound.status = SlotStatus::OutOfRange;
				continue;
			}
			bound.status = shadow.read(heap.handle, base + i, bound.desc);
			if (bound.status == SlotStatus::Ok && bound.desc.resource != 0)
				bound.resource_known = resources.find(bound.desc.resource, bound.info);
		}
	}
}
