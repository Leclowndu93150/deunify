/* C ABI over the C++ API. Each function wraps the C++ method it is named after, and the rules
 * about threads, locks and homes are the ones in those headers. Functions return a
 * DeunifyStatus unless noted. Structs that may grow start with `version`; set it to
 * DEUNIFY_ABI_VERSION.
 *
 * Link deunify.lib, or deunify_static.lib with DEUNIFY_STATIC defined.
 */

#ifndef DEUNIFY_H
#define DEUNIFY_H

#include <stddef.h>
#include <stdint.h>

#if defined(DEUNIFY_STATIC)
#define DEUNIFY_API
#elif defined(DEUNIFY_BUILDING)
#define DEUNIFY_API __declspec(dllexport)
#else
#define DEUNIFY_API __declspec(dllimport)
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define DEUNIFY_ABI_VERSION 1u

#define DEUNIFY_PAGE_BYTES         0x10000ull
#define DEUNIFY_FRAMES_PER_PAGE    16ull
#define DEUNIFY_PAGE_NUMBER_LIMIT  0x2000000ull

typedef int32_t DeunifyStatus;
enum {
	DEUNIFY_OK               = 0,
	DEUNIFY_INVALID_ARGUMENT = 1,
	DEUNIFY_UNKNOWN_PAGE     = 2,
	DEUNIFY_NOT_PLACEHOLDER  = 3,
	DEUNIFY_OUT_OF_MEMORY    = 4,
	DEUNIFY_HOST_FAILURE     = 5,
	DEUNIFY_NO_SOURCE        = 6,
	DEUNIFY_DIFFERS          = 7,
	DEUNIFY_IN_USE           = 8,
	DEUNIFY_TOO_LARGE        = 9,
	DEUNIFY_NOT_SUPPORTED    = 10,
	DEUNIFY_DECODE_FAILED    = 11,
	DEUNIFY_UNAVAILABLE      = 12,
	DEUNIFY_NOT_FOUND        = 13,
	DEUNIFY_REFUSED          = 14,
	DEUNIFY_VERSION_MISMATCH = 15,
};

/* Home (deunify::Home) */
enum {
	DEUNIFY_HOME_ZERO    = 0,
	DEUNIFY_HOME_FILE    = 1,
	DEUNIFY_HOME_TEXTURE = 2,
	DEUNIFY_HOME_CARD    = 3,
	DEUNIFY_HOME_RAM     = 4,
};

/* Kind (deunify::Kind) */
enum {
	DEUNIFY_KIND_UNKNOWN  = 0,
	DEUNIFY_KIND_SHARED   = 1,
	DEUNIFY_KIND_CPU_ONLY = 2,
	DEUNIFY_KIND_GPU_ONLY = 3,
};

/* ReadPurpose */
enum {
	DEUNIFY_READ_INSPECT        = 0,
	DEUNIFY_READ_TEXTURE_UPLOAD = 1,
};

/* Codec; values from DEUNIFY_FIRST_CUSTOM_CODEC go to the decode callback */
enum {
	DEUNIFY_CODEC_NONE = 0,
	DEUNIFY_CODEC_ZLIB = 1,
};
#define DEUNIFY_FIRST_CUSTOM_CODEC 0x100u

/* ChangeReason */
enum {
	DEUNIFY_CHANGE_CPU_WRITTEN    = 0,
	DEUNIFY_CHANGE_SHADER_WRITTEN = 1,
	DEUNIFY_CHANGE_LOADED         = 2,
	DEUNIFY_CHANGE_REMAPPED       = 3,
};

/* LogLevel */
enum {
	DEUNIFY_LOG_ERROR   = 0,
	DEUNIFY_LOG_WARNING = 1,
	DEUNIFY_LOG_INFO    = 2,
	DEUNIFY_LOG_DEBUG   = 3,
};

/* PageRunKind */
enum {
	DEUNIFY_RUN_CONSECUTIVE = 0,
	DEUNIFY_RUN_REPEATED    = 1,
	DEUNIFY_RUN_UNMAPPED    = 2,
	DEUNIFY_RUN_UNCHANGED   = 3,
};

/* SpanHome */
enum {
	DEUNIFY_SPAN_SHARED = 0,
	DEUNIFY_SPAN_CARD   = 1,
};

/* KernelWrite::Access */
enum {
	DEUNIFY_KERNEL_READ  = 0,
	DEUNIFY_KERNEL_WRITE = 1,
};

/* ParkResult */
enum {
	DEUNIFY_PARKED             = 0,
	DEUNIFY_PARK_UNKNOWN       = 1,
	DEUNIFY_PARK_NO_SOURCE     = 2,
	DEUNIFY_PARK_DIFFERS       = 3,
	DEUNIFY_PARK_TOUCHED       = 4,
	DEUNIFY_PARK_IN_USE        = 5,
	DEUNIFY_PARK_NOT_WRITTEN   = 6,
};

typedef struct DeunifyContext     DeunifyContext;
typedef struct DeunifyKernelWrite DeunifyKernelWrite;
typedef struct DeunifySites       DeunifySites;

typedef struct DeunifyRange {
	uint64_t start;
	uint64_t bytes;
} DeunifyRange;

typedef void (*DeunifyLogFn)(void* context, uint32_t level, const char* text);
typedef DeunifyStatus (*DeunifyDecodeFn)(void* context, uint32_t codec, const uint8_t* in,
                                         size_t inBytes, uint8_t* out, size_t outBytes);
typedef void (*DeunifyPagesChangedFn)(void* context, const DeunifyRange* ranges, size_t count,
                                      uint32_t reason);
typedef int32_t (*DeunifyRegionOfFn)(void* context, uint64_t gpuAddress, DeunifyRange* region);
typedef int32_t (*DeunifyTexturesOverFn)(void* context, uint64_t gpuAddress, uint64_t bytes);

/* deunify::Callbacks */
typedef struct DeunifyCallbacks {
	uint32_t              version;
	uint32_t              reserved;
	void*                 context;
	DeunifyLogFn          log;
	DeunifyDecodeFn       decode;
	DeunifyPagesChangedFn pagesChanged;
	DeunifyRegionOfFn     regionOf;
	DeunifyTexturesOverFn texturesOver;
} DeunifyCallbacks;

/* deunify::Config; device and queue are ID3D12Device* and ID3D12CommandQueue*, or null */
typedef struct DeunifyConfig {
	uint32_t version;
	uint32_t maxRecordsPerPage;
	uint64_t stagingCapBytes;
	uint64_t decodedCapBytes;
	uint32_t decodeWorkers;
	uint32_t reserved;
	void*    device;
	void*    queue;
} DeunifyConfig;

/* Fills a DeunifyConfig with the defaults of deunify::Config. */
DEUNIFY_API void deunify_config_defaults(DeunifyConfig* config);

/* Context::create / destruction / deliverChanges / lastHostError */
DEUNIFY_API DeunifyStatus deunify_create(const DeunifyConfig* config,
                                         const DeunifyCallbacks* callbacks,
                                         DeunifyContext** context);
DEUNIFY_API void          deunify_destroy(DeunifyContext* context);
DEUNIFY_API void          deunify_deliver_changes(DeunifyContext* context);
DEUNIFY_API uint32_t      deunify_last_host_error(void);

/* ---- PhysicalMemory ---- */

typedef struct DeunifyPageAt {
	uint64_t page;
	uint64_t offset;
} DeunifyPageAt;

DEUNIFY_API DeunifyStatus deunify_physical_allocate(DeunifyContext* context, uint64_t count,
                                                    uint64_t flags, uint64_t* pages);
DEUNIFY_API DeunifyStatus deunify_physical_free(DeunifyContext* context, const uint64_t* pages,
                                                size_t count);
DEUNIFY_API DeunifyStatus deunify_physical_map(DeunifyContext* context, uint64_t address,
                                               const uint64_t* pages, size_t count);
DEUNIFY_API DeunifyStatus deunify_physical_map_scatter(DeunifyContext* context,
                                                       const uint64_t* addresses,
                                                       const uint64_t* pages, size_t count);
DEUNIFY_API DeunifyStatus deunify_physical_unmap(DeunifyContext* context, uint64_t address,
                                                 uint64_t bytes);
/* DEUNIFY_NOT_FOUND if no deunify view covers the address. */
DEUNIFY_API DeunifyStatus deunify_physical_page_at(DeunifyContext* context, uint64_t address,
                                                   DeunifyPageAt* at);
/* Writes up to `capacity` addresses; `*count` is the total (may exceed capacity). */
DEUNIFY_API DeunifyStatus deunify_physical_views_of(DeunifyContext* context, uint64_t page,
                                                    uint64_t* addresses, size_t capacity,
                                                    size_t* count);

/* ---- GpuPageTable ---- */

typedef struct DeunifyPageRun {
	uint32_t kind;
	uint32_t poolPage;
	uint32_t count;
	uint32_t reserved;
} DeunifyPageRun;

typedef struct DeunifyPageBatch {
	uint64_t              gpuAddress;
	const DeunifyPageRun* runs;
	size_t                runCount;
} DeunifyPageBatch;

DEUNIFY_API DeunifyStatus deunify_gpu_follow_cpu_views(DeunifyContext* context, uint64_t base,
                                                       uint64_t bytes);
DEUNIFY_API DeunifyStatus deunify_gpu_add_range(DeunifyContext* context, uint64_t base,
                                                uint64_t bytes);
DEUNIFY_API DeunifyStatus deunify_gpu_copy_page_mappings(DeunifyContext* context,
                                                         const DeunifyPageBatch* batches,
                                                         size_t count, uint64_t poolAddress,
                                                         uint32_t poolPages);
DEUNIFY_API DeunifyStatus deunify_gpu_map(DeunifyContext* context, uint64_t gpuAddress,
                                          const uint64_t* pages, size_t count);
DEUNIFY_API DeunifyStatus deunify_gpu_unmap(DeunifyContext* context, uint64_t gpuAddress,
                                            uint64_t bytes);
DEUNIFY_API DeunifyStatus deunify_gpu_page_at(DeunifyContext* context, uint64_t gpuAddress,
                                              DeunifyPageAt* at);
DEUNIFY_API DeunifyStatus deunify_gpu_read(DeunifyContext* context, uint64_t gpuAddress,
                                           void* out, uint64_t bytes, uint32_t purpose);

/* ---- Pages ---- */

typedef struct DeunifyPageState {
	uint8_t  home;
	uint8_t  tile;
	uint8_t  committed;
	uint8_t  armed;
	uint8_t  written;
	uint8_t  reserved[3];
	uint32_t views;
	uint32_t reserved2;
} DeunifyPageState;

typedef struct DeunifyHomeCounts {
	uint64_t zero;
	uint64_t file;
	uint64_t texture;
	uint64_t card;
	uint64_t ram;
	uint64_t ramTiled;
	uint64_t parked;
	uint64_t faults;
	uint64_t readBacks;
	uint64_t replays;
} DeunifyHomeCounts;

DEUNIFY_API void          deunify_pages_set_kind(DeunifyContext* context, uint64_t address,
                                                 uint64_t bytes, uint32_t kind);
DEUNIFY_API uint32_t      deunify_pages_kind_at(DeunifyContext* context, uint64_t address);
DEUNIFY_API DeunifyStatus deunify_pages_state(DeunifyContext* context, uint64_t page,
                                              DeunifyPageState* state);
DEUNIFY_API void          deunify_pages_counts(DeunifyContext* context, DeunifyHomeCounts* counts);
/* Pages::handleFault. `exceptionPointers` is the EXCEPTION_POINTERS* the vectored handler got.
 * Returns 1 if the access may be retried, 0 otherwise. */
DEUNIFY_API int32_t       deunify_pages_handle_fault(DeunifyContext* context,
                                                     void* exceptionPointers);
/* Pages::takeWritten. Writes up to `capacity` page numbers; `*count` is how many were taken.
 * Pages that did not fit stay listed for the next call. */
DEUNIFY_API DeunifyStatus deunify_pages_take_written(DeunifyContext* context, uint64_t* pages,
                                                     size_t capacity, size_t* count);
/* Returns a DEUNIFY_PARK_* value. */
DEUNIFY_API uint32_t      deunify_pages_park(DeunifyContext* context, uint64_t page);
DEUNIFY_API DeunifyStatus deunify_pages_read_title(DeunifyContext* context, uint64_t address,
                                                   void* out, uint64_t bytes, uint32_t purpose);

/* KernelWrite: begin before the kernel call, end right after, on the same thread. On failure
 * `*bracket` is null and the kernel call must not be made. */
DEUNIFY_API DeunifyStatus deunify_kernel_begin(DeunifyContext* context, uint64_t address,
                                               uint64_t bytes, uint32_t access,
                                               DeunifyKernelWrite** bracket);
DEUNIFY_API void          deunify_kernel_end(DeunifyKernelWrite* bracket);

/* ---- Loads ---- */

typedef struct DeunifyLoadRequest {
	uint32_t        version;
	uint32_t        offsetInFirstPage;
	uint64_t        file;
	uint64_t        fileOffset;
	uint32_t        sourceBytes;
	uint32_t        firstCodec;
	uint32_t        secondCodec;
	uint32_t        intermediateBytes;
	uint32_t        destinationBytes;
	uint32_t        reserved;
	const uint64_t* pages;
	size_t          pageCount;
} DeunifyLoadRequest;

typedef struct DeunifyLoadTotals {
	uint64_t stagedBytes;
	uint64_t decodedBytes;
	uint64_t requestsLoaded;
	uint64_t requestsDecoded;
	uint64_t pagesReplayed;
	uint64_t stagedReads;
} DeunifyLoadTotals;

/* `handle` is a Win32 HANDLE; it is duplicated. */
DEUNIFY_API DeunifyStatus deunify_loads_open_handle(DeunifyContext* context, void* handle,
                                                    uint64_t* file);
DEUNIFY_API DeunifyStatus deunify_loads_open_path(DeunifyContext* context, const wchar_t* path,
                                                  uint64_t* file);
DEUNIFY_API void          deunify_loads_close_file(DeunifyContext* context, uint64_t file);
DEUNIFY_API DeunifyStatus deunify_loads_load(DeunifyContext* context,
                                             const DeunifyLoadRequest* request);
DEUNIFY_API DeunifyStatus deunify_loads_write(DeunifyContext* context, const uint64_t* pages,
                                              size_t pageCount, uint32_t offsetInFirstPage,
                                              const void* bytes, uint64_t byteCount);
DEUNIFY_API int32_t       deunify_loads_rebuildable(DeunifyContext* context, uint64_t page);
DEUNIFY_API void          deunify_loads_totals(DeunifyContext* context, DeunifyLoadTotals* totals);

/* ---- CardView (DEUNIFY_UNAVAILABLE without a device in the config) ---- */

typedef struct DeunifyBufferView {
	void*    resource; /* ID3D12Resource*, not referenced for the caller */
	uint64_t offset;
	uint64_t bytes;
} DeunifyBufferView;

DEUNIFY_API DeunifyStatus deunify_card_add_span(DeunifyContext* context, uint64_t gpuBase,
                                                uint64_t bytes, uint32_t home);
DEUNIFY_API DeunifyStatus deunify_card_buffer_for(DeunifyContext* context, uint64_t gpuAddress,
                                                  uint64_t bytes, DeunifyBufferView* view);
DEUNIFY_API DeunifyStatus deunify_card_shader_writes(DeunifyContext* context,
                                                     uint64_t gpuAddress, uint64_t bytes);
DEUNIFY_API DeunifyStatus deunify_card_before_submit(DeunifyContext* context);

/* ---- Census ---- */

typedef struct DeunifyCensusTotals {
	uint32_t          version;
	uint32_t          reserved;
	DeunifyHomeCounts homes;
	DeunifyLoadTotals loads;
	uint64_t          pagesAllocated;
	uint64_t          pagesCommitted;
	uint64_t          tiles;
	uint64_t          tileHeaps;
	uint64_t          ramAndTileBytes;
	uint64_t          tileAndTextureBytes;
	uint64_t          localUsage;
	uint64_t          localBudget;
	uint64_t          nonLocalUsage;
	uint64_t          nonLocalBudget;
} DeunifyCensusTotals;

DEUNIFY_API uint32_t      deunify_census_add_category(DeunifyContext* context, const char* name);
DEUNIFY_API DeunifyStatus deunify_census_track_heap(DeunifyContext* context, void* heap,
                                                    uint32_t category);
DEUNIFY_API DeunifyStatus deunify_census_track_resource(DeunifyContext* context, void* device,
                                                        void* resource, uint32_t category);
/* `adapter` is an IDXGIAdapter3*. */
DEUNIFY_API void          deunify_census_set_adapter(DeunifyContext* context, void* adapter);
DEUNIFY_API DeunifyStatus deunify_census_totals(DeunifyContext* context,
                                                DeunifyCensusTotals* totals);
/* Writes the census line (Census::format) into `text`, NUL-terminated and cut at `capacity`;
 * `*length` is the full length without the NUL. */
DEUNIFY_API DeunifyStatus deunify_census_text(DeunifyContext* context, char* text,
                                              size_t capacity, size_t* length);

/* ---- Patch ---- */

DEUNIFY_API DeunifyStatus deunify_sites_load(const wchar_t* path, DeunifySites** sites);
DEUNIFY_API void          deunify_sites_free(DeunifySites* sites);
DEUNIFY_API DeunifyStatus deunify_sites_offset(const DeunifySites* sites, const char* name,
                                               uint64_t* offset);
DEUNIFY_API void*         deunify_patch_allocate_near(uint64_t near, uint64_t bytes);
DEUNIFY_API void          deunify_patch_release_near(void* stub);
DEUNIFY_API DeunifyStatus deunify_patch_write_code(void* at, const uint8_t* code, size_t bytes);
DEUNIFY_API DeunifyStatus deunify_patch_redirect_call(uint8_t* imageBase, uint64_t imageBytes,
                                                      uint64_t callOffset,
                                                      uint64_t expectedTargetOffset,
                                                      const uint8_t* stub, size_t stubBytes,
                                                      uint64_t* originalTarget,
                                                      void** stubAddress);

#ifdef __cplusplus
}
#endif

#endif
