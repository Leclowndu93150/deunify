#include "deunify/census.hpp"

#include "censusLedger.hpp"
#include "d3d12Backend.hpp"
#include "gpuBackend.hpp"

#include "deunify/context.hpp"
#include "deunify/gpuPageTable.hpp"

#include <algorithm>
#include <mutex>

namespace deunify {

namespace {

constexpr const char* kCardViewName = "card view";
constexpr uint32_t    kMegabyteShift = 20;

std::string megabytes(uint64_t bytes) {
	return std::to_string(bytes >> kMegabyteShift) + " MB";
}

std::string categoriesText(const std::vector<CategoryUse>& categories, bool card) {
	uint64_t    bytes = 0;
	uint64_t    count = 0;
	std::string parts;
	for (const CategoryUse& use : categories) {
		uint64_t useBytes = card ? use.cardBytes : use.sharedBytes;
		uint64_t useCount = card ? use.cardCount : use.sharedCount;
		bytes += useBytes;
		count += useCount;
		if (useCount != 0) {
			parts += (parts.empty() ? "" : ", ") + use.name + " " + megabytes(useBytes) + " in " + std::to_string(useCount);
		}
	}
	return megabytes(bytes) + " in " + std::to_string(count) + (parts.empty() ? "" : " (" + parts + ")");
}

std::string systemText(const SystemMemory& system) {
	if (!system.known) {
		return "system: not known";
	}
	return "system: card " + megabytes(system.localUsage) + " of " + megabytes(system.localBudget) +
	       " budget, shared " + megabytes(system.nonLocalUsage) + " of " + megabytes(system.nonLocalBudget) +
	       " budget";
}

std::string homesText(const HomeCounts& homes) {
	return "homes: zero " + std::to_string(homes.zero) + ", file " + std::to_string(homes.file) + ", texture " +
	       std::to_string(homes.texture) + ", card " + std::to_string(homes.card) + ", ram " +
	       std::to_string(homes.ram) + " (" + std::to_string(homes.ramTiled) + " with a tile)";
}

std::string twiceText(const CensusSnapshot& snapshot) {
	return "held twice: " + megabytes(snapshot.ramAndTileBytes) + " in RAM and a tile, " +
	       megabytes(snapshot.tileAndTextureBytes) + " in a tile and a texture";
}

std::string loadsText(const CensusSnapshot& snapshot) {
	return "staged " + megabytes(snapshot.loads.stagedBytes) + ", decoded " + megabytes(snapshot.loads.decodedBytes) +
	       ", pages " + std::to_string(snapshot.physical.pages) + " allocated, " +
	       std::to_string(snapshot.physical.committed) + " committed";
}

} // namespace

struct Census::Impl {
	static uint64_t tilesUnderTextures(Context& context, const CardView& cardView) {
		const Callbacks& callbacks = context.callbacks();
		if (callbacks.texturesOver == nullptr) {
			return 0;
		}
		std::vector<PageNumber> pages;
		cardView.tiledPages(pages);
		std::vector<uint64_t> slots;
		return static_cast<uint64_t>(std::count_if(pages.begin(), pages.end(), [&](PageNumber page) {
			slots.clear();
			context.gpuPages().slotsOf(page, slots);
			return std::any_of(slots.begin(), slots.end(), [&](uint64_t slot) {
				return callbacks.texturesOver(callbacks.context, slot, kPageBytes);
			});
		}));
	}

	SystemMemory systemMemory() {
		std::shared_ptr<gpu::MemoryReport> report;
		{
			std::lock_guard lock(reportMutex);
			report = adapterReport;
		}
		if (report) {
			return report->systemMemory(ledger->uses());
		}
		return backend != nullptr ? backend->systemMemory(ledger->uses()) : SystemMemory {};
	}

	std::shared_ptr<detail::Ledger>    ledger = std::make_shared<detail::Ledger>();
	std::mutex                         reportMutex;
	std::shared_ptr<gpu::MemoryReport> adapterReport;
	gpu::Backend*                      backend = nullptr;
};

Census::Census(Context& context) : m_context(context), m_impl(std::make_unique<Impl>()) {
	m_impl->ledger->addCategory(kCardViewName);
}

Census::~Census() = default;

Category Census::addCategory(const char* name) {
	return m_impl->ledger->addCategory(name);
}

Status Census::track(ID3D12Heap* heap, Category category) {
	return detail::trackHeap(m_impl->ledger, heap, category);
}

Status Census::track(ID3D12Device* device, ID3D12Resource* resource, Category category) {
	return detail::trackResource(m_impl->ledger, device, resource, category);
}

CensusSnapshot Census::snapshot() {
	CensusSnapshot snapshot;
	snapshot.categories = m_impl->ledger->uses();
	snapshot.system     = m_impl->systemMemory();
	snapshot.homes           = m_context.pages().counts();
	snapshot.physical        = m_context.physical().totals();
	snapshot.loads           = m_context.loads().totals();
	snapshot.ramAndTileBytes = snapshot.homes.ramTiled * kPageBytes;
	if (CardView* cardView = m_context.cardView()) {
		snapshot.cardView                    = cardView->totals();
		snapshot.cardView.tilesUnderTextures = Impl::tilesUnderTextures(m_context, *cardView);
		snapshot.tileAndTextureBytes         = snapshot.cardView.tilesUnderTextures * kPageBytes;
	}
	return snapshot;
}

void Census::setAdapter(IDXGIAdapter3* adapter) {
	std::shared_ptr<gpu::MemoryReport> report = adapter != nullptr ? gpu::dxgiReport(adapter) : nullptr;
	std::lock_guard                    lock(m_impl->reportMutex);
	m_impl->adapterReport.swap(report);
}

std::shared_ptr<detail::Ledger> Census::ledger() const {
	return m_impl->ledger;
}

void Census::attach(gpu::Backend* backend) {
	m_impl->backend = backend;
}

std::string Census::format(const CensusSnapshot& snapshot) {
	return "card " + categoriesText(snapshot.categories, true) + "; shared " +
	       categoriesText(snapshot.categories, false) + "; " + systemText(snapshot.system) + "; " +
	       homesText(snapshot.homes) + "; " + twiceText(snapshot) + "; " + loadsText(snapshot);
}

} // namespace deunify
