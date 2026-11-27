#include "deunify.h"

#include "testFiles.hpp"

#include "deunify/cardView.hpp"
#include "deunify/context.hpp"
#include "deunify/types.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using deunify::test::compressed;
using deunify::test::pattern;
using deunify::test::TempFile;

constexpr uint32_t kOtherVersion = DEUNIFY_ABI_VERSION + 1;

TEST(CApi, ValuesMatchTheCppHeaders) {
	static_assert(DEUNIFY_PAGE_BYTES == deunify::kPageBytes);
	static_assert(DEUNIFY_FRAMES_PER_PAGE == deunify::kFramesPerPage);
	static_assert(DEUNIFY_PAGE_NUMBER_LIMIT == deunify::kPageNumberLimit);
	static_assert(DEUNIFY_VERSION_MISMATCH == static_cast<int32_t>(deunify::Status::VersionMismatch));
	static_assert(DEUNIFY_HOME_RAM == static_cast<int32_t>(deunify::Home::Ram));
	static_assert(DEUNIFY_KIND_GPU_ONLY == static_cast<int32_t>(deunify::Kind::GpuOnly));
	static_assert(DEUNIFY_FIRST_CUSTOM_CODEC == deunify::kFirstCustomCodec);
	static_assert(sizeof(DeunifyRange) == sizeof(deunify::Range));
	static_assert(sizeof(DeunifyBufferView) == 5 * sizeof(uint64_t));
	static_assert(sizeof(DeunifyVulkanSetup) == 6 * sizeof(void*) + 4 * sizeof(uint32_t));
	static_assert(sizeof(DeunifySpanAddress) == sizeof(deunify::SpanAddress));
	SUCCEED();
}

TEST(CApi, DefaultsAreTheCppDefaults) {
	DeunifyConfig config;
	deunify_config_defaults(&config);
	const deunify::Config defaults;
	EXPECT_EQ(config.version, DEUNIFY_ABI_VERSION);
	EXPECT_EQ(config.maxRecordsPerPage, defaults.maxRecordsPerPage);
	EXPECT_EQ(config.stagingCapBytes, defaults.stagingCapBytes);
	EXPECT_EQ(config.decodedCapBytes, defaults.decodedCapBytes);
	EXPECT_EQ(config.decodeWorkers, defaults.decodeWorkers);
	EXPECT_EQ(config.device, nullptr);
	EXPECT_EQ(config.queue, nullptr);
}

TEST(CApi, AnUnknownVersionCreatesNothing) {
	DeunifyConfig config;
	deunify_config_defaults(&config);
	DeunifyCallbacks callbacks {};
	callbacks.version       = DEUNIFY_ABI_VERSION;
	DeunifyContext* context = reinterpret_cast<DeunifyContext*>(&config);
	config.version          = kOtherVersion;
	EXPECT_EQ(deunify_create(&config, &callbacks, &context), DEUNIFY_VERSION_MISMATCH);
	EXPECT_EQ(context, nullptr);
	config.version    = DEUNIFY_ABI_VERSION;
	callbacks.version = kOtherVersion;
	EXPECT_EQ(deunify_create(&config, &callbacks, &context), DEUNIFY_VERSION_MISMATCH);
	EXPECT_EQ(context, nullptr);
	EXPECT_EQ(deunify_create(nullptr, &callbacks, &context), DEUNIFY_INVALID_ARGUMENT);
	EXPECT_EQ(deunify_create(&config, &callbacks, nullptr), DEUNIFY_INVALID_ARGUMENT);
}

TEST(CApi, NullHandlesAreRefusedOrIgnored) {
	deunify_destroy(nullptr);
	deunify_kernel_end(nullptr);
	deunify_sites_free(nullptr);
	DeunifyKernelWrite* bracket = reinterpret_cast<DeunifyKernelWrite*>(&bracket);
	EXPECT_EQ(deunify_kernel_begin(nullptr, 0, 0, DEUNIFY_KERNEL_WRITE, &bracket), DEUNIFY_INVALID_ARGUMENT);
	EXPECT_EQ(bracket, nullptr) << "no bracket on failure";
	DeunifyLoadRequest request {};
	request.version = DEUNIFY_ABI_VERSION;
	EXPECT_EQ(deunify_loads_load(nullptr, &request), DEUNIFY_INVALID_ARGUMENT);
	uint64_t file = 0;
	EXPECT_EQ(deunify_loads_open_path(nullptr, L"x", &file), DEUNIFY_INVALID_ARGUMENT);
	EXPECT_EQ(deunify_card_before_submit(nullptr), DEUNIFY_INVALID_ARGUMENT);
	DeunifySites* sites = nullptr;
	EXPECT_EQ(deunify_sites_load(nullptr, &sites), DEUNIFY_INVALID_ARGUMENT);
	uint64_t offset = 0;
	EXPECT_EQ(deunify_sites_offset(nullptr, "site", &offset), DEUNIFY_INVALID_ARGUMENT);
}

struct Changes {
	std::vector<DeunifyRange> ranges;
};

void collectChanges(void* context, const DeunifyRange* ranges, size_t count, uint32_t) {
	auto* changes = static_cast<Changes*>(context);
	changes->ranges.insert(changes->ranges.end(), ranges, ranges + count);
}

TEST(CApi, AContextWithoutADeviceRoundTrips) {
	DeunifyConfig config;
	deunify_config_defaults(&config);
	Changes          changes;
	DeunifyCallbacks callbacks {};
	callbacks.version      = DEUNIFY_ABI_VERSION;
	callbacks.context      = &changes;
	callbacks.pagesChanged = collectChanges;
	DeunifyContext* context = nullptr;
	ASSERT_EQ(deunify_create(&config, &callbacks, &context), DEUNIFY_OK);
	ASSERT_NE(context, nullptr);

	uint64_t pages[2] = {};
	ASSERT_EQ(deunify_physical_allocate(context, 2, 0, pages), DEUNIFY_OK);
	EXPECT_TRUE(deunify::isValidPageNumber(pages[0]));

	std::vector<uint8_t> plain  = pattern(2 * DEUNIFY_PAGE_BYTES, 1);
	std::vector<uint8_t> zipped = compressed(plain);
	TempFile             file(zipped);
	uint64_t             id = 0;
	ASSERT_EQ(deunify_loads_open_path(context, file.path(), &id), DEUNIFY_OK);
	DeunifyLoadRequest request {};
	request.version          = DEUNIFY_ABI_VERSION;
	request.file             = id;
	request.sourceBytes      = static_cast<uint32_t>(zipped.size());
	request.firstCodec       = DEUNIFY_CODEC_ZLIB;
	request.destinationBytes = static_cast<uint32_t>(plain.size());
	request.pages            = pages;
	request.pageCount        = 2;
	ASSERT_EQ(deunify_loads_load(context, &request), DEUNIFY_OK);
	request.version = kOtherVersion;
	EXPECT_EQ(deunify_loads_load(context, &request), DEUNIFY_VERSION_MISMATCH);
	deunify_loads_close_file(context, id);
	EXPECT_EQ(deunify_loads_rebuildable(context, pages[1]), 1);
	DeunifyLoadTotals totals {};
	deunify_loads_totals(context, &totals);
	EXPECT_EQ(totals.requestsLoaded, 1u);

	DeunifyKernelWrite* bracket = nullptr;
	EXPECT_EQ(deunify_card_before_submit(context), DEUNIFY_UNAVAILABLE);
	uint64_t written[1] = {};
	size_t   count      = 0;
	EXPECT_EQ(deunify_pages_take_written(context, written, 1, &count), DEUNIFY_OK);
	EXPECT_EQ(deunify_kernel_begin(context, 0, 0, 7, &bracket), DEUNIFY_INVALID_ARGUMENT);
	EXPECT_EQ(bracket, nullptr);

	DeunifyCensusTotals census {};
	census.version = DEUNIFY_ABI_VERSION;
	ASSERT_EQ(deunify_census_totals(context, &census), DEUNIFY_OK);
	EXPECT_EQ(census.pagesAllocated, 2u);
	EXPECT_EQ(census.loads.requestsLoaded, 1u);
	size_t length = 0;
	ASSERT_EQ(deunify_census_text(context, nullptr, 0, &length), DEUNIFY_OK);
	std::string text(length + 1, '\0');
	ASSERT_EQ(deunify_census_text(context, text.data(), text.size(), &length), DEUNIFY_OK);
	EXPECT_EQ(text.find('\0'), length);
	uint32_t category = deunify_census_add_category(context, "emulator buffers");
	EXPECT_NE(category, 0u);

	deunify_deliver_changes(context);
	EXPECT_EQ(deunify_physical_free(context, pages, 2), DEUNIFY_OK);
	deunify_destroy(context);
}

} // namespace
