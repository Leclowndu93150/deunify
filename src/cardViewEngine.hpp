// The card view's machinery. It reaches Pages, Loads, Census and Context through one
// interface and the graphics API through gpu::Backend, so the tests can drive it alone.
//
// Locks: the queue order lock (held while the card view puts work on the queues: tile mapping
// changes, fills and read-back copies, never while waiting for the card) is taken before the
// state lock (slots, tiles, blocks, pending work; short). Surroundings are called with neither
// held. A read-back takes what it needs under the locks, waits with none held, then checks
// that the page did not change meanwhile and retries if it did.

#pragma once

#include "deunify/cardView.hpp"
#include "gpuBackend.hpp"

#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace deunify::cardview {

using gpu::SlotMove;
using gpu::Tile;

/// A region of a span: `bytes` from `offset` of the span on.
struct Segment {
	uint64_t offset = 0;
	uint64_t bytes  = 0;
};

/// The segments covering a span of `spanBytes`: one every shape.step, each shape.bytes or what
/// is left of the span.
std::vector<Segment> segmentsOf(uint64_t spanBytes, gpu::SegmentShape shape = {});

/// The segment of the span `[spanStart, spanStart + spanBytes)` that holds `[from, to)`
/// wholly, counting from the one `from` lies in; none if the range is longer than that one.
std::optional<size_t> segmentHolding(uint64_t spanStart, uint64_t spanBytes, uint64_t from,
                                     uint64_t to, gpu::SegmentShape shape = {});

/// Where a GPU address of a span lies: the first segment holding it and its offset there.
struct Place {
	size_t   segment = 0;
	uint64_t offset  = 0;
};
Place placeOf(uint64_t spanStart, uint64_t gpuAddress, gpu::SegmentShape shape = {});

/// The last move of each slot, in slot order.
std::vector<SlotMove> lastMoves(std::vector<SlotMove> moves);

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

protected:
	~Surroundings() = default;
};

/// Everything CardView does, through `backend`. The methods are CardView's.
class Engine {
public:
	Engine(gpu::Backend& backend, Surroundings& surroundings);
	~Engine();
	Engine(const Engine&)            = delete;
	Engine& operator=(const Engine&) = delete;

	Status           start();
	Status           addSpan(uint64_t gpuBase, uint64_t bytes, SpanHome home);
	Status           bufferFor(uint64_t gpuAddress, uint64_t bytes, BufferView* view);
	Status           spanAddress(uint64_t gpuAddress, SpanAddress* address);
	bool             unboundReadsZero() const;
	void             shaderWrites(uint64_t gpuAddress, uint64_t bytes);
	Status           beforeSubmit();
	CardView::Totals totals() const;

	void slotChanged(uint64_t gpuAddress, PageNumber page);
	bool readBack(PageNumber page, uint8_t* out);
	void cardHomeLeft(PageNumber page);
	bool loaded(PageNumber page, uint64_t offset, const uint8_t* bytes, uint64_t byteCount);
	void pageForgotten(PageNumber page);
	void tiledPages(std::vector<PageNumber>& out) const;

	/// For tests: the tile slot `gpuAddress` holds, the blocks released but not yet freed (the
	/// queue may still use them) and whether `page` is Card home as far as the card view knows.
	std::optional<Tile> tileAt(uint64_t gpuAddress) const;
	size_t              retiredHeaps() const;
	bool                isCardHome(PageNumber page) const;

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace deunify::cardview
