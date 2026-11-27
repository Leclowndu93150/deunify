#pragma once

#include "deunify/types.hpp"

#include <vulkan/vulkan_core.h>

#include <memory>
#include <span>
#include <vector>

struct ID3D12Resource;

namespace deunify {

class Context;

// Where in the card view's buffers a GPU range sits: `bytes` from `offset` of one buffer on.
// On D3D12 the buffer is `resource`, not AddRef'd for you. On Vulkan it is `buffer`, and
// `deviceAddress` is the device address of `offset` in it.
struct BufferView {
	ID3D12Resource* resource      = nullptr;
	VkBuffer        buffer        = VK_NULL_HANDLE;
	uint64_t        deviceAddress = 0;
	uint64_t        offset        = 0;
	uint64_t        bytes         = 0;
};

// A span as one flat range of device addresses (Vulkan): GPU address `gpuBase + n` is device
// address `deviceAddress + n`.
struct SpanAddress {
	uint64_t gpuBase       = 0;
	uint64_t bytes         = 0;
	uint64_t deviceAddress = 0;
};

enum class SpanHome : uint8_t {
	// A tile is a copy while its page is Ram home; shaderWrites moves the page to Card.
	Shared = 0,
	// Every page that gets a tile becomes Card home at once. For memory only the GPU uses:
	// render-graph transients, UAV buffers.
	Card = 1,
};

// A D3D12 buffer view must start in the first 4 GiB of its buffer and hold at most 2^27
// elements. Segments of 4 GiB starting every 2 GiB mean any view under 2 GiB starts in the
// first half of some segment. On Vulkan a segment is as long as maxStorageBufferRange allows,
// at most 4 GiB, and they start every half segment.
inline constexpr uint64_t kSegmentStep     = 2ull << 30;
inline constexpr uint64_t kSegmentBytes    = 4ull << 30;
inline constexpr uint32_t kHeapTiles       = 1024; // 64 MiB heaps
inline constexpr uint64_t kLargestAskBytes = 256ull << 20;
inline constexpr uint64_t kUnsizedAskBytes = 4ull << 20;
inline constexpr uint32_t kMaxSpans        = 8;
inline constexpr uint32_t kSpareEmptyHeaps = 1;

// The title's GPU view, on the card, as tiles of reserved buffers.
//
// A slot gets its page's tile the first time a view asks for it, and every slot showing that
// page shares the one tile. Tiles come from the lowest heap and index first, so higher heaps
// drain; an empty heap is released once the queue is done with it, keeping kSpareEmptyHeaps.
//
// Nothing reaches the card until beforeSubmit. Tile mapping changes, home changes and fills
// are batched and go on Config::queue ahead of the title's lists (on Vulkan: bindings on the
// sparse queue, copies on the transfer queue after them). The one exception is a read-back,
// which any thread may put on the queue under the queue order lock; it then waits for the
// copy with no lock held.
class CardView {
public:
	CardView(const CardView&)            = delete;
	CardView& operator=(const CardView&) = delete;
	~CardView();

	// 64 KiB aligned, inside a GpuPageTable range, no overlaps. Buffers are made on first ask.
	Status addSpan(uint64_t gpuBase, uint64_t bytes, SpanHome home);

	// Call from the renderer when it decodes a buffer view (SRV, UAV, CBV, raw address). Ask
	// by the view's own range, never the whole buffer behind it, or you'll tile pages no view
	// can reach. With bytes 0 we ask Callbacks::regionOf, else take the aligned
	// kUnsizedAskBytes block around the address.
	//
	// TooLarge above kLargestAskBytes and NotFound outside every span: fall back to your own
	// path. OutOfMemory if no tile heap can be made. Slots the page table leaves unmapped read
	// as zero. The tiles are ready for lists submitted after the next beforeSubmit.
	Status bufferFor(uint64_t gpuAddress, uint64_t bytes, BufferView* view);

	// The span holding `gpuAddress` as one flat range of device addresses, so a translated
	// shader can turn a console pointer into a base and an offset. Vulkan only (NotSupported on
	// D3D12); NotFound outside every span. The addresses are backed where bufferFor gave tiles,
	// just like the views: ask for what a shader reads before you hand it the pointer.
	Status spanAddress(uint64_t gpuAddress, SpanAddress* address);

	// Whether a shader that reads a slot nobody mapped gets zeros. Always on D3D12; on Vulkan
	// only with residencyNonResidentStrict, and otherwise such reads are undefined.
	bool unboundReadsZero() const;

	// Shaders in later lists may write this range: every UAV and copy destination over title
	// memory, and every writable view a shader could pick from a heap at run time. Declaring
	// too much costs read-backs; declaring too little loses writes without a sound. The pages
	// become Card home at the next beforeSubmit and pagesChanged reports them as
	// ShaderWritten.
	void shaderWrites(uint64_t gpuAddress, uint64_t bytes);

	// On the submitting thread, right before ExecuteCommandLists of title work on
	// Config::queue. In order: retired heaps freed, orphaned Card pages read back to RAM, tile
	// mapping changes (one UpdateTileMappings per heap and buffer), home changes, fills, then
	// change notifications. Work that failed is kept for next time; the first failure is
	// returned.
	Status beforeSubmit();

	// lostPages above zero is a bug, not noise.
	struct Totals {
		uint64_t tiles              = 0;
		uint64_t heaps              = 0;
		uint64_t cardHomePages      = 0;
		uint64_t mappingCalls       = 0;
		uint64_t fillBytes          = 0;
		uint64_t readBacks          = 0;
		uint64_t refusedAsks        = 0;
		uint64_t tooLargeAsks       = 0;
		uint64_t lostPages          = 0;
		uint64_t tilesUnderTextures = 0; // filled in by Census::snapshot
	};
	Totals totals() const;

private:
	friend class Context;
	friend class GpuPageTable;
	friend class Pages;
	friend class Loads;
	friend class Census;

	explicit CardView(Context& context);

	Status start();

	// A slot already asked for moves to the new page's tile immediately.
	void slotChanged(uint64_t gpuAddress, PageNumber page);

	// Card home: a page goes into our own record of Card pages before we ask Pages to make it
	// Card home, so a read-back racing the change finds it. An orphan (last slot gone) keeps
	// its tile until its bytes are back in RAM.

	// Waits for the queue, unless the bytes are still on their way to the tile, in which case
	// those are returned directly.
	bool readBack(PageNumber page, uint8_t* out);

	// A CPU touch took the page to RAM; its tile is a copy again.
	void cardHomeLeft(PageNumber page);

	// True if the page has a tile and the bytes were queued for it; then they need no staging.
	bool loaded(PageNumber page, uint64_t offset, const uint8_t* bytes, uint64_t byteCount);

	void pageForgotten(PageNumber page);
	void tiledPages(std::vector<PageNumber>& out) const;

	struct Impl;
	Context&              m_context;
	std::unique_ptr<Impl> m_impl;
};

} // namespace deunify
