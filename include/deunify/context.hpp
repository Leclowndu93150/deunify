#pragma once

#include "deunify/types.hpp"

#include <vulkan/vulkan_core.h>

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

// Vulkan only. A VkQueue may be used by one thread at a time, and the card view submits from
// any thread (a read-back comes from whichever thread touched the page). If you submit to the
// queues you gave us as well, take your lock for `queue` when `lock` is true and drop it when
// it is false. We call it around each vkQueueSubmit and vkQueueBindSparse, with card view
// locks held, so don't call back into deunify from it.
using QueueLockFn = void (*)(void* context, VkQueue queue, bool lock);

// All optional, except `decode` once a request uses a custom codec.
struct Callbacks {
	void*          context      = nullptr;
	LogFn          log          = nullptr;
	DecodeFn       decode       = nullptr;
	PagesChangedFn pagesChanged = nullptr;
	RegionOfFn     regionOf     = nullptr;
	TexturesOverFn texturesOver = nullptr;
	QueueLockFn    queueLock    = nullptr;
};

// A Vulkan device the emulator made, for a card view on Vulkan. The device must be Vulkan 1.2
// or later, created with the sparseBinding, sparseResidencyBuffer and bufferDeviceAddress
// features. deunify loads every function it needs through getInstanceProcAddr and never links
// the loader.
struct VulkanSetup {
	VkInstance                instance            = VK_NULL_HANDLE;
	VkPhysicalDevice          physicalDevice      = VK_NULL_HANDLE;
	VkDevice                  device              = VK_NULL_HANDLE;
	PFN_vkGetInstanceProcAddr getInstanceProcAddr = nullptr;
	// Where tile bindings go. Its family has VK_QUEUE_SPARSE_BINDING_BIT. It may be the
	// transfer queue.
	VkQueue  sparseQueue  = VK_NULL_HANDLE;
	uint32_t sparseFamily = 0;
	// Fills and read-backs. Like Config::queue on D3D12, this has to be the queue the title's
	// work over card-view memory runs on: that is what orders our copies against it.
	VkQueue  transferQueue  = VK_NULL_HANDLE;
	uint32_t transferFamily = 0;
	// True if the device was created with the timelineSemaphore feature. Without it we wait
	// for each batch of bindings on the CPU before anything that needs them is submitted.
	bool timelineSemaphores = false;
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
	// Both null and no Vulkan device: no card view. Card home never happens and GPU-only
	// mappings become RAM views. Both set: the card view lives on this D3D12 device and puts all
	// its work on this queue, which has to be the queue that runs the title's work over
	// card-view memory. Both are AddRef'd.
	ID3D12Device*       device = nullptr;
	ID3D12CommandQueue* queue  = nullptr;
	// Or vulkan.device set (with the rest of it) and the two above null: the card view on Vulkan.
	// Nothing in it is referenced or destroyed for you; keep it alive past the context.
	VulkanSetup vulkan;
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
	// Null on failure. NotSupported if a card view was asked for and the device can't hold one:
	// on D3D12 no tiled resources tier 2, on Vulkan no sparse residency for buffers, no buffer
	// device address, or a sparse block size that doesn't divide 64 KiB.
	static std::unique_ptr<Context> create(const Config& config, const Callbacks& callbacks,
	                                       Status* status = nullptr);

	~Context();
	Context(const Context&)            = delete;
	Context& operator=(const Context&) = delete;

	PhysicalMemory& physical();
	GpuPageTable&   gpuPages();
	Pages&          pages();
	Loads&          loads();
	CardView*       cardView(); // null without a D3D12 or Vulkan device
	Census&         census();

	const Config&    config() const;
	const Callbacks& callbacks() const;

	// Runs Callbacks::pagesChanged for everything queued, on this thread. beforeSubmit does
	// this for you; without a card view, call it wherever your texture cache wants the news.
	void deliverChanges();

	// Win32 error, HRESULT or VkResult behind this thread's last HostFailure, 0 if none.
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
