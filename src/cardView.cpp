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
constexpr uint64_t kDeviceRemoved    = ~0ull;
constexpr wchar_t  kSegmentName[]    = L"deunify card view segment";
constexpr wchar_t  kHeapName[]       = L"deunify card view tiles";
constexpr wchar_t  kUploadName[]     = L"deunify card view upload";
constexpr wchar_t  kReadbackName[]   = L"deunify card view read-back";

uint64_t slotOf(uint64_t address) {
	return address / kTileBytes;
}

uint64_t addressOf(uint64_t slot) {
	return slot * kTileBytes;
}

Status statusOf(HRESULT result) {
	return result == E_OUTOFMEMORY ? Status::OutOfMemory : Status::HostFailure;
}

D3D12_RESOURCE_DESC bufferDesc(uint64_t bytes, D3D12_RESOURCE_FLAGS flags) {
	D3D12_RESOURCE_DESC desc {};
	desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
	desc.Width            = bytes;
	desc.Height           = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels        = 1;
	desc.SampleDesc.Count = 1;
	desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	desc.Flags            = flags;
	return desc;
}

struct HostBuffer {
	ComPtr<ID3D12Resource> resource;
	uint8_t*               data  = nullptr;
	uint64_t               bytes = 0;
};

HRESULT makeHostBuffer(ID3D12Device* device, D3D12_HEAP_TYPE type, uint64_t bytes,
                       HostBuffer* buffer) {
	D3D12_HEAP_PROPERTIES properties {};
	properties.Type                   = type;
	const D3D12_RESOURCE_DESC   desc  = bufferDesc(bytes, D3D12_RESOURCE_FLAG_NONE);
	const D3D12_RESOURCE_STATES state = type == D3D12_HEAP_TYPE_UPLOAD
	                                        ? D3D12_RESOURCE_STATE_GENERIC_READ
	                                        : D3D12_RESOURCE_STATE_COPY_DEST;
	ComPtr<ID3D12Resource>      resource;
	HRESULT result = device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc,
	                                                 state, nullptr, IID_PPV_ARGS(&resource));
	if (FAILED(result)) {
		return result;
	}
	void* data = nullptr;
	result     = resource->Map(0, nullptr, &data);
	if (FAILED(result)) {
		return result;
	}
	resource->SetName(type == D3D12_HEAP_TYPE_UPLOAD ? kUploadName : kReadbackName);
	*buffer = {std::move(resource), static_cast<uint8_t*>(data), bytes};
	return S_OK;
}

struct CommandSet {
	ComPtr<ID3D12CommandAllocator>    allocator;
	ComPtr<ID3D12GraphicsCommandList> list;
};

HRESULT makeCommandSet(ID3D12Device* device, CommandSet* commands) {
	HRESULT result = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
	                                                IID_PPV_ARGS(&commands->allocator));
	if (FAILED(result)) {
		return result;
	}
	result = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, commands->allocator.Get(),
	                                   nullptr, IID_PPV_ARGS(&commands->list));
	if (FAILED(result)) {
		return result;
	}
	return commands->list->Close();
}

HRESULT beginCommands(const CommandSet& commands) {
	HRESULT result = commands.allocator->Reset();
	if (FAILED(result)) {
		return result;
	}
	return commands.list->Reset(commands.allocator.Get(), nullptr);
}

void execute(ID3D12CommandQueue* queue, const CommandSet& commands) {
	ID3D12CommandList* lists[] = {commands.list.Get()};
	queue->ExecuteCommandLists(1, lists);
}

class Mapping {
public:
	void add(uint64_t slot, std::optional<uint32_t> tile) {
		const D3D12_TILE_RANGE_FLAGS flag =
		    tile ? D3D12_TILE_RANGE_FLAG_NONE : D3D12_TILE_RANGE_FLAG_NULL;
		const UINT start = tile.value_or(0);
		if (extends(slot, flag, start)) {
			++m_sizes.back().NumTiles;
			++m_counts.back();
			return;
		}
		D3D12_TILED_RESOURCE_COORDINATE coordinate {};
		coordinate.X = static_cast<UINT>(slot);
		D3D12_TILE_REGION_SIZE size {};
		size.NumTiles = 1;
		m_coordinates.push_back(coordinate);
		m_sizes.push_back(size);
		m_flags.push_back(flag);
		m_starts.push_back(start);
		m_counts.push_back(1);
	}

	void append(const Mapping& other) {
		m_coordinates.insert(m_coordinates.end(), other.m_coordinates.begin(),
		                     other.m_coordinates.end());
		m_sizes.insert(m_sizes.end(), other.m_sizes.begin(), other.m_sizes.end());
		m_flags.insert(m_flags.end(), other.m_flags.begin(), other.m_flags.end());
		m_starts.insert(m_starts.end(), other.m_starts.begin(), other.m_starts.end());
		m_counts.insert(m_counts.end(), other.m_counts.begin(), other.m_counts.end());
	}

	bool empty() const { return m_coordinates.empty(); }

	uint32_t send(ID3D12CommandQueue* queue, ID3D12Resource* buffer, ID3D12Heap* heap) const {
		if (empty()) {
			return 0;
		}
		const UINT ranges = static_cast<UINT>(m_coordinates.size());
		queue->UpdateTileMappings(buffer, ranges, m_coordinates.data(), m_sizes.data(), heap,
		                          ranges, m_flags.data(), m_starts.data(), m_counts.data(),
		                          D3D12_TILE_MAPPING_FLAG_NONE);
		return 1;
	}

private:
	bool extends(uint64_t slot, D3D12_TILE_RANGE_FLAGS flag, UINT start) const {
		if (empty() || m_flags.back() != flag) {
			return false;
		}
		const UINT tiles = m_sizes.back().NumTiles;
		if (m_coordinates.back().X + uint64_t {tiles} != slot) {
			return false;
		}
		return flag == D3D12_TILE_RANGE_FLAG_NULL || m_starts.back() + tiles == start;
	}

	std::vector<D3D12_TILED_RESOURCE_COORDINATE> m_coordinates;
	std::vector<D3D12_TILE_REGION_SIZE>          m_sizes;
	std::vector<D3D12_TILE_RANGE_FLAGS>          m_flags;
	std::vector<UINT>                            m_starts;
	std::vector<UINT>                            m_counts;
};

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

	const uint64_t                           start;
	const uint64_t                           bytes;
	const SpanHome                           home;
	std::unique_ptr<std::atomic<uint64_t>[]> asked;
	std::vector<ComPtr<ID3D12Resource>>      buffers;
	std::atomic<bool>                        made {false};
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
	uint64_t   fence;
	CommandSet commands;
	HostBuffer upload;
};

struct Retired {
	uint64_t           fence;
	ComPtr<ID3D12Heap> heap;
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
	ID3D12Resource*          buffer  = nullptr;
	uint64_t                 offset  = 0;
	uint64_t                 version = 0;
	std::vector<LoadedPiece> overlays;
	CommandSet               commands;
	HostBuffer               readback;
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
	using HeapMaker = std::function<ComPtr<ID3D12Heap>()>;

	std::map<uint64_t, PageNumber>            shown;
	std::map<uint64_t, SlotTile>              mapped;
	std::unordered_map<PageNumber, PageTile>  tiles;
	std::vector<ComPtr<ID3D12Heap>>           heaps;
	std::vector<uint32_t>                     used;
	std::set<Tile>                            free;
	std::vector<ComPtr<ID3D12Heap>>           retiring;
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
	std::vector<CommandSet>                   lists;
	std::vector<HostBuffer>                   uploads;
	std::vector<HostBuffer>                   readbacks;
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

	void addHeap(ComPtr<ID3D12Heap> heap) {
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
			ComPtr<ID3D12Heap> heap = makeHeap();
			if (!heap) {
				return std::nullopt;
			}
			addHeap(std::move(heap));
		}
		const Tile tile = *free.begin();
		free.erase(free.begin());
		++used[tile.heap];
		++tilesHeld;
		return tile;
	}

	void freeTile(Tile tile) {
		free.insert(tile);
		--used[tile.heap];
		--tilesHeld;
		if (isEmptyHeap(tile.heap) && emptyHeaps() > kSpareEmptyHeaps) {
			free.erase(free.lower_bound(Tile {tile.heap, 0}),
			           free.lower_bound(Tile {tile.heap + 1, 0}));
			retiring.push_back(std::move(heaps[tile.heap]));
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

std::vector<Segment> segmentsOf(uint64_t spanBytes) {
	std::vector<Segment> segments;
	for (uint64_t offset = 0; offset < spanBytes; offset += kSegmentStep) {
		segments.push_back(Segment {offset, std::min(kSegmentBytes, spanBytes - offset)});
	}
	return segments;
}

std::optional<size_t> segmentHolding(uint64_t spanStart, uint64_t spanBytes, uint64_t from,
                                     uint64_t to) {
	const uint64_t index  = (from - spanStart) / kSegmentStep;
	const uint64_t offset = index * kSegmentStep;
	if (offset >= spanBytes ||
	    to > spanStart + offset + std::min(kSegmentBytes, spanBytes - offset)) {
		return std::nullopt;
	}
	return static_cast<size_t>(index);
}

Place placeOf(uint64_t spanStart, uint64_t gpuAddress) {
	const uint64_t index = (gpuAddress - spanStart) / kSegmentStep;
	return Place {static_cast<size_t>(index), gpuAddress - spanStart - index * kSegmentStep};
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

uint32_t mapTiles(ID3D12CommandQueue* queue, ID3D12Resource* buffer,
                  std::span<const ComPtr<ID3D12Heap>> heaps, std::span<const SlotMove> moves) {
	Mapping                     unmapped;
	std::map<uint32_t, Mapping> byHeap;
	for (const SlotMove& move: moves) {
		if (move.tile) {
			byHeap[move.tile->heap].add(move.slot, move.tile->index);
		} else {
			unmapped.add(move.slot, std::nullopt);
		}
	}
	uint32_t calls = 0;
	for (auto& [heap, mapping]: byHeap) {
		if (!unmapped.empty()) {
			mapping.append(unmapped);
			unmapped = Mapping {};
		}
		calls += mapping.send(queue, buffer, heaps[heap].Get());
	}
	return calls + unmapped.send(queue, buffer, nullptr);
}

HRESULT makeTileHeap(ID3D12Device* device, ComPtr<ID3D12Heap>* heap) {
	D3D12_HEAP_DESC desc {};
	desc.SizeInBytes     = uint64_t {kHeapTiles} * kTileBytes;
	desc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
	desc.Alignment       = kTileBytes;
	desc.Flags           = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
	const HRESULT result = device->CreateHeap(&desc, IID_PPV_ARGS(heap->ReleaseAndGetAddressOf()));
	if (SUCCEEDED(result)) {
		(*heap)->SetName(kHeapName);
	}
	return result;
}

HRESULT makeReservedBuffer(ID3D12Device* device, uint64_t bytes, ComPtr<ID3D12Resource>* buffer) {
	const D3D12_RESOURCE_DESC desc = bufferDesc(bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
	return device->CreateReservedResource(&desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
	                                      IID_PPV_ARGS(buffer->ReleaseAndGetAddressOf()));
}

bool hasTiledResourcesTier2(ID3D12Device* device) {
	D3D12_FEATURE_DATA_D3D12_OPTIONS options {};
	return SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options,
	                                             sizeof(options))) &&
	       options.TiledResourcesTier >= D3D12_TILED_RESOURCES_TIER_2;
}

struct Engine::Impl {
	Impl(ID3D12Device* device, ID3D12CommandQueue* queue, Surroundings& surroundings)
	    : device(device), queue(queue), surroundings(surroundings),
	      makeHeap([this] { return newHeap(); }) {}

	~Impl() {
		if (fence) {
			std::unique_lock order(queueOrder);
			const uint64_t   value = signal();
			order.unlock();
			waitFor(value);
		}
	}

	ComPtr<ID3D12Device>                         device;
	ComPtr<ID3D12CommandQueue>                   queue;
	Surroundings&                                surroundings;
	State::HeapMaker                             makeHeap;
	ComPtr<ID3D12Fence>                          fence;
	std::array<std::unique_ptr<Span>, kMaxSpans> spans;
	std::atomic<uint32_t>                        spanCount {0};
	std::mutex                                   adding;
	std::mutex                                   making;
	std::mutex                                   queueOrder;
	mutable std::mutex                           stateLock;
	State                                        state;
	uint64_t                                     fenced = 0;

	ComPtr<ID3D12Heap> newHeap() {
		ComPtr<ID3D12Heap> heap;
		if (FAILED(makeTileHeap(device.Get(), &heap))) {
			return nullptr;
		}
		surroundings.heapMade(heap.Get());
		return heap;
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

	std::pair<ID3D12Resource*, uint64_t> bufferAt(uint64_t slot) const {
		const Span* span  = spanHolding(addressOf(slot));
		const Place place = placeOf(span->start, addressOf(slot));
		return {span->buffers[place.segment].Get(), place.offset};
	}

	uint64_t signal() {
		++fenced;
		queue->Signal(fence.Get(), fenced);
		return fenced;
	}

	bool waitFor(uint64_t value) const {
		if (fence->GetCompletedValue() < value &&
		    FAILED(fence->SetEventOnCompletion(value, nullptr))) {
			return false;
		}
		return fence->GetCompletedValue() != kDeviceRemoved;
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

	Status takeCommands(CommandSet* commands) {
		if (!state.lists.empty()) {
			*commands = std::move(state.lists.back());
			state.lists.pop_back();
			return Status::Ok;
		}
		const HRESULT result = makeCommandSet(device.Get(), commands);
		return FAILED(result) ? statusOf(result) : Status::Ok;
	}

	Status takeHostBuffer(std::vector<HostBuffer>& pool, D3D12_HEAP_TYPE type, uint64_t bytes,
	                      HostBuffer* buffer) {
		auto fits = std::find_if(pool.begin(), pool.end(),
		                         [bytes](const HostBuffer& held) { return held.bytes >= bytes; });
		if (fits != pool.end()) {
			*buffer = std::move(*fits);
			pool.erase(fits);
			return Status::Ok;
		}
		const HRESULT result = makeHostBuffer(device.Get(), type, bytes, buffer);
		if (FAILED(result)) {
			return statusOf(result);
		}
		surroundings.bufferMade(buffer->resource.Get());
		return Status::Ok;
	}

	Status takeUpload(uint64_t bytes, HostBuffer* buffer) {
		return takeHostBuffer(state.uploads, D3D12_HEAP_TYPE_UPLOAD,
		                      std::bit_ceil(std::max(bytes, kUploadLeastBytes)), buffer);
	}

	void recycle() {
		const uint64_t     done = fence->GetCompletedValue();
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
		std::vector<ComPtr<ID3D12Resource>> buffers;
		for (const Segment& segment: segmentsOf(span.bytes)) {
			ComPtr<ID3D12Resource> buffer;
			const HRESULT result = makeReservedBuffer(device.Get(), segment.bytes, &buffer);
			if (FAILED(result)) {
				return statusOf(result);
			}
			buffer->SetName(kSegmentName);
			buffers.push_back(std::move(buffer));
		}
		span.buffers = std::move(buffers);
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
		    segmentHolding(span->start, span->bytes, asked.start, asked.end());
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
		view->resource = span->buffers[*segment].Get();
		view->offset   = gpuAddress - span->start - *segment * kSegmentStep;
		view->bytes    = asked.end() - gpuAddress;
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
		std::tie(plan.buffer, plan.offset) = bufferAt(*slot);
		plan.version                       = state.versionOf(page);
		plan.overlays                      = state.loadedOf(page, 0);
		if (takeCommands(&plan.commands) != Status::Ok ||
		    takeHostBuffer(state.readbacks, D3D12_HEAP_TYPE_READBACK, kPageBytes, &plan.readback) !=
		        Status::Ok) {
			return Planned::Lost;
		}
		return Planned::Copy;
	}

	bool copyTile(const ReadPlan& plan, uint64_t* value) {
		std::lock_guard order(queueOrder);
		if (FAILED(beginCommands(plan.commands))) {
			return false;
		}
		plan.commands.list->CopyBufferRegion(plan.readback.resource.Get(), 0, plan.buffer,
		                                     plan.offset, kPageBytes);
		if (FAILED(plan.commands.list->Close())) {
			return false;
		}
		execute(queue.Get(), plan.commands);
		*value = signal();
		return true;
	}

	bool finishReadBack(PageNumber page, ReadPlan& plan, bool copied, uint8_t* out) {
		std::lock_guard lock(stateLock);
		if (copied) {
			std::memcpy(out, plan.readback.data, kPageBytes);
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
			const bool copied = copyTile(plan, &value) && waitFor(value);
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

	uint32_t mapSegments(const std::vector<SlotMove>&           moves,
	                     const std::vector<ComPtr<ID3D12Heap>>& heaps) {
		uint32_t       calls = 0;
		const uint32_t count = spanCount.load(std::memory_order_acquire);
		for (uint32_t i = 0; i < count; ++i) {
			const Span& span = *spans[i];
			if (!span.made.load(std::memory_order_acquire)) {
				continue;
			}
			const std::vector<Segment> segments = segmentsOf(span.bytes);
			for (size_t k = 0; k < segments.size(); ++k) {
				const uint64_t        first = slotOf(span.start + segments[k].offset);
				const uint64_t        last  = first + slotOf(segments[k].bytes);
				std::vector<SlotMove> inside;
				for (const SlotMove& move: moves) {
					if (first <= move.slot && move.slot < last) {
						inside.push_back(SlotMove {move.slot - first, move.tile});
					}
				}
				calls += mapTiles(queue.Get(), span.buffers[k].Get(), heaps, inside);
			}
		}
		return calls;
	}

	void sendMoves() {
		std::lock_guard                 order(queueOrder);
		std::vector<SlotMove>           moves;
		std::vector<ComPtr<ID3D12Heap>> heaps;
		std::vector<ComPtr<ID3D12Heap>> retiring;
		{
			std::lock_guard lock(stateLock);
			moves    = lastMoves(takeAll(state.moves));
			retiring = takeAll(state.retiring);
			heaps    = state.heaps;
		}
		if (moves.empty() && retiring.empty()) {
			return;
		}
		const uint32_t  calls = mapSegments(moves, heaps);
		const uint64_t  value = signal();
		std::lock_guard lock(stateLock);
		for (const SlotMove& move: moves) {
			state.onQueue.set(move.slot, move.tile);
		}
		for (ComPtr<ID3D12Heap>& heap: retiring) {
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
	                   HostBuffer* upload, CommandSet* commands) {
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
			if (upload->resource) {
				state.uploads.push_back(std::move(*upload));
			}
			pieces.clear();
			return status;
		}
		copyHeldBytes(pieces, upload->data);
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

	std::vector<Fate> recordFills(const std::vector<Piece>& pieces, const HostBuffer& upload,
	                              const CommandSet& commands) {
		std::vector<Fate> fates = fatesOf(pieces, upload.data);
		for (size_t i = 0; i < pieces.size(); ++i) {
			if (fates[i] != Fate::Copy) {
				continue;
			}
			const Piece& piece          = pieces[i];
			const auto [buffer, offset] = bufferAt(*state.onQueue.slotShowing(piece.tile));
			commands.list->CopyBufferRegion(buffer, offset + piece.offset, upload.resource.Get(),
			                                piece.uploadOffset, piece.bytes);
			state.counters.fillBytes += piece.bytes;
		}
		return fates;
	}

	Status sendFills(const std::vector<Piece>& pieces, HostBuffer upload, CommandSet commands) {
		std::lock_guard   order(queueOrder);
		std::lock_guard   lock(stateLock);
		std::vector<Fate> fates;
		HRESULT           result = beginCommands(commands);
		if (SUCCEEDED(result)) {
			fates  = recordFills(pieces, upload, commands);
			result = commands.list->Close();
		}
		if (FAILED(result)) {
			for (const Piece& piece: pieces) {
				state.requeue(piece);
			}
			state.lists.push_back(std::move(commands));
			state.uploads.push_back(std::move(upload));
			return statusOf(result);
		}
		execute(queue.Get(), commands);
		state.spent.push_back(Spent {signal(), std::move(commands), std::move(upload)});
		for (size_t i = 0; i < pieces.size(); ++i) {
			if (fates[i] != Fate::Kept) {
				state.sent(pieces[i]);
			} else {
				state.requeue(pieces[i]);
			}
		}
		return Status::Ok;
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
		HostBuffer         upload;
		CommandSet         commands;
		const Status       status = gatherFills(written, pieces, &upload, &commands);
		if (status != Status::Ok || pieces.empty()) {
			return status;
		}
		readFromHomes(pieces, upload.data);
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

Engine::Engine(ID3D12Device* device, ID3D12CommandQueue* queue, Surroundings& surroundings)
    : m_impl(std::make_unique<Impl>(device, queue, surroundings)) {}

Engine::~Engine() = default;

Status Engine::start() {
	if (!hasTiledResourcesTier2(m_impl->device.Get())) {
		return Status::NotSupported;
	}
	const HRESULT result =
	    m_impl->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_impl->fence));
	return FAILED(result) ? statusOf(result) : Status::Ok;
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
	    : context(context), engine(context.config().device, context.config().queue, *this) {}

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

	void heapMade(ID3D12Heap* heap) override {
		static_cast<void>(context.census().track(heap, kCardViewCategory));
	}

	void bufferMade(ID3D12Resource* buffer) override {
		static_cast<void>(
		    context.census().track(context.config().device, buffer, kCardViewCategory));
	}

	Context&         context;
	cardview::Engine engine;
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
