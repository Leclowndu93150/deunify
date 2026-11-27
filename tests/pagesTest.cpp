#include "deunify/deunify.hpp"

#include <gtest/gtest.h>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace {

using deunify::Context;
using deunify::Home;
using deunify::kPageBytes;
using deunify::PageNumber;
using deunify::Status;

constexpr uint64_t kKindBase = 0x10000;

deunify::Pages* g_pages = nullptr;

LONG CALLBACK onFault(EXCEPTION_POINTERS* exception) {
	return g_pages != nullptr && g_pages->handleFault(exception) ? EXCEPTION_CONTINUE_EXECUTION
	                                                             : EXCEPTION_CONTINUE_SEARCH;
}

std::vector<uint8_t> pattern(uint64_t bytes) {
	std::vector<uint8_t> out(bytes);
	for (uint64_t i = 0; i < bytes; ++i) {
		out[i] = static_cast<uint8_t>((i * 7 + i / kPageBytes) % 251);
	}
	return out;
}

class PagesTest : public ::testing::Test {
protected:
	void SetUp() override {
		m_context = Context::create(deunify::Config {}, deunify::Callbacks {});
		ASSERT_NE(m_context, nullptr);
		g_pages   = &m_context->pages();
		m_handler = AddVectoredExceptionHandler(1, onFault);
	}

	void TearDown() override {
		RemoveVectoredExceptionHandler(m_handler);
		for (const deunify::Range& range : m_placeholders) {
			m_context->physical().unmap(range.start, range.bytes);
			VirtualFree(reinterpret_cast<void*>(range.start), 0, MEM_RELEASE);
		}
		g_pages = nullptr;
		m_context.reset();
		if (!m_file.empty()) {
			std::filesystem::remove(m_file);
		}
	}

	deunify::Pages&          pages() { return m_context->pages(); }
	deunify::PhysicalMemory& physical() { return m_context->physical(); }

	std::vector<PageNumber> allocate(uint64_t count) {
		std::vector<PageNumber> out(count);
		EXPECT_EQ(physical().allocate(count, 0, out.data()), Status::Ok);
		return out;
	}

	uint64_t mapped(const std::vector<PageNumber>& shown) {
		const uint64_t bytes = shown.size() * kPageBytes;
		void* at = VirtualAlloc2(GetCurrentProcess(), nullptr, bytes,
		                         MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
		EXPECT_NE(at, nullptr);
		const uint64_t address = reinterpret_cast<uint64_t>(at);
		m_placeholders.push_back(deunify::Range {address, bytes});
		EXPECT_EQ(physical().map(address, shown), Status::Ok);
		return address;
	}

	std::vector<PageNumber> taken() {
		std::vector<PageNumber> out;
		pages().takeWritten(out);
		std::sort(out.begin(), out.end());
		return out;
	}

	std::vector<PageNumber> loaded(const std::vector<uint8_t>& bytes,
	                               const std::vector<PageNumber>& into) {
		m_file = std::filesystem::temp_directory_path() /
		         ("deunify-pages-" + std::to_string(GetCurrentProcessId()) + ".bin");
		std::FILE* file = nullptr;
		EXPECT_EQ(_wfopen_s(&file, m_file.c_str(), L"wb"), 0);
		std::fwrite(bytes.data(), 1, bytes.size(), file);
		std::fclose(file);
		deunify::FileId id = deunify::kNoFile;
		EXPECT_EQ(m_context->loads().openFile(m_file.c_str(), &id), Status::Ok);
		deunify::LoadRequest request;
		request.file             = id;
		request.sourceBytes      = static_cast<uint32_t>(bytes.size());
		request.destinationBytes = static_cast<uint32_t>(bytes.size());
		request.pages            = into;
		EXPECT_EQ(m_context->loads().load(request), Status::Ok);
		return into;
	}

	Home home(PageNumber page) const { return m_context->pages().state(page).home; }

	static MEMORY_BASIC_INFORMATION query(uint64_t address) {
		MEMORY_BASIC_INFORMATION info {};
		EXPECT_NE(VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)), 0u);
		return info;
	}

	static void write(uint64_t address, uint8_t byte) {
		*reinterpret_cast<volatile uint8_t*>(address) = byte;
	}

	static uint8_t read(uint64_t address) {
		return *reinterpret_cast<const volatile uint8_t*>(address);
	}

	static bool same(uint64_t address, const uint8_t* expected, uint64_t bytes) {
		return std::equal(expected, expected + bytes, reinterpret_cast<const uint8_t*>(address));
	}

	std::unique_ptr<Context>    m_context;
	void*                       m_handler = nullptr;
	std::vector<deunify::Range> m_placeholders;
	std::filesystem::path       m_file;
};

TEST(Pages, ANewPageStateIsZeroHome) {
	const deunify::PageState state;
	EXPECT_EQ(state.home, Home::Zero);
	EXPECT_FALSE(state.committed);
}

TEST(Context, ACardViewNeedsBothADeviceAndAQueue) {
	ID3D12CommandQueue* const notUsed = reinterpret_cast<ID3D12CommandQueue*>(&g_pages);
	deunify::Config           config;
	config.queue  = notUsed;
	Status status = Status::Ok;
	EXPECT_EQ(Context::create(config, deunify::Callbacks {}, &status), nullptr);
	EXPECT_EQ(status, Status::InvalidArgument);
}

TEST(Context, NoHostErrorOnANewThread) {
	uint32_t seen = 1;
	std::thread([&seen] { seen = Context::lastHostError(); }).join();
	EXPECT_EQ(seen, 0u);
}

TEST_F(PagesTest, KindsOverrideEarlierOnesWhereTheyOverlap) {
	pages().setKind(kKindBase, 4 * kPageBytes, deunify::Kind::GpuOnly);
	pages().setKind(kKindBase + kPageBytes, kPageBytes, deunify::Kind::CpuOnly);
	EXPECT_EQ(pages().kindAt(kKindBase - 1), deunify::Kind::Unknown);
	EXPECT_EQ(pages().kindAt(kKindBase), deunify::Kind::GpuOnly);
	EXPECT_EQ(pages().kindAt(kKindBase + kPageBytes), deunify::Kind::CpuOnly);
	EXPECT_EQ(pages().kindAt(kKindBase + 2 * kPageBytes - 1), deunify::Kind::CpuOnly);
	EXPECT_EQ(pages().kindAt(kKindBase + 2 * kPageBytes), deunify::Kind::GpuOnly);
	EXPECT_EQ(pages().kindAt(kKindBase + 4 * kPageBytes), deunify::Kind::Unknown);
	pages().setKind(0, 8 * kPageBytes, deunify::Kind::Unknown);
	EXPECT_EQ(pages().kindAt(kKindBase), deunify::Kind::Unknown);
}

TEST_F(PagesTest, AWriteThroughOneViewOfAPageIsFoundAtEveryViewAndAgainAfter) {
	const std::vector<PageNumber> shown = allocate(2);
	const uint64_t                one   = mapped(shown);
	const uint64_t                two   = mapped(shown);
	write(one, 1);
	write(one + kPageBytes, 1);
	EXPECT_EQ(taken(), shown);
	EXPECT_EQ(query(one + kPageBytes).Protect, static_cast<DWORD>(PAGE_READONLY));
	EXPECT_EQ(query(two + kPageBytes).Protect, static_cast<DWORD>(PAGE_READONLY));
	write(two + kPageBytes + 8, 5);
	EXPECT_EQ(taken(), std::vector<PageNumber> {shown[1]});
	EXPECT_EQ(read(one + kPageBytes + 8), 5);
	write(two + kPageBytes + 9, 6);
	EXPECT_EQ(taken(), std::vector<PageNumber> {shown[1]});
}

TEST_F(PagesTest, ACpuWriteIntoALazilyCommittedPoolPageIsFound) {
	const std::vector<PageNumber> shown = allocate(2);
	const uint64_t                cpu   = mapped(shown);
	const uint64_t                gpu   = mapped(shown);
	EXPECT_TRUE(taken().empty());
	EXPECT_EQ(query(gpu).State, static_cast<DWORD>(MEM_RESERVE));
	EXPECT_EQ(query(cpu + kPageBytes).State, static_cast<DWORD>(MEM_RESERVE));
	write(cpu + kPageBytes + 16, 9);
	EXPECT_EQ(query(cpu + kPageBytes).State, static_cast<DWORD>(MEM_COMMIT));
	EXPECT_EQ(query(cpu).State, static_cast<DWORD>(MEM_RESERVE));
	EXPECT_EQ(home(shown[0]), Home::Zero);
	EXPECT_EQ(home(shown[1]), Home::Ram);
	EXPECT_EQ(taken(), std::vector<PageNumber> {shown[1]});
	EXPECT_EQ(read(gpu + kPageBytes + 16), 9);
	EXPECT_EQ(query(cpu + kPageBytes).Protect, static_cast<DWORD>(PAGE_READONLY));
	write(cpu + kPageBytes + 24, 10);
	EXPECT_EQ(taken(), std::vector<PageNumber> {shown[1]});
	EXPECT_EQ(read(gpu + kPageBytes + 24), 10);
}

TEST_F(PagesTest, APageTheKernelWritesIsFoundFromTheListUntilItsWriteIsDone) {
	const std::vector<PageNumber> shown = allocate(1);
	const uint64_t                one   = mapped(shown);
	const uint64_t                two   = mapped(shown);
	write(one, 1);
	EXPECT_EQ(taken(), shown);
	{
		const deunify::KernelWrite writing(pages(), one, kPageBytes);
		ASSERT_EQ(writing.status(), Status::Ok);
		EXPECT_EQ(taken(), shown);
		EXPECT_EQ(query(one).Protect, static_cast<DWORD>(PAGE_READWRITE));
		write(one + 3, 7);
	}
	EXPECT_EQ(taken(), shown);
	EXPECT_EQ(query(one).Protect, static_cast<DWORD>(PAGE_READONLY));
	EXPECT_TRUE(taken().empty());
	EXPECT_EQ(read(two + 3), 7);
}

TEST_F(PagesTest, AKernelReadOfAZeroPageCommitsItAndLeavesArmedPagesArmed) {
	const std::vector<PageNumber> shown = allocate(2);
	const uint64_t                at    = mapped(shown);
	write(at, 1);
	EXPECT_EQ(taken(), std::vector<PageNumber> {shown[0]});
	const deunify::KernelWrite reading(pages(), at, 2 * kPageBytes,
	                                   deunify::KernelWrite::Access::Read);
	ASSERT_EQ(reading.status(), Status::Ok);
	EXPECT_EQ(query(at + kPageBytes).State, static_cast<DWORD>(MEM_COMMIT));
	EXPECT_EQ(home(shown[1]), Home::Ram);
	EXPECT_TRUE(pages().state(shown[0]).armed);
	EXPECT_EQ(query(at).Protect, static_cast<DWORD>(PAGE_READONLY));
}

TEST_F(PagesTest, ANewViewOfAReadOnlyPageIsMadeReadOnlyAndItsWritesFound) {
	const std::vector<PageNumber> shown = allocate(1);
	const uint64_t                one   = mapped(shown);
	write(one, 1);
	EXPECT_EQ(taken(), shown);
	const uint64_t three = mapped(shown);
	EXPECT_EQ(query(three).Protect, static_cast<DWORD>(PAGE_READONLY));
	write(three + 8, 4);
	EXPECT_EQ(taken(), shown);
	EXPECT_EQ(read(one + 8), 4);
}

TEST_F(PagesTest, AWriteBetweenTakesIsFoundByTheNextTakeOnly) {
	const std::vector<PageNumber> shown = allocate(2);
	const uint64_t                one   = mapped(shown);
	const uint64_t                two   = mapped(shown);
	write(one, 1);
	write(one + kPageBytes, 1);
	EXPECT_EQ(taken(), shown);
	EXPECT_TRUE(taken().empty());
	write(two + kPageBytes + 8, 5);
	EXPECT_EQ(taken(), std::vector<PageNumber> {shown[1]});
	EXPECT_EQ(query(one + kPageBytes).Protect, static_cast<DWORD>(PAGE_READONLY));
	EXPECT_EQ(query(two + kPageBytes).Protect, static_cast<DWORD>(PAGE_READONLY));
	EXPECT_TRUE(taken().empty());
	EXPECT_EQ(read(one + kPageBytes + 8), 5);
	write(one + 16, 6);
	EXPECT_EQ(taken(), std::vector<PageNumber> {shown[0]});
	EXPECT_TRUE(taken().empty());
}

TEST_F(PagesTest, APageNotWrittenStaysReadOnlyWhenAnotherIsWritten) {
	const std::vector<PageNumber> shown = allocate(2);
	const uint64_t                at    = mapped(shown);
	write(at, 1);
	write(at + kPageBytes, 1);
	EXPECT_EQ(taken(), shown);
	write(at + 8, 2);
	EXPECT_EQ(query(at).Protect, static_cast<DWORD>(PAGE_READWRITE));
	EXPECT_EQ(query(at + kPageBytes).Protect, static_cast<DWORD>(PAGE_READONLY));
	EXPECT_EQ(taken(), std::vector<PageNumber> {shown[0]});
}

TEST_F(PagesTest, AWriteWhileItsViewIsMappedAgainLandsAndIsFound) {
	const std::vector<PageNumber> shown = allocate(2);
	const uint64_t                gpu   = mapped(shown);
	const uint64_t                cpu   = mapped(shown);
	write(cpu, 1);
	write(cpu + kPageBytes, 1);
	EXPECT_EQ(taken(), shown);
	std::atomic<bool> started = false;
	std::thread       writer([&started, cpu] {
        started = true;
        write(cpu + kPageBytes + 8, 3);
    });
	while (!started) {
		std::this_thread::yield();
	}
	EXPECT_EQ(physical().map(cpu, shown), Status::Ok);
	writer.join();
	EXPECT_EQ(read(gpu + kPageBytes + 8), 3);
	const std::vector<PageNumber> written = taken();
	EXPECT_NE(std::find(written.begin(), written.end(), shown[1]), written.end());
	EXPECT_EQ(query(cpu).Protect, static_cast<DWORD>(PAGE_READONLY));
}

TEST_F(PagesTest, TwoSharedPagesLoadedStayUncommittedAndATouchBringsOne) {
	const std::vector<PageNumber> shown = allocate(2);
	const uint64_t                at    = mapped(shown);
	pages().setKind(at, 2 * kPageBytes, deunify::Kind::Shared);
	const std::vector<uint8_t> plain = pattern(2 * kPageBytes);
	loaded(plain, shown);
	EXPECT_EQ(home(shown[0]), Home::File);
	EXPECT_EQ(home(shown[1]), Home::File);
	EXPECT_EQ(query(at).State, static_cast<DWORD>(MEM_RESERVE));
	EXPECT_EQ(query(at + kPageBytes).State, static_cast<DWORD>(MEM_RESERVE));
	EXPECT_TRUE(same(at + kPageBytes, plain.data() + kPageBytes, kPageBytes));
	EXPECT_EQ(home(shown[1]), Home::Ram);
	EXPECT_EQ(query(at).State, static_cast<DWORD>(MEM_RESERVE));
	{
		const deunify::KernelWrite writing(pages(), at + 8, 16);
		ASSERT_EQ(writing.status(), Status::Ok);
	}
	EXPECT_EQ(home(shown[0]), Home::Ram);
	EXPECT_TRUE(same(at, plain.data(), kPageBytes));
}

TEST_F(PagesTest, ReadTitleGivesStagedBytesOnceThenReplaysWithoutMovingHome) {
	const std::vector<PageNumber> shown = allocate(2);
	const uint64_t                at    = mapped(shown);
	pages().setKind(at, 2 * kPageBytes, deunify::Kind::GpuOnly);
	const std::vector<uint8_t> plain = pattern(2 * kPageBytes);
	loaded(plain, shown);
	std::vector<uint8_t> part(0x200);
	ASSERT_EQ(pages().readTitle(at + kPageBytes + 0x100, part.data(), part.size(),
	                            deunify::ReadPurpose::Inspect),
	          Status::Ok);
	EXPECT_TRUE(std::equal(part.begin(), part.end(), plain.begin() + kPageBytes + 0x100));
	std::vector<uint8_t> whole(kPageBytes);
	ASSERT_EQ(pages().readTitle(at + kPageBytes, whole.data(), whole.size(),
	                            deunify::ReadPurpose::Inspect),
	          Status::Ok);
	EXPECT_TRUE(std::equal(whole.begin(), whole.end(), plain.begin() + kPageBytes));
	EXPECT_GE(pages().counts().replays, 1u);
	EXPECT_EQ(home(shown[1]), Home::File);
	EXPECT_EQ(query(at + kPageBytes).State, static_cast<DWORD>(MEM_RESERVE));
	ASSERT_EQ(pages().readTitle(at, whole.data(), whole.size(),
	                            deunify::ReadPurpose::TextureUpload),
	          Status::Ok);
	EXPECT_TRUE(std::equal(whole.begin(), whole.end(), plain.begin()));
	EXPECT_EQ(home(shown[0]), Home::Texture);
	EXPECT_EQ(pages().readTitle(at + 2 * kPageBytes, whole.data(), 1, deunify::ReadPurpose::Inspect),
	          Status::UnknownPage);
}

TEST_F(PagesTest, ARamPageItsRecordsRebuildIsParkedAndComesBackOnATouch) {
	const std::vector<PageNumber> shown = allocate(1);
	const uint64_t                at    = mapped(shown);
	const std::vector<uint8_t>    plain = pattern(kPageBytes);
	loaded(plain, shown);
	EXPECT_EQ(home(shown[0]), Home::Ram);
	EXPECT_EQ(pages().park(shown[0]), deunify::ParkResult::Parked);
	EXPECT_EQ(home(shown[0]), Home::File);
	EXPECT_EQ(query(at).Protect, static_cast<DWORD>(PAGE_NOACCESS));
	EXPECT_EQ(pages().counts().parked, 1u);
	EXPECT_EQ(read(at + 5), plain[5]);
	EXPECT_EQ(home(shown[0]), Home::Ram);
	write(at + 5, static_cast<uint8_t>(plain[5] ^ 1));
	EXPECT_EQ(pages().park(shown[0]), deunify::ParkResult::Differs);
	EXPECT_EQ(query(at).Protect, static_cast<DWORD>(PAGE_READWRITE));
	EXPECT_EQ(pages().park(PageNumber {deunify::kFramesPerPage * 4096}),
	          deunify::ParkResult::Unknown);
}

TEST_F(PagesTest, CpuWritesAreDeliveredAsChangesOfTheirGpuSlots) {
	struct Seen {
		std::vector<deunify::Range> ranges;
		deunify::ChangeReason       reason = deunify::ChangeReason::Loaded;
	} seen;
	deunify::Callbacks callbacks;
	callbacks.context      = &seen;
	callbacks.pagesChanged = [](void* context, const deunify::Range* ranges, size_t count,
	                            deunify::ChangeReason reason) {
		auto* into = static_cast<Seen*>(context);
		if (reason == deunify::ChangeReason::CpuWritten) {
			into->ranges.assign(ranges, ranges + count);
			into->reason = reason;
		}
	};
	g_pages = nullptr;
	m_context.reset();
	m_context = Context::create(deunify::Config {}, callbacks);
	ASSERT_NE(m_context, nullptr);
	g_pages                             = &m_context->pages();
	const std::vector<PageNumber> shown = allocate(2);
	void* at = VirtualAlloc2(GetCurrentProcess(), nullptr, 2 * kPageBytes,
	                         MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
	ASSERT_NE(at, nullptr);
	const uint64_t address = reinterpret_cast<uint64_t>(at);
	m_placeholders.push_back(deunify::Range {address, 2 * kPageBytes});
	ASSERT_EQ(m_context->gpuPages().followCpuViews(address, 2 * kPageBytes), Status::Ok);
	ASSERT_EQ(physical().map(address, shown), Status::Ok);
	write(address, 1);
	write(address + kPageBytes, 1);
	EXPECT_EQ(taken(), shown);
	m_context->deliverChanges();
	EXPECT_EQ(seen.reason, deunify::ChangeReason::CpuWritten);
	ASSERT_EQ(seen.ranges.size(), 1u);
	EXPECT_EQ(seen.ranges[0].start, address);
	EXPECT_EQ(seen.ranges[0].bytes, 2 * kPageBytes);
}

} // namespace
