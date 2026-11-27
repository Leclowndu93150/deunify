#pragma once

#include "deunify/physicalMemory.hpp"
#include "deunify/types.hpp"

#include <memory>
#include <span>
#include <vector>

struct _EXCEPTION_POINTERS;

namespace deunify {

class Context;

struct PageState {
	Home     home      = Home::Zero;
	bool     tile      = false;
	bool     committed = false;
	bool     armed     = false; // Ram: views read-only so the next write faults
	bool     written   = false; // Ram: written since the last takeWritten
	uint32_t views     = 0;
};

// Current homes, then counters since start.
struct HomeCounts {
	uint64_t zero      = 0;
	uint64_t file      = 0;
	uint64_t texture   = 0;
	uint64_t card      = 0;
	uint64_t ram       = 0;
	uint64_t ramTiled  = 0; // held twice
	uint64_t parked    = 0;
	uint64_t faults    = 0;
	uint64_t readBacks = 0;
	uint64_t replays   = 0;
};

enum class ParkResult : uint32_t {
	Parked     = 0, // File home now, or already was
	Unknown    = 1,
	NoSource   = 2,
	Differs    = 3,
	Touched    = 4, // came back from File three times; we stop trying
	InUse      = 5, // a view is armed, bracketed or otherwise not plainly open
	NotWritten = 6, // not in RAM
};

// Homes, and the transfers between them.
//
// The rule everything here serves: a page has one home, and while that home isn't RAM every
// CPU view of it is closed (PAGE_NOACCESS or uncommitted). Any CPU access therefore faults
// into handleFault, which brings the page home first.
//
// The Pages lock (5) covers state and protection changes only. Read-backs, replays and decodes
// run without it; their bytes land only if the page hasn't changed meanwhile, otherwise the
// transfer starts over. Holding this lock across a wait for the card stalls the submitting
// thread for the whole wait.
class Pages {
public:
	Pages(const Pages&)            = delete;
	Pages& operator=(const Pages&) = delete;
	~Pages();

	// Later calls win where ranges overlap. Without a declaration, memory counts as
	// CPU-reachable and loads go to RAM: correct, and saves nothing.
	void setKind(uint64_t address, uint64_t bytes, Kind kind);
	Kind kindAt(uint64_t address) const;

	PageState  state(PageNumber page) const;
	HomeCounts counts() const;

	// Call this first from your vectored handler, for every access violation, including ones
	// your own code thinks it already explained. True means it was ours: return
	// EXCEPTION_CONTINUE_EXECUTION.
	//
	// Runs on the faulting title thread and may wait for the card (Card page) or decode (File
	// page), so never call it while holding a lock that some deunify call on another thread
	// could be waiting for.
	//
	// A fault can land while another thread is changing views and the address is briefly
	// unmapped. We read PhysicalMemory::remapGeneration before and after trying; a fault is
	// handed back as not ours only if the generation was even and unchanged across the
	// attempt. Otherwise we wait, with no lock held, until it is even (at most 10 s) and try
	// again.
	bool handleFault(_EXCEPTION_POINTERS* exception);

	// Appends the pages written since the last call and arms them again, so the next write is
	// seen. Pages under a write bracket stay open and are listed again when it ends.
	// beforeSubmit calls this; without a card view, call it yourself for your uploads. One
	// caller at a time.
	//
	// Arming order, under the Pages lock: the armed bit is set before any view goes read-only
	// and cleared only after every view is writable again. A write fault that finds a
	// read-only view always finds the bit set.
	void takeWritten(std::vector<PageNumber>& out);

	// Give a Ram page's memory back if its records rebuild it byte for byte. For emulators that
	// learn the CPU has stopped looking at a page.
	ParkResult park(PageNumber page);

	// Reads from each page's home without committing or opening anything. Prefer this to a
	// read at the title address, which would drag the page into RAM.
	Status readTitle(uint64_t address, void* out, uint64_t bytes, ReadPurpose purpose);

private:
	friend class Context;
	friend class PhysicalMemory;
	friend class GpuPageTable;
	friend class Loads;
	friend class CardView;
	friend class KernelWrite;

	explicit Pages(Context& context);

	void pagesAllocated(std::span<const PageNumber> pages);
	void pageFreed(PageNumber page);
	// Freed and the last view gone: drop everything, release the tile and records.
	void pageForgotten(PageNumber page);

	// Called by PhysicalMemory after mapping, before it returns: close the view unless the page
	// is Ram home, arm it if the page is armed.
	void viewMapped(const View& view);
	void viewUnmapping(uint64_t address, uint64_t bytes);

	enum class Placement : uint8_t {
		Ram,  // written into the page, listed as written
		File, // recorded and staged; views closed, RAM reset
		Card, // queued for the tile at the next beforeSubmit
	};
	// Asked once per destination page before a load writes anything; the answer is final.
	// File needs all of: no view in Unknown or CpuOnly memory, no write bracket, records that
	// rebuild the page, and either a whole-page load or earlier bytes that are themselves
	// rebuildable (Zero, File, Texture). A page with no views qualifies.
	Placement placeLoad(PageNumber page, bool whole);

	// Tile fill: the page's bytes from wherever they are, home unchanged. False if lost.
	bool bytesForTile(PageNumber page, uint8_t* out);

	// beforeSubmit, for pages shaders may write. Views are closed first, then the bytes are
	// taken, then RAM is reset. In the other order a CPU write between the copy and the close
	// is lost. False if it already was Card home.
	bool becomeCardHome(PageNumber page, uint8_t* out);

	// An orphaned Card page's bytes came back from the tile. Ignored if the page is no longer
	// Card home.
	void cardHomeLost(PageNumber page, const uint8_t* bytes);

	void tileChanged(PageNumber page, bool tile);

	Status readPage(PageNumber page, uint64_t offset, void* out, uint64_t bytes,
	                ReadPurpose purpose);

	Status beginKernelAccess(uint64_t address, uint64_t bytes, bool writes, uint64_t& ticket);
	void   endKernelAccess(uint64_t ticket);

	struct Impl;
	Context&              m_context;
	std::unique_ptr<Impl> m_impl;
};

// Wrap every host kernel call that reads or writes title memory: ReadFile into a title
// buffer, WriteFile from one, a socket receive. The kernel doesn't fault on a closed page, it
// fails, and the title gets the failure.
//
// While the bracket lives, every page in the range is Ram home and readable; a write bracket
// also keeps it writable and unarmed, and lists the pages as written when it ends. Create it
// right before the call and destroy it right after, on the same thread. Brackets may nest and
// overlap.
class KernelWrite {
public:
	enum class Access : uint8_t { Read, Write };

	KernelWrite(Pages& pages, uint64_t address, uint64_t bytes, Access access = Access::Write);
	~KernelWrite();
	KernelWrite(const KernelWrite&)            = delete;
	KernelWrite& operator=(const KernelWrite&) = delete;

	// Anything but Ok (usually NoSource) means: don't make the kernel call.
	Status status() const { return m_status; }

private:
	Pages*   m_pages  = nullptr;
	uint64_t m_ticket = 0;
	Status   m_status = Status::Ok;
};

} // namespace deunify
