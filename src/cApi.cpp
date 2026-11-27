#include "deunify.h"

#include "deunify/deunify.hpp"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace deunify;

struct DeunifyContext {
	DeunifyCallbacks         callbacks {};
	std::unique_ptr<Context> context;
	std::vector<uint64_t>    written;
};

struct DeunifyKernelWrite {
	KernelWrite bracket;
};

struct DeunifySites {
	patch::Sites sites;
};

namespace {

constexpr uint32_t kLastPurpose     = DEUNIFY_READ_TEXTURE_UPLOAD;
constexpr uint32_t kLastKind        = DEUNIFY_KIND_GPU_ONLY;
constexpr uint32_t kLastRunKind     = DEUNIFY_RUN_UNCHANGED;
constexpr uint32_t kLastSpanHome    = DEUNIFY_SPAN_CARD;
constexpr uint32_t kLastAccess      = DEUNIFY_KERNEL_WRITE;
constexpr uint32_t kNoCategory      = UINT32_MAX;

DeunifyStatus toC(Status status) {
	return static_cast<DeunifyStatus>(status);
}

const DeunifyCallbacks& callbacksOf(void* context) {
	return static_cast<DeunifyContext*>(context)->callbacks;
}

void logThrough(void* context, LogLevel level, const char* text) {
	const DeunifyCallbacks& c = callbacksOf(context);
	c.log(c.context, static_cast<uint32_t>(level), text);
}

Status decodeThrough(void* context, Codec codec, const uint8_t* in, size_t inBytes, uint8_t* out,
                     size_t outBytes) {
	const DeunifyCallbacks& c = callbacksOf(context);
	return static_cast<Status>(c.decode(c.context, static_cast<uint32_t>(codec), in, inBytes, out, outBytes));
}

void pagesChangedThrough(void* context, const Range* ranges, size_t count, ChangeReason reason) {
	const DeunifyCallbacks&   c = callbacksOf(context);
	std::vector<DeunifyRange> converted(count);
	std::transform(ranges, ranges + count, converted.begin(),
	               [](const Range& range) { return DeunifyRange {range.start, range.bytes}; });
	c.pagesChanged(c.context, converted.data(), count, static_cast<uint32_t>(reason));
}

bool regionOfThrough(void* context, uint64_t gpuAddress, Range* region) {
	const DeunifyCallbacks& c     = callbacksOf(context);
	DeunifyRange            found = {};
	if (c.regionOf(c.context, gpuAddress, &found) == 0) {
		return false;
	}
	*region = Range {found.start, found.bytes};
	return true;
}

bool texturesOverThrough(void* context, uint64_t gpuAddress, uint64_t bytes) {
	const DeunifyCallbacks& c = callbacksOf(context);
	return c.texturesOver(c.context, gpuAddress, bytes) != 0;
}

Callbacks callbacksFor(DeunifyContext* holder) {
	const DeunifyCallbacks& c = holder->callbacks;
	Callbacks               callbacks;
	callbacks.context      = holder;
	callbacks.log          = c.log != nullptr ? logThrough : nullptr;
	callbacks.decode       = c.decode != nullptr ? decodeThrough : nullptr;
	callbacks.pagesChanged = c.pagesChanged != nullptr ? pagesChangedThrough : nullptr;
	callbacks.regionOf     = c.regionOf != nullptr ? regionOfThrough : nullptr;
	callbacks.texturesOver = c.texturesOver != nullptr ? texturesOverThrough : nullptr;
	return callbacks;
}

Config configOf(const DeunifyConfig& c) {
	Config config;
	config.stagingCapBytes   = c.stagingCapBytes;
	config.decodedCapBytes   = c.decodedCapBytes;
	config.maxRecordsPerPage = c.maxRecordsPerPage;
	config.decodeWorkers     = c.decodeWorkers;
	config.device            = static_cast<ID3D12Device*>(c.device);
	config.queue             = static_cast<ID3D12CommandQueue*>(c.queue);
	return config;
}

std::vector<PageNumber> pagesOf(const uint64_t* pages, size_t count) {
	std::vector<PageNumber> converted(count);
	std::transform(pages, pages + count, converted.begin(), [](uint64_t page) { return PageNumber {page}; });
	return converted;
}

DeunifyPageAt pageAtOf(const PageAt& at) {
	return DeunifyPageAt {value(at.page), at.offset};
}

DeunifyHomeCounts homeCountsOf(const HomeCounts& homes) {
	return DeunifyHomeCounts {homes.zero, homes.file,     homes.texture, homes.card,      homes.ram,
	                          homes.ramTiled, homes.parked, homes.faults, homes.readBacks, homes.replays};
}

DeunifyLoadTotals loadTotalsOf(const Loads::Totals& totals) {
	return DeunifyLoadTotals {totals.stagedBytes,     totals.decodedBytes,  totals.requestsLoaded,
	                          totals.requestsDecoded, totals.pagesReplayed, totals.stagedReads};
}

LoadRequest requestOf(const DeunifyLoadRequest& c, const std::vector<PageNumber>& pages) {
	LoadRequest request;
	request.file              = FileId {c.file};
	request.fileOffset        = c.fileOffset;
	request.sourceBytes       = c.sourceBytes;
	request.firstCodec        = static_cast<Codec>(c.firstCodec);
	request.secondCodec       = static_cast<Codec>(c.secondCodec);
	request.intermediateBytes = c.intermediateBytes;
	request.destinationBytes  = c.destinationBytes;
	request.pages             = pages;
	request.offsetInFirstPage = c.offsetInFirstPage;
	return request;
}

bool hasPages(const uint64_t* pages, size_t count) {
	return pages != nullptr || count == 0;
}

void handOutWritten(DeunifyContext* context, uint64_t* pages, size_t capacity, size_t* count) {
	std::vector<uint64_t>& written = context->written;
	size_t                 given   = std::min(capacity, written.size());
	std::copy_n(written.begin(), given, pages);
	written.erase(written.begin(), written.begin() + static_cast<ptrdiff_t>(given));
	*count = given;
}

} // namespace

extern "C" {

void deunify_config_defaults(DeunifyConfig* config) {
	if (config == nullptr) {
		return;
	}
	const Config defaults;
	*config                   = DeunifyConfig {};
	config->version           = DEUNIFY_ABI_VERSION;
	config->maxRecordsPerPage = defaults.maxRecordsPerPage;
	config->stagingCapBytes   = defaults.stagingCapBytes;
	config->decodedCapBytes   = defaults.decodedCapBytes;
	config->decodeWorkers     = defaults.decodeWorkers;
}

DeunifyStatus deunify_create(const DeunifyConfig* config, const DeunifyCallbacks* callbacks,
                             DeunifyContext** context) {
	if (config == nullptr || context == nullptr) {
		return DEUNIFY_INVALID_ARGUMENT;
	}
	*context = nullptr;
	if (config->version != DEUNIFY_ABI_VERSION || (callbacks != nullptr && callbacks->version != DEUNIFY_ABI_VERSION)) {
		return DEUNIFY_VERSION_MISMATCH;
	}
	auto holder = std::make_unique<DeunifyContext>();
	if (callbacks != nullptr) {
		holder->callbacks = *callbacks;
	}
	Status status   = Status::Ok;
	holder->context = Context::create(configOf(*config), callbacksFor(holder.get()), &status);
	if (holder->context == nullptr) {
		return toC(status);
	}
	*context = holder.release();
	return DEUNIFY_OK;
}

void deunify_destroy(DeunifyContext* context) {
	delete context;
}

void deunify_deliver_changes(DeunifyContext* context) {
	if (context != nullptr) {
		context->context->deliverChanges();
	}
}

uint32_t deunify_last_host_error(void) {
	return Context::lastHostError();
}

DeunifyStatus deunify_physical_allocate(DeunifyContext* context, uint64_t count, uint64_t flags,
                                        uint64_t* pages) {
	if (context == nullptr || !hasPages(pages, count)) {
		return DEUNIFY_INVALID_ARGUMENT;
	}
	std::vector<PageNumber> allocated(count);
	Status                  status = context->context->physical().allocate(count, flags, allocated.data());
	if (status == Status::Ok) {
		std::transform(allocated.begin(), allocated.end(), pages, [](PageNumber page) { return value(page); });
	}
	return toC(status);
}

DeunifyStatus deunify_physical_free(DeunifyContext* context, const uint64_t* pages, size_t count) {
	if (context == nullptr || !hasPages(pages, count)) {
		return DEUNIFY_INVALID_ARGUMENT;
	}
	return toC(context->context->physical().free(pagesOf(pages, count)));
}

DeunifyStatus deunify_physical_map(DeunifyContext* context, uint64_t address, const uint64_t* pages,
                                   size_t count) {
	if (context == nullptr || !hasPages(pages, count)) {
		return DEUNIFY_INVALID_ARGUMENT;
	}
	return toC(context->context->physical().map(address, pagesOf(pages, count)));
}

DeunifyStatus deunify_physical_map_scatter(DeunifyContext* context, const uint64_t* addresses,
                                           const uint64_t* pages, size_t count) {
	if (context == nullptr || !hasPages(addresses, count) || !hasPages(pages, count)) {
		return DEUNIFY_INVALID_ARGUMENT;
	}
	std::span<const uint64_t> at(addresses, count);
	return toC(context->context->physical().mapScatter(at, pagesOf(pages, count)));
}

DeunifyStatus deunify_physical_unmap(DeunifyContext* context, uint64_t address, uint64_t bytes) {
	if (context == nullptr) {
		return DEUNIFY_INVALID_ARGUMENT;
	}
	return toC(context->context->physical().unmap(address, bytes));
}

DeunifyStatus deunify_physical_page_at(DeunifyContext* context, uint64_t address, DeunifyPageAt* at) {
	if (context == nullptr || at == nullptr) {
		return DEUNIFY_INVALID_ARGUMENT;
	}
	std::optional<PageAt> found = context->context->physical().pageAt(address);
	if (!found) {
		return DEUNIFY_NOT_FOUND;
	}
	*at = pageAtOf(*found);
	return DEUNIFY_OK;
}

DeunifyStatus deunify_physical_views_of(DeunifyContext* context, uint64_t page, uint64_t* addresses,
                                        size_t capacity, size_t* count) {
	if (context == nullptr || count == nullptr || !hasPages(addresses, capacity)) {
		return DEUNIFY_INVALID_ARGUMENT;
	}
	std::vector<uint64_t> views;
	context->context->physical().viewsOf(PageNumber {page}, views);
	std::copy_n(views.begin(), std::min(capacity, views.size()), addresses);
	*count = views.size();
	return DEUNIFY_OK;
}

DeunifyStatus deunify_gpu_follow_cpu_views(DeunifyContext* context, uint64_t base, uint64_t bytes) {
	if (context == nullptr) {
		return DEUNIFY_INVALID_ARGUMENT;
	}
	return toC(context->context->gpuPages().followCpuViews(base, bytes));
}

DeunifyStatus deunify_gpu_add_range(DeunifyContext* context, uint64_t base, uint64_t bytes) {
	if (context == nullptr) {
		return DEUNIFY_INVALID_ARGUMENT;
	}
	return toC(context->context->gpuPages().addGpuRange(base, bytes));
}

DeunifyStatus deunify_gpu_copy_page_mappings(DeunifyContext* context, const DeunifyPageBatch* batches,
                                             size_t count, uint64_t poolAddress, uint32_t poolPages) {
	if (context == nullptr || (batches == nullptr && count != 0)) {
		return DEUNIFY_INVALID_ARGUMENT;
	}
	std::vector<std::vector<PageRun>> runs(count);
	std::vector<PageBatch>            converted(count);
	for (size_t i = 0; i < count; ++i) {
		const DeunifyPageBatch& batch = batches[i];
		if (batch.runs == nullptr && batch.runCount != 0) {
			return DEUNIFY_INVALID_ARGUMENT;
		}
		for (size_t j = 0; j < batch.runCount; ++j) {
			const DeunifyPageRun& run = batch.runs[j];
			if (run.kind > kLastRunKind) {
				return DEUNIFY_INVALID_ARGUMENT;
			}
			runs[i].push_back(PageRun {static_cast<PageRunKind>(run.kind), run.poolPage, run.count});
		}
		converted[i] = PageBatch {batch.gpuAddress, runs[i]};
	}
	return toC(context->context->gpuPages().copyPageMappings(converted, poolAddress, poolPages));
}

DeunifyStatus deunify_gpu_map(DeunifyContext* context, uint64_t gpuAddress, const uint64_t* pages,
                              size_t count) {
	if (context == nullptr || !hasPages(pages, count)) {
		return DEUNIFY_INVALID_ARGUMENT;
	}
	return toC(context->context->gpuPages().map(gpuAddress, pagesOf(pages, count)));
}

DeunifyStatus deunify_gpu_unmap(DeunifyContext* context, uint64_t gpuAddress, uint64_t bytes) {
	if (context == nullptr) {
		return DEUNIFY_INVALID_ARGUMENT;
	}
	return toC(context->context->gpuPages().unmap(gpuAddress, bytes));
}

DeunifyStatus deunify_gpu_page_at(DeunifyContext* context, uint64_t gpuAddress, DeunifyPageAt* at) {
	if (context == nullptr || at == nullptr) {
		return DEUNIFY_INVALID_ARGUMENT;
	}
	std::optional<PageAt> found = context->context->gpuPages().pageAt(gpuAddress);
	if (!found) {
		return DEUNIFY_NOT_FOUND;
	}
	*at = pageAtOf(*found);
	return DEUNIFY_OK;
}

DeunifyStatus deunify_gpu_read(DeunifyContext* context, uint64_t gpuAddress, void* out, uint64_t bytes,
                               uint32_t purpose) {
	if (context == nullptr || out == nullptr || purpose > kLastPurpose) {
		return DEUNIFY_INVALID_ARGUMENT;
	}
	return toC(context->context->gpuPages().read(gpuAddress, out, bytes, static_cast<ReadPurpose>(purpose)));
}

void deunify_pages_set_kind(DeunifyContext* context, uint64_t address, uint64_t bytes, uint32_t kind) {
	if (context == nullptr || kind > kLastKind) {
		return;
	}
	context->context->pages().setKind(address, bytes, static_cast<Kind>(kind));
}

uint32_t deunify_pages_kind_at(DeunifyContext* context, uint64_t address) {
	if (context == nullptr) {
		return DEUNIFY_KIND_UNKNOWN;
	}
	return static_cast<uint32_t>(context->context->pages().kindAt(address));
}

DeunifyStatus deunify_pages_state(DeunifyContext* context, uint64_t page, DeunifyPageState* state) {
	if (context == nullptr || state == nullptr) {
		return DEUNIFY_INVALID_ARGUMENT;
	}
	PageState found  = context->context->pages().state(PageNumber {page});
	*state           = DeunifyPageState {};
	state->home      = static_cast<uint8_t>(found.home);
	state->tile      = found.tile;
	state->committed = found.committed;
	state->armed     = found.armed;
	state->written   = found.written;
	state->views     = found.views;
	return DEUNIFY_OK;
}

void deunify_pages_counts(DeunifyContext* context, DeunifyHomeCounts* counts) {
	if (context != nullptr && counts != nullptr) {
		*counts = homeCountsOf(context->context->pages().counts());
	}
}

int32_t deunify_pages_handle_fault(DeunifyContext* context, void* exceptionPointers) {
	if (context == nullptr || exceptionPointers == nullptr) {
		return 0;
	}
	return context->context->pages().handleFault(static_cast<_EXCEPTION_POINTERS*>(exceptionPointers)) ? 1 : 0;
}

DeunifyStatus deunify_pages_take_written(DeunifyContext* context, uint64_t* pages, size_t capacity,
                                         size_t* count) {
	if (context == nullptr || count == nullptr || !hasPages(pages, capacity)) {
		return DEUNIFY_INVALID_ARGUMENT;
	}
	if (context->written.size() < capacity) {
		std::vector<PageNumber> taken;
		context->context->pages().takeWritten(taken);
		std::transform(taken.begin(), taken.end(), std::back_inserter(context->written),
		               [](PageNumber page) { return value(page); });
	}
	handOutWritten(context, pages, capacity, count);
	return DEUNIFY_OK;
}

uint32_t deunify_pages_park(DeunifyContext* context, uint64_t page) {
	if (context == nullptr) {
		return DEUNIFY_PARK_UNKNOWN;
	}
	return static_cast<uint32_t>(context->context->pages().park(PageNumber {page}));
}

DeunifyStatus deunify_pages_read_title(DeunifyContext* context, uint64_t address, void* out, uint64_t bytes,
                                       uint32_t purpose) {
	if (context == nullptr || out == nullptr || purpose > kLastPurpose) {
		return DEUNIFY_INVALID_ARGUMENT;
	}
	return toC(context->context->pages().readTitle(address, out, bytes, static_cast<ReadPurpose>(purpose)));
}

DeunifyStatus deunify_kernel_begin(DeunifyContext* context, uint64_t address, uint64_t bytes, uint32_t access,
                                   DeunifyKernelWrite** bracket) {
	if (bracket == nullptr) {
		return DEUNIFY_INVALID_ARGUMENT;
	}
	*bracket = nullptr;
	if (context == nullptr || access > kLastAccess) {
		return DEUNIFY_INVALID_ARGUMENT;
	}
	KernelWrite::Access kind = access == DEUNIFY_KERNEL_WRITE ? KernelWrite::Access::Write : KernelWrite::Access::Read;
	auto opened = std::unique_ptr<DeunifyKernelWrite>(
	    new DeunifyKernelWrite {KernelWrite(context->context->pages(), address, bytes, kind)});
	Status status = opened->bracket.status();
	if (status != Status::Ok) {
		return toC(status);
	}
	*bracket = opened.release();
	return DEUNIFY_OK;
}

void deunify_kernel_end(DeunifyKernelWrite* bracket) {
	delete bracket;
}

DeunifyStatus deunify_loads_open_handle(DeunifyContext* context, void* handle, uint64_t* file) {
	if (context == nullptr || file == nullptr) {
		return DEUNIFY_INVALID_ARGUMENT;
	}
	FileId opened = kNoFile;
	Status status = context->context->loads().openFile(handle, &opened);
	*file         = static_cast<uint64_t>(opened);
	return toC(status);
}

DeunifyStatus deunify_loads_open_path(DeunifyContext* context, const wchar_t* path, uint64_t* file) {
	if (context == nullptr || file == nullptr) {
		return DEUNIFY_INVALID_ARGUMENT;
	}
	FileId opened = kNoFile;
	Status status = context->context->loads().openFile(path, &opened);
	*file         = static_cast<uint64_t>(opened);
	return toC(status);
}

void deunify_loads_close_file(DeunifyContext* context, uint64_t file) {
	if (context != nullptr) {
		context->context->loads().closeFile(FileId {file});
	}
}

DeunifyStatus deunify_loads_load(DeunifyContext* context, const DeunifyLoadRequest* request) {
	if (context == nullptr || request == nullptr) {
		return DEUNIFY_INVALID_ARGUMENT;
	}
	if (request->version != DEUNIFY_ABI_VERSION) {
		return DEUNIFY_VERSION_MISMATCH;
	}
	if (!hasPages(request->pages, request->pageCount)) {
		return DEUNIFY_INVALID_ARGUMENT;
	}
	std::vector<PageNumber> pages = pagesOf(request->pages, request->pageCount);
	return toC(context->context->loads().load(requestOf(*request, pages)));
}

DeunifyStatus deunify_loads_write(DeunifyContext* context, const uint64_t* pages, size_t pageCount,
                                  uint32_t offsetInFirstPage, const void* bytes, uint64_t byteCount) {
	if (context == nullptr || !hasPages(pages, pageCount)) {
		return DEUNIFY_INVALID_ARGUMENT;
	}
	return toC(context->context->loads().write(pagesOf(pages, pageCount), offsetInFirstPage, bytes, byteCount));
}

int32_t deunify_loads_rebuildable(DeunifyContext* context, uint64_t page) {
	if (context == nullptr) {
		return 0;
	}
	return context->context->loads().rebuildable(PageNumber {page}) ? 1 : 0;
}

void deunify_loads_totals(DeunifyContext* context, DeunifyLoadTotals* totals) {
	if (context != nullptr && totals != nullptr) {
		*totals = loadTotalsOf(context->context->loads().totals());
	}
}

DeunifyStatus deunify_card_add_span(DeunifyContext* context, uint64_t gpuBase, uint64_t bytes, uint32_t home) {
	if (context == nullptr || home > kLastSpanHome) {
		return DEUNIFY_INVALID_ARGUMENT;
	}
	CardView* cardView = context->context->cardView();
	if (cardView == nullptr) {
		return DEUNIFY_UNAVAILABLE;
	}
	return toC(cardView->addSpan(gpuBase, bytes, static_cast<SpanHome>(home)));
}

DeunifyStatus deunify_card_buffer_for(DeunifyContext* context, uint64_t gpuAddress, uint64_t bytes,
                                      DeunifyBufferView* view) {
	if (context == nullptr || view == nullptr) {
		return DEUNIFY_INVALID_ARGUMENT;
	}
	CardView* cardView = context->context->cardView();
	if (cardView == nullptr) {
		return DEUNIFY_UNAVAILABLE;
	}
	BufferView found;
	Status     status = cardView->bufferFor(gpuAddress, bytes, &found);
	*view             = DeunifyBufferView {found.resource, found.offset, found.bytes};
	return toC(status);
}

DeunifyStatus deunify_card_shader_writes(DeunifyContext* context, uint64_t gpuAddress, uint64_t bytes) {
	if (context == nullptr) {
		return DEUNIFY_INVALID_ARGUMENT;
	}
	CardView* cardView = context->context->cardView();
	if (cardView == nullptr) {
		return DEUNIFY_UNAVAILABLE;
	}
	cardView->shaderWrites(gpuAddress, bytes);
	return DEUNIFY_OK;
}

DeunifyStatus deunify_card_before_submit(DeunifyContext* context) {
	if (context == nullptr) {
		return DEUNIFY_INVALID_ARGUMENT;
	}
	CardView* cardView = context->context->cardView();
	if (cardView == nullptr) {
		return DEUNIFY_UNAVAILABLE;
	}
	return toC(cardView->beforeSubmit());
}

uint32_t deunify_census_add_category(DeunifyContext* context, const char* name) {
	if (context == nullptr) {
		return kNoCategory;
	}
	return static_cast<uint32_t>(context->context->census().addCategory(name != nullptr ? name : ""));
}

DeunifyStatus deunify_census_track_heap(DeunifyContext* context, void* heap, uint32_t category) {
	if (context == nullptr) {
		return DEUNIFY_INVALID_ARGUMENT;
	}
	return toC(context->context->census().track(static_cast<ID3D12Heap*>(heap), Category {category}));
}

DeunifyStatus deunify_census_track_resource(DeunifyContext* context, void* device, void* resource,
                                            uint32_t category) {
	if (context == nullptr) {
		return DEUNIFY_INVALID_ARGUMENT;
	}
	return toC(context->context->census().track(static_cast<ID3D12Device*>(device),
	                                            static_cast<ID3D12Resource*>(resource), Category {category}));
}

void deunify_census_set_adapter(DeunifyContext* context, void* adapter) {
	if (context != nullptr) {
		context->context->census().setAdapter(static_cast<IDXGIAdapter3*>(adapter));
	}
}

DeunifyStatus deunify_census_totals(DeunifyContext* context, DeunifyCensusTotals* totals) {
	if (context == nullptr || totals == nullptr) {
		return DEUNIFY_INVALID_ARGUMENT;
	}
	if (totals->version != DEUNIFY_ABI_VERSION) {
		return DEUNIFY_VERSION_MISMATCH;
	}
	CensusSnapshot snapshot     = context->context->census().snapshot();
	*totals                     = DeunifyCensusTotals {};
	totals->version             = DEUNIFY_ABI_VERSION;
	totals->homes               = homeCountsOf(snapshot.homes);
	totals->loads               = loadTotalsOf(snapshot.loads);
	totals->pagesAllocated      = snapshot.physical.pages;
	totals->pagesCommitted      = snapshot.physical.committed;
	totals->tiles               = snapshot.cardView.tiles;
	totals->tileHeaps           = snapshot.cardView.heaps;
	totals->ramAndTileBytes     = snapshot.ramAndTileBytes;
	totals->tileAndTextureBytes = snapshot.tileAndTextureBytes;
	totals->localUsage          = snapshot.system.localUsage;
	totals->localBudget         = snapshot.system.localBudget;
	totals->nonLocalUsage       = snapshot.system.nonLocalUsage;
	totals->nonLocalBudget      = snapshot.system.nonLocalBudget;
	return DEUNIFY_OK;
}

DeunifyStatus deunify_census_text(DeunifyContext* context, char* text, size_t capacity, size_t* length) {
	if (context == nullptr || length == nullptr || (text == nullptr && capacity != 0)) {
		return DEUNIFY_INVALID_ARGUMENT;
	}
	std::string line = Census::format(context->context->census().snapshot());
	*length          = line.size();
	if (capacity != 0) {
		size_t copied = std::min(capacity - 1, line.size());
		std::memcpy(text, line.data(), copied);
		text[copied] = '\0';
	}
	return DEUNIFY_OK;
}

DeunifyStatus deunify_sites_load(const wchar_t* path, DeunifySites** sites) {
	if (path == nullptr || sites == nullptr) {
		return DEUNIFY_INVALID_ARGUMENT;
	}
	*sites                              = nullptr;
	Status                      status  = Status::Ok;
	std::optional<patch::Sites> loaded  = patch::Sites::load(path, &status);
	if (!loaded) {
		return toC(status);
	}
	*sites = new DeunifySites {std::move(*loaded)};
	return DEUNIFY_OK;
}

void deunify_sites_free(DeunifySites* sites) {
	delete sites;
}

DeunifyStatus deunify_sites_offset(const DeunifySites* sites, const char* name, uint64_t* offset) {
	if (sites == nullptr || name == nullptr || offset == nullptr) {
		return DEUNIFY_INVALID_ARGUMENT;
	}
	std::optional<uint64_t> found = sites->sites.offset(name);
	if (!found) {
		return DEUNIFY_NOT_FOUND;
	}
	*offset = *found;
	return DEUNIFY_OK;
}

void* deunify_patch_allocate_near(uint64_t near, uint64_t bytes) {
	return patch::allocateNear(near, bytes);
}

void deunify_patch_release_near(void* stub) {
	patch::releaseNear(static_cast<uint8_t*>(stub));
}

DeunifyStatus deunify_patch_write_code(void* at, const uint8_t* code, size_t bytes) {
	if (at == nullptr || (code == nullptr && bytes != 0)) {
		return DEUNIFY_INVALID_ARGUMENT;
	}
	return toC(patch::writeCode(static_cast<uint8_t*>(at), std::span<const uint8_t>(code, bytes)));
}

DeunifyStatus deunify_patch_redirect_call(uint8_t* imageBase, uint64_t imageBytes, uint64_t callOffset,
                                          uint64_t expectedTargetOffset, const uint8_t* stub, size_t stubBytes,
                                          uint64_t* originalTarget, void** stubAddress) {
	if (imageBase == nullptr || (stub == nullptr && stubBytes != 0)) {
		return DEUNIFY_INVALID_ARGUMENT;
	}
	patch::Redirect redirect;
	Status          status = patch::redirectCall(patch::Image {imageBase, imageBytes}, callOffset, expectedTargetOffset,
	                                             std::span<const uint8_t>(stub, stubBytes), &redirect);
	if (status == Status::Ok && originalTarget != nullptr) {
		*originalTarget = redirect.originalTarget;
	}
	if (status == Status::Ok && stubAddress != nullptr) {
		*stubAddress = redirect.stub;
	}
	return toC(status);
}

}
