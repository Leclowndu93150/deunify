#pragma once

#include "deunify/types.hpp"

#include <cstdint>
#include <memory>

struct ID3D12Device;
struct ID3D12CommandQueue;

namespace deunify {

class PhysicalMemory;
class GpuPageTable;
class Pages;
class Loads;
class CardView;
class Census;

enum class LogLevel : uint32_t {
	Error   = 0,
	Warning = 1,
	Info    = 2,
	Debug   = 3,
};

// Any thread, possibly with library locks held. Must not call back into deunify.
using LogFn = void (*)(void* context, LogLevel level, const char* text);

// Decode `in` with `codec` (kFirstCustomCodec or above) into exactly `outBytes`. Return Ok or
// DecodeFailed.
//
// This runs on decode workers and also on title threads inside the vectored exception handler,
// when a fault replays a File page. Keep it pure computation: no title memory, no locks the
// title might hold, no calls into deunify.
using DecodeFn = Status (*)(void* context, Codec codec, const uint8_t* in, size_t inBytes,
                            uint8_t* out, size_t outBytes);

// Bytes under `ranges` (GPU addresses) changed; meant for the texture cache. Called only from
// CardView::beforeSubmit or Context::deliverChanges, on that thread, with no lock held, so it
// may call back in (GpuPageTable::read, say).
using PagesChangedFn = void (*)(void* context, const Range* ranges, size_t count,
                                ChangeReason reason);

// The title allocation around `gpuAddress`, for CardView::bufferFor with bytes 0. Return false
// if you don't know.
using RegionOfFn = bool (*)(void* context, uint64_t gpuAddress, Range* region);

// Census only: does a host texture cover any of these bytes?
using TexturesOverFn = bool (*)(void* context, uint64_t gpuAddress, uint64_t bytes);

// All optional, except `decode` once a request uses a custom codec.
struct Callbacks {
	void*          context      = nullptr;
	LogFn          log          = nullptr;
	DecodeFn       decode       = nullptr;
	PagesChangedFn pagesChanged = nullptr;
	RegionOfFn     regionOf     = nullptr;
	TexturesOverFn texturesOver = nullptr;
};

struct Config {
	// Decoded load bytes kept for first readers. Past the cap the oldest pages are dropped and
	// will be replayed from the file.
	uint64_t stagingCapBytes = 512ull << 20;
	// Whole decoded requests, so the pages of one request share a decode.
	uint64_t decodedCapBytes = 128ull << 20;
	// A page written by more requests than this can't be rebuilt and is kept in RAM.
	uint32_t maxRecordsPerPage = 128;
	// 0 means half the logical processors, clamped to 2..8.
	uint32_t decodeWorkers = 0;
	// Both null: no card view. Card home never happens and GPU-only mappings become RAM views.
	// Both set: the card view lives on this device and puts all its work on this queue, which
	// has to be the queue that runs the title's work over card-view memory. Both are AddRef'd.
	ID3D12Device*       device = nullptr;
	ID3D12CommandQueue* queue  = nullptr;
};

// One per emulated title process. Component methods are callable from any thread unless their
// comment says otherwise; creating and destroying the context is not.
//
// Lock order. A thread only takes a lock while holding lower-numbered ones:
//   1. PhysicalMemory mapping (held across host mapping calls)
//   2. PhysicalMemory table
//   3. PhysicalMemory views
//   4. GpuPageTable
//   5. Pages
//   6. Loads: records, then staging, then decoded
//   7. CardView: queue order, then state
//   8. Census
// 2 and 3 are leaves: taken briefly under anything, nothing taken under them. Callbacks other
// than `log` run with no lock held.
//
// The library never touches a title address. It reaches page bytes through its own view of
// each section, so none of its threads can fault on title memory while holding a lock. Keep it
// that way if you change it.
class Context {
public:
	// Null on failure. NotSupported if a card view was asked for and the device lacks tiled
	// resources tier 2.
	static std::unique_ptr<Context> create(const Config& config, const Callbacks& callbacks,
	                                       Status* status = nullptr);

	~Context();
	Context(const Context&)            = delete;
	Context& operator=(const Context&) = delete;

	PhysicalMemory& physical();
	GpuPageTable&   gpuPages();
	Pages&          pages();
	Loads&          loads();
	CardView*       cardView(); // null without a device
	Census&         census();

	const Config&    config() const;
	const Callbacks& callbacks() const;

	// Runs Callbacks::pagesChanged for everything queued, on this thread. beforeSubmit does
	// this for you; without a card view, call it wherever your texture cache wants the news.
	void deliverChanges();

	// Win32 error or HRESULT behind this thread's last HostFailure, 0 if none.
	static uint32_t lastHostError();

	void log(LogLevel level, const char* text) const;

private:
	Context(const Config& config, const Callbacks& callbacks);

	friend class PhysicalMemory;
	friend class GpuPageTable;
	friend class Pages;
	friend class Loads;
	friend class CardView;
	friend class Census;

	// Safe with any locks held.
	void queueChange(Range gpuRange, ChangeReason reason);

	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace deunify
