#include "deunify/gpuPageTable.hpp"

#include "deunify/cardView.hpp"
#include "deunify/context.hpp"
#include "deunify/pages.hpp"
#include "deunify/physicalMemory.hpp"

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <unordered_map>

namespace deunify {

namespace {

bool slotAligned(uint64_t value) {
	return value % kPageBytes == 0;
}

bool validRange(uint64_t base, uint64_t bytes) {
	return slotAligned(base) && slotAligned(bytes) && bytes != 0 && bytes <= UINT64_MAX - base;
}

bool overlaps(const Range& a, const Range& b) {
	return a.start < b.end() && b.start < a.end();
}

bool inside(const Range& outer, uint64_t address, uint64_t bytes) {
	return address >= outer.start && bytes <= outer.bytes && address - outer.start <= outer.bytes - bytes;
}

std::optional<Range> intersection(const Range& a, const Range& b) {
	const uint64_t start = std::max(a.start, b.start);
	const uint64_t end   = std::min(a.end(), b.end());
	if (start >= end) {
		return std::nullopt;
	}
	return Range {start, end - start};
}

Status firstFailure(Status kept, Status next) {
	return kept == Status::Ok ? next : kept;
}

} // namespace

struct GpuPageTable::Impl {
	explicit Impl(Context& owner) : context(owner) {}

	Context& context;

	mutable std::mutex                                  state;
	std::vector<Range>                                  unified;
	std::vector<Range>                                  gpuOnly;
	std::map<uint64_t, PageNumber>                      slots;
	std::unordered_map<uint64_t, std::vector<uint64_t>> slotsByPage;

	PhysicalMemory& physical() const { return context.physical(); }
	CardView*       cardView() const { return context.cardView(); }

	bool declared(const Range& range) const {
		const auto hits = [&](const Range& other) { return overlaps(range, other); };
		return std::any_of(unified.begin(), unified.end(), hits) ||
		       std::any_of(gpuOnly.begin(), gpuOnly.end(), hits);
	}

	bool inUnified(uint64_t address, uint64_t bytes) const {
		return std::any_of(unified.begin(), unified.end(),
		                   [&](const Range& range) { return inside(range, address, bytes); });
	}

	bool inGpuOnly(uint64_t address, uint64_t bytes) const {
		return std::any_of(gpuOnly.begin(), gpuOnly.end(),
		                   [&](const Range& range) { return inside(range, address, bytes); });
	}

	void showCpuView(const Range& part, PageNumber firstPage, uint64_t viewStart) {
		if (CardView* card = cardView()) {
			for (uint64_t slot = part.start; slot < part.end(); slot += kPageBytes) {
				const uint64_t pages = (slot - viewStart) / kPageBytes;
				card->slotChanged(slot, PageNumber {value(firstPage) + pages * kFramesPerPage});
			}
		}
		context.queueChange(part, ChangeReason::Remapped);
	}

	void hideCpuView(const Range& part) {
		if (CardView* card = cardView()) {
			for (uint64_t slot = part.start; slot < part.end(); slot += kPageBytes) {
				card->slotChanged(slot, kNoPage);
			}
		}
		context.queueChange(part, ChangeReason::Remapped);
	}

	Status followCpuViews(uint64_t base, uint64_t bytes) {
		if (!validRange(base, bytes)) {
			return Status::InvalidArgument;
		}
		const Range      range {base, bytes};
		std::lock_guard  lock(state);
		if (declared(range)) {
			return Status::InvalidArgument;
		}
		unified.push_back(range);
		std::vector<View> views;
		physical().viewsIn(base, bytes, views);
		for (const View& view: views) {
			if (const auto part = intersection(range, Range {view.address, view.bytes})) {
				showCpuView(*part, view.firstPage, view.address);
			}
		}
		return Status::Ok;
	}

	Status addGpuRange(uint64_t base, uint64_t bytes) {
		if (!validRange(base, bytes)) {
			return Status::InvalidArgument;
		}
		const Range     range {base, bytes};
		std::lock_guard lock(state);
		if (declared(range)) {
			return Status::InvalidArgument;
		}
		gpuOnly.push_back(range);
		return Status::Ok;
	}

	void cpuViewMapped(const View& view) {
		std::lock_guard lock(state);
		for (const Range& range: unified) {
			if (const auto part = intersection(range, Range {view.address, view.bytes})) {
				showCpuView(*part, view.firstPage, view.address);
			}
		}
	}

	void cpuViewUnmapped(uint64_t address, uint64_t bytes) {
		std::lock_guard lock(state);
		for (const Range& range: unified) {
			if (const auto part = intersection(range, Range {address, bytes})) {
				hideCpuView(*part);
			}
		}
	}

	void dropSlot(uint64_t slot, PageNumber page) {
		auto& list = slotsByPage[value(page)];
		std::erase(list, slot);
		if (list.empty()) {
			slotsByPage.erase(value(page));
		}
		physical().releasePage(page);
	}

	void setSlot(uint64_t slot, PageNumber page) {
		if (const auto old = slots.find(slot); old != slots.end()) {
			const PageNumber was = old->second;
			slots.erase(old);
			dropSlot(slot, was);
		}
		if (page != kNoPage) {
			slots.emplace(slot, page);
			slotsByPage[value(page)].push_back(slot);
		}
		cardView()->slotChanged(slot, page);
	}

	bool holdAll(std::span<const PageNumber> pages) {
		for (size_t i = 0; i < pages.size(); ++i) {
			if (pages[i] != kNoPage && !physical().holdPage(pages[i])) {
				for (const PageNumber held: pages.first(i)) {
					if (held != kNoPage) {
						physical().releasePage(held);
					}
				}
				return false;
			}
		}
		return true;
	}

	Status mapOnCard(uint64_t gpuAddress, std::span<const PageNumber> pages) {
		std::lock_guard lock(state);
		if (!inGpuOnly(gpuAddress, pages.size() * kPageBytes)) {
			return Status::InvalidArgument;
		}
		if (!holdAll(pages)) {
			return Status::UnknownPage;
		}
		for (size_t i = 0; i < pages.size(); ++i) {
			setSlot(gpuAddress + i * kPageBytes, pages[i]);
		}
		return Status::Ok;
	}

	Status unmapOnCard(uint64_t gpuAddress, uint64_t bytes) {
		std::lock_guard lock(state);
		if (!inGpuOnly(gpuAddress, bytes)) {
			return Status::InvalidArgument;
		}
		std::vector<uint64_t> shown;
		for (auto it = slots.lower_bound(gpuAddress); it != slots.end() && it->first < gpuAddress + bytes;
		     ++it) {
			shown.push_back(it->first);
		}
		for (const uint64_t slot: shown) {
			setSlot(slot, kNoPage);
		}
		return Status::Ok;
	}

	Status mapAsViews(uint64_t gpuAddress, std::span<const PageNumber> pages) {
		{
			std::lock_guard lock(state);
			if (!inGpuOnly(gpuAddress, pages.size() * kPageBytes)) {
				return Status::InvalidArgument;
			}
		}
		return physical().map(gpuAddress, pages);
	}

	Status unmapViews(uint64_t gpuAddress, uint64_t bytes) {
		{
			std::lock_guard lock(state);
			if (!inGpuOnly(gpuAddress, bytes)) {
				return Status::InvalidArgument;
			}
		}
		return physical().unmap(gpuAddress, bytes);
	}

	Status map(uint64_t gpuAddress, std::span<const PageNumber> pages) {
		if (pages.empty()) {
			return Status::Ok;
		}
		if (!slotAligned(gpuAddress) || pages.size() > (UINT64_MAX - gpuAddress) / kPageBytes) {
			return Status::InvalidArgument;
		}
		const Status status = cardView() != nullptr ? mapOnCard(gpuAddress, pages)
		                                            : mapAsViews(gpuAddress, pages);
		if (status != Status::InvalidArgument && status != Status::UnknownPage) {
			context.queueChange(Range {gpuAddress, pages.size() * kPageBytes}, ChangeReason::Remapped);
		}
		return status;
	}

	Status unmap(uint64_t gpuAddress, uint64_t bytes) {
		if (bytes == 0) {
			return Status::Ok;
		}
		if (!validRange(gpuAddress, bytes)) {
			return Status::InvalidArgument;
		}
		const Status status = cardView() != nullptr ? unmapOnCard(gpuAddress, bytes)
		                                            : unmapViews(gpuAddress, bytes);
		if (status != Status::InvalidArgument) {
			context.queueChange(Range {gpuAddress, bytes}, ChangeReason::Remapped);
		}
		return status;
	}

	std::optional<PageNumber> poolPage(uint64_t poolAddress, uint64_t index) const {
		const auto at = physical().pageAt(poolAddress + index * kPageBytes);
		if (!at) {
			return std::nullopt;
		}
		return at->page;
	}

	Status resolvePool(uint64_t poolAddress, uint64_t first, uint64_t count,
	                 std::vector<PageNumber>& out) const {
		for (uint64_t k = 0; k < count; ++k) {
			const auto page = poolPage(poolAddress, first + k);
			if (!page) {
				return Status::UnknownPage;
			}
			out.push_back(*page);
		}
		return Status::Ok;
	}

	Status applyRun(uint64_t at, const PageRun& run, uint64_t poolAddress, uint32_t poolPages) {
		const uint64_t bytes = uint64_t {run.count} * kPageBytes;
		const uint64_t first = run.poolPage;
		std::vector<PageNumber> pages;
		switch (run.kind) {
			case PageRunKind::Unchanged: return Status::Ok;
			case PageRunKind::Unmapped: return unmap(at, bytes);
			case PageRunKind::Consecutive: {
				if (first + run.count > poolPages) {
					return Status::InvalidArgument;
				}
				const Status found = resolvePool(poolAddress, first, run.count, pages);
				return found == Status::Ok ? map(at, pages) : found;
			}
			case PageRunKind::Repeated: {
				if (first + 1 > poolPages) {
					return Status::InvalidArgument;
				}
				const auto page = poolPage(poolAddress, first);
				if (!page) {
					return Status::UnknownPage;
				}
				pages.assign(run.count, *page);
				return map(at, pages);
			}
		}
		return Status::InvalidArgument;
	}

	Status copyPageMappings(std::span<const PageBatch> batches, uint64_t poolAddress,
	                        uint32_t poolPages) {
		if (!slotAligned(poolAddress)) {
			return Status::InvalidArgument;
		}
		Status status = Status::Ok;
		for (const PageBatch& batch: batches) {
			uint64_t at = batch.gpuAddress;
			for (const PageRun& run: batch.runs) {
				status = firstFailure(status, applyRun(at, run, poolAddress, poolPages));
				at += uint64_t {run.count} * kPageBytes;
			}
		}
		return status;
	}

	std::optional<PageAt> pageAt(uint64_t gpuAddress) const {
		bool explicitSlot = false;
		{
			std::lock_guard lock(state);
			if (!inUnified(gpuAddress, 1)) {
				if (!inGpuOnly(gpuAddress, 1)) {
					return std::nullopt;
				}
				explicitSlot = cardView() != nullptr;
			}
			if (explicitSlot) {
				const auto found = slots.find(gpuAddress - gpuAddress % kPageBytes);
				if (found == slots.end()) {
					return std::nullopt;
				}
				return PageAt {found->second, gpuAddress % kPageBytes};
			}
		}
		return physical().pageAt(gpuAddress);
	}

	void slotsOf(PageNumber page, std::vector<uint64_t>& out) const {
		std::vector<uint64_t> views;
		physical().viewsOf(page, views);
		const bool      viewsAreSlots = cardView() == nullptr;
		std::lock_guard lock(state);
		for (const uint64_t address: views) {
			if (inUnified(address, kPageBytes) || (viewsAreSlots && inGpuOnly(address, kPageBytes))) {
				out.push_back(address);
			}
		}
		if (const auto found = slotsByPage.find(value(page)); found != slotsByPage.end()) {
			out.insert(out.end(), found->second.begin(), found->second.end());
		}
	}

	Status read(uint64_t gpuAddress, void* out, uint64_t bytes, ReadPurpose purpose) {
		if ((out == nullptr && bytes != 0) || bytes > UINT64_MAX - gpuAddress) {
			return Status::InvalidArgument;
		}
		auto*  into   = static_cast<uint8_t*>(out);
		Status status = Status::Ok;
		while (bytes != 0) {
			const uint64_t offset = gpuAddress % kPageBytes;
			const uint64_t piece  = std::min(bytes, kPageBytes - offset);
			const auto     at     = pageAt(gpuAddress);
			const Status   pieceStatus =
			    at ? context.pages().readPage(at->page, offset, into, piece, purpose)
			       : Status::UnknownPage;
			if (pieceStatus != Status::Ok) {
				std::memset(into, 0, piece);
			}
			status = firstFailure(status, pieceStatus);
			gpuAddress += piece;
			into += piece;
			bytes -= piece;
		}
		return status;
	}
};

GpuPageTable::GpuPageTable(Context& context)
    : m_context(context), m_impl(std::make_unique<Impl>(context)) {}

GpuPageTable::~GpuPageTable() = default;

Status GpuPageTable::followCpuViews(uint64_t base, uint64_t bytes) {
	return m_impl->followCpuViews(base, bytes);
}

Status GpuPageTable::addGpuRange(uint64_t base, uint64_t bytes) {
	return m_impl->addGpuRange(base, bytes);
}

Status GpuPageTable::copyPageMappings(std::span<const PageBatch> batches, uint64_t poolAddress,
                                      uint32_t poolPages) {
	return m_impl->copyPageMappings(batches, poolAddress, poolPages);
}

Status GpuPageTable::map(uint64_t gpuAddress, std::span<const PageNumber> pages) {
	return m_impl->map(gpuAddress, pages);
}

Status GpuPageTable::unmap(uint64_t gpuAddress, uint64_t bytes) {
	return m_impl->unmap(gpuAddress, bytes);
}

std::optional<PageAt> GpuPageTable::pageAt(uint64_t gpuAddress) const {
	return m_impl->pageAt(gpuAddress);
}

void GpuPageTable::slotsOf(PageNumber page, std::vector<uint64_t>& out) const {
	m_impl->slotsOf(page, out);
}

Status GpuPageTable::read(uint64_t gpuAddress, void* out, uint64_t bytes, ReadPurpose purpose) {
	return m_impl->read(gpuAddress, out, bytes, purpose);
}

void GpuPageTable::cpuViewMapped(const View& view) {
	m_impl->cpuViewMapped(view);
}

void GpuPageTable::cpuViewUnmapped(uint64_t address, uint64_t bytes) {
	m_impl->cpuViewUnmapped(address, bytes);
}

} // namespace deunify
