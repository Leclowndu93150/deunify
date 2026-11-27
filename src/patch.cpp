#include "deunify/patch.hpp"

#include "hostError.hpp"

#include <windows.h>

#include <charconv>
#include <cstring>
#include <fstream>
#include <iterator>

namespace deunify::patch {

namespace {

constexpr uint64_t kStubGranularity    = 64ull * 1024;
constexpr uint64_t kNearSearchBytes    = 1ull << 30;
constexpr int64_t  kRel32Reach         = 0x7fff0000;
constexpr uint8_t  kCallRel32          = 0xe8;
constexpr size_t   kCallBytes          = 5;
constexpr size_t   kCallOpcodeBytes    = 1;
constexpr int      kHexadecimal        = 16;
constexpr char     kCommentMark        = '#';
constexpr uint8_t  kJumpIndirect[]     = {0xff, 0x25, 0x00, 0x00, 0x00, 0x00};
constexpr size_t   kShortBranchBytes   = 1;
constexpr uint32_t kFirstLine          = 1;
constexpr char     kWhitespace[]       = " \t\r\v\f";

void report(Status* status, uint32_t* errorLine, Status value, uint32_t line) {
	if (status != nullptr) {
		*status = value;
	}
	if (errorLine != nullptr) {
		*errorLine = line;
	}
}

Status hostFailure() {
	detail::setLastHostError(GetLastError());
	return Status::HostFailure;
}

std::string_view withoutComment(std::string_view line) {
	return line.substr(0, line.find(kCommentMark));
}

std::string_view nextField(std::string_view& rest) {
	const size_t start = rest.find_first_not_of(kWhitespace);
	if (start == std::string_view::npos) {
		rest = {};
		return {};
	}
	rest.remove_prefix(start);
	const size_t end   = rest.find_first_of(kWhitespace);
	const auto   field = rest.substr(0, end);
	rest.remove_prefix(field.size());
	return field;
}

std::optional<uint64_t> parseHex(std::string_view text) {
	if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
		text.remove_prefix(2);
	}
	uint64_t   value = 0;
	const auto end   = text.data() + text.size();
	const auto [at, error] = std::from_chars(text.data(), end, value, kHexadecimal);
	if (text.empty() || error != std::errc {} || at != end) {
		return std::nullopt;
	}
	return value;
}

uint64_t roundUpToStub(uint64_t bytes) {
	return (bytes + kStubGranularity - 1) & ~(kStubGranularity - 1);
}

uint8_t* reserveAt(uint64_t address, uint64_t bytes) {
	return static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(address), bytes,
	                                          MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
}

void flush(const void* at, uint64_t bytes) {
	FlushInstructionCache(GetCurrentProcess(), at, bytes);
}

} // namespace

std::optional<Sites> Sites::load(const wchar_t* path, Status* status, uint32_t* errorLine) {
	std::ifstream file(path, std::ios::binary);
	if (!file) {
		report(status, errorLine, Status::NotFound, 0);
		return std::nullopt;
	}
	const std::string text {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
	return parse(text, status, errorLine);
}

std::optional<Sites> Sites::parse(std::string_view text, Status* status, uint32_t* errorLine) {
	Sites    sites;
	uint32_t line = kFirstLine;
	while (!text.empty()) {
		const size_t newline = text.find('\n');
		auto         rest    = withoutComment(text.substr(0, newline));
		text.remove_prefix(newline == std::string_view::npos ? text.size() : newline + 1);
		const auto name   = nextField(rest);
		const auto offset = nextField(rest);
		if (!name.empty()) {
			const auto value = parseHex(offset);
			if (!value || !nextField(rest).empty()) {
				report(status, errorLine, Status::InvalidArgument, line);
				return std::nullopt;
			}
			sites.m_offsets[std::string(name)] = *value;
		}
		++line;
	}
	report(status, errorLine, Status::Ok, 0);
	return sites;
}

std::optional<uint64_t> Sites::offset(std::string_view name) const {
	const auto found = m_offsets.find(std::string(name));
	if (found == m_offsets.end()) {
		return std::nullopt;
	}
	return found->second;
}

bool Image::contains(uint64_t address, uint64_t length) const {
	const auto start = reinterpret_cast<uint64_t>(base);
	return base != nullptr && address >= start && length <= bytes &&
	       address - start <= bytes - length;
}

uint8_t* allocateNear(uint64_t nearAddress, uint64_t bytes) {
	const uint64_t size = roundUpToStub(bytes);
	if (size == 0) {
		return nullptr;
	}
	const uint64_t start = (nearAddress & ~(kStubGranularity - 1)) + kStubGranularity;
	for (uint64_t offset = 0; offset < kNearSearchBytes; offset += kStubGranularity) {
		if (uint8_t* above = reserveAt(start + offset, size)) {
			return above;
		}
		if (start >= kStubGranularity + offset) {
			if (uint8_t* below = reserveAt(start - kStubGranularity - offset, size)) {
				return below;
			}
		}
	}
	return nullptr;
}

void releaseNear(uint8_t* stub) {
	if (stub != nullptr) {
		VirtualFree(stub, 0, MEM_RELEASE);
	}
}

Status protectExecutable(uint8_t* at, uint64_t bytes) {
	if (at == nullptr || bytes == 0) {
		return Status::InvalidArgument;
	}
	DWORD old = 0;
	if (!VirtualProtect(at, bytes, PAGE_EXECUTE_READ, &old)) {
		return hostFailure();
	}
	flush(at, bytes);
	return Status::Ok;
}

Status writeCode(uint8_t* at, std::span<const uint8_t> code) {
	if (at == nullptr || code.empty()) {
		return Status::InvalidArgument;
	}
	DWORD old = 0;
	if (!VirtualProtect(at, code.size(), PAGE_EXECUTE_READWRITE, &old)) {
		return hostFailure();
	}
	std::memcpy(at, code.data(), code.size());
	DWORD unused = 0;
	VirtualProtect(at, code.size(), old, &unused);
	flush(at, code.size());
	return Status::Ok;
}

bool matches(const uint8_t* at, std::span<const uint8_t> expected, std::span<const uint8_t> mask) {
	if (mask.empty()) {
		return std::memcmp(at, expected.data(), expected.size()) == 0;
	}
	if (mask.size() != expected.size()) {
		return false;
	}
	for (size_t i = 0; i < expected.size(); ++i) {
		if ((at[i] & mask[i]) != (expected[i] & mask[i])) {
			return false;
		}
	}
	return true;
}

std::optional<int32_t> rel32(uint64_t instructionEnd, uint64_t target) {
	const auto distance = static_cast<int64_t>(target - instructionEnd);
	if (distance > kRel32Reach || distance < -kRel32Reach) {
		return std::nullopt;
	}
	return static_cast<int32_t>(distance);
}

StubBuilder& StubBuilder::bytes(std::span<const uint8_t> code) {
	m_code.insert(m_code.end(), code.begin(), code.end());
	return *this;
}

StubBuilder& StubBuilder::byte(uint8_t value) {
	m_code.push_back(value);
	return *this;
}

StubBuilder& StubBuilder::dword(uint32_t value) {
	uint8_t encoded[sizeof(value)];
	std::memcpy(encoded, &value, sizeof(value));
	return bytes(encoded);
}

StubBuilder& StubBuilder::qword(uint64_t value) {
	uint8_t encoded[sizeof(value)];
	std::memcpy(encoded, &value, sizeof(value));
	return bytes(encoded);
}

StubBuilder& StubBuilder::jumpAbsolute(uint64_t target) {
	return bytes(kJumpIndirect).qword(target);
}

size_t StubBuilder::shortBranch(uint8_t opcode) {
	byte(opcode);
	const size_t position = m_code.size();
	byte(0);
	return position;
}

StubBuilder& StubBuilder::label(size_t position) {
	m_code[position] = static_cast<uint8_t>(m_code.size() - (position + kShortBranchBytes));
	return *this;
}

Status redirectCall(const Image& image, uint64_t callOffset, uint64_t expectedTargetOffset,
                    std::span<const uint8_t> stub, Redirect* result) {
	const auto base = reinterpret_cast<uint64_t>(image.base);
	const auto site = base + callOffset;
	if (stub.empty() || !image.contains(site, kCallBytes)) {
		return Status::InvalidArgument;
	}
	auto*   call         = reinterpret_cast<uint8_t*>(site);
	int32_t displacement = 0;
	std::memcpy(&displacement, call + kCallOpcodeBytes, sizeof(displacement));
	const uint64_t target = site + kCallBytes + static_cast<int64_t>(displacement);
	if (call[0] != kCallRel32 || target != base + expectedTargetOffset) {
		return Status::Refused;
	}
	uint8_t* code = allocateNear(site, stub.size());
	if (code == nullptr) {
		return Status::OutOfMemory;
	}
	const auto reach = rel32(site + kCallBytes, reinterpret_cast<uint64_t>(code));
	if (!reach) {
		releaseNear(code);
		return Status::OutOfMemory;
	}
	std::memcpy(code, stub.data(), stub.size());
	uint8_t newDisplacement[sizeof(int32_t)];
	std::memcpy(newDisplacement, &*reach, sizeof(newDisplacement));
	Status status = protectExecutable(code, stub.size());
	if (status == Status::Ok) {
		status = writeCode(call + kCallOpcodeBytes, newDisplacement);
	}
	if (status != Status::Ok) {
		releaseNear(code);
		return status;
	}
	if (result != nullptr) {
		*result = Redirect {site, target, code};
	}
	return Status::Ok;
}

} // namespace deunify::patch
