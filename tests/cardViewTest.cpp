#include "../src/cardViewEngine.hpp"
#include "../src/censusLedger.hpp"
#include "deunify/cardView.hpp"
#include "deunify/context.hpp"
#include "deunify/gpuPageTable.hpp"
#include "deunify/loads.hpp"
#include "deunify/physicalMemory.hpp"
#include "testCard.hpp"

#include <cstring>
#include <gtest/gtest.h>
#include <map>
#include <set>

namespace {

using deunify::BufferView;
using deunify::kPageBytes;
using deunify::PageNumber;
using deunify::Range;
using deunify::SpanHome;
using deunify::Status;
using deunify::cardview::Engine;
using deunify::cardview::SlotMove;
using deunify::cardview::Tile;
using deunify::gpu::BindGroup;
using Runs = std::vector<deunify::gpu::Run>;
using deunify::test::Api;
using deunify::test::TestCard;

constexpr uint64_t kGiB      = 1ull << 30;
constexpr uint64_t kBase     = 0x10'0000'0000ull;
constexpr uint64_t kSlots    = 64;
constexpr uint64_t kProbe    = 256;
constexpr uint64_t kHeapSize = uint64_t {deunify::kHeapTiles};

PageNumber page(uint64_t index) {
	return PageNumber {index * deunify::kFramesPerPage};
}

uint64_t slot(uint64_t index) {
	return kBase + index * kPageBytes;
}

std::vector<uint8_t> pattern(uint8_t seed) {
	std::vector<uint8_t> bytes(kPageBytes);
	for (size_t i = 0; i < bytes.size(); ++i) {
		bytes[i] = static_cast<uint8_t>(i * 7 + seed);
	}
	return bytes;
}

std::vector<uint8_t> filled(uint64_t count, uint8_t value) {
	return std::vector<uint8_t>(count, value);
}

class Home final: public deunify::cardview::Surroundings {
public:
	bool bytesForTile(PageNumber at, uint8_t* out) override {
		if (card.contains(at)) {
			return false;
		}
		auto bytes = ram.find(at);
		if (bytes == ram.end()) {
			std::memset(out, 0, kPageBytes);
		} else {
			std::memcpy(out, bytes->second.data(), kPageBytes);
		}
		return true;
	}

	bool becomeCardHome(PageNumber at, uint8_t* out) override {
		if (!bytesForTile(at, out)) {
			return false;
		}
		card.insert(at);
		ram.erase(at);
		return true;
	}

	void cardHomeLost(PageNumber at, const uint8_t* bytes) override {
		if (card.erase(at) > 0) {
			ram[at].assign(bytes, bytes + kPageBytes);
			cameHome.push_back(at);
		}
	}

	void tileChanged(PageNumber at, bool tile) override { tiles[at] = tile; }

	void takeWritten(std::vector<PageNumber>& out) override {
		out.insert(out.end(), written.begin(), written.end());
		written.clear();
	}

	void prepare(std::span<const PageNumber>) override {}

	void shaderWritten(Range gpuRange) override { shaderWrites.push_back(gpuRange); }

	void deliverChanges() override {}

	std::optional<Range> regionOf(uint64_t) override { return std::nullopt; }

	std::map<PageNumber, std::vector<uint8_t>> ram;
	std::set<PageNumber>                       card;
	std::vector<PageNumber>                    written;
	std::vector<PageNumber>                    cameHome;
	std::map<PageNumber, bool>                 tiles;
	std::vector<Range>                         shaderWrites;
};

class CardViewOnCard: public ::testing::TestWithParam<Api> {
protected:
	void SetUp() override {
		card = deunify::test::makeTestCard(GetParam());
		if (const std::string missing = card->missing(); !missing.empty()) {
			GTEST_SKIP() << missing;
		}
		ledger->addCategory("card view");
		backend = deunify::gpu::makeBackend(card->config(), deunify::Callbacks {}, ledger);
		engine  = std::make_unique<Engine>(*backend, home);
		ASSERT_EQ(engine->start(), Status::Ok);
	}

	std::vector<uint8_t> readBack(PageNumber at) {
		std::vector<uint8_t> bytes(kPageBytes);
		EXPECT_TRUE(engine->readBack(at, bytes.data()));
		return bytes;
	}

	uint32_t bind(deunify::gpu::Region& region, std::span<deunify::gpu::Block* const> blocks,
	              const std::vector<SlotMove>& moves) {
		const std::vector<BindGroup> groups = deunify::gpu::bindGroups(moves);
		backend->bind(region, groups, blocks);
		backend->submitBinds();
		return static_cast<uint32_t>(groups.size());
	}

	uint64_t cardHeaps() const { return ledger->uses()[0].cardCount; }

	bool zeros() const { return backend->unboundReadsZero(); }

	std::unique_ptr<TestCard>                card;
	std::shared_ptr<deunify::detail::Ledger> ledger = std::make_shared<deunify::detail::Ledger>();
	Home                                     home;
	std::unique_ptr<deunify::gpu::Backend>   backend;
	std::unique_ptr<Engine>                  engine;
};

INSTANTIATE_TEST_SUITE_P(Backends, CardViewOnCard, ::testing::ValuesIn(deunify::test::kApis),
                         deunify::test::apiName);

TEST(CardView, EveryTwoGiBViewFitsInOneSegment) {
	static_assert(deunify::kSegmentBytes - deunify::kSegmentStep >= (2ull << 30));
	static_assert(deunify::kHeapTiles * deunify::kPageBytes == (64ull << 20));
	const uint64_t start    = 0x5'0000'0000ull;
	const uint64_t bytes    = 16 * kGiB;
	const auto     segments = deunify::cardview::segmentsOf(bytes);
	ASSERT_EQ(segments.size(), 8u);
	EXPECT_EQ(segments[7].offset, 14 * kGiB);
	EXPECT_EQ(segments[7].bytes, 2 * kGiB);
	EXPECT_EQ(segments[6].bytes, 4 * kGiB);
	for (uint64_t from: {start, start + 2 * kGiB - kPageBytes, start + 5 * kGiB + 12345,
	                     start + bytes - (256ull << 20)}) {
		const uint64_t              to = std::min(from + 2 * kGiB, start + bytes);
		const std::optional<size_t> k  = deunify::cardview::segmentHolding(start, bytes, from, to);
		ASSERT_TRUE(k.has_value());
		EXPECT_LT(from - start - segments[*k].offset, deunify::kSegmentStep);
		EXPECT_LE(to, start + segments[*k].offset + segments[*k].bytes);
	}
	EXPECT_FALSE(deunify::cardview::segmentHolding(start, bytes, start, start + 4 * kGiB + 1));
}

TEST(CardView, ASlotIsPlacedInItsSpansSegment) {
	using deunify::cardview::placeOf;
	const uint64_t view = 0x5'0000'0000ull;
	EXPECT_EQ(placeOf(view, view).segment, 0u);
	EXPECT_EQ(placeOf(view, view + 2 * kGiB - kPageBytes).offset, 2 * kGiB - kPageBytes);
	EXPECT_EQ(placeOf(view, view + 5 * kGiB + 3 * kPageBytes).segment, 2u);
	EXPECT_EQ(placeOf(view, view + 5 * kGiB + 3 * kPageBytes).offset, kGiB + 3 * kPageBytes);
}

TEST(CardView, OnlyTheLastMoveOfASlotCounts) {
	const Tile                  a {0, 2};
	const std::vector<SlotMove> moves = deunify::cardview::lastMoves(
	    {{9, a}, {3, Tile {0, 1}}, {3, std::nullopt}, {9, std::nullopt}, {4, a}});
	const std::vector<SlotMove> expected = {{3, std::nullopt}, {4, a}, {9, std::nullopt}};
	EXPECT_EQ(moves, expected);
}

TEST(CardView, MovesGoUpAsRunsGroupedByBlock) {
	const std::vector<SlotMove>  moves  = {{0, Tile {0, 0}},  {1, Tile {0, 1}}, {2, Tile {1, 0}},
	                                       {3, Tile {1, 1}},  {4, Tile {0, 2}}, {5, std::nullopt},
	                                       {6, std::nullopt}, {8, Tile {0, 3}}};
	const std::vector<BindGroup> groups = deunify::gpu::bindGroups(moves);
	ASSERT_EQ(groups.size(), 2u);
	EXPECT_EQ(groups[0].block, 0u);
	const Runs first = {{0, 2, 0}, {4, 1, 2}, {8, 1, 3}, {5, 2, std::nullopt}};
	EXPECT_EQ(groups[0].runs, first);
	EXPECT_EQ(groups[1].block, 1u);
	EXPECT_EQ(groups[1].runs, (Runs {{2, 2, 0}}));
	const std::vector<SlotMove>  unbindOnly = {{3, std::nullopt}, {4, std::nullopt}};
	const std::vector<BindGroup> alone      = deunify::gpu::bindGroups(unbindOnly);
	ASSERT_EQ(alone.size(), 1u);
	EXPECT_FALSE(alone[0].block.has_value());
	EXPECT_EQ(alone[0].runs, (Runs {{3, 2, std::nullopt}}));
}

TEST_P(CardViewOnCard, RunsAndTheirUnmappingsGoUpInOneCallPerHeap) {
	std::shared_ptr<deunify::gpu::Block> a;
	std::shared_ptr<deunify::gpu::Block> b;
	ASSERT_EQ(backend->makeBlock(&a), Status::Ok);
	ASSERT_EQ(backend->makeBlock(&b), Status::Ok);
	const std::vector<deunify::gpu::Block*> heaps = {a.get(), b.get()};
	std::unique_ptr<deunify::gpu::Region>   region;
	ASSERT_EQ(backend->makeRegion(6 * kPageBytes, &region), Status::Ok);
	const BufferView            buffer = region->view(0, 6 * kPageBytes);
	const std::vector<SlotMove> first  = {
	    {0, Tile {0, 0}}, {1, Tile {0, 1}}, {2, Tile {1, 0}}, {3, Tile {1, 1}}, {4, Tile {0, 2}}};
	EXPECT_EQ(bind(*region, heaps, first), 2u);
	for (uint8_t i = 0; i < 5; ++i) {
		card->store(buffer, i * kPageBytes, filled(kProbe, static_cast<uint8_t>(i + 1)));
	}
	const std::vector<SlotMove> second = {{4, std::nullopt}, {5, Tile {0, 1}}};
	EXPECT_EQ(bind(*region, heaps, second), 1u);
	if (zeros()) {
		EXPECT_EQ(card->fetch(buffer, 4 * kPageBytes, kProbe), filled(kProbe, 0));
	}
	EXPECT_EQ(card->fetch(buffer, 5 * kPageBytes, kProbe), filled(kProbe, 2));
	const std::vector<SlotMove> third = {{5, Tile {1, 1}}};
	bind(*region, heaps, third);
	EXPECT_EQ(card->fetch(buffer, 5 * kPageBytes, kProbe), filled(kProbe, 4));
	EXPECT_EQ(card->fetch(buffer, 0, kProbe), filled(kProbe, 1));
	EXPECT_EQ(card->fetch(buffer, 2 * kPageBytes, kProbe), filled(kProbe, 3));
	const std::vector<SlotMove> unmapOnly = {{0, std::nullopt}, {1, std::nullopt}};
	EXPECT_EQ(bind(*region, heaps, unmapOnly), 1u);
	card->drain();
}

TEST_P(CardViewOnCard, SlotsShowTheirTilesAndNothingOnceUnmapped) {
	std::shared_ptr<deunify::gpu::Block> block;
	ASSERT_EQ(backend->makeBlock(&block), Status::Ok);
	const std::vector<deunify::gpu::Block*> heaps = {block.get()};
	std::unique_ptr<deunify::gpu::Region>   region;
	ASSERT_EQ(backend->makeRegion(4 * kPageBytes, &region), Status::Ok);
	const BufferView            buffer = region->view(0, 4 * kPageBytes);
	const Tile                  a {0, 2};
	const Tile                  b {0, 0};
	const std::vector<SlotMove> mapped = {{0, a}, {1, b}, {2, a}};
	bind(*region, heaps, mapped);
	card->store(buffer, 0, filled(kProbe, 0x11));
	card->store(buffer, kPageBytes, filled(kProbe, 0x22));
	EXPECT_EQ(card->fetch(buffer, 2 * kPageBytes, kProbe), filled(kProbe, 0x11));
	EXPECT_EQ(card->fetch(buffer, kPageBytes, kProbe), filled(kProbe, 0x22));
	if (zeros()) {
		EXPECT_EQ(card->fetch(buffer, 3 * kPageBytes, kProbe), filled(kProbe, 0));
	}
	const std::vector<SlotMove> unmapped = {{0, std::nullopt}};
	bind(*region, heaps, unmapped);
	if (zeros()) {
		EXPECT_EQ(card->fetch(buffer, 0, kProbe), filled(kProbe, 0));
	}
	EXPECT_EQ(card->fetch(buffer, 2 * kPageBytes, kProbe), filled(kProbe, 0x11));
	card->drain();
}

TEST_P(CardViewOnCard, APageHasOneTileForAllTheSlotsThatShowIt) {
	ASSERT_EQ(engine->addSpan(kBase, kSlots * kPageBytes, SpanHome::Shared), Status::Ok);
	home.ram[page(1)] = pattern(1);
	home.ram[page(2)] = pattern(2);
	engine->slotChanged(slot(0), page(1));
	engine->slotChanged(slot(5), page(1));
	engine->slotChanged(slot(1), page(2));
	BufferView view;
	ASSERT_EQ(engine->bufferFor(slot(0), 6 * kPageBytes, &view), Status::Ok);
	EXPECT_EQ(view.offset, 0u);
	EXPECT_EQ(view.bytes, 6 * kPageBytes);
	EXPECT_EQ(engine->tileAt(slot(0)), engine->tileAt(slot(5)));
	EXPECT_NE(engine->tileAt(slot(0)), engine->tileAt(slot(1)));
	EXPECT_EQ(engine->totals().tiles, 2u);
	EXPECT_TRUE(home.tiles[page(1)]);
	ASSERT_EQ(engine->beforeSubmit(), Status::Ok);
	EXPECT_EQ(card->fetch(view, 0, kPageBytes), pattern(1));
	EXPECT_EQ(card->fetch(view, 5 * kPageBytes, kPageBytes), pattern(1));
	EXPECT_EQ(card->fetch(view, kPageBytes, kPageBytes), pattern(2));
	if (zeros()) {
		EXPECT_EQ(card->fetch(view, 2 * kPageBytes, kProbe), filled(kProbe, 0));
	}
	engine->slotChanged(slot(0), deunify::kNoPage);
	EXPECT_EQ(engine->totals().tiles, 2u);
	engine->slotChanged(slot(5), deunify::kNoPage);
	EXPECT_EQ(engine->totals().tiles, 1u);
	EXPECT_FALSE(home.tiles[page(1)]);
	ASSERT_EQ(engine->beforeSubmit(), Status::Ok);
	if (zeros()) {
		EXPECT_EQ(card->fetch(view, 0, kProbe), filled(kProbe, 0));
	}
}

TEST_P(CardViewOnCard, AViewAsksForTheSlotsOfItsOwnRangeOnly) {
	ASSERT_EQ(engine->addSpan(kBase, kSlots * kPageBytes, SpanHome::Shared), Status::Ok);
	for (uint64_t i = 0; i < 5; ++i) {
		engine->slotChanged(slot(i), page(i + 1));
	}
	BufferView view;
	ASSERT_EQ(engine->bufferFor(slot(2) + 100, kPageBytes, &view), Status::Ok);
	EXPECT_EQ(view.offset, 2 * kPageBytes + 100);
	EXPECT_EQ(view.bytes, kPageBytes);
	EXPECT_FALSE(engine->tileAt(slot(1)));
	EXPECT_TRUE(engine->tileAt(slot(2)));
	EXPECT_TRUE(engine->tileAt(slot(3)));
	EXPECT_FALSE(engine->tileAt(slot(4)));
	EXPECT_EQ(engine->bufferFor(kBase - kPageBytes, kPageBytes, &view), Status::NotFound);
	EXPECT_EQ(engine->bufferFor(slot(kSlots - 1), 2 * kPageBytes, &view), Status::NotFound);
	ASSERT_EQ(engine->bufferFor(slot(4) + 10, 0, &view), Status::Ok);
	EXPECT_EQ(view.bytes, kSlots * kPageBytes - 4 * kPageBytes - 10);
	EXPECT_TRUE(engine->tileAt(slot(0)));
	const uint64_t wide = kBase + 64 * kGiB;
	ASSERT_EQ(engine->addSpan(wide, 2 * deunify::kLargestAskBytes, SpanHome::Shared), Status::Ok);
	EXPECT_EQ(engine->bufferFor(wide, deunify::kLargestAskBytes + kPageBytes, &view),
	          Status::TooLarge);
	EXPECT_EQ(engine->totals().tooLargeAsks, 1u);
	EXPECT_EQ(engine->addSpan(wide, kPageBytes, SpanHome::Shared), Status::InvalidArgument);
}

TEST_P(CardViewOnCard, TilesComeFromTheLowestHeapAndEmptyHeapsPastTheSpareGo) {
	ASSERT_EQ(engine->addSpan(kBase, deunify::kLargestAskBytes, SpanHome::Shared), Status::Ok);
	const uint64_t pages = 2 * kHeapSize + 1;
	for (uint64_t i = 0; i < pages; ++i) {
		engine->slotChanged(slot(i), page(i + 1));
	}
	BufferView view;
	ASSERT_EQ(engine->bufferFor(kBase, pages * kPageBytes, &view), Status::Ok);
	EXPECT_EQ(engine->totals().heaps, 3u);
	EXPECT_EQ(cardHeaps(), 3u);
	EXPECT_EQ(engine->tileAt(slot(0)), (Tile {0, 0}));
	EXPECT_EQ(engine->tileAt(slot(kHeapSize - 1)), (Tile {0, uint32_t(kHeapSize - 1)}));
	EXPECT_EQ(engine->tileAt(slot(kHeapSize)), (Tile {1, 0}));
	EXPECT_EQ(engine->tileAt(slot(2 * kHeapSize)), (Tile {2, 0}));
	engine->slotChanged(slot(5), deunify::kNoPage);
	const uint64_t later = 3000;
	engine->slotChanged(slot(later), page(later + 1));
	ASSERT_EQ(engine->bufferFor(slot(later), kPageBytes, &view), Status::Ok);
	EXPECT_EQ(engine->tileAt(slot(later)), (Tile {0, 5}));
	for (uint64_t i = kHeapSize; i < 2 * kHeapSize; ++i) {
		engine->slotChanged(slot(i), deunify::kNoPage);
	}
	EXPECT_EQ(engine->totals().heaps, 3u);
	EXPECT_EQ(engine->retiredHeaps(), 0u);
	engine->slotChanged(slot(2 * kHeapSize), deunify::kNoPage);
	EXPECT_EQ(engine->totals().heaps, 2u);
	EXPECT_EQ(engine->retiredHeaps(), 1u);
	ASSERT_EQ(engine->beforeSubmit(), Status::Ok);
	EXPECT_EQ(engine->retiredHeaps(), 1u);
	card->drain();
	ASSERT_EQ(engine->beforeSubmit(), Status::Ok);
	EXPECT_EQ(engine->retiredHeaps(), 0u);
	engine->slotChanged(slot(later + 1), page(later + 2));
	ASSERT_EQ(engine->bufferFor(slot(later + 1), kPageBytes, &view), Status::Ok);
	EXPECT_EQ(engine->tileAt(slot(later + 1)), (Tile {1, 0}));
	EXPECT_EQ(engine->totals().heaps, 2u);
	EXPECT_EQ(cardHeaps(), 2u) << "the spare heap took the tile, no heap was made";
}

TEST_P(CardViewOnCard, ACardPageKeepsItsTileWithoutSlotsUntilItsBytesAreReadBack) {
	ASSERT_EQ(engine->addSpan(kBase, kSlots * kPageBytes, SpanHome::Card), Status::Ok);
	const PageNumber p = page(1);
	home.ram[p]        = pattern(3);
	engine->slotChanged(slot(2), p);
	BufferView view;
	ASSERT_EQ(engine->bufferFor(slot(2), kPageBytes, &view), Status::Ok);
	ASSERT_EQ(engine->bufferFor(slot(3), kPageBytes, &view), Status::Ok);
	ASSERT_EQ(engine->beforeSubmit(), Status::Ok);
	EXPECT_TRUE(engine->isCardHome(p));
	EXPECT_TRUE(home.card.contains(p));
	EXPECT_EQ(readBack(p), pattern(3));
	card->store(view, 2 * kPageBytes, filled(kProbe, 0xAB));
	std::vector<uint8_t> written = pattern(3);
	std::fill_n(written.begin(), kProbe, uint8_t {0xAB});
	EXPECT_EQ(readBack(p), written);
	const std::optional<Tile> tile = engine->tileAt(slot(2));
	engine->slotChanged(slot(2), deunify::kNoPage);
	EXPECT_EQ(engine->totals().tiles, 1u);
	engine->slotChanged(slot(3), p);
	EXPECT_EQ(engine->tileAt(slot(3)), tile);
	ASSERT_EQ(engine->beforeSubmit(), Status::Ok);
	EXPECT_EQ(card->fetch(view, 3 * kPageBytes, kPageBytes), written);
	engine->slotChanged(slot(3), deunify::kNoPage);
	ASSERT_EQ(engine->beforeSubmit(), Status::Ok);
	EXPECT_EQ(home.cameHome, std::vector<PageNumber> {p});
	EXPECT_EQ(home.ram[p], written);
	EXPECT_FALSE(engine->isCardHome(p));
	EXPECT_EQ(engine->totals().tiles, 0u);
}

TEST_P(CardViewOnCard, ALoadIntoAPageShadersWroteGoesToItsTile) {
	ASSERT_EQ(engine->addSpan(kBase, kSlots * kPageBytes, SpanHome::Shared), Status::Ok);
	const PageNumber p = page(1);
	home.ram[p]        = pattern(4);
	engine->slotChanged(slot(0), p);
	BufferView view;
	ASSERT_EQ(engine->bufferFor(slot(0), kPageBytes, &view), Status::Ok);
	ASSERT_EQ(engine->beforeSubmit(), Status::Ok);
	EXPECT_FALSE(engine->isCardHome(p));
	engine->shaderWrites(slot(0), kPageBytes);
	ASSERT_EQ(engine->beforeSubmit(), Status::Ok);
	EXPECT_TRUE(engine->isCardHome(p));
	ASSERT_EQ(home.shaderWrites.size(), 1u);
	EXPECT_EQ(home.shaderWrites[0].start, slot(0));
	EXPECT_EQ(home.shaderWrites[0].bytes, kPageBytes);
	const uint64_t             within = 0x100;
	const std::vector<uint8_t> load   = filled(32, 0x5C);
	EXPECT_TRUE(engine->loaded(p, within, load.data(), load.size()));
	std::vector<uint8_t> expected = pattern(4);
	std::copy(load.begin(), load.end(), expected.begin() + within);
	EXPECT_EQ(readBack(p), expected);
	ASSERT_EQ(engine->beforeSubmit(), Status::Ok);
	EXPECT_EQ(card->fetch(view, 0, kPageBytes), expected);
	EXPECT_EQ(readBack(p), expected);
	EXPECT_FALSE(engine->loaded(page(9), 0, load.data(), load.size()));
}

TEST_P(CardViewOnCard, PagesTheCpuWroteAreSentToTheirTiles) {
	ASSERT_EQ(engine->addSpan(kBase, kSlots * kPageBytes, SpanHome::Shared), Status::Ok);
	const PageNumber p = page(1);
	engine->slotChanged(slot(7), p);
	BufferView view;
	ASSERT_EQ(engine->bufferFor(slot(7), kPageBytes, &view), Status::Ok);
	ASSERT_EQ(engine->beforeSubmit(), Status::Ok);
	EXPECT_EQ(card->fetch(view, 7 * kPageBytes, kPageBytes), filled(kPageBytes, 0));
	home.ram[p] = pattern(9);
	home.written.push_back(p);
	home.written.push_back(page(2));
	ASSERT_EQ(engine->beforeSubmit(), Status::Ok);
	EXPECT_EQ(card->fetch(view, 7 * kPageBytes, kPageBytes), pattern(9));
	EXPECT_EQ(engine->totals().fillBytes, 2 * kPageBytes);
}

class Library: public ::testing::TestWithParam<Api> {
protected:
	void SetUp() override {
		card = deunify::test::makeTestCard(GetParam());
		if (const std::string missing = card->missing(); !missing.empty()) {
			GTEST_SKIP() << missing;
		}
		Status status = Status::Ok;
		context       = deunify::Context::create(card->config(), deunify::Callbacks {}, &status);
		ASSERT_EQ(status, Status::Ok);
		ASSERT_EQ(context->physical().allocate(1, 0, &p), Status::Ok);
		ASSERT_EQ(context->gpuPages().addGpuRange(kBase, kSlots * kPageBytes), Status::Ok);
		ASSERT_EQ(context->cardView()->addSpan(kBase, kSlots * kPageBytes, SpanHome::Shared),
		          Status::Ok);
		const PageNumber pages[] = {p};
		ASSERT_EQ(context->gpuPages().map(kBase, pages), Status::Ok);
		ASSERT_EQ(context->cardView()->bufferFor(kBase, kPageBytes, &view), Status::Ok);
		context->cardView()->shaderWrites(kBase, kPageBytes);
		ASSERT_EQ(context->cardView()->beforeSubmit(), Status::Ok);
	}

	std::unique_ptr<TestCard>         card;
	std::unique_ptr<deunify::Context> context;
	PageNumber                        p {};
	BufferView                        view;
};

INSTANTIATE_TEST_SUITE_P(Backends, Library, ::testing::ValuesIn(deunify::test::kApis),
                         deunify::test::apiName);

TEST_P(Library, ACardPageReadByTheEmulatorShowsWhatTheCardWrote) {
	card->store(view, view.offset, filled(kProbe, 0x3C));
	std::vector<uint8_t> bytes(kProbe);
	ASSERT_EQ(context->gpuPages().read(kBase, bytes.data(), kProbe, deunify::ReadPurpose::Inspect),
	          Status::Ok);
	EXPECT_EQ(bytes, filled(kProbe, 0x3C));
	EXPECT_EQ(context->cardView()->totals().cardHomePages, 1u);
}

TEST_P(Library, AWriteIntoAPageShadersWroteGoesToItsTile) {
	const std::vector<uint8_t> load    = filled(32, 0x77);
	const PageNumber           pages[] = {p};
	ASSERT_EQ(context->loads().write(pages, 0x40, load.data(), load.size()), Status::Ok);
	ASSERT_EQ(context->cardView()->beforeSubmit(), Status::Ok);
	EXPECT_EQ(card->fetch(view, view.offset + 0x40, load.size()), load);
}

} // namespace
