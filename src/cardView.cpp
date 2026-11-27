#include "deunify/cardView.hpp"

#include "cardViewEngine.hpp"
#include "deunify/census.hpp"
#include "deunify/context.hpp"
#include "deunify/loads.hpp"
#include "deunify/pages.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace deunify::cardview {

namespace {

constexpr uint64_t kTileBytes        = kPageBytes;
constexpr uint64_t kBitsPerWord      = 64;
constexpr uint64_t kUploadLeastBytes = 4ull << 20;

uint64_t slotOf(uint64_t address) {
	return address / kTileBytes;
}

uint64_t addressOf(uint64_t slot) {
	return slot * kTileBytes;
}

using BlockPtr    = std::shared_ptr<gpu::Block>;
using CommandsPtr = std::unique_ptr<gpu::Commands>;
using HostPtr     = std::unique_ptr<gpu::HostBuffer>;

struct SlotRange {
	uint64_t first = 0;
	uint64_t last  = 0;
};

struct Span {
	Span(uint64_t start, uint64_t bytes, SpanHome home)
	    : start(start), bytes(bytes), home(home),
	      asked(std::make_unique<std::atomic<uint64_t>[]>((slotOf(bytes) + kBitsPerWord - 1) /
	                                                      kBitsPerWord)) {}

	uint64_t end() const { return start + bytes; }

	bool holds(uint64_t address) const { return start <= address && address < end(); }

	bool overlaps(uint64_t from, uint64_t to) const { return from < end() && start < to; }

	std::optional<SlotRange> slots(uint64_t from, uint64_t to) const {
		const uint64_t first = std::max(from, start);
		const uint64_t last  = std::min(to, end());
		if (first >= last) {
			return std::nullopt;
		}
		return SlotRange {slotOf(first), slotOf(last + kTileBytes - 1)};
	}

	bool isAsked(uint64_t slot) const {
		const uint64_t index = slot - slotOf(start);
		return asked[index / kBitsPerWord].load(std::memory_order_relaxed) & bitOf(index);
	}

	void setAsked(uint64_t slot) {
		const uint64_t index = slot - slotOf(start);
		asked[index / kBitsPerWord].fetch_or(bitOf(index), std::memory_order_relaxed);
	}

	bool allAsked(SlotRange slots) const {
		for (uint64_t slot = slots.first; slot < slots.last; ++slot) {
			if (!isAsked(slot)) {
				return false;
			}
		}
		return true;
	}

	static uint64_t bitOf(uint64_t index) { return 1ull << (index % kBitsPerWord); }

	const uint64_t                            start;
	const uint64_t                            bytes;
	const SpanHome                            home;
	std::unique_ptr<std::atomic<uint64_t>[]>  asked;
	std::vector<std::unique_ptr<gpu::Region>> regions;
	std::unique_ptr<gpu::Region>              flat;
	std::atomic<bool>                         made {false};
};

class QueueTiles {
public:
	void set(uint64_t slot, std::optional<Tile> tile) {
		if (auto shown = m_bySlot.find(slot); shown != m_bySlot.end()) {
			auto slots = m_byTile.find(shown->second);
			slots->second.erase(slot);
			if (slots->second.empty()) {
				m_byTile.erase(slots);
			}
			m_bySlot.erase(shown);
		}
		if (tile) {
			m_bySlot[slot] = *tile;
			m_byTile[*tile].insert(slot);
		}
	}

	std::optional<uint64_t> slotShowing(Tile tile) const {
		auto slots = m_byTile.find(tile);
		if (slots == m_byTile.end()) {
			return std::nullopt;
		}
		return *slots->second.begin();
	}

private:
	std::map<uint64_t, Tile>           m_bySlot;
	std::map<Tile, std::set<uint64_t>> m_byTile;
};

using PageBuffer = std::shared_ptr<std::vector<uint8_t>>;

PageBuffer newPageBuffer() {
	return std::make_shared<std::vector<uint8_t>>(kPageBytes);
}

struct PageTile {
	Tile               tile;
	std::set<uint64_t> slots;
};

struct SlotTile {
	PageNumber page;
	Tile       tile;
};

struct LoadedPiece {
	PageNumber           page;
	uint64_t             offset;
	std::vector<uint8_t> bytes;
};

struct PageBytes {
	uint64_t   sequence;
	PageBuffer bytes;
};

struct Spent {
	uint64_t    fence;
	CommandsPtr commands;
	HostPtr     upload;
};

struct Retired {
	uint64_t fence;
	BlockPtr block;
};

enum class Source : uint8_t { Loaded, Sending, Home };

enum class Fate : uint8_t { Copy, Covered, Kept };

struct Piece {
	PageNumber page;
	Tile       tile;
	uint64_t   offset;
	uint64_t   bytes;
	uint64_t   sequence;
	Source     source;
	uint64_t   uploadOffset = 0;
	bool       ready        = true;
};

struct ReadPlan {
	const gpu::Region*       region  = nullptr;
	uint64_t                 offset  = 0;
	uint64_t                 version = 0;
	std::vector<LoadedPiece> overlays;
	CommandsPtr              commands;
	HostPtr                  readback;
};

void overlay(const LoadedPiece& piece, uint8_t* page) {
	std::memcpy(page + piece.offset, piece.bytes.data(), piece.bytes.size());
}

template <typename Value>
std::vector<Value> takeAll(std::vector<Value>& values) {
	std::vector<Value> taken;
	taken.swap(values);
	return taken;
}

struct State {
	using HeapMaker = std::function<BlockPtr()>;

	std::map<uint64_t, PageNumber>            shown;
	std::map<uint64_t, SlotTile>              mapped;
	std::unordered_map<PageNumber, PageTile>  tiles;
	std::vector<BlockPtr>                     heaps;
	std::vector<uint32_t>                     used;
	std::set<Tile>                            free;
	std::vector<BlockPtr>                     retiring;
	std::vector<Retired>                      retired;
	std::vector<SlotMove>                     moves;
	std::unordered_set<PageNumber>            fills;
	std::unordered_set<PageNumber>            cardHome;
	std::unordered_set<PageNumber>            closing;
	std::unordered_set<PageNumber>            orphans;
	std::map<uint64_t, LoadedPiece>           loaded;
	std::unordered_map<PageNumber, PageBytes> sending;
	std::unordered_map<PageNumber, uint64_t>  versions;
	std::set<uint64_t>                        noted;
	std::unordered_map<PageNumber, bool>      tileNews;
	QueueTiles                                onQueue;
	std::vector<CommandsPtr>                  lists;
	std::vector<HostPtr>                      uploads;
	std::vector<HostPtr>                      readbacks;
	std::vector<Spent>                        spent;
	uint64_t                                  sequence  = 0;
	uint64_t                                  tilesHeld = 0;
	CardView::Totals                          counters;

	uint64_t nextSequence() { return ++sequence; }

	void changed(PageNumber page) { ++versions[page]; }

	uint64_t versionOf(PageNumber page) const {
		auto version = versions.find(page);
		return version == versions.end() ? 0 : version->second;
	}

	uint64_t liveHeaps() const {
		return static_cast<uint64_t>(std::count_if(
		    heaps.begin(), heaps.end(), [](const auto& heap) { return heap != nullptr; }));
	}

	bool isEmptyHeap(uint32_t heap) const { return heaps[heap] && used[heap] == 0; }

	size_t emptyHeaps() const {
		size_t empty = 0;
		for (uint32_t heap = 0; heap < heaps.size(); ++heap) {
			empty += isEmptyHeap(heap) ? 1 : 0;
		}
		return empty;
	}

	void addHeap(BlockPtr heap) {
		auto vacant = std::find(heaps.begin(), heaps.end(), nullptr);
		if (vacant == heaps.end()) {
			heaps.push_back(nullptr);
			used.push_back(0);
			vacant = heaps.end() - 1;
		}
		const uint32_t index = static_cast<uint32_t>(vacant - heaps.begin());
		*vacant              = std::move(heap);
		for (uint32_t tile = 0; tile < kHeapTiles; ++tile) {
			free.insert(Tile {index, tile});
		}
	}

	std::optional<Tile> take(const HeapMaker& makeHeap) {
		if (free.empty()) {
			BlockPtr heap = makeHeap();
			if (!heap) {
				return std::nullopt;
			}
			addHeap(std::move(heap));
		}
		const Tile tile = *free.begin();
		free.erase(free.begin());
		++used[tile.block];
		++tilesHeld;
		return tile;
	}

	void freeTile(Tile tile) {
		free.insert(tile);
		--used[tile.block];
		--tilesHeld;
		if (isEmptyHeap(tile.block) && emptyHeaps() > kSpareEmptyHeaps) {
			free.erase(free.lower_bound(Tile {tile.block, 0}),
			           free.lower_bound(Tile {tile.block + 1, 0}));
			retiring.push_back(std::move(heaps[tile.block]));
		}
	}

	std::optional<Tile> tileOf(PageNumber page) const {
		auto held = tiles.find(page);
		if (held == tiles.end()) {
			return std::nullopt;
		}
		return held->second.tile;
	}

	bool give(uint64_t slot, PageNumber page, const HeapMaker& makeHeap) {
		if (auto had = mapped.find(slot); had != mapped.end()) {
			if (had->second.page == page) {
				return true;
			}
			release(slot);
		}
		Tile tile;
		if (auto held = tiles.find(page); held != tiles.end()) {
			held->second.slots.insert(slot);
			orphans.erase(page);
			tile = held->second.tile;
		} else {
			std::optional<Tile> taken = take(makeHeap);
			if (!taken) {
				return false;
			}
			tile        = *taken;
			tiles[page] = PageTile {tile, {slot}};
			fills.insert(page);
			tileNews[page] = true;
			changed(page);
		}
		mapped[slot] = SlotTile {page, tile};
		moves.push_back(SlotMove {slot, tile});
		return true;
	}

	bool giveIn(uint64_t slot, PageNumber page, bool cardSpan, const HeapMaker& makeHeap) {
		if (!give(slot, page, makeHeap)) {
			return false;
		}
		if (cardSpan && !cardHome.contains(page)) {
			closing.insert(page);
		}
		return true;
	}

	void release(uint64_t slot) {
		auto had = mapped.find(slot);
		if (had == mapped.end()) {
			return;
		}
		const PageNumber page = had->second.page;
		mapped.erase(had);
		moves.push_back(SlotMove {slot, std::nullopt});
		PageTile& held = tiles.at(page);
		held.slots.erase(slot);
		if (!held.slots.empty()) {
			return;
		}
		if (cardHome.contains(page)) {
			orphans.insert(page);
			return;
		}
		dropTile(page);
	}

	void dropTile(PageNumber page) {
		auto held = tiles.find(page);
		freeTile(held->second.tile);
		tiles.erase(held);
		fills.erase(page);
		tileNews[page] = false;
		changed(page);
	}

	void noteShaderWrites(SlotRange slots) {
		for (auto it = mapped.lower_bound(slots.first);
		     it != mapped.end() && it->first < slots.last; ++it) {
			noted.insert(it->first);
			if (!cardHome.contains(it->second.page)) {
				closing.insert(it->second.page);
			}
		}
	}

	std::vector<LoadedPiece> loadedOf(PageNumber page, uint64_t after) const {
		std::vector<LoadedPiece> pieces;
		for (auto it = loaded.upper_bound(after); it != loaded.end(); ++it) {
			if (it->second.page == page) {
				pieces.push_back(it->second);
			}
		}
		return pieces;
	}

	void eraseLoaded(PageNumber page) {
		std::erase_if(loaded, [page](const auto& entry) { return entry.second.page == page; });
	}

	bool copySending(PageNumber page, uint8_t* out) const {
		auto bytes = sending.find(page);
		if (bytes == sending.end()) {
			return false;
		}
		std::memcpy(out, bytes->second.bytes->data(), kPageBytes);
		for (const LoadedPiece& piece: loadedOf(page, bytes->second.sequence)) {
			overlay(piece, out);
		}
		return true;
	}

	uint64_t holdSending(PageNumber page, PageBuffer bytes) {
		const uint64_t at = nextSequence();
		sending[page]     = PageBytes {at, std::move(bytes)};
		changed(page);
		return at;
	}

	void eraseSending(PageNumber page, uint64_t at) {
		auto bytes = sending.find(page);
		if (bytes != sending.end() && bytes->second.sequence == at) {
			sending.erase(bytes);
			changed(page);
		}
	}

	void left(PageNumber page) {
		cardHome.erase(page);
		const bool unsent = sending.erase(page) > 0;
		changed(page);
		if (orphans.erase(page) > 0) {
			dropTile(page);
			return;
		}
		if (unsent && tiles.contains(page)) {
			fills.insert(page);
		}
	}

	void cameHome(PageNumber page, uint64_t at) {
		eraseSending(page, at);
		cardHome.erase(page);
		changed(page);
		if (orphans.erase(page) > 0) {
			dropTile(page);
		} else if (tiles.contains(page)) {
			fills.insert(page);
		}
	}

	void forget(PageNumber page) {
		cardHome.erase(page);
		closing.erase(page);
		orphans.erase(page);
		sending.erase(page);
		eraseLoaded(page);
		if (auto held = tiles.find(page); held != tiles.end()) {
			for (uint64_t slot: std::set<uint64_t>(held->second.slots)) {
				release(slot);
			}
		}
		if (tiles.contains(page)) {
			dropTile(page);
		}
		fills.erase(page);
		tileNews.erase(page);
		versions.erase(page);
	}

	bool stillWanted(const Piece& piece) const {
		if (tileOf(piece.page) != piece.tile) {
			return false;
		}
		switch (piece.source) {
			case Source::Loaded: return loaded.contains(piece.sequence);
			case Source::Sending: {
				auto bytes = sending.find(piece.page);
				return bytes != sending.end() && bytes->second.sequence == piece.sequence;
			}
			case Source::Home: return !cardHome.contains(piece.page);
		}
		return false;
	}

	void sent(const Piece& piece) {
		if (piece.source == Source::Loaded) {
			loaded.erase(piece.sequence);
		} else if (piece.source == Source::Sending) {
			eraseSending(piece.page, piece.sequence);
		}
	}

	void requeue(const Piece& piece) {
		if (piece.source == Source::Home && piece.ready && tiles.contains(piece.page)) {
			fills.insert(piece.page);
		}
	}
};

} // namespace

std::vector<Segment> segmentsOf(uint64_t spanBytes, gpu::SegmentShape shape) {
	std::vector<Segment> segments;
	for (uint64_t offset = 0; offset < spanBytes; offset += shape.step) {
		segments.push_back(Segment {offset, std::min(shape.bytes, spanBytes - offset)});
	}
	return segments;
}

std::optional<size_t> segmentHolding(uint64_t spanStart, uint64_t spanBytes, uint64_t from,
                                     uint64_t to, gpu::SegmentShape shape) {
	const uint64_t index  = (from - spanStart) / shape.step;
	const uint64_t offset = index * shape.step;
	if (offset >= spanBytes ||
	    to > spanStart + offset + std::min(shape.bytes, spanBytes - offset)) {
		return std::nullopt;
	}
	return static_cast<size_t>(index);
}

Place placeOf(uint64_t spanStart, uint64_t gpuAddress, gpu::SegmentShape shape) {
	const uint64_t index = (gpuAddress - spanStart) / shape.step;
	return Place {static_cast<size_t>(index), gpuAddress - spanStart - index * shape.step};
}

std::vector<SlotMove> lastMoves(std::vector<SlotMove> moves) {
	std::map<uint64_t, std::optional<Tile>> last;
	for (const SlotMove& move: moves) {
		last[move.slot] = move.tile;
	}
	std::vector<SlotMove> result;
	result.reserve(last.size());
	for (const auto& [slot, tile]: last) {
		result.push_back(SlotMove {slot, tile});
	}
	return result;
}

struct Engine::Impl {
	Impl(gpu::Backend& backend, Surroundings& surroundings)
	    : backend(backend), surroundings(surroundings), makeHeap([this] { return newHeap(); }) {}

	~Impl() {
		if (started) {
			std::unique_lock order(queueOrder);
			const uint64_t   value = backend.signal();
			order.unlock();
			backend.wait(value);
		}
	}

	gpu::Backend&                                backend;
	Surroundings&                                surroundings;
	State::HeapMaker                             makeHeap;
	bool                                         started = false;
	gpu::SegmentShape                            shape;
	std::array<std::unique_ptr<Span>, kMaxSpans> spans;
	std::atomic<uint32_t>                        spanCount {0};
	std::mutex                                   adding;
	std::mutex                                   making;
	std::mutex                                   queueOrder;
	mutable std::mutex                           stateLock;
	State                                        state;

	BlockPtr newHeap() {
		BlockPtr block;
		if (backend.makeBlock(&block) != Status::Ok) {
			return nullptr;
		}
		return block;
	}

	Span* spanHolding(uint64_t address) const {
		const uint32_t count = spanCount.load(std::memory_order_acquire);
		for (uint32_t i = 0; i < count; ++i) {
			if (spans[i]->holds(address)) {
				return spans[i].get();
			}
		}
		return nullptr;
	}

	std::pair<const gpu::Region*, uint64_t> bufferAt(uint64_t slot) const {
		const Span* span  = spanHolding(addressOf(slot));
		const Place place = placeOf(span->start, addressOf(slot), shape);
		return {span->regions[place.segment].get(), place.offset};
	}

	void tellPages() {
		std::unordered_map<PageNumber, bool> news;
		{
			std::lock_guard lock(stateLock);
			news.swap(state.tileNews);
		}
		while (!news.empty()) {
			for (const auto& [page, tile]: news) {
				surroundings.tileChanged(page, tile);
			}
			std::lock_guard lock(stateLock);
			std::erase_if(news, [this](const auto& told) {
				return state.tiles.contains(told.first) == told.second ||
				       !state.versions.contains(told.first);
			});
			for (auto& [page, tile]: news) {
				tile = !tile;
			}
		}
	}

	Status takeCommands(CommandsPtr* commands) {
		if (!state.lists.empty()) {
			*commands = std::move(state.lists.back());
			state.lists.pop_back();
			return Status::Ok;
		}
		return backend.makeCommands(commands);
	}

	Status takeHostBuffer(std::vector<HostPtr>& pool, bool upload, uint64_t bytes,
	                      HostPtr* buffer) {
		auto fits = std::find_if(pool.begin(), pool.end(),
		                         [bytes](const HostPtr& held) { return held->bytes >= bytes; });
		if (fits != pool.end()) {
			*buffer = std::move(*fits);
			pool.erase(fits);
			return Status::Ok;
		}
		return upload ? backend.makeUpload(bytes, buffer) : backend.makeReadback(bytes, buffer);
	}

	Status takeUpload(uint64_t bytes, HostPtr* buffer) {
		return takeHostBuffer(state.uploads, true,
		                      std::bit_ceil(std::max(bytes, kUploadLeastBytes)), buffer);
	}

	void recycle() {
		const uint64_t     done = backend.completed();
		std::lock_guard    lock(stateLock);
		std::vector<Spent> waiting;
		for (Spent& spent: state.spent) {
			if (spent.fence <= done) {
				state.lists.push_back(std::move(spent.commands));
				state.uploads.push_back(std::move(spent.upload));
			} else {
				waiting.push_back(std::move(spent));
			}
		}
		state.spent.swap(waiting);
		std::erase_if(state.retired, [done](const Retired& heap) { return heap.fence <= done; });
	}

	Status addSpan(uint64_t gpuBase, uint64_t bytes, SpanHome home) {
		std::lock_guard lock(adding);
		const uint32_t  count = spanCount.load(std::memory_order_relaxed);
		const uint64_t  end   = gpuBase + bytes;
		const bool      overlaps =
		    std::any_of(spans.begin(), spans.begin() + count,
		                [&](const auto& span) { return span->overlaps(gpuBase, end); });
		if (bytes == 0 || gpuBase % kTileBytes != 0 || bytes % kTileBytes != 0 || end < gpuBase ||
		    count == kMaxSpans || overlaps) {
			return Status::InvalidArgument;
		}
		spans[count] = std::make_unique<Span>(gpuBase, bytes, home);
		spanCount.store(count + 1, std::memory_order_release);
		return Status::Ok;
	}

	Status makeSegments(Span& span) {
		if (span.made.load(std::memory_order_acquire)) {
			return Status::Ok;
		}
		std::lock_guard lock(making);
		if (span.made.load(std::memory_order_relaxed)) {
			return Status::Ok;
		}
		const std::vector<Segment>                segments = segmentsOf(span.bytes, shape);
		std::vector<std::unique_ptr<gpu::Region>> regions(segments.size());
		for (size_t k = 0; k < segments.size(); ++k) {
			const Status status = backend.makeRegion(segments[k].bytes, &regions[k]);
			if (status != Status::Ok) {
				return status;
			}
		}
		std::unique_ptr<gpu::Region> flat;
		if (backend.flatSpans() && segments.front().bytes != span.bytes) {
			const Status status = backend.makeRegion(span.bytes, &flat);
			if (status != Status::Ok) {
				return status;
			}
		}
		span.regions = std::move(regions);
		span.flat    = std::move(flat);
		span.made.store(true, std::memory_order_release);
		return Status::Ok;
	}

	Range blockAround(uint64_t address, const Span& span) {
		if (std::optional<Range> region = surroundings.regionOf(address);
		    region && region->bytes > 0) {
			const uint64_t from = std::max(region->start, span.start);
			const uint64_t to   = std::min(region->end(), span.end());
			if (from <= address && address < to) {
				return Range {from, to - from};
			}
		}
		const uint64_t from = std::max(address & ~(kUnsizedAskBytes - 1), span.start);
		const uint64_t to   = std::min(from + kUnsizedAskBytes, span.end());
		return Range {from, to - from};
	}

	Status giveAsked(Span& span, SlotRange slots) {
		if (span.allAsked(slots)) {
			return Status::Ok;
		}
		const bool      cardSpan = span.home == SpanHome::Card;
		std::lock_guard lock(stateLock);
		for (uint64_t slot = slots.first; slot < slots.last; ++slot) {
			if (span.isAsked(slot)) {
				continue;
			}
			auto shown = state.shown.find(slot);
			if (shown != state.shown.end() &&
			    !state.giveIn(slot, shown->second, cardSpan, makeHeap)) {
				++state.counters.refusedAsks;
				return Status::OutOfMemory;
			}
			span.setAsked(slot);
		}
		return Status::Ok;
	}

	Status bufferFor(uint64_t gpuAddress, uint64_t bytes, BufferView* view) {
		if (view == nullptr) {
			return Status::InvalidArgument;
		}
		Span*          span = spanHolding(gpuAddress);
		const uint64_t end  = gpuAddress + std::max<uint64_t>(bytes, 1);
		if (span == nullptr || end < gpuAddress || end > span->end()) {
			return Status::NotFound;
		}
		const Range asked = bytes == 0 ? blockAround(gpuAddress, *span) : Range {gpuAddress, bytes};
		const std::optional<size_t> segment =
		    segmentHolding(span->start, span->bytes, asked.start, asked.end(), shape);
		if (asked.bytes > kLargestAskBytes || !segment) {
			std::lock_guard lock(stateLock);
			++state.counters.tooLargeAsks;
			return Status::TooLarge;
		}
		Status status = makeSegments(*span);
		if (status != Status::Ok) {
			return status;
		}
		status = giveAsked(*span, *span->slots(asked.start, asked.end()));
		tellPages();
		if (status != Status::Ok) {
			return status;
		}
		*view = span->regions[*segment]->view(gpuAddress - span->start - *segment * shape.step,
		                                      asked.end() - gpuAddress);
		return Status::Ok;
	}

	void shaderWrites(uint64_t gpuAddress, uint64_t bytes) {
		const uint32_t  count = spanCount.load(std::memory_order_acquire);
		std::lock_guard lock(stateLock);
		for (uint32_t i = 0; i < count; ++i) {
			if (std::optional<SlotRange> slots = spans[i]->slots(gpuAddress, gpuAddress + bytes)) {
				state.noteShaderWrites(*slots);
			}
		}
	}

	void slotChanged(uint64_t gpuAddress, PageNumber page) {
		const Span* span = spanHolding(gpuAddress);
		if (span == nullptr) {
			return;
		}
		const uint64_t slot = slotOf(gpuAddress);
		{
			std::lock_guard lock(stateLock);
			if (page == kNoPage) {
				state.shown.erase(slot);
				state.release(slot);
			} else {
				state.shown[slot] = page;
				if (span->isAsked(slot) &&
				    !state.giveIn(slot, page, span->home == SpanHome::Card, makeHeap)) {
					++state.counters.refusedAsks;
				}
			}
		}
		tellPages();
	}

	enum class Planned : uint8_t { Done, Lost, Copy };

	Planned planReadBack(PageNumber page, uint8_t* out, ReadPlan& plan) {
		std::lock_guard lock(stateLock);
		if (state.copySending(page, out)) {
			return Planned::Done;
		}
		const std::optional<Tile>     tile = state.tileOf(page);
		const std::optional<uint64_t> slot = tile ? state.onQueue.slotShowing(*tile) : std::nullopt;
		if (!slot) {
			return Planned::Lost;
		}
		std::tie(plan.region, plan.offset) = bufferAt(*slot);
		plan.version                       = state.versionOf(page);
		plan.overlays                      = state.loadedOf(page, 0);
		if (takeCommands(&plan.commands) != Status::Ok ||
		    takeHostBuffer(state.readbacks, false, kPageBytes, &plan.readback) != Status::Ok) {
			return Planned::Lost;
		}
		return Planned::Copy;
	}

	bool copyTile(const ReadPlan& plan, uint64_t* value) {
		std::lock_guard order(queueOrder);
		if (backend.begin(*plan.commands) != Status::Ok) {
			return false;
		}
		backend.copyBack(*plan.commands, *plan.readback, *plan.region, plan.offset, kPageBytes);
		if (backend.end(*plan.commands) != Status::Ok) {
			return false;
		}
		*value = backend.submit(*plan.commands);
		return true;
	}

	bool finishReadBack(PageNumber page, ReadPlan& plan, bool copied, uint8_t* out) {
		std::lock_guard lock(stateLock);
		if (copied) {
			std::memcpy(out, plan.readback->data, kPageBytes);
			for (const LoadedPiece& piece: plan.overlays) {
				overlay(piece, out);
			}
			++state.counters.readBacks;
		}
		state.lists.push_back(std::move(plan.commands));
		state.readbacks.push_back(std::move(plan.readback));
		return state.versionOf(page) == plan.version;
	}

	bool readBack(PageNumber page, uint8_t* out) {
		for (;;) {
			ReadPlan      plan;
			const Planned planned = planReadBack(page, out, plan);
			if (planned != Planned::Copy) {
				return planned == Planned::Done;
			}
			uint64_t   value  = 0;
			const bool copied = copyTile(plan, &value) && backend.wait(value);
			if (finishReadBack(page, plan, copied, out)) {
				return copied;
			}
		}
	}

	void cardHomeLeft(PageNumber page) {
		std::lock_guard lock(stateLock);
		state.left(page);
	}

	bool loaded(PageNumber page, uint64_t offset, const uint8_t* bytes, uint64_t byteCount) {
		std::lock_guard lock(stateLock);
		if (!state.tiles.contains(page)) {
			return false;
		}
		state.loaded.emplace(
		    state.nextSequence(),
		    LoadedPiece {page, offset, std::vector<uint8_t>(bytes, bytes + byteCount)});
		state.changed(page);
		return true;
	}

	void pageForgotten(PageNumber page) {
		std::lock_guard lock(stateLock);
		state.forget(page);
	}

	void bringHome(PageNumber page) {
		uint64_t mark = 0;
		{
			std::lock_guard lock(stateLock);
			mark = state.sequence;
		}
		PageBuffer bytes = newPageBuffer();
		const bool read  = readBack(page, bytes->data());
		uint64_t   at    = 0;
		{
			std::lock_guard lock(stateLock);
			if (!state.orphans.contains(page)) {
				return;
			}
			if (!read) {
				++state.counters.lostPages;
				state.orphans.erase(page);
				state.dropTile(page);
				return;
			}
			for (const LoadedPiece& piece: state.loadedOf(page, mark)) {
				overlay(piece, bytes->data());
			}
			state.eraseLoaded(page);
			at = state.holdSending(page, bytes);
		}
		surroundings.cardHomeLost(page, bytes->data());
		std::lock_guard lock(stateLock);
		state.cameHome(page, at);
	}

	void bringOrphansHome() {
		std::vector<PageNumber> orphans;
		{
			std::lock_guard lock(stateLock);
			orphans.assign(state.orphans.begin(), state.orphans.end());
		}
		for (PageNumber page: orphans) {
			bringHome(page);
		}
	}

	uint32_t mapRegion(gpu::Region& region, uint64_t first, uint64_t last,
	                   const std::vector<SlotMove>& moves, std::span<gpu::Block* const> blocks) {
		std::vector<SlotMove> inside;
		for (const SlotMove& move: moves) {
			if (first <= move.slot && move.slot < last) {
				inside.push_back(SlotMove {move.slot - first, move.tile});
			}
		}
		const std::vector<gpu::BindGroup> groups = gpu::bindGroups(inside);
		if (!groups.empty()) {
			backend.bind(region, groups, blocks);
		}
		return static_cast<uint32_t>(groups.size());
	}

	uint32_t mapSegments(const std::vector<SlotMove>& moves, std::span<gpu::Block* const> blocks) {
		uint32_t       calls = 0;
		const uint32_t count = spanCount.load(std::memory_order_acquire);
		for (uint32_t i = 0; i < count; ++i) {
			const Span& span = *spans[i];
			if (!span.made.load(std::memory_order_acquire)) {
				continue;
			}
			const std::vector<Segment> segments = segmentsOf(span.bytes, shape);
			for (size_t k = 0; k < segments.size(); ++k) {
				const uint64_t first = slotOf(span.start + segments[k].offset);
				calls += mapRegion(*span.regions[k], first, first + slotOf(segments[k].bytes),
				                   moves, blocks);
			}
			if (span.flat) {
				calls +=
				    mapRegion(*span.flat, slotOf(span.start), slotOf(span.end()), moves, blocks);
			}
		}
		return calls;
	}

	void sendMoves() {
		std::lock_guard       order(queueOrder);
		std::vector<SlotMove> moves;
		std::vector<BlockPtr> heaps;
		std::vector<BlockPtr> retiring;
		{
			std::lock_guard lock(stateLock);
			moves    = lastMoves(takeAll(state.moves));
			retiring = takeAll(state.retiring);
			heaps    = state.heaps;
		}
		if (moves.empty() && retiring.empty()) {
			return;
		}
		std::vector<gpu::Block*> blocks(heaps.size());
		std::transform(heaps.begin(), heaps.end(), blocks.begin(),
		               [](const BlockPtr& heap) { return heap.get(); });
		const uint32_t  calls = mapSegments(moves, blocks);
		const uint64_t  value = backend.submitBinds();
		std::lock_guard lock(stateLock);
		for (const SlotMove& move: moves) {
			state.onQueue.set(move.slot, move.tile);
		}
		for (BlockPtr& heap: retiring) {
			state.retired.push_back(Retired {value, std::move(heap)});
		}
		state.counters.mappingCalls += calls;
	}

	void takeHome(PageNumber page, const PageBuffer& bytes, uint64_t at) {
		{
			std::lock_guard lock(stateLock);
			state.cardHome.insert(page);
			state.changed(page);
		}
		const bool taken = surroundings.becomeCardHome(page, bytes->data());
		bool       tiled = false;
		{
			std::lock_guard lock(stateLock);
			if (!taken) {
				state.eraseSending(page, at);
			}
			tiled = state.tiles.contains(page);
		}
		if (!taken || tiled) {
			return;
		}
		surroundings.cardHomeLost(page, bytes->data());
		std::lock_guard lock(stateLock);
		state.cameHome(page, at);
	}

	void takeHomeClosing() {
		std::vector<std::tuple<PageNumber, PageBuffer, uint64_t>> taking;
		{
			std::lock_guard lock(stateLock);
			for (PageNumber page: state.closing) {
				if (state.tiles.contains(page) && !state.cardHome.contains(page)) {
					PageBuffer bytes = newPageBuffer();
					taking.emplace_back(page, bytes, state.holdSending(page, bytes));
				}
			}
			state.closing.clear();
		}
		for (const auto& [page, bytes, at]: taking) {
			takeHome(page, bytes, at);
		}
	}

	void gatherPieces(const std::vector<PageNumber>& written, std::vector<Piece>& pieces) {
		for (auto it = state.loaded.begin(); it != state.loaded.end();) {
			const std::optional<Tile> tile = state.tileOf(it->second.page);
			if (!tile) {
				it = state.loaded.erase(it);
				continue;
			}
			pieces.push_back(Piece {it->second.page, *tile, it->second.offset,
			                        it->second.bytes.size(), it->first, Source::Loaded});
			++it;
		}
		for (const auto& [page, bytes]: state.sending) {
			if (std::optional<Tile> tile = state.tileOf(page)) {
				pieces.push_back(
				    Piece {page, *tile, 0, kPageBytes, bytes.sequence, Source::Sending});
			}
		}
		std::unordered_set<PageNumber> fromHome;
		fromHome.swap(state.fills);
		fromHome.insert(written.begin(), written.end());
		for (PageNumber page: fromHome) {
			const std::optional<Tile> tile = state.tileOf(page);
			if (tile && !state.cardHome.contains(page) && !state.sending.contains(page)) {
				pieces.push_back(
				    Piece {page, *tile, 0, kPageBytes, state.nextSequence(), Source::Home});
			}
		}
		std::sort(pieces.begin(), pieces.end(),
		          [](const Piece& a, const Piece& b) { return a.sequence < b.sequence; });
	}

	void copyHeldBytes(const std::vector<Piece>& pieces, uint8_t* upload) const {
		for (const Piece& piece: pieces) {
			if (piece.source == Source::Loaded) {
				const std::vector<uint8_t>& bytes = state.loaded.at(piece.sequence).bytes;
				std::memcpy(upload + piece.uploadOffset, bytes.data(), bytes.size());
			} else if (piece.source == Source::Sending) {
				std::memcpy(upload + piece.uploadOffset, state.sending.at(piece.page).bytes->data(),
				            kPageBytes);
			}
		}
	}

	Status gatherFills(const std::vector<PageNumber>& written, std::vector<Piece>& pieces,
	                   HostPtr* upload, CommandsPtr* commands) {
		std::lock_guard lock(stateLock);
		gatherPieces(written, pieces);
		uint64_t total = 0;
		for (Piece& piece: pieces) {
			piece.uploadOffset = total;
			total += piece.bytes;
		}
		if (pieces.empty()) {
			return Status::Ok;
		}
		Status status = takeUpload(total, upload);
		if (status == Status::Ok) {
			status = takeCommands(commands);
		}
		if (status != Status::Ok) {
			for (const Piece& piece: pieces) {
				state.requeue(piece);
			}
			if (*upload) {
				state.uploads.push_back(std::move(*upload));
			}
			pieces.clear();
			return status;
		}
		copyHeldBytes(pieces, (*upload)->data);
		return Status::Ok;
	}

	void readFromHomes(std::vector<Piece>& pieces, uint8_t* upload) {
		std::vector<PageNumber> pages;
		for (const Piece& piece: pieces) {
			if (piece.source == Source::Home) {
				pages.push_back(piece.page);
			}
		}
		if (pages.empty()) {
			return;
		}
		surroundings.prepare(pages);
		for (Piece& piece: pieces) {
			if (piece.source == Source::Home) {
				piece.ready = surroundings.bytesForTile(piece.page, upload + piece.uploadOffset);
			}
		}
	}

	bool usable(const Piece& piece) const {
		return piece.ready && state.stillWanted(piece) &&
		       state.onQueue.slotShowing(piece.tile).has_value();
	}

	std::vector<Fate> fatesOf(const std::vector<Piece>& pieces, uint8_t* upload) const {
		std::unordered_map<PageNumber, size_t> whole;
		for (size_t i = 0; i < pieces.size(); ++i) {
			if (pieces[i].source != Source::Loaded) {
				whole[pieces[i].page] = i;
			}
		}
		std::vector<Fate>                                  fates(pieces.size(), Fate::Kept);
		std::unordered_map<PageNumber, std::vector<Range>> copied;
		std::unordered_set<PageNumber>                     blocked;
		for (size_t i = 0; i < pieces.size(); ++i) {
			const Piece& piece = pieces[i];
			auto         base  = whole.find(piece.page);
			if (base != whole.end()) {
				const Piece& page = pieces[base->second];
				if (!usable(page) || !usable(piece)) {
					continue;
				}
				if (i > base->second) {
					std::memcpy(upload + page.uploadOffset + piece.offset,
					            upload + piece.uploadOffset, piece.bytes);
				}
				fates[i] = i == base->second ? Fate::Copy : Fate::Covered;
				continue;
			}
			std::vector<Range>& ranges = copied[piece.page];
			const Range         range  = Range {piece.offset, piece.bytes};
			const bool overlaps = std::any_of(ranges.begin(), ranges.end(), [&](const Range& done) {
				return range.start < done.end() && done.start < range.end();
			});
			if (blocked.contains(piece.page) || overlaps || !usable(piece)) {
				blocked.insert(piece.page);
				continue;
			}
			ranges.push_back(range);
			fates[i] = Fate::Copy;
		}
		return fates;
	}

	std::vector<Fate> recordFills(const std::vector<Piece>& pieces, const gpu::HostBuffer& upload,
	                              gpu::Commands& commands) {
		std::vector<Fate> fates = fatesOf(pieces, upload.data);
		for (size_t i = 0; i < pieces.size(); ++i) {
			if (fates[i] != Fate::Copy) {
				continue;
			}
			const Piece& piece          = pieces[i];
			const auto [region, offset] = bufferAt(*state.onQueue.slotShowing(piece.tile));
			backend.copyUp(commands, *region, offset + piece.offset, upload, piece.uploadOffset,
			               piece.bytes);
			state.counters.fillBytes += piece.bytes;
		}
		return fates;
	}

	Status sendFills(const std::vector<Piece>& pieces, HostPtr upload, CommandsPtr commands) {
		std::lock_guard   order(queueOrder);
		std::lock_guard   lock(stateLock);
		std::vector<Fate> fates;
		Status            status = backend.begin(*commands);
		if (status == Status::Ok) {
			fates  = recordFills(pieces, *upload, *commands);
			status = backend.end(*commands);
		}
		if (status != Status::Ok) {
			for (const Piece& piece: pieces) {
				state.requeue(piece);
			}
			state.lists.push_back(std::move(commands));
			state.uploads.push_back(std::move(upload));
			return status;
		}
		const uint64_t value = backend.submit(*commands);
		state.spent.push_back(Spent {value, std::move(commands), std::move(upload)});
		for (size_t i = 0; i < pieces.size(); ++i) {
			if (fates[i] != Fate::Kept) {
				state.sent(pieces[i]);
			} else {
				state.requeue(pieces[i]);
			}
		}
		return Status::Ok;
	}

	void fenceTitleWork() {
		std::lock_guard order(queueOrder);
		backend.fenceTitleWork();
	}

	bool hasTiles() const {
		std::lock_guard lock(stateLock);
		return !state.tiles.empty();
	}

	Status fill() {
		std::vector<PageNumber> written;
		if (hasTiles()) {
			surroundings.takeWritten(written);
		}
		std::vector<Piece> pieces;
		HostPtr            upload;
		CommandsPtr        commands;
		const Status       status = gatherFills(written, pieces, &upload, &commands);
		if (status != Status::Ok || pieces.empty()) {
			return status;
		}
		readFromHomes(pieces, upload->data);
		return sendFills(pieces, std::move(upload), std::move(commands));
	}

	void notifyShaderWrites() {
		std::set<uint64_t> noted;
		{
			std::lock_guard lock(stateLock);
			noted.swap(state.noted);
		}
		auto run = noted.begin();
		while (run != noted.end()) {
			const uint64_t first = *run;
			uint64_t       last  = first + 1;
			for (++run; run != noted.end() && *run == last; ++run) {
				++last;
			}
			surroundings.shaderWritten(Range {addressOf(first), addressOf(last - first)});
		}
	}

	Status beforeSubmit() {
		recycle();
		bringOrphansHome();
		sendMoves();
		takeHomeClosing();
		const Status status = fill();
		fenceTitleWork();
		notifyShaderWrites();
		tellPages();
		surroundings.deliverChanges();
		return status;
	}

	CardView::Totals totals() const {
		std::lock_guard  lock(stateLock);
		CardView::Totals totals = state.counters;
		totals.tiles            = state.tilesHeld;
		totals.heaps            = state.liveHeaps();
		totals.cardHomePages    = state.cardHome.size();
		return totals;
	}
};

Engine::Engine(gpu::Backend& backend, Surroundings& surroundings)
    : m_impl(std::make_unique<Impl>(backend, surroundings)) {}

Engine::~Engine() = default;

Status Engine::start() {
	const Status status = m_impl->backend.start();
	if (status == Status::Ok) {
		m_impl->started = true;
		m_impl->shape   = m_impl->backend.segmentShape();
	}
	return status;
}

Status Engine::addSpan(uint64_t gpuBase, uint64_t bytes, SpanHome home) {
	return m_impl->addSpan(gpuBase, bytes, home);
}

Status Engine::bufferFor(uint64_t gpuAddress, uint64_t bytes, BufferView* view) {
	return m_impl->bufferFor(gpuAddress, bytes, view);
}

void Engine::shaderWrites(uint64_t gpuAddress, uint64_t bytes) {
	m_impl->shaderWrites(gpuAddress, bytes);
}

Status Engine::beforeSubmit() {
	return m_impl->beforeSubmit();
}

CardView::Totals Engine::totals() const {
	return m_impl->totals();
}

void Engine::slotChanged(uint64_t gpuAddress, PageNumber page) {
	m_impl->slotChanged(gpuAddress, page);
}

bool Engine::readBack(PageNumber page, uint8_t* out) {
	return m_impl->readBack(page, out);
}

void Engine::cardHomeLeft(PageNumber page) {
	m_impl->cardHomeLeft(page);
}

bool Engine::loaded(PageNumber page, uint64_t offset, const uint8_t* bytes, uint64_t byteCount) {
	return m_impl->loaded(page, offset, bytes, byteCount);
}

void Engine::pageForgotten(PageNumber page) {
	m_impl->pageForgotten(page);
}

void Engine::tiledPages(std::vector<PageNumber>& out) const {
	std::lock_guard lock(m_impl->stateLock);
	for (const auto& [page, tile]: m_impl->state.tiles) {
		out.push_back(page);
	}
}

std::optional<Tile> Engine::tileAt(uint64_t gpuAddress) const {
	std::lock_guard lock(m_impl->stateLock);
	auto            mapped = m_impl->state.mapped.find(slotOf(gpuAddress));
	if (mapped == m_impl->state.mapped.end()) {
		return std::nullopt;
	}
	return mapped->second.tile;
}

size_t Engine::retiredHeaps() const {
	std::lock_guard lock(m_impl->stateLock);
	return m_impl->state.retiring.size() + m_impl->state.retired.size();
}

bool Engine::isCardHome(PageNumber page) const {
	std::lock_guard lock(m_impl->stateLock);
	return m_impl->state.cardHome.contains(page);
}

} // namespace deunify::cardview

namespace deunify {

struct CardView::Impl final: cardview::Surroundings {
	explicit Impl(Context& context)
	    : context(context), backend(gpu::makeBackend(context.config(), context.callbacks(),
	                                                 context.census().ledger())),
	      engine(*backend, *this) {
		context.census().attach(backend.get());
	}

	bool bytesForTile(PageNumber page, uint8_t* out) override {
		return context.pages().bytesForTile(page, out);
	}

	bool becomeCardHome(PageNumber page, uint8_t* out) override {
		return context.pages().becomeCardHome(page, out);
	}

	void cardHomeLost(PageNumber page, const uint8_t* bytes) override {
		context.pages().cardHomeLost(page, bytes);
	}

	void tileChanged(PageNumber page, bool tile) override {
		context.pages().tileChanged(page, tile);
	}

	void takeWritten(std::vector<PageNumber>& out) override { context.pages().takeWritten(out); }

	void prepare(std::span<const PageNumber> pages) override { context.loads().prepare(pages); }

	void shaderWritten(Range gpuRange) override {
		context.queueChange(gpuRange, ChangeReason::ShaderWritten);
	}

	void deliverChanges() override { context.deliverChanges(); }

	std::optional<Range> regionOf(uint64_t gpuAddress) override {
		const Callbacks& callbacks = context.callbacks();
		Range            region;
		if (callbacks.regionOf != nullptr &&
		    callbacks.regionOf(callbacks.context, gpuAddress, &region)) {
			return region;
		}
		return std::nullopt;
	}

	Context&                      context;
	std::unique_ptr<gpu::Backend> backend;
	cardview::Engine              engine;
};

CardView::CardView(Context& context): m_context(context), m_impl(std::make_unique<Impl>(context)) {}

CardView::~CardView() = default;

Status CardView::addSpan(uint64_t gpuBase, uint64_t bytes, SpanHome home) {
	return m_impl->engine.addSpan(gpuBase, bytes, home);
}

Status CardView::bufferFor(uint64_t gpuAddress, uint64_t bytes, BufferView* view) {
	return m_impl->engine.bufferFor(gpuAddress, bytes, view);
}

void CardView::shaderWrites(uint64_t gpuAddress, uint64_t bytes) {
	m_impl->engine.shaderWrites(gpuAddress, bytes);
}

Status CardView::beforeSubmit() {
	return m_impl->engine.beforeSubmit();
}

CardView::Totals CardView::totals() const {
	return m_impl->engine.totals();
}

Status CardView::start() {
	return m_impl->engine.start();
}

void CardView::slotChanged(uint64_t gpuAddress, PageNumber page) {
	m_impl->engine.slotChanged(gpuAddress, page);
}

bool CardView::readBack(PageNumber page, uint8_t* out) {
	return m_impl->engine.readBack(page, out);
}

void CardView::cardHomeLeft(PageNumber page) {
	m_impl->engine.cardHomeLeft(page);
}

bool CardView::loaded(PageNumber page, uint64_t offset, const uint8_t* bytes, uint64_t byteCount) {
	return m_impl->engine.loaded(page, offset, bytes, byteCount);
}

void CardView::pageForgotten(PageNumber page) {
	m_impl->engine.pageForgotten(page);
}

void CardView::tiledPages(std::vector<PageNumber>& out) const {
	m_impl->engine.tiledPages(out);
}

} // namespace deunify
