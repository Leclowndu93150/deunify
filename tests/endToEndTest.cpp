#include "deunify/deunify.hpp"

#include "testFiles.hpp"
#include "warp.hpp"

#include <windows.h>

#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <vector>

namespace {

using deunify::BufferView;
using deunify::Home;
using deunify::kPageBytes;
using deunify::PageNumber;
using deunify::Status;

constexpr uint64_t kPages        = 2;
constexpr uint64_t kGpuBase      = 0x20'0000'0000ull;
constexpr uint64_t kShaderOffset = 0x100;
constexpr uint64_t kShaderBytes  = 0x80;
constexpr uint8_t  kShaderValue  = 0x3C;
constexpr uint32_t kFileSeed     = 7;

deunify::Pages* g_pages = nullptr;

LONG CALLBACK sendFaultToPages(EXCEPTION_POINTERS* exception) {
	if (g_pages != nullptr && g_pages->handleFault(exception)) {
		return EXCEPTION_CONTINUE_EXECUTION;
	}
	return EXCEPTION_CONTINUE_SEARCH;
}

std::vector<uint8_t> readTitle(uint64_t address, uint64_t bytes) {
	std::vector<uint8_t> out(bytes);
	const auto*          from = reinterpret_cast<const volatile uint8_t*>(address);
	for (uint64_t i = 0; i < bytes; ++i) {
		out[i] = from[i];
	}
	return out;
}

std::vector<uint8_t> slice(const std::vector<uint8_t>& bytes, uint64_t from, uint64_t count) {
	return std::vector<uint8_t>(bytes.begin() + from, bytes.begin() + from + count);
}

class EndToEnd: public ::testing::Test {
protected:
	void SetUp() override {
		if (!warp.tiled()) {
			GTEST_SKIP() << "WARP without tiled resources tier 2";
		}
		deunify::Config config;
		config.device = warp.device.Get();
		config.queue  = warp.queue.Get();
		Status status = Status::HostFailure;
		context       = deunify::Context::create(config, deunify::Callbacks {}, &status);
		ASSERT_EQ(status, Status::Ok);
		g_pages   = &context->pages();
		m_handler = AddVectoredExceptionHandler(1, sendFaultToPages);
		cpuBase   = reinterpret_cast<uint64_t>(
            VirtualAlloc2(nullptr, nullptr, kPages * kPageBytes,
		                    MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0));
		ASSERT_NE(cpuBase, 0u);
	}

	void TearDown() override {
		if (m_handler != nullptr) {
			RemoveVectoredExceptionHandler(m_handler);
		}
		g_pages = nullptr;
		context.reset();
		if (cpuBase != 0) {
			VirtualFree(reinterpret_cast<void*>(cpuBase), 0, MEM_RELEASE);
		}
	}

	deunify::test::Warp               warp;
	std::unique_ptr<deunify::Context> context;
	uint64_t                          cpuBase = 0;

private:
	void* m_handler = nullptr;
};

TEST_F(EndToEnd, ALoadedFileGoesThroughStagingTilesRamAndTheCard) {
	PageNumber pages[kPages] = {};
	ASSERT_EQ(context->physical().allocate(kPages, 0, pages), Status::Ok);
	ASSERT_EQ(context->physical().map(cpuBase, pages), Status::Ok);
	context->pages().setKind(cpuBase, kPages * kPageBytes, deunify::Kind::Shared);
	ASSERT_EQ(context->gpuPages().addGpuRange(kGpuBase, kPages * kPageBytes), Status::Ok);
	ASSERT_EQ(context->cardView()->addSpan(kGpuBase, kPages * kPageBytes, deunify::SpanHome::Shared),
	          Status::Ok);
	ASSERT_EQ(context->gpuPages().map(kGpuBase, pages), Status::Ok);

	const std::vector<uint8_t> plain  = deunify::test::pattern(kPages * kPageBytes, kFileSeed);
	const std::vector<uint8_t> packed = deunify::test::compressed(plain);
	deunify::test::TempFile    file(packed);
	deunify::FileId            id = deunify::kNoFile;
	ASSERT_EQ(context->loads().openFile(file.path(), &id), Status::Ok);
	deunify::LoadRequest request;
	request.file             = id;
	request.sourceBytes      = static_cast<uint32_t>(packed.size());
	request.firstCodec       = deunify::Codec::Zlib;
	request.destinationBytes = static_cast<uint32_t>(plain.size());
	request.pages            = pages;
	ASSERT_EQ(context->loads().load(request), Status::Ok);
	for (PageNumber page: pages) {
		EXPECT_EQ(context->pages().state(page).home, Home::File);
		EXPECT_FALSE(context->pages().state(page).committed);
	}
	EXPECT_EQ(context->physical().totals().committed, 0u);
	EXPECT_EQ(context->loads().totals().stagedBytes, kPages * kPageBytes);

	BufferView view;
	ASSERT_EQ(context->cardView()->bufferFor(kGpuBase, kPages * kPageBytes, &view), Status::Ok);
	ASSERT_EQ(context->cardView()->beforeSubmit(), Status::Ok);
	EXPECT_EQ(context->loads().totals().stagedReads, kPages);
	EXPECT_EQ(context->loads().totals().stagedBytes, 0u);
	EXPECT_EQ(warp.fetch(view.resource, view.offset, kPages * kPageBytes), plain);
	EXPECT_EQ(context->pages().state(pages[0]).home, Home::File);
	EXPECT_TRUE(context->pages().state(pages[0]).tile);

	EXPECT_EQ(readTitle(cpuBase, kPageBytes), slice(plain, 0, kPageBytes));
	EXPECT_EQ(context->pages().state(pages[0]).home, Home::Ram);
	EXPECT_TRUE(context->pages().state(pages[0]).committed);
	EXPECT_EQ(context->pages().state(pages[1]).home, Home::File);
	EXPECT_EQ(context->pages().counts().replays, 1u);

	context->cardView()->shaderWrites(kGpuBase, kPageBytes);
	ASSERT_EQ(context->cardView()->beforeSubmit(), Status::Ok);
	EXPECT_EQ(context->pages().state(pages[0]).home, Home::Card);
	EXPECT_EQ(context->cardView()->totals().cardHomePages, 1u);
	warp.store(view.resource, view.offset + kShaderOffset,
	           std::vector<uint8_t>(kShaderBytes, kShaderValue));
	std::vector<uint8_t> expected = slice(plain, 0, kPageBytes);
	std::fill_n(expected.begin() + kShaderOffset, kShaderBytes, kShaderValue);
	EXPECT_EQ(readTitle(cpuBase, kPageBytes), expected);
	EXPECT_EQ(context->pages().state(pages[0]).home, Home::Ram);
	EXPECT_EQ(context->pages().counts().readBacks, 1u);

	const deunify::CensusSnapshot census = context->census().snapshot();
	EXPECT_EQ(census.homes.ram, 1u);
	EXPECT_EQ(census.homes.ramTiled, 1u);
	EXPECT_EQ(census.homes.file, 1u);
	EXPECT_EQ(census.homes.card, 0u);
	EXPECT_EQ(census.homes.readBacks, 1u);
	EXPECT_EQ(census.homes.replays, 1u);
	EXPECT_EQ(census.physical.pages, kPages);
	EXPECT_EQ(census.physical.committed, 1u);
	EXPECT_EQ(census.loads.requestsLoaded, 1u);
	EXPECT_EQ(census.cardView.tiles, kPages);
	EXPECT_EQ(census.cardView.heaps, 1u);
	EXPECT_EQ(census.ramAndTileBytes, kPageBytes);
	ASSERT_FALSE(census.categories.empty());
	EXPECT_EQ(census.categories[0].cardCount, 1u);
	EXPECT_EQ(census.categories[0].cardBytes, uint64_t {deunify::kHeapTiles} * kPageBytes);
}

} // namespace
