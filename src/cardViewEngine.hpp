// The card view's machinery. It reaches Pages, Loads, Census and Context through one
// interface so the tests can drive it alone.
//
// Locks: the queue order lock (held while the card view puts work on the queue: tile mapping
// changes, fills and read-back copies, never while waiting for the card) is taken before the
// state lock (slots, tiles, heaps, pending work; short). Surroundings are called with neither
// held, except heapMade and bufferMade. A read-back takes what it needs under the locks,
// waits with none held, then checks that the page did not change meanwhile and retries if
// it did.

#pragma once

#include "deunify/cardView.hpp"

#include <compare>
#include <d3d12.h>
#include <memory>
#include <optional>
#include <span>
#include <vector>
#include <wrl/client.h>

namespace deunify::cardview {

using Microsoft::WRL::ComPtr;

/// One 64 KiB tile of a tile heap.
struct Tile {
	uint32_t heap  = 0;
	uint32_t index = 0;

	auto operator<=>(const Tile&) const = default;
};

/// A reserved buffer of a span: `bytes` from `offset` of the span on.
struct Segment {
	uint64_t offset = 0;
	uint64_t bytes  = 0;
};

/// The segments covering a span of `spanBytes`: one every kSegmentStep, each kSegmentBytes or
/// what is left of the span.
std::vector<Segment> segmentsOf(uint64_t spanBytes);

/// The segment of the span `[spanStart, spanStart + spanBytes)` that holds `[from, to)`
/// wholly, counting from the one `from` lies in; none if the range is longer than that one.
std::optional<size_t> segmentHolding(uint64_t spanStart, uint64_t spanBytes, uint64_t from,
                                     uint64_t to);

/// Where a GPU address of a span lies: the first segment holding it and its offset there.
struct Place {
	size_t   segment = 0;
	uint64_t offset  = 0;
};
Place placeOf(uint64_t spanStart, uint64_t gpuAddress);

/// A slot (64 KiB of GPU address, numbered by address / kPageBytes, or by tile inside one
/// buffer for mapTiles) given a tile, or unmapped.
struct SlotMove {
	uint64_t            slot = 0;
	std::optional<Tile> tile;

	bool operator==(const SlotMove&) const = default;
};

/// The last move of each slot, in slot order.
std::vector<SlotMove> lastMoves(std::vector<SlotMove> moves);

/// Applies `moves` (slots counted from the start of `buffer`) with one UpdateTileMappings
/// call per heap they use; unmappings go with the first heap's call, or alone if no tile is
/// given. Returns the number of calls.
uint32_t mapTiles(ID3D12CommandQueue* queue, ID3D12Resource* buffer,
                  std::span<const ComPtr<ID3D12Heap>> heaps, std::span<const SlotMove> moves);

/// A heap of kHeapTiles tiles for buffers.
HRESULT makeTileHeap(ID3D12Device* device, ComPtr<ID3D12Heap>* heap);

/// A reserved buffer of `bytes` that shaders may write.
HRESULT makeReservedBuffer(ID3D12Device* device, uint64_t bytes, ComPtr<ID3D12Resource>* buffer);

/// Whether the device has tiled resources tier 2 or above.
bool hasTiledResourcesTier2(ID3D12Device* device);

/// What the card view asks of the rest of the library. Called with no card view lock held.
class Surroundings {
public:
	virtual bool                 bytesForTile(PageNumber page, uint8_t* out)         = 0;
	virtual bool                 becomeCardHome(PageNumber page, uint8_t* out)       = 0;
	virtual void                 cardHomeLost(PageNumber page, const uint8_t* bytes) = 0;
	virtual void                 tileChanged(PageNumber page, bool tile)             = 0;
	virtual void                 takeWritten(std::vector<PageNumber>& out)           = 0;
	virtual void                 prepare(std::span<const PageNumber> pages)          = 0;
	virtual void                 shaderWritten(Range gpuRange)                       = 0;
	virtual void                 deliverChanges()                                    = 0;
	virtual std::optional<Range> regionOf(uint64_t gpuAddress)                       = 0;
	/// Called with the card view's state lock held: they must only count the allocation.
	virtual void heapMade(ID3D12Heap* heap)         = 0;
	virtual void bufferMade(ID3D12Resource* buffer) = 0;

protected:
	~Surroundings() = default;
};

/// Everything CardView does, on `device` and `queue`. The methods are CardView's.
class Engine {
public:
	Engine(ID3D12Device* device, ID3D12CommandQueue* queue, Surroundings& surroundings);
	~Engine();
	Engine(const Engine&)            = delete;
	Engine& operator=(const Engine&) = delete;

	Status           start();
	Status           addSpan(uint64_t gpuBase, uint64_t bytes, SpanHome home);
	Status           bufferFor(uint64_t gpuAddress, uint64_t bytes, BufferView* view);
	void             shaderWrites(uint64_t gpuAddress, uint64_t bytes);
	Status           beforeSubmit();
	CardView::Totals totals() const;

	void slotChanged(uint64_t gpuAddress, PageNumber page);
	bool readBack(PageNumber page, uint8_t* out);
	void cardHomeLeft(PageNumber page);
	bool loaded(PageNumber page, uint64_t offset, const uint8_t* bytes, uint64_t byteCount);
	void pageForgotten(PageNumber page);
	void tiledPages(std::vector<PageNumber>& out) const;

	/// For tests: the tile slot `gpuAddress` holds, the heaps released but not yet freed (the
	/// queue may still use them) and whether `page` is Card home as far as the card view knows.
	std::optional<Tile> tileAt(uint64_t gpuAddress) const;
	size_t              retiredHeaps() const;
	bool                isCardHome(PageNumber page) const;

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace deunify::cardview
