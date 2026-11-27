#include "deunify/gpuPageTable.hpp"

#include "memoryFixture.hpp"

#include <gtest/gtest.h>

#include <algorithm>

namespace {

using deunify::ChangeReason;
using deunify::kNoPage;
using deunify::PageBatch;
using deunify::PageNumber;
using deunify::PageRun;
using deunify::PageRunKind;
using deunify::ReadPurpose;
using deunify::Status;
using memoryTest::P;
using memoryTest::readByte;
using memoryTest::writeByte;

class GpuPageTableTest : public memoryTest::Fixture {
protected:
	PageNumber gpuPage(uint64_t address) {
		const auto at = gpu().pageAt(address);
		return at ? at->page : kNoPage;
	}
};

std::span<const PageNumber> one(const PageNumber& page) {
	return {&page, 1};
}

TEST_F(GpuPageTableTest, UnifiedRangesFollowCpuViews) {
	const auto     pages = allocate(2);
	const uint64_t base  = reserve(4);
	ASSERT_EQ(physical().map(base, one(pages[0])), Status::Ok);
	ASSERT_EQ(gpu().followCpuViews(base, 4 * P), Status::Ok);
	const auto taken = gpu().pageAt(base + 3);
	ASSERT_TRUE(taken.has_value());
	EXPECT_EQ(taken->page, pages[0]);
	EXPECT_EQ(taken->offset, 3u);

	delivered();
	ASSERT_EQ(physical().map(base + 2 * P, one(pages[1])), Status::Ok);
	EXPECT_EQ(gpuPage(base + 2 * P), pages[1]);
	std::vector<uint64_t> slots;
	gpu().slotsOf(pages[1], slots);
	EXPECT_EQ(slots, std::vector {base + 2 * P});
	EXPECT_TRUE(covered(delivered(), base + 2 * P, ChangeReason::Remapped));

	ASSERT_EQ(physical().unmap(base + 2 * P, P), Status::Ok);
	EXPECT_EQ(gpuPage(base + 2 * P), kNoPage);
	EXPECT_EQ(gpu().followCpuViews(base + P, P), Status::InvalidArgument);
	EXPECT_EQ(gpu().addGpuRange(base, P), Status::InvalidArgument);
}

TEST_F(GpuPageTableTest, ExplicitMappingsWithoutACardViewAreViewsAtTheGpuAddresses) {
	const auto     pages   = allocate(2);
	const uint64_t cpu     = reserve(2);
	const uint64_t gpuBase = reserve(4);
	ASSERT_EQ(physical().map(cpu, pages), Status::Ok);
	writeByte(cpu, 3);
	writeByte(cpu + P, 4);
	ASSERT_EQ(gpu().addGpuRange(gpuBase, 4 * P), Status::Ok);

	const PageNumber shown[] = {pages[1], kNoPage, pages[0]};
	ASSERT_EQ(gpu().map(gpuBase, shown), Status::Ok);
	EXPECT_EQ(gpuPage(gpuBase), pages[1]);
	EXPECT_EQ(gpuPage(gpuBase + P), kNoPage);
	EXPECT_EQ(gpuPage(gpuBase + 2 * P), pages[0]);
	EXPECT_EQ(readByte(gpuBase), 4);
	EXPECT_EQ(readByte(gpuBase + 2 * P), 3);
	EXPECT_TRUE(covered(delivered(), gpuBase + P, ChangeReason::Remapped));

	std::vector<uint64_t> slots;
	gpu().slotsOf(pages[0], slots);
	EXPECT_EQ(slots, std::vector {gpuBase + 2 * P});

	ASSERT_EQ(gpu().unmap(gpuBase, 4 * P), Status::Ok);
	EXPECT_EQ(gpuPage(gpuBase), kNoPage);
	EXPECT_EQ(gpu().map(cpu, pages), Status::InvalidArgument);
}

TEST_F(GpuPageTableTest, PageMappingCopiesApplyEachRunKind) {
	const auto     pages   = allocate(3);
	const uint64_t pool    = reserve(3);
	const uint64_t gpuBase = reserve(8);
	ASSERT_EQ(physical().map(pool, pages), Status::Ok);
	ASSERT_EQ(gpu().addGpuRange(gpuBase, 8 * P), Status::Ok);
	ASSERT_EQ(gpu().map(gpuBase + 4 * P, one(pages[0])), Status::Ok);
	ASSERT_EQ(gpu().map(gpuBase + 5 * P, one(pages[1])), Status::Ok);

	const PageRun runs[] = {{PageRunKind::Consecutive, 0, 2},
	                        {PageRunKind::Repeated, 2, 2},
	                        {PageRunKind::Unmapped, 0, 1},
	                        {PageRunKind::Unchanged, 0, 1}};
	const PageBatch batch {gpuBase, runs};
	ASSERT_EQ(gpu().copyPageMappings({&batch, 1}, pool, 3), Status::Ok);
	EXPECT_EQ(gpuPage(gpuBase), pages[0]);
	EXPECT_EQ(gpuPage(gpuBase + P), pages[1]);
	EXPECT_EQ(gpuPage(gpuBase + 2 * P), pages[2]);
	EXPECT_EQ(gpuPage(gpuBase + 3 * P), pages[2]);
	EXPECT_EQ(gpuPage(gpuBase + 4 * P), kNoPage);
	EXPECT_EQ(gpuPage(gpuBase + 5 * P), pages[1]);

	const PageRun   past[] = {{PageRunKind::Consecutive, 2, 2}};
	const PageBatch pastBatch {gpuBase + 6 * P, past};
	EXPECT_EQ(gpu().copyPageMappings({&pastBatch, 1}, pool, 3), Status::InvalidArgument);
	EXPECT_EQ(gpuPage(gpuBase + 6 * P), kNoPage);

	ASSERT_EQ(physical().unmap(pool + P, P), Status::Ok);
	const PageRun   gap[] = {{PageRunKind::Repeated, 1, 1}, {PageRunKind::Consecutive, 0, 1}};
	const PageBatch gapBatch {gpuBase + 6 * P, gap};
	EXPECT_EQ(gpu().copyPageMappings({&gapBatch, 1}, pool, 3), Status::UnknownPage);
	EXPECT_EQ(gpuPage(gpuBase + 6 * P), kNoPage);
	EXPECT_EQ(gpuPage(gpuBase + 7 * P), pages[0]);
}

TEST_F(GpuPageTableTest, ReadTakesBytesFromEachPageAndZerosForUnmappedSlots) {
	const auto     pages   = allocate(2);
	const uint64_t cpu     = reserve(2);
	const uint64_t gpuBase = reserve(3);
	ASSERT_EQ(physical().map(cpu, pages), Status::Ok);
	writeByte(cpu + P - 1, 0x11);
	writeByte(cpu + P, 0x22);
	writeByte(cpu + 2 * P - 1, 0x33);
	ASSERT_EQ(gpu().addGpuRange(gpuBase, 3 * P), Status::Ok);
	ASSERT_EQ(gpu().map(gpuBase, pages), Status::Ok);

	uint8_t out[2] = {};
	ASSERT_EQ(gpu().read(gpuBase + P - 1, out, sizeof(out), ReadPurpose::Inspect), Status::Ok);
	EXPECT_EQ(out[0], 0x11);
	EXPECT_EQ(out[1], 0x22);

	EXPECT_EQ(gpu().read(gpuBase + 2 * P - 1, out, sizeof(out), ReadPurpose::Inspect),
	          Status::UnknownPage);
	EXPECT_EQ(out[0], 0x33);
	EXPECT_EQ(out[1], 0);
}

} // namespace
