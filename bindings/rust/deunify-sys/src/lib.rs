//! Raw bindings to deunify's C ABI (`include/deunify.h`), written by hand. Keep in step with the
//! header: every structure is `#[repr(C)]` with the same fields in the same order.

#![allow(non_camel_case_types, non_snake_case)]

use std::ffi::{c_char, c_void};

pub const DEUNIFY_ABI_VERSION: u32 = 1;
pub const DEUNIFY_PAGE_BYTES: u64 = 0x10000;
pub const DEUNIFY_FRAMES_PER_PAGE: u64 = 16;
pub const DEUNIFY_PAGE_NUMBER_LIMIT: u64 = 0x200_0000;
pub const DEUNIFY_FIRST_CUSTOM_CODEC: u32 = 0x100;

pub type DeunifyStatus = i32;

pub const DEUNIFY_OK: DeunifyStatus = 0;
pub const DEUNIFY_INVALID_ARGUMENT: DeunifyStatus = 1;
pub const DEUNIFY_UNKNOWN_PAGE: DeunifyStatus = 2;
pub const DEUNIFY_NOT_PLACEHOLDER: DeunifyStatus = 3;
pub const DEUNIFY_OUT_OF_MEMORY: DeunifyStatus = 4;
pub const DEUNIFY_HOST_FAILURE: DeunifyStatus = 5;
pub const DEUNIFY_NO_SOURCE: DeunifyStatus = 6;
pub const DEUNIFY_DIFFERS: DeunifyStatus = 7;
pub const DEUNIFY_IN_USE: DeunifyStatus = 8;
pub const DEUNIFY_TOO_LARGE: DeunifyStatus = 9;
pub const DEUNIFY_NOT_SUPPORTED: DeunifyStatus = 10;
pub const DEUNIFY_DECODE_FAILED: DeunifyStatus = 11;
pub const DEUNIFY_UNAVAILABLE: DeunifyStatus = 12;
pub const DEUNIFY_NOT_FOUND: DeunifyStatus = 13;
pub const DEUNIFY_REFUSED: DeunifyStatus = 14;
pub const DEUNIFY_VERSION_MISMATCH: DeunifyStatus = 15;
pub const DEUNIFY_HOME_ZERO: u32 = 0;
pub const DEUNIFY_HOME_FILE: u32 = 1;
pub const DEUNIFY_HOME_TEXTURE: u32 = 2;
pub const DEUNIFY_HOME_CARD: u32 = 3;
pub const DEUNIFY_HOME_RAM: u32 = 4;
pub const DEUNIFY_KIND_UNKNOWN: u32 = 0;
pub const DEUNIFY_KIND_SHARED: u32 = 1;
pub const DEUNIFY_KIND_CPU_ONLY: u32 = 2;
pub const DEUNIFY_KIND_GPU_ONLY: u32 = 3;
pub const DEUNIFY_READ_INSPECT: u32 = 0;
pub const DEUNIFY_READ_TEXTURE_UPLOAD: u32 = 1;
pub const DEUNIFY_CODEC_NONE: u32 = 0;
pub const DEUNIFY_CODEC_ZLIB: u32 = 1;
pub const DEUNIFY_CHANGE_CPU_WRITTEN: u32 = 0;
pub const DEUNIFY_CHANGE_SHADER_WRITTEN: u32 = 1;
pub const DEUNIFY_CHANGE_LOADED: u32 = 2;
pub const DEUNIFY_CHANGE_REMAPPED: u32 = 3;
pub const DEUNIFY_LOG_ERROR: u32 = 0;
pub const DEUNIFY_LOG_WARNING: u32 = 1;
pub const DEUNIFY_LOG_INFO: u32 = 2;
pub const DEUNIFY_LOG_DEBUG: u32 = 3;
pub const DEUNIFY_RUN_CONSECUTIVE: u32 = 0;
pub const DEUNIFY_RUN_REPEATED: u32 = 1;
pub const DEUNIFY_RUN_UNMAPPED: u32 = 2;
pub const DEUNIFY_RUN_UNCHANGED: u32 = 3;
pub const DEUNIFY_SPAN_SHARED: u32 = 0;
pub const DEUNIFY_SPAN_CARD: u32 = 1;
pub const DEUNIFY_KERNEL_READ: u32 = 0;
pub const DEUNIFY_KERNEL_WRITE: u32 = 1;
pub const DEUNIFY_PARKED: u32 = 0;
pub const DEUNIFY_PARK_UNKNOWN: u32 = 1;
pub const DEUNIFY_PARK_NO_SOURCE: u32 = 2;
pub const DEUNIFY_PARK_DIFFERS: u32 = 3;
pub const DEUNIFY_PARK_TOUCHED: u32 = 4;
pub const DEUNIFY_PARK_IN_USE: u32 = 5;
pub const DEUNIFY_PARK_NOT_WRITTEN: u32 = 6;

#[repr(C)]
pub struct DeunifyContext {
    _private: [u8; 0],
}

#[repr(C)]
pub struct DeunifyKernelWrite {
    _private: [u8; 0],
}

#[repr(C)]
pub struct DeunifySites {
    _private: [u8; 0],
}

pub type DeunifyLogFn = Option<unsafe extern "C" fn(context: *mut c_void, level: u32, text: *const c_char)>;
pub type DeunifyDecodeFn = Option<unsafe extern "C" fn(context: *mut c_void, codec: u32, input: *const u8, input_bytes: usize, output: *mut u8, output_bytes: usize) -> DeunifyStatus>;
pub type DeunifyPagesChangedFn = Option<unsafe extern "C" fn(context: *mut c_void, ranges: *const DeunifyRange, count: usize, reason: u32)>;
pub type DeunifyRegionOfFn = Option<unsafe extern "C" fn(context: *mut c_void, gpuAddress: u64, region: *mut DeunifyRange) -> i32>;
pub type DeunifyTexturesOverFn = Option<unsafe extern "C" fn(context: *mut c_void, gpuAddress: u64, bytes: u64) -> i32>;

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct DeunifyRange {
    pub start: u64,
    pub bytes: u64,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct DeunifyCallbacks {
    pub version: u32,
    pub reserved: u32,
    pub context: *mut c_void,
    pub log: DeunifyLogFn,
    pub decode: DeunifyDecodeFn,
    pub pagesChanged: DeunifyPagesChangedFn,
    pub regionOf: DeunifyRegionOfFn,
    pub texturesOver: DeunifyTexturesOverFn,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct DeunifyConfig {
    pub version: u32,
    pub maxRecordsPerPage: u32,
    pub stagingCapBytes: u64,
    pub decodedCapBytes: u64,
    pub decodeWorkers: u32,
    pub reserved: u32,
    pub device: *mut c_void,
    pub queue: *mut c_void,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct DeunifyPageAt {
    pub page: u64,
    pub offset: u64,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct DeunifyPageRun {
    pub kind: u32,
    pub poolPage: u32,
    pub count: u32,
    pub reserved: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct DeunifyPageBatch {
    pub gpuAddress: u64,
    pub runs: *const DeunifyPageRun,
    pub runCount: usize,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct DeunifyPageState {
    pub home: u8,
    pub tile: u8,
    pub committed: u8,
    pub armed: u8,
    pub written: u8,
    pub reserved: [u8; 3],
    pub views: u32,
    pub reserved2: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct DeunifyHomeCounts {
    pub zero: u64,
    pub file: u64,
    pub texture: u64,
    pub card: u64,
    pub ram: u64,
    pub ramTiled: u64,
    pub parked: u64,
    pub faults: u64,
    pub readBacks: u64,
    pub replays: u64,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct DeunifyLoadRequest {
    pub version: u32,
    pub offsetInFirstPage: u32,
    pub file: u64,
    pub fileOffset: u64,
    pub sourceBytes: u32,
    pub firstCodec: u32,
    pub secondCodec: u32,
    pub intermediateBytes: u32,
    pub destinationBytes: u32,
    pub reserved: u32,
    pub pages: *const u64,
    pub pageCount: usize,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct DeunifyLoadTotals {
    pub stagedBytes: u64,
    pub decodedBytes: u64,
    pub requestsLoaded: u64,
    pub requestsDecoded: u64,
    pub pagesReplayed: u64,
    pub stagedReads: u64,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct DeunifyBufferView {
    /// ID3D12Resource*, not referenced for the caller.
    pub resource: *mut c_void,
    pub offset: u64,
    pub bytes: u64,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct DeunifyCensusTotals {
    pub version: u32,
    pub reserved: u32,
    pub homes: DeunifyHomeCounts,
    pub loads: DeunifyLoadTotals,
    pub pagesAllocated: u64,
    pub pagesCommitted: u64,
    pub tiles: u64,
    pub tileHeaps: u64,
    pub ramAndTileBytes: u64,
    pub tileAndTextureBytes: u64,
    pub localUsage: u64,
    pub localBudget: u64,
    pub nonLocalUsage: u64,
    pub nonLocalBudget: u64,
}

unsafe extern "C" {
    pub fn deunify_config_defaults(config: *mut DeunifyConfig);
    pub fn deunify_create(config: *const DeunifyConfig, callbacks: *const DeunifyCallbacks, context: *mut *mut DeunifyContext) -> DeunifyStatus;
    pub fn deunify_destroy(context: *mut DeunifyContext);
    pub fn deunify_deliver_changes(context: *mut DeunifyContext);
    pub fn deunify_last_host_error() -> u32;
    pub fn deunify_physical_allocate(context: *mut DeunifyContext, count: u64, flags: u64, pages: *mut u64) -> DeunifyStatus;
    pub fn deunify_physical_free(context: *mut DeunifyContext, pages: *const u64, count: usize) -> DeunifyStatus;
    pub fn deunify_physical_map(context: *mut DeunifyContext, address: u64, pages: *const u64, count: usize) -> DeunifyStatus;
    pub fn deunify_physical_map_scatter(context: *mut DeunifyContext, addresses: *const u64, pages: *const u64, count: usize) -> DeunifyStatus;
    pub fn deunify_physical_unmap(context: *mut DeunifyContext, address: u64, bytes: u64) -> DeunifyStatus;
    pub fn deunify_physical_page_at(context: *mut DeunifyContext, address: u64, at: *mut DeunifyPageAt) -> DeunifyStatus;
    pub fn deunify_physical_views_of(context: *mut DeunifyContext, page: u64, addresses: *mut u64, capacity: usize, count: *mut usize) -> DeunifyStatus;
    pub fn deunify_gpu_follow_cpu_views(context: *mut DeunifyContext, base: u64, bytes: u64) -> DeunifyStatus;
    pub fn deunify_gpu_add_range(context: *mut DeunifyContext, base: u64, bytes: u64) -> DeunifyStatus;
    pub fn deunify_gpu_copy_page_mappings(context: *mut DeunifyContext, batches: *const DeunifyPageBatch, count: usize, poolAddress: u64, poolPages: u32) -> DeunifyStatus;
    pub fn deunify_gpu_map(context: *mut DeunifyContext, gpuAddress: u64, pages: *const u64, count: usize) -> DeunifyStatus;
    pub fn deunify_gpu_unmap(context: *mut DeunifyContext, gpuAddress: u64, bytes: u64) -> DeunifyStatus;
    pub fn deunify_gpu_page_at(context: *mut DeunifyContext, gpuAddress: u64, at: *mut DeunifyPageAt) -> DeunifyStatus;
    pub fn deunify_gpu_read(context: *mut DeunifyContext, gpuAddress: u64, out: *mut c_void, bytes: u64, purpose: u32) -> DeunifyStatus;
    pub fn deunify_pages_set_kind(context: *mut DeunifyContext, address: u64, bytes: u64, kind: u32);
    pub fn deunify_pages_kind_at(context: *mut DeunifyContext, address: u64) -> u32;
    pub fn deunify_pages_state(context: *mut DeunifyContext, page: u64, state: *mut DeunifyPageState) -> DeunifyStatus;
    pub fn deunify_pages_counts(context: *mut DeunifyContext, counts: *mut DeunifyHomeCounts);
    pub fn deunify_pages_handle_fault(context: *mut DeunifyContext, exceptionPointers: *mut c_void) -> i32;
    pub fn deunify_pages_take_written(context: *mut DeunifyContext, pages: *mut u64, capacity: usize, count: *mut usize) -> DeunifyStatus;
    pub fn deunify_pages_park(context: *mut DeunifyContext, page: u64) -> u32;
    pub fn deunify_pages_read_title(context: *mut DeunifyContext, address: u64, out: *mut c_void, bytes: u64, purpose: u32) -> DeunifyStatus;
    pub fn deunify_kernel_begin(context: *mut DeunifyContext, address: u64, bytes: u64, access: u32, bracket: *mut *mut DeunifyKernelWrite) -> DeunifyStatus;
    pub fn deunify_kernel_end(bracket: *mut DeunifyKernelWrite);
    pub fn deunify_loads_open_handle(context: *mut DeunifyContext, handle: *mut c_void, file: *mut u64) -> DeunifyStatus;
    pub fn deunify_loads_open_path(context: *mut DeunifyContext, path: *const u16, file: *mut u64) -> DeunifyStatus;
    pub fn deunify_loads_close_file(context: *mut DeunifyContext, file: u64);
    pub fn deunify_loads_load(context: *mut DeunifyContext, request: *const DeunifyLoadRequest) -> DeunifyStatus;
    pub fn deunify_loads_write(context: *mut DeunifyContext, pages: *const u64, pageCount: usize, offsetInFirstPage: u32, bytes: *const c_void, byteCount: u64) -> DeunifyStatus;
    pub fn deunify_loads_rebuildable(context: *mut DeunifyContext, page: u64) -> i32;
    pub fn deunify_loads_totals(context: *mut DeunifyContext, totals: *mut DeunifyLoadTotals);
    pub fn deunify_card_add_span(context: *mut DeunifyContext, gpuBase: u64, bytes: u64, home: u32) -> DeunifyStatus;
    pub fn deunify_card_buffer_for(context: *mut DeunifyContext, gpuAddress: u64, bytes: u64, view: *mut DeunifyBufferView) -> DeunifyStatus;
    pub fn deunify_card_shader_writes(context: *mut DeunifyContext, gpuAddress: u64, bytes: u64) -> DeunifyStatus;
    pub fn deunify_card_before_submit(context: *mut DeunifyContext) -> DeunifyStatus;
    pub fn deunify_census_add_category(context: *mut DeunifyContext, name: *const c_char) -> u32;
    pub fn deunify_census_track_heap(context: *mut DeunifyContext, heap: *mut c_void, category: u32) -> DeunifyStatus;
    pub fn deunify_census_track_resource(context: *mut DeunifyContext, device: *mut c_void, resource: *mut c_void, category: u32) -> DeunifyStatus;
    pub fn deunify_census_set_adapter(context: *mut DeunifyContext, adapter: *mut c_void);
    pub fn deunify_census_totals(context: *mut DeunifyContext, totals: *mut DeunifyCensusTotals) -> DeunifyStatus;
    pub fn deunify_census_text(context: *mut DeunifyContext, text: *mut c_char, capacity: usize, length: *mut usize) -> DeunifyStatus;
    pub fn deunify_sites_load(path: *const u16, sites: *mut *mut DeunifySites) -> DeunifyStatus;
    pub fn deunify_sites_free(sites: *mut DeunifySites);
    pub fn deunify_sites_offset(sites: *const DeunifySites, name: *const c_char, offset: *mut u64) -> DeunifyStatus;
    pub fn deunify_patch_allocate_near(near: u64, bytes: u64) -> *mut c_void;
    pub fn deunify_patch_release_near(stub: *mut c_void);
    pub fn deunify_patch_write_code(at: *mut c_void, code: *const u8, bytes: usize) -> DeunifyStatus;
    pub fn deunify_patch_redirect_call(imageBase: *mut u8, imageBytes: u64, callOffset: u64, expectedTargetOffset: u64, stub: *const u8, stubBytes: usize, originalTarget: *mut u64, stubAddress: *mut *mut c_void) -> DeunifyStatus;
}
