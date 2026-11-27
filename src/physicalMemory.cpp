#include "deunify/physicalMemory.hpp"

#include "deunify/context.hpp"
#include "deunify/gpuPageTable.hpp"
#include "deunify/pages.hpp"
#include "hostError.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

namespace deunify {

namespace {

constexpr uint64_t kShortViewPages = 64;
constexpr uint64_t kBitsPerWord    = 64;
constexpr uint64_t kFirstNumber    = kFramesPerPage;
constexpr uint64_t kNumberCount    = kPageNumberLimit / kFramesPerPage - 1;

uint64_t numberAt(PageNumber first, uint64_t pages) {
	return value(first) + pages * kFramesPerPage;
}

uint64_t pagesBetween(uint64_t from, uint64_t to) {
	return (to - from) / kPageBytes;
}

bool pageAligned(uint64_t value) {
	return value % kPageBytes == 0;
}

void* pointer(uint64_t address) {
	return reinterpret_cast<void*>(address);
}

bool recordHostError() {
	detail::setLastHostError(GetLastError());
	return false;
}

class Section {
public:
	Section(HANDLE handle, uint64_t pages, uint64_t flags, std::atomic<uint64_t>& committedPages,
	        std::atomic<uint64_t>& sections)
	    : flags(flags), m_handle(handle), m_pages(pages),
	      m_committed(new std::atomic<uint64_t>[(pages + kBitsPerWord - 1) / kBitsPerWord]()),
	      m_committedPages(committedPages), m_sections(sections) {
		m_sections.fetch_add(1, std::memory_order_relaxed);
	}

	~Section() {
		if (uint8_t* base = m_window.load(std::memory_order_acquire)) {
			UnmapViewOfFile(base);
		}
		m_committedPages.fetch_sub(committedCount(), std::memory_order_relaxed);
		m_sections.fetch_sub(1, std::memory_order_relaxed);
		CloseHandle(m_handle);
	}

	Section(const Section&)            = delete;
	Section& operator=(const Section&) = delete;

	const uint64_t flags;

	HANDLE handle() const { return m_handle; }

	uint8_t* window() {
		if (uint8_t* base = m_window.load(std::memory_order_acquire)) {
			return base;
		}
		std::lock_guard lock(m_windowMutex);
		if (m_window.load(std::memory_order_relaxed) == nullptr) {
			auto* base = static_cast<uint8_t*>(
			    MapViewOfFile(m_handle, FILE_MAP_WRITE, 0, 0, m_pages * kPageBytes));
			if (base == nullptr) {
				recordHostError();
			}
			m_window.store(base, std::memory_order_release);
		}
		return m_window.load(std::memory_order_relaxed);
	}

	uint8_t* pageBytes(uint64_t index) {
		uint8_t* base = window();
		return base == nullptr ? nullptr : base + index * kPageBytes;
	}

	bool committed(uint64_t index) const {
		return (word(index).load(std::memory_order_acquire) & bit(index)) != 0;
	}

	bool commit(uint64_t index) {
		if (committed(index)) {
			return true;
		}
		uint8_t* bytes = pageBytes(index);
		if (bytes == nullptr || VirtualAlloc(bytes, kPageBytes, MEM_COMMIT, PAGE_READWRITE) == nullptr) {
			return recordHostError();
		}
		if ((word(index).fetch_or(bit(index), std::memory_order_acq_rel) & bit(index)) == 0) {
			m_committedPages.fetch_add(1, std::memory_order_relaxed);
		}
		return true;
	}

private:
	std::atomic<uint64_t>& word(uint64_t index) const { return m_committed[index / kBitsPerWord]; }

	static uint64_t bit(uint64_t index) { return 1ull << (index % kBitsPerWord); }

	uint64_t committedCount() const {
		uint64_t count = 0;
		for (uint64_t index = 0; index < m_pages; ++index) {
			count += committed(index) ? 1 : 0;
		}
		return count;
	}

	HANDLE                                   m_handle;
	uint64_t                                 m_pages;
	std::mutex                               m_windowMutex;
	std::atomic<uint8_t*>                    m_window {nullptr};
	std::unique_ptr<std::atomic<uint64_t>[]> m_committed;
	std::atomic<uint64_t>&                   m_committedPages;
	std::atomic<uint64_t>&                   m_sections;
};

struct Slice {
	std::shared_ptr<Section> section;
	uint64_t                 index = 0;
};

struct Entry {
	Slice    slice;
	bool     freed = false;
	uint32_t views = 0;
	uint32_t holds = 0;
};

struct HostView {
	uint64_t   bytes = 0;
	Slice      first;
	PageNumber firstPage = kNoPage;
};

struct Run {
	Slice      first;
	PageNumber firstPage = kNoPage;
	uint64_t   pages     = 0;
};

bool continues(const Run& run, const Slice& slice, PageNumber page) {
	return slice.section == run.first.section && slice.index == run.first.index + run.pages &&
	       value(page) == numberAt(run.firstPage, run.pages);
}

void* mapView(const Slice& first, uint64_t address, uint64_t bytes) {
	return MapViewOfFile3(first.section->handle(), GetCurrentProcess(), pointer(address),
	                      first.index * kPageBytes, bytes, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE,
	                      nullptr, 0);
}

bool splitPlaceholder(uint64_t address, uint64_t bytes) {
	return VirtualFree(pointer(address), bytes, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) != FALSE;
}

bool carvePlaceholder(uint64_t address, uint64_t bytes) {
	const uint64_t end    = address + bytes;
	uint64_t       pieces = 0;
	for (uint64_t at = address; at < end; ++pieces) {
		MEMORY_BASIC_INFORMATION info {};
		if (VirtualQuery(pointer(at), &info, sizeof(info)) == 0 || info.State != MEM_RESERVE) {
			return false;
		}
		const uint64_t regionEnd = reinterpret_cast<uint64_t>(info.BaseAddress) + info.RegionSize;
		const uint64_t pieceEnd  = std::min(regionEnd, end);
		const bool     exact = reinterpret_cast<uint64_t>(info.AllocationBase) == at && regionEnd == pieceEnd;
		if (!exact && !splitPlaceholder(at, pieceEnd - at)) {
			return false;
		}
		at = pieceEnd;
	}
	return pieces <= 1 ||
	       VirtualFree(pointer(address), bytes, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS) != FALSE;
}

bool protectAddress(uint64_t address, uint32_t protection) {
	DWORD old = 0;
	if (!VirtualProtect(pointer(address), kPageBytes, protection, &old)) {
		return recordHostError();
	}
	const bool wasOpen = old == PAGE_READONLY || old == PAGE_READWRITE;
	if (protection == PAGE_READONLY && !wasOpen) {
		VirtualProtect(pointer(address), kPageBytes, old, &old);
		return false;
	}
	return true;
}

} // namespace

struct PhysicalMemory::Impl {
	explicit Impl(Context& owner) : context(owner) { freeNumbers.emplace(kFirstNumber, kNumberCount); }

	~Impl() {
		for (const auto& [address, view]: byAddress) {
			UnmapViewOfFile2(GetCurrentProcess(), pointer(address), MEM_PRESERVE_PLACEHOLDER);
		}
	}

	Context& context;

	std::atomic<uint64_t> allocatedPages {0};
	std::atomic<uint64_t> committedPages {0};
	std::atomic<uint64_t> sections {0};
	std::atomic<uint64_t> remapGeneration {0};

	std::mutex                             mapping;
	std::unordered_map<uint64_t, uint64_t> holes;

	mutable std::shared_mutex              table;
	std::unordered_map<uint64_t, Entry>    entries;
	std::map<uint64_t, uint64_t>           freeNumbers;
	uint64_t                               freeCount = kNumberCount;

	mutable std::shared_mutex                           views;
	std::map<uint64_t, HostView>                        byAddress;
	std::unordered_map<uint64_t, std::vector<uint64_t>> shortViews;
	std::vector<View>                                   longViews;

	class ViewChange {
	public:
		explicit ViewChange(Impl& impl) : m_impl(impl), m_lock(impl.mapping) {
			m_impl.remapGeneration.fetch_add(1, std::memory_order_acq_rel);
		}

		~ViewChange() { m_impl.remapGeneration.fetch_add(1, std::memory_order_acq_rel); }

		ViewChange(const ViewChange&)            = delete;
		ViewChange& operator=(const ViewChange&) = delete;

	private:
		Impl&                       m_impl;
		std::lock_guard<std::mutex> m_lock;
	};

	void takeNumbers(uint64_t count, PageNumber* out) {
		uint64_t taken = 0;
		while (taken < count) {
			const auto [first, pages] = *freeNumbers.begin();
			const uint64_t here       = std::min(pages, count - taken);
			freeNumbers.erase(freeNumbers.begin());
			if (here < pages) {
				freeNumbers.emplace(first + here * kFramesPerPage, pages - here);
			}
			for (uint64_t k = 0; k < here; ++k) {
				out[taken++] = PageNumber {first + k * kFramesPerPage};
			}
		}
		freeCount -= count;
	}

	void giveNumber(uint64_t number) {
		uint64_t first = number;
		uint64_t pages = 1;
		auto     next  = freeNumbers.lower_bound(number);
		if (next != freeNumbers.end() && next->first == number + kFramesPerPage) {
			pages += next->second;
			next = freeNumbers.erase(next);
		}
		if (next != freeNumbers.begin()) {
			const auto before = std::prev(next);
			if (before->first + before->second * kFramesPerPage == number) {
				first = before->first;
				pages += before->second;
				freeNumbers.erase(before);
			}
		}
		freeNumbers.emplace(first, pages);
		++freeCount;
	}

	Status allocate(uint64_t count, uint64_t flags, PageNumber* out) {
		if (count == 0 || out == nullptr || count > kNumberCount) {
			return Status::InvalidArgument;
		}
		const uint64_t bytes  = count * kPageBytes;
		HANDLE         handle = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
		                                           PAGE_READWRITE | SEC_RESERVE,
		                                           static_cast<DWORD>(bytes >> 32),
		                                           static_cast<DWORD>(bytes), nullptr);
		if (handle == nullptr) {
			recordHostError();
			return Status::OutOfMemory;
		}
		auto section = std::make_shared<Section>(handle, count, flags, committedPages, sections);
		{
			std::unique_lock lock(table);
			if (freeCount < count) {
				return Status::OutOfMemory;
			}
			takeNumbers(count, out);
			for (uint64_t index = 0; index < count; ++index) {
				entries.emplace(value(out[index]), Entry {Slice {section, index}});
			}
		}
		allocatedPages.fetch_add(count, std::memory_order_relaxed);
		context.pages().pagesAllocated({out, count});
		return Status::Ok;
	}

	Status free(std::span<const PageNumber> pages) {
		{
			std::shared_lock lock(table);
			for (const PageNumber page: pages) {
				const auto found = entries.find(value(page));
				if (found == entries.end() || found->second.freed) {
					return Status::UnknownPage;
				}
			}
		}
		for (const PageNumber page: pages) {
			context.pages().pageFreed(page);
		}
		{
			std::unique_lock lock(table);
			for (const PageNumber page: pages) {
				const auto found = entries.find(value(page));
				if (found != entries.end() && !found->second.freed) {
					found->second.freed = true;
					allocatedPages.fetch_sub(1, std::memory_order_relaxed);
				}
			}
		}
		forgetUnreachable({pages.begin(), pages.end()});
		return Status::Ok;
	}

	void forgetUnreachable(const std::vector<PageNumber>& candidates) {
		std::vector<PageNumber> forgotten;
		std::vector<Slice>      released;
		{
			std::unique_lock lock(table);
			for (const PageNumber page: candidates) {
				const auto found = entries.find(value(page));
				if (found == entries.end()) {
					continue;
				}
				const Entry& entry = found->second;
				if (entry.freed && entry.views == 0 && entry.holds == 0) {
					released.push_back(entry.slice);
					forgotten.push_back(page);
					entries.erase(found);
				}
			}
		}
		for (const PageNumber page: forgotten) {
			context.pages().pageForgotten(page);
		}
		std::unique_lock lock(table);
		for (const PageNumber page: forgotten) {
			giveNumber(value(page));
		}
	}

	std::optional<Slice> slice(PageNumber page) const {
		std::shared_lock lock(table);
		const auto       found = entries.find(value(page));
		if (found == entries.end()) {
			return std::nullopt;
		}
		return found->second.slice;
	}

	bool pin(std::span<const PageNumber> pages, std::vector<Slice>& slices) {
		std::unique_lock lock(table);
		for (const PageNumber page: pages) {
			if (page != kNoPage && !entries.contains(value(page))) {
				return false;
			}
		}
		for (const PageNumber page: pages) {
			if (page == kNoPage) {
				slices.emplace_back();
				continue;
			}
			Entry& entry = entries.at(value(page));
			++entry.views;
			slices.push_back(entry.slice);
		}
		return true;
	}

	void unpin(PageNumber first, uint64_t pages, std::vector<PageNumber>& candidates) {
		std::unique_lock lock(table);
		for (uint64_t k = 0; k < pages; ++k) {
			const PageNumber page {numberAt(first, k)};
			const auto       found = entries.find(value(page));
			if (found != entries.end()) {
				--found->second.views;
			}
			candidates.push_back(page);
		}
	}

	void noteView(uint64_t address, const HostView& view) {
		const uint64_t pages = view.bytes / kPageBytes;
		{
			std::unique_lock lock(views);
			byAddress.emplace(address, view);
			if (pages > kShortViewPages) {
				longViews.push_back(View {address, view.bytes, view.firstPage});
			} else {
				for (uint64_t k = 0; k < pages; ++k) {
					shortViews[numberAt(view.firstPage, k)].push_back(address + k * kPageBytes);
				}
			}
		}
		const View shown {address, view.bytes, view.firstPage};
		context.pages().viewMapped(shown);
		context.gpuPages().cpuViewMapped(shown);
	}

	HostView forgetView(uint64_t address) {
		std::unique_lock lock(views);
		const auto       found = byAddress.find(address);
		HostView         view  = std::move(found->second);
		byAddress.erase(found);
		const uint64_t pages = view.bytes / kPageBytes;
		if (pages > kShortViewPages) {
			std::erase_if(longViews, [&](const View& item) { return item.address == address; });
			return view;
		}
		for (uint64_t k = 0; k < pages; ++k) {
			const auto list = shortViews.find(numberAt(view.firstPage, k));
			std::erase(list->second, address + k * kPageBytes);
			if (list->second.empty()) {
				shortViews.erase(list);
			}
		}
		return view;
	}

	Status mapRun(uint64_t address, const Run& run) {
		const uint64_t bytes = run.pages * kPageBytes;
		const auto     hole  = holes.find(address);
		const bool     own   = hole != holes.end() && hole->second == bytes;
		if (hole != holes.end()) {
			holes.erase(hole);
		}
		void* at = own ? mapView(run.first, address, bytes) : nullptr;
		if (at == nullptr && carvePlaceholder(address, bytes)) {
			at = mapView(run.first, address, bytes);
		}
		if (at == nullptr) {
			recordHostError();
			return Status::NotPlaceholder;
		}
		noteView(address, HostView {bytes, run.first, run.firstPage});
		return Status::Ok;
	}

	std::vector<uint64_t> viewsHit(uint64_t address, uint64_t end) const {
		std::shared_lock      lock(views);
		std::vector<uint64_t> hit;
		auto                  it = byAddress.upper_bound(address);
		if (it != byAddress.begin()) {
			const auto before = std::prev(it);
			if (before->first + before->second.bytes > address) {
				hit.push_back(before->first);
			}
		}
		for (; it != byAddress.end() && it->first < end; ++it) {
			hit.push_back(it->first);
		}
		return hit;
	}

	Status cutView(uint64_t at, uint64_t address, uint64_t end, std::vector<PageNumber>& candidates) {
		const uint64_t size = byAddressBytes(at);
		context.pages().viewUnmapping(at, size);
		const HostView view = forgetView(at);
		if (!UnmapViewOfFile2(GetCurrentProcess(), pointer(at), MEM_PRESERVE_PLACEHOLDER)) {
			recordHostError();
			noteView(at, view);
			return Status::HostFailure;
		}
		context.gpuPages().cpuViewUnmapped(at, size);
		const uint64_t innerStart = std::max(at, address);
		const uint64_t innerEnd   = std::min(at + size, end);
		unpin(PageNumber {numberAt(view.firstPage, pagesBetween(at, innerStart))},
		      pagesBetween(innerStart, innerEnd), candidates);
		if (at >= address && at + size <= end) {
			holes[at] = size;
		}
		Status status = Status::Ok;
		if (at < address) {
			status = remapPart(view, at, at, address, candidates);
		}
		if (at + size > end) {
			const Status after = remapPart(view, at, end, at + size, candidates);
			status             = status == Status::Ok ? after : status;
		}
		return status;
	}

	uint64_t byAddressBytes(uint64_t at) const {
		std::shared_lock lock(views);
		return byAddress.at(at).bytes;
	}

	Status remapPart(const HostView& view, uint64_t viewStart, uint64_t from, uint64_t to,
	                 std::vector<PageNumber>& candidates) {
		const uint64_t skipped = pagesBetween(viewStart, from);
		const Run      part {Slice {view.first.section, view.first.index + skipped},
                        PageNumber {numberAt(view.firstPage, skipped)}, pagesBetween(from, to)};
		const Status   status = mapRun(from, part);
		if (status != Status::Ok) {
			unpin(part.firstPage, part.pages, candidates);
		}
		return status;
	}

	Status unmapLocked(uint64_t address, uint64_t bytes, std::vector<PageNumber>& candidates) {
		Status status = Status::Ok;
		for (const uint64_t at: viewsHit(address, address + bytes)) {
			const Status cut = cutView(at, address, address + bytes, candidates);
			status           = status == Status::Ok ? cut : status;
		}
		return status;
	}

	Status mapLocked(uint64_t address, std::span<const PageNumber> pages,
	                 std::vector<PageNumber>& candidates) {
		std::vector<Slice> slices;
		if (!pin(pages, slices)) {
			return Status::UnknownPage;
		}
		Status status = unmapLocked(address, pages.size() * kPageBytes, candidates);
		size_t i      = 0;
		while (i < pages.size()) {
			if (pages[i] == kNoPage) {
				++i;
				continue;
			}
			Run run {slices[i], pages[i], 1};
			while (i + run.pages < pages.size() && pages[i + run.pages] != kNoPage &&
			       continues(run, slices[i + run.pages], pages[i + run.pages])) {
				++run.pages;
			}
			const Status mapped = mapRun(address + i * kPageBytes, run);
			if (mapped != Status::Ok) {
				unpin(run.firstPage, run.pages, candidates);
				status = status == Status::Ok ? mapped : status;
			}
			i += run.pages;
		}
		return status;
	}

	Status map(uint64_t address, std::span<const PageNumber> pages) {
		if (!pageAligned(address) || pages.size() > (UINT64_MAX - address) / kPageBytes) {
			return Status::InvalidArgument;
		}
		std::vector<PageNumber> candidates;
		Status                  status = Status::Ok;
		{
			ViewChange change(*this);
			status = mapLocked(address, pages, candidates);
		}
		forgetUnreachable(candidates);
		return status;
	}

	Status mapScatter(std::span<const uint64_t> addresses, std::span<const PageNumber> pages) {
		if (addresses.size() != pages.size() ||
		    !std::all_of(addresses.begin(), addresses.end(), pageAligned)) {
			return Status::InvalidArgument;
		}
		std::vector<PageNumber> candidates;
		Status                  status = Status::Ok;
		{
			ViewChange change(*this);
			for (size_t i = 0; i < addresses.size(); ++i) {
				const Status mapped = mapLocked(addresses[i], pages.subspan(i, 1), candidates);
				status              = status == Status::Ok ? mapped : status;
			}
		}
		forgetUnreachable(candidates);
		return status;
	}

	Status unmap(uint64_t address, uint64_t bytes) {
		if (!pageAligned(address) || !pageAligned(bytes) || bytes > UINT64_MAX - address) {
			return Status::InvalidArgument;
		}
		std::vector<PageNumber> candidates;
		Status                  status = Status::Ok;
		{
			ViewChange change(*this);
			status = unmapLocked(address, bytes, candidates);
		}
		forgetUnreachable(candidates);
		return status;
	}

	std::optional<PageAt> pageAt(uint64_t address) const {
		std::shared_lock lock(views);
		auto             it = byAddress.upper_bound(address);
		if (it == byAddress.begin()) {
			return std::nullopt;
		}
		--it;
		const uint64_t into = address - it->first;
		if (into >= it->second.bytes) {
			return std::nullopt;
		}
		return PageAt {PageNumber {numberAt(it->second.firstPage, into / kPageBytes)},
		               into % kPageBytes};
	}

	void viewsOf(PageNumber page, std::vector<uint64_t>& out) const {
		std::shared_lock lock(views);
		if (const auto found = shortViews.find(value(page)); found != shortViews.end()) {
			out.insert(out.end(), found->second.begin(), found->second.end());
		}
		for (const View& view: longViews) {
			const uint64_t pages = view.bytes / kPageBytes;
			if (value(page) >= value(view.firstPage) && value(page) < numberAt(view.firstPage, pages)) {
				out.push_back(view.address + (value(page) - value(view.firstPage)) / kFramesPerPage *
				                                 kPageBytes);
			}
		}
	}

	void viewsIn(uint64_t address, uint64_t bytes, std::vector<View>& out) const {
		const uint64_t   end = bytes > UINT64_MAX - address ? UINT64_MAX : address + bytes;
		std::shared_lock lock(views);
		auto             it = byAddress.upper_bound(address);
		if (it != byAddress.begin() && std::prev(it)->first + std::prev(it)->second.bytes > address) {
			--it;
		}
		for (; it != byAddress.end() && it->first < end; ++it) {
			out.push_back(View {it->first, it->second.bytes, it->second.firstPage});
		}
	}

	bool allocated(PageNumber page) const {
		std::shared_lock lock(table);
		const auto       found = entries.find(value(page));
		return found != entries.end() && !found->second.freed;
	}

	bool committed(PageNumber page) const {
		const auto found = slice(page);
		return found && found->section->committed(found->index);
	}

	uint8_t* bytesOf(PageNumber page, bool commit) {
		const auto found = slice(page);
		if (!found) {
			return nullptr;
		}
		const bool ready = commit ? found->section->commit(found->index)
		                          : found->section->committed(found->index);
		return ready ? found->section->pageBytes(found->index) : nullptr;
	}

	bool commit(PageNumber page) {
		const auto found = slice(page);
		return found && found->section->commit(found->index);
	}

	void reset(PageNumber page) {
		const auto found = slice(page);
		if (!found || !found->section->committed(found->index)) {
			return;
		}
		uint8_t* bytes = found->section->pageBytes(found->index);
		VirtualAlloc(bytes, kPageBytes, MEM_RESET, PAGE_READWRITE);
		VirtualUnlock(bytes, kPageBytes);
		std::vector<uint64_t> addresses;
		viewsOf(page, addresses);
		for (const uint64_t address: addresses) {
			VirtualUnlock(pointer(address), kPageBytes);
		}
	}

	bool protectViews(PageNumber page, uint32_t protection) {
		if (!committed(page)) {
			return true;
		}
		std::vector<uint64_t> addresses;
		viewsOf(page, addresses);
		bool done = true;
		for (const uint64_t address: addresses) {
			done = protectAddress(address, protection) && done;
		}
		return done;
	}

	bool protectView(uint64_t address, uint32_t protection) {
		const auto at = pageAt(address);
		if (!at) {
			return false;
		}
		return !committed(at->page) || protectAddress(address - at->offset, protection);
	}

	bool holdPage(PageNumber page) {
		std::unique_lock lock(table);
		const auto       found = entries.find(value(page));
		if (found == entries.end()) {
			return false;
		}
		++found->second.holds;
		return true;
	}

	void releasePage(PageNumber page) {
		{
			std::unique_lock lock(table);
			const auto       found = entries.find(value(page));
			if (found == entries.end()) {
				return;
			}
			--found->second.holds;
		}
		forgetUnreachable({page});
	}
};

PhysicalMemory::PhysicalMemory(Context& context)
    : m_context(context), m_impl(std::make_unique<Impl>(context)) {}

PhysicalMemory::~PhysicalMemory() = default;

Status PhysicalMemory::allocate(uint64_t count, uint64_t flags, PageNumber* pages) {
	return m_impl->allocate(count, flags, pages);
}

Status PhysicalMemory::free(std::span<const PageNumber> pages) {
	return m_impl->free(pages);
}

Status PhysicalMemory::map(uint64_t address, std::span<const PageNumber> pages) {
	return m_impl->map(address, pages);
}

Status PhysicalMemory::mapScatter(std::span<const uint64_t>   addresses,
                                  std::span<const PageNumber> pages) {
	return m_impl->mapScatter(addresses, pages);
}

Status PhysicalMemory::unmap(uint64_t address, uint64_t bytes) {
	return m_impl->unmap(address, bytes);
}

std::optional<PageAt> PhysicalMemory::pageAt(uint64_t address) const {
	return m_impl->pageAt(address);
}

void PhysicalMemory::viewsOf(PageNumber page, std::vector<uint64_t>& out) const {
	m_impl->viewsOf(page, out);
}

void PhysicalMemory::viewsIn(uint64_t address, uint64_t bytes, std::vector<View>& out) const {
	m_impl->viewsIn(address, bytes, out);
}

bool PhysicalMemory::allocated(PageNumber page) const {
	return m_impl->allocated(page);
}

bool PhysicalMemory::committed(PageNumber page) const {
	return m_impl->committed(page);
}

PhysicalMemory::Totals PhysicalMemory::totals() const {
	return Totals {m_impl->allocatedPages.load(std::memory_order_relaxed),
	               m_impl->committedPages.load(std::memory_order_relaxed),
	               m_impl->sections.load(std::memory_order_relaxed)};
}

uint8_t* PhysicalMemory::bytesOf(PageNumber page, bool commit) {
	return m_impl->bytesOf(page, commit);
}

bool PhysicalMemory::commit(PageNumber page) {
	return m_impl->commit(page);
}

void PhysicalMemory::reset(PageNumber page) {
	m_impl->reset(page);
}

bool PhysicalMemory::protectViews(PageNumber page, uint32_t protection) {
	return m_impl->protectViews(page, protection);
}

bool PhysicalMemory::protectView(uint64_t address, uint32_t protection) {
	return m_impl->protectView(address, protection);
}

bool PhysicalMemory::holdPage(PageNumber page) {
	return m_impl->holdPage(page);
}

void PhysicalMemory::releasePage(PageNumber page) {
	m_impl->releasePage(page);
}

uint64_t PhysicalMemory::remapGeneration() const {
	return m_impl->remapGeneration.load(std::memory_order_acquire);
}

} // namespace deunify
