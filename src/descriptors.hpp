// Turns "descriptor table bound at root parameter N, plus index i" into a resource (SRV/UAV) or a
// buffer range (CBV), and reads constant buffer contents.
//
// What ReShade 6.8.0 tracks itself and what this file adds:
//  - RTV/DSV handles: ReShade maps them to resources ('get_resource_from_view'), copies included.
//  - SRV/UAV: ReShade registers the view only for the CPU handle it was created at. Copies inside
//    CBV_SRV_UAV heaps ('CopyDescriptors', 'CopyDescriptorsSimple') are NOT followed, and a slot that is
//    later overwritten with a CBV keeps its old SRV registration.
//  - CBV: not tracked by ReShade at all, only reported through 'update_descriptor_tables'.
// So 'DescriptorShadow' mirrors the contents of every CBV_SRV_UAV heap, keyed by heap and index, fed
// by the 'update_descriptor_tables' and 'copy_descriptor_tables' events.
// 'ResourceTable' mirrors the resources that are alive, so that nothing read from the shadow is ever
// dereferenced unless it is known to exist.
#pragma once

#include "common.hpp"

#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace acv
{
	struct ResourceInfo
	{
		api::resource_type type = api::resource_type::unknown;
		api::memory_heap heap = api::memory_heap::unknown;
		uint64_t size = 0; // Buffers: size in bytes
		uint32_t width = 0, height = 0;
		api::format format = api::format::unknown;
	};

	enum class ReadResult : uint8_t
	{
		NotRead,        // No attempt was made
		Ok,
		NotCbv,         // The descriptor is not a CBV the shadow knows about
		NullBuffer,
		NotTracked,     // Buffer is not (or no longer) in the resource table
		NotBuffer,
		NotCpuReadable, // Buffer lives in a heap the CPU cannot map
		OutOfBounds,
		MapFailed
	};
	const char *read_result_name(ReadResult result) noexcept;

	// Resources that are currently alive ('init_resource' .. 'destroy_resource', plus swap chain buffers).
	class ResourceTable
	{
	public:
		void add(uint64_t resource, const api::resource_desc &desc);
		void remove(uint64_t resource);
		bool find(uint64_t resource, ResourceInfo &out) const;

		// Checks that the buffer is alive, CPU-readable and large enough, then maps it (read-only use), copies
		// 'size' bytes starting at 'offset' and unmaps again. Never writes to the buffer. The table lock is held
		// meanwhile, which keeps 'destroy_resource' for that buffer from completing.
		ReadResult read_buffer(uint64_t resource, uint64_t offset, void *dest, size_t size) const;

	private:
		mutable std::shared_mutex _mutex;
		std::unordered_map<uint64_t, ResourceInfo> _resources;
	};

	enum class DescType : uint8_t
	{
		Empty, // Never written, or written by a path that could not be followed
		Cbv,
		Srv,
		Uav,
		Other  // Acceleration structure, or a value that does not fit the packed slot
	};

	struct Descriptor
	{
		DescType type = DescType::Empty;
		uint64_t resource = 0; // Zero for a null descriptor
		uint64_t offset = 0;   // CBV only
		uint32_t size = 0;     // CBV only
	};

	enum class SlotStatus : uint8_t
	{
		Ok,
		HeapNotTracked, // Heap never seen in a descriptor write, or not a CBV_SRV_UAV heap
		OutOfRange
	};

	class DescriptorShadow
	{
	public:
		DescriptorShadow() = default;
		DescriptorShadow(const DescriptorShadow &) = delete;
		DescriptorShadow &operator=(const DescriptorShadow &) = delete;
		~DescriptorShadow();

		void on_update(api::device *device, const api::descriptor_table_update &update);
		void on_copy(api::device *device, const api::descriptor_table_copy &copy);

		SlotStatus read(uint64_t heap, uint32_t index, Descriptor &out) const noexcept;

	private:
		// 16 bytes per descriptor. Written with three plain stores (a = 0, b, a) and read with a
		// re-check of 'a', which yields consistent snapshots under x86-64's store ordering without a lock.
		//   a: bits 56..63 DescType, bits 0..55 resource pointer
		//   b: CBV: bits 0..39 offset, bits 40..63 size
		struct Slot
		{
			std::atomic<uint64_t> a;
			std::atomic<uint64_t> b;
		};
		struct Heap
		{
			uint64_t handle = 0;
			uint32_t count = 0;
			Slot *slots = nullptr; // Null for heaps that are not tracked (sampler heaps, allocation failure)
			bool alloc_failed = false; // The shadow could not be allocated: settled, do not retry on every write
		};

		static constexpr uint32_t kMaxHeaps = 128;
		static constexpr uint32_t kDiagLines = 8;

		const Heap *find(uint64_t handle) const noexcept;
		// Returns the shadow for a heap, creating it (or replacing a stale one) when 'last_index' does not fit.
		const Heap *acquire(uint64_t handle, uint64_t last_index, bool expect_tracked);

		static void store(Slot &slot, const Descriptor &desc) noexcept;
		static bool load(const Slot &slot, uint64_t &a, uint64_t &b) noexcept;

		std::atomic<const Heap *> _table[kMaxHeaps] = {};
		std::atomic<uint32_t> _table_size { 0 };
		std::mutex _create_mutex;
		std::vector<Heap *> _owned; // Every shadow ever created; released with the device
		std::atomic<uint32_t> _update_diag { 0 };
		std::atomic<uint32_t> _copy_diag { 0 };
	};

	// One descriptor of a bound table, resolved when the draw or dispatch was recorded.
	struct BoundDescriptor
	{
		SlotStatus status = SlotStatus::HeapNotTracked;
		Descriptor desc;
		bool resource_known = false; // 'info' is valid: the resource is alive and tracked
		ResourceInfo info;
	};

	inline constexpr uint32_t kTableDumpCount = 16;

	struct BoundTable
	{
		uint32_t param = 0;   // Root parameter index
		uint64_t table = 0;   // GPU descriptor handle the game bound
		uint64_t heap = 0;    // Zero when ReShade could not map the handle to a heap
		uint32_t base = 0;    // Index of the first descriptor in that heap
		BoundDescriptor descriptors[kTableDumpCount];
	};

	void resolve_table(api::device *device, const DescriptorShadow &shadow, const ResourceTable &resources, uint32_t param, uint64_t table, BoundTable &out);

	// Process-wide totals of CBV/SRV/UAV descriptors written and copied, for the overlay and the frame log.
	// Every thread batches 64 events before it touches the shared counter, so the totals lag slightly.
	uint64_t descriptors_written_total() noexcept;
	uint64_t descriptors_copied_total() noexcept;
}
