#pragma once

#include "deunify/physicalMemory.hpp"
#include "deunify/types.hpp"

#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace deunify {

class Context;

// Shared with the C ABI.
enum class PageRunKind : uint32_t {
	Consecutive = 0, // pool pages from poolPage on, one each
	Repeated    = 1, // poolPage, count times
	Unmapped    = 2,
	Unchanged   = 3,
};

struct PageRun {
	PageRunKind kind     = PageRunKind::Unchanged;
	uint32_t    poolPage = 0; // index into the pool, not a page number
	uint32_t    count    = 0;
};

struct PageBatch {
	uint64_t                 gpuAddress = 0;
	std::span<const PageRun> runs;
};

// What the GPU sees, one 64 KiB slot at a time. There are two kinds of range:
//
// - followCpuViews: the GPU sees whatever the CPU sees at the same address. PhysicalMemory
//   keeps these slots up to date as it maps.
// - addGpuRange: only explicit mappings fill it. On the console the title changes these through
//   the queue, so call copyPageMappings at the same point of your submission order.
//
// With a card view a slot just points at its page's tile and no RAM view is made. Without one,
// explicit mappings become PhysicalMemory views at the GPU addresses, which you must have
// reserved as placeholders; addGpuRange doesn't do it for you.
class GpuPageTable {
public:
	GpuPageTable(const GpuPageTable&)            = delete;
	GpuPageTable& operator=(const GpuPageTable&) = delete;
	~GpuPageTable();

	// Views already mapped in the range are picked up at once. Ranges may not overlap.
	Status followCpuViews(uint64_t base, uint64_t bytes);
	Status addGpuRange(uint64_t base, uint64_t bytes);

	// Pool page i must be mapped at `poolAddress + i * kPageBytes`; that is how we learn its
	// number. A run naming an unmapped pool page fails with UnknownPage, the other runs still
	// apply, and the first failure is returned.
	Status copyPageMappings(std::span<const PageBatch> batches, uint64_t poolAddress,
	                        uint32_t poolPages);

	Status map(uint64_t gpuAddress, std::span<const PageNumber> pages);
	Status unmap(uint64_t gpuAddress, uint64_t bytes);

	std::optional<PageAt> pageAt(uint64_t gpuAddress) const;
	void                  slotsOf(PageNumber page, std::vector<uint64_t>& out) const;

	// Reads from each page's home without moving it; the one exception is File becoming
	// Texture for a TextureUpload. A Card page waits for the queue. Use this, not the title
	// address, whenever the emulator wants bytes for itself. Unmapped slots read as zero and
	// make the call return UnknownPage.
	Status read(uint64_t gpuAddress, void* out, uint64_t bytes, ReadPurpose purpose);

private:
	friend class Context;
	friend class PhysicalMemory;
	friend class CardView;

	explicit GpuPageTable(Context& context);

	void cpuViewMapped(const View& view);
	void cpuViewUnmapped(uint64_t address, uint64_t bytes);

	struct Impl;
	Context&              m_context;
	std::unique_ptr<Impl> m_impl;
};

} // namespace deunify
