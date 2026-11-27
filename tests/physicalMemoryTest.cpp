#include "deunify/physicalMemory.hpp"

#include "memoryFixture.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <thread>

namespace {

using deunify::kNoPage;
using deunify::PageNumber;
using deunify::Status;
using memoryTest::P;
using memoryTest::readByte;
using memoryTest::stateAt;
using memoryTest::writeByte;

constexpr uint64_t kTitleNumberMask = 0x1fffff;
constexpr uint64_t kTitleNumberShift = 4;
constexpr int      kRemapRounds      = 200;

class PhysicalMemoryTest : public memoryTest::Fixture {};

std::span<const PageNumber> one(const PageNumber& page) {
	return {&page, 1};
}

TEST(PhysicalMemory, PageNumbersAreFrameNumbersOfWholePages) {
	EXPECT_TRUE(deunify::isValidPageNumber(deunify::kFramesPerPage));
	EXPECT_FALSE(deunify::isValidPageNumber(0));
	EXPECT_FALSE(deunify::isValidPageNumber(deunify::kFramesPerPage + 1));
	EXPECT_FALSE(deunify::isValidPageNumber(deunify::kPageNumberLimit));
}

TEST_F(PhysicalMemoryTest, NumbersArePackedAndReusedOnlyAfterTheLastViewIsGone) {
	const auto pages = allocate(3);
	EXPECT_EQ(pages, (std::vector {PageNumber {16}, PageNumber {32}, PageNumber {48}}));
	for (const PageNumber page: pages) {
		EXPECT_EQ(((value(page) >> kTitleNumberShift) & kTitleNumberMask) << kTitleNumberShift,
		          value(page));
	}
	const uint64_t base = reserve(1);
	ASSERT_EQ(physical().map(base, one(pages[1])), Status::Ok);
	ASSERT_EQ(physical().free(pages), Status::Ok);
	EXPECT_FALSE(physical().allocated(pages[1]));
	EXPECT_EQ(physical().totals().pages, 0u);

	EXPECT_EQ(allocate(2), (std::vector {pages[0], pages[2]}));
	ASSERT_EQ(physical().unmap(base, P), Status::Ok);
	EXPECT_EQ(allocate(1), std::vector {pages[1]});
	EXPECT_EQ(physical().totals().pages, 3u);

	const PageNumber unknown[] = {pages[0], PageNumber {value(pages[2]) + 100 * deunify::kFramesPerPage}};
	EXPECT_EQ(physical().free(unknown), Status::UnknownPage);
	EXPECT_TRUE(physical().allocated(pages[0]));
}

TEST_F(PhysicalMemoryTest, MappingCommitsNothingAndATouchCommitsThatPageOnly) {
	const auto     pages  = allocate(2);
	const uint64_t base   = reserve(2);
	const uint64_t second = base + P;
	ASSERT_EQ(physical().map(base, pages), Status::Ok);
	EXPECT_FALSE(physical().committed(pages[0]));
	EXPECT_FALSE(physical().committed(pages[1]));
	EXPECT_EQ(physical().totals().committed, 0u);
	EXPECT_EQ(stateAt(second), static_cast<DWORD>(MEM_RESERVE));

	EXPECT_EQ(readByte(second + 5), 0);
	EXPECT_TRUE(physical().committed(pages[1]));
	EXPECT_FALSE(physical().committed(pages[0]));
	EXPECT_EQ(stateAt(second), static_cast<DWORD>(MEM_COMMIT));
	EXPECT_EQ(stateAt(base), static_cast<DWORD>(MEM_RESERVE));
	EXPECT_EQ(physical().totals().committed, 1u);

	writeByte(second, 7);
	ASSERT_EQ(physical().unmap(base, 2 * P), Status::Ok);
	ASSERT_EQ(physical().map(base, pages), Status::Ok);
	EXPECT_EQ(readByte(second), 7);
	EXPECT_EQ(physical().totals().committed, 1u);
}

TEST_F(PhysicalMemoryTest, TwoViewsOfOnePageSeeEachOthersWrites) {
	const auto     pages = allocate(3);
	const uint64_t a     = reserve(3);
	const uint64_t b     = reserve(3);
	ASSERT_EQ(physical().map(a, pages), Status::Ok);
	ASSERT_EQ(physical().map(b + P, one(pages[2])), Status::Ok);

	writeByte(a + 2 * P, 7);
	EXPECT_EQ(readByte(b + P), 7);
	writeByte(b + P + 1, 9);
	EXPECT_EQ(readByte(a + 2 * P + 1), 9);

	std::vector<uint64_t> views;
	physical().viewsOf(pages[2], views);
	std::sort(views.begin(), views.end());
	std::vector<uint64_t> expected {a + 2 * P, b + P};
	std::sort(expected.begin(), expected.end());
	EXPECT_EQ(views, expected);

	const auto at = physical().pageAt(b + P + 5);
	ASSERT_TRUE(at.has_value());
	EXPECT_EQ(at->page, pages[2]);
	EXPECT_EQ(at->offset, 5u);
	EXPECT_FALSE(physical().pageAt(b).has_value());

	std::vector<deunify::View> inA;
	physical().viewsIn(a, 3 * P, inA);
	ASSERT_EQ(inA.size(), 1u);
	EXPECT_EQ(inA[0].address, a);
	EXPECT_EQ(inA[0].bytes, 3 * P);
	EXPECT_EQ(inA[0].firstPage, pages[0]);
}

TEST_F(PhysicalMemoryTest, AFreedPageStaysReadableThroughARemainingView) {
	const auto     pages = allocate(1);
	const uint64_t base  = reserve(1);
	ASSERT_EQ(physical().map(base, pages), Status::Ok);
	writeByte(base, 5);
	ASSERT_EQ(physical().free(pages), Status::Ok);
	EXPECT_EQ(readByte(base), 5);
	EXPECT_FALSE(physical().allocated(pages[0]));
	ASSERT_TRUE(physical().pageAt(base).has_value());
	EXPECT_EQ(physical().pageAt(base)->page, pages[0]);
	EXPECT_EQ(physical().totals().sections, 1u);

	ASSERT_EQ(physical().unmap(base, P), Status::Ok);
	EXPECT_EQ(physical().totals().sections, 0u);
	EXPECT_EQ(physical().map(base, pages), Status::UnknownPage);
}

TEST_F(PhysicalMemoryTest, UnmapCutsALongerViewAndKeepsTheOutsideParts) {
	const auto     pages = allocate(4);
	const uint64_t base  = reserve(4);
	ASSERT_EQ(physical().map(base, pages), Status::Ok);
	for (uint64_t k = 0; k < 4; ++k) {
		writeByte(base + k * P, static_cast<uint8_t>(10 + k));
	}

	ASSERT_EQ(physical().unmap(base + P, 2 * P), Status::Ok);
	std::vector<deunify::View> views;
	physical().viewsIn(base, 4 * P, views);
	ASSERT_EQ(views.size(), 2u);
	EXPECT_EQ(views[0].address, base);
	EXPECT_EQ(views[0].bytes, P);
	EXPECT_EQ(views[0].firstPage, pages[0]);
	EXPECT_EQ(views[1].address, base + 3 * P);
	EXPECT_EQ(views[1].firstPage, pages[3]);
	EXPECT_FALSE(physical().pageAt(base + P).has_value());
	EXPECT_EQ(stateAt(base + P), static_cast<DWORD>(MEM_RESERVE));
	EXPECT_EQ(readByte(base), 10);
	EXPECT_EQ(readByte(base + 3 * P), 13);

	const PageNumber swapped[] = {pages[2], pages[1]};
	ASSERT_EQ(physical().map(base + P, swapped), Status::Ok);
	EXPECT_EQ(readByte(base + P), 12);
	EXPECT_EQ(readByte(base + 2 * P), 11);

	ASSERT_EQ(physical().unmap(base, 4 * P), Status::Ok);
	ASSERT_EQ(physical().map(base, pages), Status::Ok);
	views.clear();
	physical().viewsIn(base, 4 * P, views);
	ASSERT_EQ(views.size(), 1u);
	EXPECT_EQ(views[0].bytes, 4 * P);
	EXPECT_EQ(readByte(base + 2 * P), 12);
}

TEST_F(PhysicalMemoryTest, ScatterMapsOnePageAtEachAddressAndNoPageUnmaps) {
	const auto     pages = allocate(3);
	const uint64_t a     = reserve(3);
	ASSERT_EQ(physical().map(a, pages), Status::Ok);
	writeByte(a, 1);
	ASSERT_EQ(physical().unmap(a + P, P), Status::Ok);
	EXPECT_EQ(stateAt(a + P), static_cast<DWORD>(MEM_RESERVE));
	EXPECT_EQ(readByte(a), 1);

	const uint64_t addresses[] = {a + P};
	ASSERT_EQ(physical().mapScatter(addresses, one(pages[0])), Status::Ok);
	EXPECT_EQ(readByte(a + P), 1);

	ASSERT_EQ(physical().mapScatter(addresses, one(kNoPage)), Status::Ok);
	EXPECT_FALSE(physical().pageAt(a + P).has_value());
	EXPECT_EQ(stateAt(a + P), static_cast<DWORD>(MEM_RESERVE));

	const uint64_t   twice[] = {a + P, a + P};
	const PageNumber later[] = {pages[0], pages[2]};
	ASSERT_EQ(physical().mapScatter(twice, later), Status::Ok);
	EXPECT_EQ(physical().pageAt(a + P)->page, pages[2]);
}

TEST_F(PhysicalMemoryTest, MapRefusesUnknownPagesAndTargetsThatAreNoPlaceholders) {
	const auto       pages     = allocate(1);
	const uint64_t   base      = reserve(2);
	const PageNumber unknown[] = {pages[0], PageNumber {value(pages[0]) + 100 * deunify::kFramesPerPage}};
	EXPECT_EQ(physical().map(base, unknown), Status::UnknownPage);
	EXPECT_FALSE(physical().pageAt(base).has_value());
	EXPECT_EQ(physical().map(base + 1, pages), Status::InvalidArgument);

	void* committed = VirtualAlloc(nullptr, P, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	ASSERT_NE(committed, nullptr);
	EXPECT_EQ(physical().map(reinterpret_cast<uint64_t>(committed), pages), Status::NotPlaceholder);
	VirtualFree(committed, 0, MEM_RELEASE);
	EXPECT_EQ(physical().map(base, pages), Status::Ok);
}

TEST_F(PhysicalMemoryTest, AWriteWhileItsViewIsCutOrReplacedWaitsForTheRemap) {
	const auto     pages = allocate(4);
	const uint64_t base  = reserve(4);
	ASSERT_EQ(physical().map(base, pages), Status::Ok);
	writeByte(base, 0);
	std::atomic<bool> stop {false};
	std::thread       writer([&] {
        uint8_t next = 0;
        while (!stop.load()) {
            writeByte(base, ++next);
        }
    });
	bool mapped = true;
	for (int round = 0; round < kRemapRounds && mapped; ++round) {
		mapped = physical().unmap(base + 2 * P, P) == Status::Ok &&
		         physical().map(base, pages) == Status::Ok;
	}
	stop.store(true);
	writer.join();
	EXPECT_TRUE(mapped);
	writeByte(base, 0x5a);
	EXPECT_EQ(readByte(base), 0x5a);
}

TEST_F(PhysicalMemoryTest, LongViewsAreFoundPageByPage) {
	constexpr uint64_t kPages = 80;
	const auto         pages  = allocate(kPages);
	const uint64_t     base   = reserve(kPages);
	ASSERT_EQ(physical().map(base, pages), Status::Ok);
	std::vector<uint64_t> views;
	physical().viewsOf(pages[70], views);
	EXPECT_EQ(views, std::vector {base + 70 * P});
	EXPECT_EQ(physical().pageAt(base + 70 * P + 3)->page, pages[70]);

	ASSERT_EQ(physical().unmap(base + 60 * P, P), Status::Ok);
	views.clear();
	physical().viewsOf(pages[70], views);
	EXPECT_EQ(views, std::vector {base + 70 * P});
	views.clear();
	physical().viewsOf(pages[60], views);
	EXPECT_TRUE(views.empty());
}

} // namespace
