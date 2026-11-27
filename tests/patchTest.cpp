#include "deunify/patch.hpp"

#include <windows.h>

#include <gtest/gtest.h>

#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

using deunify::Status;
using namespace deunify::patch;

constexpr uint64_t kImageBytes    = 64ull * 1024;
constexpr uint64_t kCallerOffset  = 0x0;
constexpr uint64_t kCallOffset    = 0x4;
constexpr uint64_t kTargetOffset  = 0x100;
constexpr uint64_t kOtherOffset   = 0x200;
constexpr int      kOriginalValue = 7;
constexpr int      kStubValue     = 42;
constexpr int64_t  kReach         = 0x7fff0000;
constexpr uint64_t kNearReach     = (1ull << 30) + 2 * kImageBytes;

const uint8_t kCaller[] = {
    0x48, 0x83, 0xec, 0x28,
    0xe8, 0xf7, 0x00, 0x00, 0x00,
    0x48, 0x83, 0xc4, 0x28,
    0xc3,
};

const uint8_t kReturnOriginal[] = {0xb8, kOriginalValue, 0x00, 0x00, 0x00, 0xc3};

class CodeImage {
public:
	CodeImage() {
		m_base = static_cast<uint8_t*>(
		    VirtualAlloc(nullptr, kImageBytes, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
		std::memcpy(m_base + kCallerOffset, kCaller, sizeof(kCaller));
		std::memcpy(m_base + kTargetOffset, kReturnOriginal, sizeof(kReturnOriginal));
		FlushInstructionCache(GetCurrentProcess(), m_base, kImageBytes);
	}

	~CodeImage() { VirtualFree(m_base, 0, MEM_RELEASE); }

	CodeImage(const CodeImage&)            = delete;
	CodeImage& operator=(const CodeImage&) = delete;

	Image image() const { return Image {m_base, kImageBytes}; }

	int call(int argument) const {
		using Caller = int (*)(int);
		return reinterpret_cast<Caller>(m_base + kCallerOffset)(argument);
	}

	uint8_t* at(uint64_t offset) const { return m_base + offset; }

private:
	uint8_t* m_base = nullptr;
};

std::vector<uint8_t> stubThatAnswersForNonZero(uint64_t originalTarget) {
	const uint8_t testEcx[]  = {0x85, 0xc9};
	const uint8_t movEax[]   = {0xb8};
	const uint8_t ret        = 0xc3;
	constexpr uint8_t kJz    = 0x74;
	StubBuilder   builder;
	builder.bytes(testEcx);
	const size_t toOriginal = builder.shortBranch(kJz);
	builder.bytes(movEax).dword(kStubValue).byte(ret);
	builder.label(toOriginal).jumpAbsolute(originalTarget);
	const auto code = builder.code();
	return {code.begin(), code.end()};
}

TEST(Patch, SitesAreNamedHexOffsetsWithCommentsAndBlankLines) {
	Status     status = Status::HostFailure;
	uint32_t   line   = 99;
	const auto sites  = Sites::parse("# sites\n\nalpha 1f\nbeta 0x200 # trailing\r\n  gamma\t10\n",
	                                 &status, &line);
	ASSERT_TRUE(sites.has_value());
	EXPECT_EQ(status, Status::Ok);
	EXPECT_EQ(sites->size(), 3u);
	EXPECT_EQ(sites->offset("alpha"), 0x1fu);
	EXPECT_EQ(sites->offset("beta"), 0x200u);
	EXPECT_EQ(sites->offset("gamma"), 0x10u);
	EXPECT_FALSE(sites->offset("delta").has_value());
}

TEST(Patch, AMalformedSiteLineIsReportedWithItsNumber) {
	Status   status = Status::Ok;
	uint32_t line   = 0;
	EXPECT_FALSE(Sites::parse("alpha 1\nbeta zz\n", &status, &line).has_value());
	EXPECT_EQ(status, Status::InvalidArgument);
	EXPECT_EQ(line, 2u);
	EXPECT_FALSE(Sites::parse("alpha\n", &status, &line).has_value());
	EXPECT_EQ(line, 1u);
	EXPECT_FALSE(Sites::parse("a 1\nb 2\nc 3 4\n", &status, &line).has_value());
	EXPECT_EQ(line, 3u);
}

TEST(Patch, SitesLoadFromAFile) {
	wchar_t directory[MAX_PATH] = {};
	ASSERT_NE(GetTempPathW(MAX_PATH, directory), 0u);
	const std::wstring path = std::wstring(directory) + L"deunify-sites-test.txt";
	{
		std::ofstream file(path.c_str(), std::ios::binary);
		file << "entry 0x1234\n";
	}
	Status     status = Status::HostFailure;
	const auto sites  = Sites::load(path.c_str(), &status);
	DeleteFileW(path.c_str());
	ASSERT_TRUE(sites.has_value());
	EXPECT_EQ(sites->offset("entry"), 0x1234u);

	const std::wstring missing = std::wstring(directory) + L"deunify-no-such-sites.txt";
	EXPECT_FALSE(Sites::load(missing.c_str(), &status).has_value());
	EXPECT_EQ(status, Status::NotFound);
}

TEST(Patch, Rel32ReachesTwoGibibytesLessSixtyFourKibibytesEachWay) {
	constexpr uint64_t from = 0x100000000ull;
	EXPECT_EQ(rel32(from, from + kReach), static_cast<int32_t>(kReach));
	EXPECT_FALSE(rel32(from, from + kReach + 1).has_value());
	EXPECT_EQ(rel32(from, from - kReach), static_cast<int32_t>(-kReach));
	EXPECT_FALSE(rel32(from, from - kReach - 1).has_value());
}

TEST(Patch, StubBuilderEncodesAbsoluteJumpsAndShortBranches) {
	StubBuilder  builder;
	const size_t branch = builder.shortBranch(0x74);
	builder.byte(0x90).byte(0x90).label(branch).jumpAbsolute(0x1122334455667788ull);
	const std::vector<uint8_t> expected = {0x74, 0x02, 0x90, 0x90, 0xff, 0x25, 0x00, 0x00, 0x00,
	                                       0x00, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11};
	const auto code = builder.code();
	EXPECT_EQ(std::vector<uint8_t>(code.begin(), code.end()), expected);
}

TEST(Patch, MatchesComparesTheMaskedBits) {
	const uint8_t code[]     = {0x48, 0x8b, 0x05, 0x12};
	const uint8_t expected[] = {0x48, 0x8b, 0x00, 0x00};
	const uint8_t mask[]     = {0xff, 0xff, 0x00, 0x00};
	EXPECT_TRUE(matches(code, expected, mask));
	EXPECT_FALSE(matches(code, expected));
	EXPECT_TRUE(matches(code, code));
}

TEST(Patch, MemoryNearAnAddressIsWithinRel32Reach) {
	CodeImage      code;
	const auto     nearAddress = reinterpret_cast<uint64_t>(code.at(0));
	uint8_t* const stub        = allocateNear(nearAddress, 100);
	ASSERT_NE(stub, nullptr);
	const auto address = reinterpret_cast<uint64_t>(stub);
	EXPECT_EQ(address % kImageBytes, 0u);
	EXPECT_LE(address > nearAddress ? address - nearAddress : nearAddress - address, kNearReach);
	EXPECT_TRUE(rel32(nearAddress, address).has_value());
	releaseNear(stub);
}

TEST(Patch, WriteCodeKeepsTheOldProtection) {
	auto* code = static_cast<uint8_t*>(
	    VirtualAlloc(nullptr, kImageBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
	ASSERT_NE(code, nullptr);
	ASSERT_EQ(protectExecutable(code, kImageBytes), Status::Ok);
	const uint8_t patch[] = {0xcc, 0xc3};
	ASSERT_EQ(writeCode(code + 8, patch), Status::Ok);
	EXPECT_TRUE(matches(code + 8, patch));
	MEMORY_BASIC_INFORMATION info {};
	VirtualQuery(code, &info, sizeof(info));
	EXPECT_EQ(info.Protect, static_cast<DWORD>(PAGE_EXECUTE_READ));
	VirtualFree(code, 0, MEM_RELEASE);
}

TEST(Patch, ARedirectedCallRunsTheStubWhichMayJumpOnToTheOriginal) {
	CodeImage code;
	EXPECT_EQ(code.call(0), kOriginalValue);
	EXPECT_EQ(code.call(1), kOriginalValue);

	const auto stub     = stubThatAnswersForNonZero(reinterpret_cast<uint64_t>(code.at(kTargetOffset)));
	Redirect   redirect;
	ASSERT_EQ(redirectCall(code.image(), kCallOffset, kTargetOffset, stub, &redirect), Status::Ok);
	EXPECT_EQ(redirect.site, reinterpret_cast<uint64_t>(code.at(kCallOffset)));
	EXPECT_EQ(redirect.originalTarget, reinterpret_cast<uint64_t>(code.at(kTargetOffset)));
	ASSERT_NE(redirect.stub, nullptr);
	EXPECT_EQ(code.call(0), kOriginalValue);
	EXPECT_EQ(code.call(1), kStubValue);
	releaseNear(redirect.stub);
}

TEST(Patch, ARedirectIsRefusedWhenTheCodeIsNotTheExpectedCall) {
	CodeImage  code;
	const auto stub   = stubThatAnswersForNonZero(reinterpret_cast<uint64_t>(code.at(kTargetOffset)));
	Redirect   redirect;
	EXPECT_EQ(redirectCall(code.image(), kCallerOffset, kTargetOffset, stub, &redirect), Status::Refused);
	EXPECT_EQ(redirectCall(code.image(), kCallOffset, kOtherOffset, stub, &redirect), Status::Refused);
	EXPECT_EQ(redirectCall(code.image(), kImageBytes - 2, kTargetOffset, stub, &redirect),
	          Status::InvalidArgument);
	EXPECT_TRUE(matches(code.at(kCallerOffset), kCaller));
	EXPECT_EQ(code.call(1), kOriginalValue);
}

} // namespace
