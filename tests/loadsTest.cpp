#include "deunify/loads.hpp"

#include "codecs.hpp"
#include "decodedStore.hpp"
#include "loadBook.hpp"
#include "staging.hpp"
#include "testFiles.hpp"
#include "workerPool.hpp"

#include "deunify/context.hpp"
#include "deunify/pages.hpp"
#include "deunify/physicalMemory.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <limits>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace deunify;
using deunify::test::compressed;
using deunify::test::pattern;
using deunify::test::TempFile;

constexpr uint64_t kUnlimited = std::numeric_limits<uint64_t>::max();
constexpr size_t   kPage      = kPageBytes;
constexpr uint64_t kUnopenedFile = 0xD00D;

PageNumber page(uint64_t index) {
	return PageNumber {index * kFramesPerPage};
}

std::vector<uint8_t> takeWhole(detail::Staging& staging, PageNumber number) {
	std::vector<uint8_t> out(kPage);
	return staging.takeWhole(number, out.data()) ? out : std::vector<uint8_t> {};
}

TEST(Loads, CustomCodecsFollowTheBuiltInOnes) {
	EXPECT_EQ(static_cast<uint32_t>(deunify::customCodec(0)), deunify::kFirstCustomCodec);
}

TEST(Staging, StagedBytesAreGivenOnce) {
	detail::Staging      staging;
	std::vector<uint8_t> bytes = pattern(kPage, 1);
	staging.put(page(7), 0, bytes, kUnlimited);
	EXPECT_EQ(staging.held(), kPage);
	std::vector<uint8_t> out(kPage);
	ASSERT_TRUE(staging.take(page(7), 0, out));
	EXPECT_EQ(out, bytes);
	EXPECT_EQ(staging.held(), 0u) << "gone once given";
	EXPECT_FALSE(staging.take(page(7), 0, out)) << "a second read goes to the files";
	EXPECT_TRUE(takeWhole(staging, page(7)).empty());
}

TEST(Staging, PartsOfAPageAreGivenOnceEach) {
	detail::Staging      staging;
	std::vector<uint8_t> bytes = pattern(kPage, 2);
	staging.put(page(3), 0, bytes, kUnlimited);
	std::vector<uint8_t> part(0x1000);
	ASSERT_TRUE(staging.take(page(3), 0x2000, part));
	EXPECT_EQ(part, std::vector<uint8_t>(bytes.begin() + 0x2000, bytes.begin() + 0x3000));
	EXPECT_FALSE(staging.take(page(3), 0x2800, part)) << "overlaps what was given";
	ASSERT_TRUE(staging.take(page(3), 0x3000, part));
	EXPECT_EQ(part, std::vector<uint8_t>(bytes.begin() + 0x3000, bytes.begin() + 0x4000));
	EXPECT_TRUE(takeWhole(staging, page(3)).empty()) << "no longer whole";
	EXPECT_EQ(staging.held(), 0u) << "and forgotten with it";
}

TEST(Staging, APageLoadedAgainReplacesItsStagedBytes) {
	detail::Staging staging;
	staging.put(page(5), 0, pattern(kPage, 3), kUnlimited);
	std::vector<uint8_t> again = pattern(kPage, 4);
	staging.put(page(5), 0, again, kUnlimited);
	EXPECT_EQ(takeWhole(staging, page(5)), again);
}

TEST(Staging, PartialLoadsBuildAPageOverZeroes) {
	detail::Staging      staging;
	std::vector<uint8_t> first  = pattern(0x100, 5);
	std::vector<uint8_t> second = pattern(0x300, 6);
	staging.put(page(9), 0x100, first, kUnlimited);
	EXPECT_FALSE(staging.covers(page(9), 0, kPage));
	std::vector<uint8_t> out(0x100);
	EXPECT_FALSE(staging.take(page(9), 0, out)) << "never loaded";
	staging.put(page(9), 0x200, second, kUnlimited);
	out.resize(0x400);
	ASSERT_TRUE(staging.take(page(9), 0x100, out)) << "two loads side by side read as one";
	EXPECT_EQ(std::vector<uint8_t>(out.begin(), out.begin() + 0x100), first);
	EXPECT_EQ(std::vector<uint8_t>(out.begin() + 0x100, out.end()), second);
}

TEST(Staging, BytesTheCardTookAtTheirLoadAreNotGivenAgain) {
	detail::Staging      staging;
	std::vector<uint8_t> bytes = pattern(kPage, 7);
	staging.put(page(11), 0, bytes, kUnlimited);
	staging.takenElsewhere(page(11), 0, 0x1000);
	std::vector<uint8_t> out(0x1000);
	EXPECT_FALSE(staging.take(page(11), 0, out));
	ASSERT_TRUE(staging.take(page(11), 0x1000, out));
	EXPECT_EQ(out, std::vector<uint8_t>(bytes.begin() + 0x1000, bytes.begin() + 0x2000));
	staging.takenElsewhere(page(11), 0x2000, kPage - 0x2000);
	EXPECT_EQ(staging.held(), 0u);
}

TEST(Staging, PastTheCapTheOldestAreDropped) {
	detail::Staging staging;
	for (uint32_t i = 0; i < 4; ++i) {
		staging.put(page(i), 0, pattern(kPage, i), 3 * kPage);
	}
	EXPECT_EQ(staging.held(), 3 * kPage);
	EXPECT_TRUE(takeWhole(staging, page(0)).empty()) << "the oldest went: its bytes come from its files";
	staging.put(page(1), 0, pattern(kPage, 9), 3 * kPage);
	staging.put(page(4), 0, pattern(kPage, 4), 3 * kPage);
	EXPECT_TRUE(takeWhole(staging, page(2)).empty()) << "a page loaded again is the newest";
	EXPECT_EQ(takeWhole(staging, page(1)), pattern(kPage, 9));
	EXPECT_EQ(takeWhole(staging, page(3)), pattern(kPage, 3));
	EXPECT_EQ(takeWhole(staging, page(4)), pattern(kPage, 4));
	staging.put(page(5), 0, pattern(kPage, 5), 0);
	EXPECT_EQ(staging.held(), 0u) << "a cap of nothing stages nothing";
}

struct IntHash {
	size_t operator()(uint32_t key) const { return key; }
};

using Store = detail::DecodedStore<uint32_t, IntHash>;

detail::Decoded bytesOf(std::vector<uint8_t> bytes) {
	return std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
}

TEST(DecodedStore, OneDecodePerRequestWhoeverAsks) {
	constexpr int    kAskers = 8;
	Store            store;
	std::atomic<int> made {0};
	std::vector<std::thread> askers;
	std::atomic<int>         right {0};
	for (int i = 0; i < kAskers; ++i) {
		askers.emplace_back([&] {
			detail::Decoded bytes = store.kept(1, kUnlimited, [&] {
				made.fetch_add(1);
				std::this_thread::sleep_for(std::chrono::milliseconds(20));
				return bytesOf(std::vector<uint8_t>(16, 7));
			});
			right.fetch_add(bytes != nullptr && *bytes == std::vector<uint8_t>(16, 7) ? 1 : 0);
		});
	}
	for (std::thread& asker : askers) {
		asker.join();
	}
	EXPECT_EQ(made.load(), 1);
	EXPECT_EQ(right.load(), kAskers);
}

TEST(DecodedStore, PastTheCapARequestIsDecodedAgain) {
	Store store;
	int   made = 0;
	auto  ask  = [&](uint32_t key) {
        return store.kept(key, 24, [&] {
            ++made;
            return bytesOf(std::vector<uint8_t>(16, static_cast<uint8_t>(key)));
        });
	};
	ask(1);
	ask(1);
	EXPECT_EQ(made, 1);
	ask(2);
	EXPECT_EQ(made, 2);
	EXPECT_EQ(*ask(1), std::vector<uint8_t>(16, 1)) << "dropped for the newer one, made again";
	EXPECT_EQ(made, 3);
}

TEST(DecodedStore, AFailedDecodeIsNotKept) {
	Store store;
	EXPECT_EQ(store.kept(1, kUnlimited, [] { return detail::Decoded {}; }), nullptr);
	EXPECT_FALSE(store.has(1));
	detail::Decoded again = store.kept(1, kUnlimited, [] { return bytesOf({1}); });
	ASSERT_NE(again, nullptr);
	EXPECT_EQ(*again, std::vector<uint8_t> {1});
}

constexpr uint8_t kXorKey = 0x5A;

Status xorCodec(void* context, Codec codec, const uint8_t* in, size_t inBytes, uint8_t* out, size_t outBytes) {
	*static_cast<int*>(context) += 1;
	if (codec != customCodec(0) || inBytes != outBytes) {
		return Status::DecodeFailed;
	}
	for (size_t i = 0; i < inBytes; ++i) {
		out[i] = in[i] ^ kXorKey;
	}
	return Status::Ok;
}

std::vector<uint8_t> xored(std::vector<uint8_t> bytes) {
	for (uint8_t& byte : bytes) {
		byte ^= kXorKey;
	}
	return bytes;
}

TEST(Codecs, ZlibRoundTrip) {
	std::vector<uint8_t> plain = pattern(3 * kPage + 0x123, 1);
	std::vector<uint8_t> bytes = compressed(plain);
	detail::CodecSteps   steps {Codec::Zlib, Codec::None, 0, static_cast<uint32_t>(plain.size())};
	ASSERT_EQ(detail::decode(steps, {}, bytes), Status::Ok);
	EXPECT_EQ(bytes, plain);
}

TEST(Codecs, ZlibOfTheWrongSizeFails) {
	std::vector<uint8_t> plain = pattern(0x1000, 2);
	for (uint32_t size : {0x0FFFu, 0x1001u}) {
		std::vector<uint8_t> bytes = compressed(plain);
		detail::CodecSteps   steps {Codec::Zlib, Codec::None, 0, size};
		EXPECT_EQ(detail::decode(steps, {}, bytes), Status::DecodeFailed) << size;
	}
}

TEST(Codecs, DamagedZlibFails) {
	std::vector<uint8_t> plain = pattern(0x1000, 3);
	std::vector<uint8_t> bytes = compressed(plain);
	bytes.resize(bytes.size() / 2);
	detail::CodecSteps steps {Codec::Zlib, Codec::None, 0, static_cast<uint32_t>(plain.size())};
	EXPECT_EQ(detail::decode(steps, {}, bytes), Status::DecodeFailed);
}

TEST(Codecs, ACustomCodecFollowsZlibThroughTheCallback) {
	std::vector<uint8_t>  plain = pattern(0x2345, 4);
	std::vector<uint8_t>  bytes = compressed(xored(plain));
	int                   calls = 0;
	detail::CustomDecoder custom {xorCodec, &calls};
	uint32_t              size = static_cast<uint32_t>(plain.size());
	detail::CodecSteps    steps {Codec::Zlib, customCodec(0), size, size};
	ASSERT_EQ(detail::decode(steps, custom, bytes), Status::Ok);
	EXPECT_EQ(bytes, plain);
	EXPECT_EQ(calls, 1);
}

TEST(Codecs, ACustomCodecWithoutACallbackIsNotSupported) {
	std::vector<uint8_t> bytes = pattern(0x100, 5);
	detail::CodecSteps   steps {customCodec(3), Codec::None, 0, 0x100};
	EXPECT_EQ(detail::decode(steps, {}, bytes), Status::NotSupported);
}

TEST(Codecs, UncompressedBytesMustHaveTheirSize) {
	std::vector<uint8_t> bytes = pattern(0x100, 6);
	EXPECT_EQ(detail::decode({Codec::None, Codec::None, 0, 0x100}, {}, bytes), Status::Ok);
	EXPECT_EQ(detail::decode({Codec::None, Codec::None, 0, 0x80}, {}, bytes), Status::DecodeFailed);
}

TEST(WorkerPool, EveryJobRunsOnceAcrossTheThreads) {
	constexpr size_t   kJobs = 1000;
	detail::WorkerPool pool(4);
	std::vector<std::atomic<int>> runs(kJobs);
	for (int round = 0; round < 3; ++round) {
		pool.each(kJobs, [&](size_t i) { runs[i].fetch_add(1); });
	}
	for (const std::atomic<int>& count : runs) {
		EXPECT_EQ(count.load(), 3);
	}
}

TEST(LoadBook, PiecesFollowTheDestinationAcrossPages) {
	std::vector<PageNumber>        pages  = {page(1), page(2), page(3)};
	std::vector<detail::PagePiece> pieces = detail::piecesOf(pages, 0x80, 2 * kPage);
	ASSERT_EQ(pieces.size(), 3u);
	EXPECT_EQ(pieces[0].within, 0x80u);
	EXPECT_EQ(pieces[0].length, kPage - 0x80);
	EXPECT_TRUE(pieces[1].whole());
	EXPECT_EQ(pieces[2].length, 0x80u);
	EXPECT_TRUE(detail::piecesOf(pages, 0, 3 * kPage + 1).empty()) << "too few pages";
	EXPECT_TRUE(detail::piecesOf(pages, kPage, 1).empty()) << "an offset past the first page";
	EXPECT_TRUE(detail::piecesOf(pages, 0, 0).empty());
}

struct Book {
	explicit Book(detail::LoadLimits limits = {kUnlimited, kUnlimited, 128, 4}, detail::CustomDecoder custom = {})
	    : book(limits, custom) {}

	FileId open(const TempFile& file) {
		FileId id = kNoFile;
		EXPECT_EQ(book.openPath(file.path(), &id), Status::Ok);
		return id;
	}

	void record(FileId id, const LoadRequest& request) {
		std::vector<detail::PagePiece> pieces =
		    detail::piecesOf(request.pages, request.offsetInFirstPage, request.destinationBytes);
		book.record(book.file(id), detail::keyOf(request), request.offsetInFirstPage, pieces);
	}

	std::vector<uint8_t> replay(PageNumber number) {
		std::vector<uint8_t> out(kPage);
		return book.replay(number, out.data()) ? out : std::vector<uint8_t> {};
	}

	detail::LoadBook book;
};

LoadRequest plainRequest(FileId file, uint64_t offset, uint32_t bytes, std::span<const PageNumber> pages,
                         uint32_t within) {
	LoadRequest request;
	request.file              = file;
	request.fileOffset        = offset;
	request.sourceBytes       = bytes;
	request.destinationBytes  = bytes;
	request.pages             = pages;
	request.offsetInFirstPage = within;
	return request;
}

std::vector<uint8_t> pageOver(const std::vector<uint8_t>& bytes, size_t from, size_t within, size_t length) {
	std::vector<uint8_t> out(kPage);
	std::copy_n(bytes.begin() + static_cast<ptrdiff_t>(from), length, out.begin() + static_cast<ptrdiff_t>(within));
	return out;
}

TEST(LoadBook, ThePagesOfARequestDecodeItOnce) {
	constexpr uint32_t kBegins = 0x80;
	std::vector<uint8_t> plain = pattern(3 * kPage + 0x100, 1);
	std::vector<uint8_t> zipped = compressed(plain);
	TempFile             file(zipped);
	Book                 book;
	FileId               id    = book.open(file);
	std::vector<PageNumber> pages = {page(1), page(2), page(3), page(4)};
	LoadRequest request  = plainRequest(id, 0, static_cast<uint32_t>(zipped.size()), pages, kBegins);
	request.firstCodec       = Codec::Zlib;
	request.destinationBytes = static_cast<uint32_t>(plain.size());
	book.record(id, request);
	book.book.prepare(pages);
	EXPECT_EQ(book.book.totals().requestsDecoded, 1u);
	std::vector<std::vector<uint8_t>> replayed(pages.size());
	std::vector<std::thread>          readers;
	for (size_t i = 0; i < pages.size(); ++i) {
		readers.emplace_back([&, i] { replayed[i] = book.replay(pages[i]); });
	}
	for (std::thread& reader : readers) {
		reader.join();
	}
	size_t done = 0;
	for (size_t i = 0; i < pages.size(); ++i) {
		size_t within = i == 0 ? kBegins : 0;
		size_t length = std::min(kPage - within, plain.size() - done);
		EXPECT_EQ(replayed[i], pageOver(plain, done, within, length)) << i;
		done += length;
	}
	EXPECT_EQ(book.book.totals().requestsDecoded, 1u) << "every page from the one decode";
	EXPECT_EQ(book.book.totals().pagesReplayed, pages.size());
}

TEST(LoadBook, ManyAskersOfOnePageDecodeItsRequestOnce) {
	constexpr int        kAskers = 8;
	std::vector<uint8_t> plain  = pattern(kPage, 2);
	std::vector<uint8_t> zipped = compressed(plain);
	TempFile             file(zipped);
	Book                 book;
	FileId               id      = book.open(file);
	std::vector<PageNumber> pages = {page(9)};
	LoadRequest request = plainRequest(id, 0, static_cast<uint32_t>(zipped.size()), pages, 0);
	request.firstCodec       = Codec::Zlib;
	request.destinationBytes = static_cast<uint32_t>(kPage);
	book.record(id, request);
	std::atomic<int>         right {0};
	std::vector<std::thread> askers;
	for (int i = 0; i < kAskers; ++i) {
		askers.emplace_back([&] { right.fetch_add(book.replay(page(9)) == plain ? 1 : 0); });
	}
	for (std::thread& asker : askers) {
		asker.join();
	}
	EXPECT_EQ(right.load(), kAskers);
	EXPECT_EQ(book.book.totals().requestsDecoded, 1u);
}

TEST(LoadBook, TheDecodedKeyIncludesSizesAndCodecs) {
	std::vector<uint8_t> plain = pattern(0x3000, 3);
	TempFile             file(plain);
	Book                 book;
	FileId               id    = book.open(file);
	std::vector<PageNumber> first = {page(1)};
	std::vector<PageNumber> second = {page(2)};
	book.record(id, plainRequest(id, 0, 0x1000, first, 0));
	book.record(id, plainRequest(id, 0, 0x3000, second, 0));
	EXPECT_EQ(book.replay(page(1)), pageOver(plain, 0, 0, 0x1000));
	EXPECT_EQ(book.replay(page(2)), pageOver(plain, 0, 0, 0x3000)) << "same file and offset, other size";
	EXPECT_EQ(book.book.totals().requestsDecoded, 2u);
}

TEST(LoadBook, AFailedDecodeIsNotKept) {
	std::vector<uint8_t> garbage = pattern(0x400, 4);
	TempFile             file(garbage);
	Book                 book;
	FileId               id      = book.open(file);
	std::vector<PageNumber> pages = {page(5)};
	LoadRequest request = plainRequest(id, 0, 0x400, pages, 0);
	request.firstCodec       = Codec::Zlib;
	request.destinationBytes = 0x1000;
	book.record(id, request);
	EXPECT_TRUE(book.replay(page(5)).empty());
	EXPECT_TRUE(book.replay(page(5)).empty());
	EXPECT_EQ(book.book.totals().requestsDecoded, 2u) << "asked again, decoded again";
	EXPECT_EQ(book.book.totals().decodedBytes, 0u);
}

TEST(LoadBook, PartialRecordsStopAtTheLimitAndAWholeLoadStartsAnew) {
	std::vector<uint8_t> plain = pattern(kPage, 5);
	TempFile             file(plain);
	Book                 book({kUnlimited, kUnlimited, 2, 1});
	FileId               id    = book.open(file);
	std::vector<PageNumber> pages = {page(3)};
	book.record(id, plainRequest(id, 0, 0x100, pages, 0));
	book.record(id, plainRequest(id, 0x100, 0x100, pages, 0x100));
	EXPECT_TRUE(book.book.rebuildable(page(3)));
	book.record(id, plainRequest(id, 0x200, 0x100, pages, 0x200));
	EXPECT_FALSE(book.book.rebuildable(page(3))) << "a third record would overflow: the page is not rebuildable";
	EXPECT_TRUE(book.replay(page(3)).empty());
	book.record(id, plainRequest(id, 0, static_cast<uint32_t>(kPage), pages, 0));
	EXPECT_TRUE(book.book.rebuildable(page(3)));
	EXPECT_EQ(book.replay(page(3)), plain);
}

TEST(LoadBook, AWholePageLoadReplacesOlderRecords) {
	std::vector<uint8_t> plain = pattern(2 * kPage, 6);
	TempFile             file(plain);
	Book                 book;
	FileId               id    = book.open(file);
	std::vector<PageNumber> pages = {page(4)};
	book.record(id, plainRequest(id, 0, 0x2000, pages, 0x1000));
	book.record(id, plainRequest(id, kPage, static_cast<uint32_t>(kPage), pages, 0));
	EXPECT_EQ(book.replay(page(4)), pageOver(plain, kPage, 0, kPage));
	EXPECT_EQ(book.book.totals().requestsDecoded, 1u) << "the older record is gone";
}

TEST(LoadBook, StagedBytesAreGivenOnceThenThePageIsReplayed) {
	std::vector<uint8_t> plain = pattern(kPage, 7);
	TempFile             file(plain);
	Book                 book;
	FileId               id    = book.open(file);
	std::vector<PageNumber> pages = {page(6)};
	book.record(id, plainRequest(id, 0, static_cast<uint32_t>(kPage), pages, 0));
	book.book.stage(page(6), 0, plain);
	std::vector<uint8_t> part(0x1000);
	ASSERT_TRUE(book.book.takeStaged(page(6), 0x4000, part.data(), part.size()));
	EXPECT_EQ(part, std::vector<uint8_t>(plain.begin() + 0x4000, plain.begin() + 0x5000));
	EXPECT_FALSE(book.book.takeStaged(page(6), 0x4000, part.data(), part.size())) << "given once";
	ASSERT_TRUE(book.book.takeStaged(page(6), 0x5000, part.data(), part.size())) << "the rest is still staged";
	std::vector<uint8_t> whole(kPage);
	EXPECT_FALSE(book.book.takeStagedPage(page(6), whole.data())) << "no longer whole, dropped";
	EXPECT_EQ(book.book.totals().stagedBytes, 0u);
	EXPECT_EQ(book.replay(page(6)), plain);
	EXPECT_EQ(book.book.totals().stagedReads, 2u);
}

TEST(LoadBook, PastTheStagingCapTheOldestPagesAreReplayed) {
	std::vector<uint8_t> plain = pattern(3 * kPage, 8);
	TempFile             file(plain);
	Book                 book({2 * kPage, kUnlimited, 128, 1});
	FileId               id    = book.open(file);
	std::vector<PageNumber> pages = {page(1), page(2), page(3)};
	book.record(id, plainRequest(id, 0, static_cast<uint32_t>(plain.size()), pages, 0));
	for (size_t i = 0; i < pages.size(); ++i) {
		book.book.stage(pages[i], 0, std::span<const uint8_t>(plain.data() + i * kPage, kPage));
	}
	EXPECT_EQ(book.book.totals().stagedBytes, 2 * kPage);
	std::vector<uint8_t> out(kPage);
	EXPECT_FALSE(book.book.takeStagedPage(page(1), out.data())) << "the oldest was dropped";
	EXPECT_EQ(book.replay(page(1)), pageOver(plain, 0, 0, kPage)) << "and is replayed";
	ASSERT_TRUE(book.book.takeStagedPage(page(3), out.data()));
	EXPECT_EQ(out, pageOver(plain, 2 * kPage, 0, kPage));
}

TEST(LoadBook, MemoryWritesAreNotRebuildableUntilAWholeLoad) {
	std::vector<uint8_t> plain = pattern(kPage, 9);
	TempFile             file(plain);
	Book                 book;
	FileId               id    = book.open(file);
	std::vector<PageNumber> pages = {page(8)};
	book.record(id, plainRequest(id, 0, static_cast<uint32_t>(kPage), pages, 0));
	book.book.markUnknown(detail::piecesOf(pages, 0x10, 0x20));
	EXPECT_FALSE(book.book.rebuildable(page(8)));
	EXPECT_TRUE(book.replay(page(8)).empty());
	book.record(id, plainRequest(id, 0x100, 0x100, pages, 0));
	EXPECT_FALSE(book.book.rebuildable(page(8))) << "a partial load does not make it known";
	book.record(id, plainRequest(id, 0, static_cast<uint32_t>(kPage), pages, 0));
	EXPECT_TRUE(book.book.rebuildable(page(8)));
}

TEST(LoadBook, ARecordKeepsItsFileAfterTheEmulatorClosesIt) {
	std::vector<uint8_t> plain = pattern(kPage, 10);
	TempFile             file(plain);
	Book                 book;
	FileId               id    = book.open(file);
	std::vector<PageNumber> pages = {page(2)};
	book.record(id, plainRequest(id, 0, static_cast<uint32_t>(kPage), pages, 0));
	book.book.closeFile(id);
	EXPECT_EQ(book.book.file(id), nullptr);
	EXPECT_EQ(book.replay(page(2)), plain);
	book.book.forget(page(2));
	EXPECT_FALSE(book.book.rebuildable(page(2)));
}

TEST(LoadBook, AFileOpenedByHandleIsDuplicated) {
	std::vector<uint8_t> plain = pattern(kPage, 11);
	TempFile             file(plain);
	HANDLE handle = CreateFileW(file.path(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
	                            FILE_ATTRIBUTE_NORMAL, nullptr);
	ASSERT_NE(handle, INVALID_HANDLE_VALUE);
	detail::LoadBook book({kUnlimited, kUnlimited, 128, 1}, {});
	FileId           id = kNoFile;
	ASSERT_EQ(book.openHandle(handle, &id), Status::Ok);
	CloseHandle(handle);
	std::vector<uint8_t> out(0x100);
	ASSERT_TRUE(book.file(id)->read(0x200, out.data(), 0x100));
	EXPECT_EQ(out, std::vector<uint8_t>(plain.begin() + 0x200, plain.begin() + 0x300));
	EXPECT_FALSE(book.file(id)->read(kPage - 0x80, out.data(), 0x100)) << "past the end";
	std::wstring missing = std::wstring(file.path()) + L".missing";
	EXPECT_EQ(book.openPath(missing.c_str(), &id), Status::NotFound);
}

struct ContextFixture : ::testing::Test {
	void SetUp() override {
		Status status = Status::Ok;
		context       = Context::create(Config {}, Callbacks {}, &status);
		ASSERT_NE(context, nullptr) << static_cast<int>(status);
	}

	std::vector<PageNumber> allocate(uint64_t count) {
		std::vector<PageNumber> pages(count);
		EXPECT_EQ(context->physical().allocate(count, 0, pages.data()), Status::Ok);
		return pages;
	}

	std::unique_ptr<Context> context;
};

TEST_F(ContextFixture, ALoadRecordsEveryPageItWrites) {
	std::vector<uint8_t> plain  = pattern(2 * kPage, 12);
	std::vector<uint8_t> zipped = compressed(plain);
	TempFile             file(zipped);
	FileId               id = kNoFile;
	ASSERT_EQ(context->loads().openFile(file.path(), &id), Status::Ok);
	std::vector<PageNumber> pages   = allocate(2);
	LoadRequest             request = plainRequest(id, 0, static_cast<uint32_t>(zipped.size()), pages, 0);
	request.firstCodec              = Codec::Zlib;
	request.destinationBytes        = static_cast<uint32_t>(plain.size());
	ASSERT_EQ(context->loads().load(request), Status::Ok);
	EXPECT_TRUE(context->loads().rebuildable(pages[0]));
	EXPECT_TRUE(context->loads().rebuildable(pages[1]));
	EXPECT_EQ(context->loads().totals().requestsLoaded, 1u);
	context->loads().closeFile(id);
}

TEST_F(ContextFixture, ALoadIntoACpuReachablePageIsWrittenToRam) {
	constexpr uint64_t kReserve = 4 * kPageBytes;
	void* base = VirtualAlloc2(nullptr, nullptr, kReserve, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS,
	                           nullptr, 0);
	ASSERT_NE(base, nullptr);
	uint64_t                address = reinterpret_cast<uint64_t>(base);
	std::vector<PageNumber> pages   = allocate(1);
	ASSERT_EQ(context->physical().map(address, pages), Status::Ok);
	std::vector<uint8_t> plain = pattern(0x800, 13);
	TempFile             file(plain);
	FileId               id = kNoFile;
	ASSERT_EQ(context->loads().openFile(file.path(), &id), Status::Ok);
	ASSERT_EQ(context->loads().load(plainRequest(id, 0, 0x800, pages, 0x400)), Status::Ok);
	EXPECT_EQ(context->pages().state(pages[0]).home, Home::Ram);
	std::vector<uint8_t> out(0x800);
	ASSERT_EQ(context->pages().readTitle(address + 0x400, out.data(), out.size(), ReadPurpose::Inspect), Status::Ok);
	EXPECT_EQ(out, plain);
	ASSERT_EQ(context->physical().unmap(address, kReserve), Status::Ok);
	VirtualFree(base, 0, MEM_RELEASE);
}

TEST_F(ContextFixture, AMemoryWriteIsNotRebuildable) {
	std::vector<PageNumber> pages = allocate(1);
	std::vector<uint8_t>    bytes = pattern(kPage, 14);
	ASSERT_EQ(context->loads().write(pages, 0, bytes.data(), bytes.size()), Status::Ok);
	EXPECT_FALSE(context->loads().rebuildable(pages[0]));
	EXPECT_EQ(context->pages().state(pages[0]).home, Home::Ram) << "bytes from no file are never staged";
}

TEST_F(ContextFixture, RequestsThatCannotBeDoneChangeNothing) {
	std::vector<PageNumber> pages = allocate(1);
	std::vector<uint8_t>    plain = pattern(0x100, 15);
	TempFile                file(plain);
	FileId                  id = kNoFile;
	ASSERT_EQ(context->loads().openFile(file.path(), &id), Status::Ok);
	EXPECT_EQ(context->loads().load(plainRequest(FileId {kUnopenedFile}, 0, 0x100, pages, 0)), Status::NotFound);
	LoadRequest custom = plainRequest(id, 0, 0x100, pages, 0);
	custom.firstCodec  = customCodec(1);
	EXPECT_EQ(context->loads().load(custom), Status::NotSupported);
	LoadRequest damaged       = plainRequest(id, 0, 0x100, pages, 0);
	damaged.firstCodec        = Codec::Zlib;
	damaged.destinationBytes  = 0x1000;
	EXPECT_EQ(context->loads().load(damaged), Status::DecodeFailed);
	EXPECT_EQ(context->loads().load(plainRequest(id, 0, 0x100, {}, 0)), Status::InvalidArgument);
	EXPECT_FALSE(context->loads().rebuildable(pages[0]));
	EXPECT_EQ(context->pages().state(pages[0]).home, Home::Zero);
}

} // namespace
