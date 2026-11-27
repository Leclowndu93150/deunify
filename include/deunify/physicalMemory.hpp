#pragma once

#include "deunify/types.hpp"

#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace deunify {

class Context;

struct PageAt {
	PageNumber page   = kNoPage;
	uint64_t   offset = 0;
};

// `bytes` at `address` showing consecutive pages from `firstPage` on.
struct View {
	uint64_t   address   = 0;
	uint64_t   bytes     = 0;
	PageNumber firstPage = kNoPage;
};

// Physical pages and the addresses the title sees them at.
//
// Each allocate() is one pagefile-backed SEC_RESERVE section, so allocating gigabytes costs no
// commit charge. Views go into placeholders the emulator reserved (VirtualAlloc2 with
// MEM_RESERVE_PLACEHOLDER); we split them as needed. A page can be shown at any number of
// addresses. Mapping never commits: Pages commits a page when it comes home to RAM, and every
// view sees it then.
//
// map() and unmap() tell Pages and GpuPageTable about the new views before they return.
// Mapping calls are serialized (lock 1), but pageAt() and viewsOf() never wait for one in
// progress, so the fault handler can use them while another thread maps.
class PhysicalMemory {
public:
	PhysicalMemory(const PhysicalMemory&)            = delete;
	PhysicalMemory& operator=(const PhysicalMemory&) = delete;
	~PhysicalMemory();

	// Hands out the lowest free numbers, so a title that frees and reallocates gets them back
	// packed. `flags` is stored, not interpreted; reject flags you don't understand before
	// calling. New pages are Zero home.
	Status allocate(uint64_t count, uint64_t flags, PageNumber* pages);

	// A freed page stays readable where it is still mapped. All or nothing: one unknown number
	// and nothing is freed.
	Status free(std::span<const PageNumber> pages);

	// One page per kPageBytes from `address` (64 KiB aligned), replacing our views there.
	// kNoPage leaves a hole. Consecutive pages of one section become one host view. A run whose
	// target is neither placeholder nor ours fails with NotPlaceholder and is left alone.
	Status map(uint64_t address, std::span<const PageNumber> pages);

	// addresses[i] shows pages[i], applied in order.
	Status mapScatter(std::span<const uint64_t> addresses, std::span<const PageNumber> pages);

	// Back to placeholders. A view sticking out at either end is cut and its outer part
	// remapped.
	Status unmap(uint64_t address, uint64_t bytes);

	std::optional<PageAt> pageAt(uint64_t address) const;

	// These append.
	void viewsOf(PageNumber page, std::vector<uint64_t>& out) const;
	void viewsIn(uint64_t address, uint64_t bytes, std::vector<View>& out) const;

	bool allocated(PageNumber page) const;
	bool committed(PageNumber page) const;

	struct Totals {
		uint64_t pages     = 0;
		uint64_t committed = 0;
		uint64_t sections  = 0;
	};
	Totals totals() const;

private:
	friend class Context;
	friend class GpuPageTable;
	friend class Pages;
	friend class Loads;
	friend class CardView;

	explicit PhysicalMemory(Context& context);

	// The page through our own view of its section, never a title address. With `commit`
	// false, an uncommitted page gives null instead of being committed: readers use that.
	uint8_t* bytesOf(PageNumber page, bool commit);

	bool commit(PageNumber page);

	// MEM_RESET and VirtualUnlock through our view: contents become undefined, commit stays.
	// Pages calls it when a page leaves RAM.
	void reset(PageNumber page);

	// Views of uncommitted pages are left alone; they fault anyway. Arming (PAGE_READONLY)
	// never opens a closed view: that view keeps its protection and we return false.
	bool protectViews(PageNumber page, uint32_t protection);
	bool protectView(uint64_t address, uint32_t protection);

	// Card view only: explicit GPU slots pin the page. A freed page is forgotten (and its
	// number reused) once it has no views and no held slots.
	bool holdPage(PageNumber page);
	void releasePage(PageNumber page);

	// Bumped at the start and at the end of every view change, so it is odd while one is
	// under way. See Pages::handleFault for how faults use it.
	uint64_t remapGeneration() const;

	struct Impl;
	Context&              m_context;
	std::unique_ptr<Impl> m_impl;
};

} // namespace deunify
