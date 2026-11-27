// Small tools for patching title code at load. The library knows no title and ships no
// sites; those stay with your emulator.

#pragma once

#include "deunify/types.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace deunify::patch {

// Named offsets into a title image, `name hex-offset` per line, `#` for comments. Code looks
// sites up by name, so no offset ever lives in source.
class Sites {
public:
	// NotFound if the file won't open; InvalidArgument with `errorLine` set for a bad line.
	static std::optional<Sites> load(const wchar_t* path, Status* status = nullptr,
	                                 uint32_t* errorLine = nullptr);

	static std::optional<Sites> parse(std::string_view text, Status* status = nullptr,
	                                  uint32_t* errorLine = nullptr);

	std::optional<uint64_t> offset(std::string_view name) const;

	size_t size() const { return m_offsets.size(); }

private:
	std::unordered_map<std::string, uint64_t> m_offsets;
};

struct Image {
	uint8_t* base  = nullptr;
	uint64_t bytes = 0;

	bool contains(uint64_t address, uint64_t length) const;
};

// Read-write memory within rel32 reach of `nearAddress`, in 64 KiB granules, searched up to
// 1 GiB either way. Seal it with protectExecutable. Null if nothing is free.
uint8_t* allocateNear(uint64_t nearAddress, uint64_t bytes);
void     releaseNear(uint8_t* stub);

// Read-execute, and flushes the instruction cache.
Status protectExecutable(uint8_t* at, uint64_t bytes);

// Only while no title thread can be running these bytes, which in practice means at load,
// before the entry point. Restores the old protection afterwards.
Status writeCode(uint8_t* at, std::span<const uint8_t> code);

// An empty mask compares every bit.
bool matches(const uint8_t* at, std::span<const uint8_t> expected,
             std::span<const uint8_t> mask = {});

std::optional<int32_t> rel32(uint64_t instructionEnd, uint64_t target);

class StubBuilder {
public:
	StubBuilder& bytes(std::span<const uint8_t> code);
	StubBuilder& byte(uint8_t value);
	StubBuilder& dword(uint32_t value);
	StubBuilder& qword(uint64_t value);
	// jmp [rip+0] and the 64-bit target: 14 bytes, any distance.
	StubBuilder& jumpAbsolute(uint64_t target);
	// Emits a short forward branch and returns where its displacement is; label() fills it in
	// with the current end.
	size_t       shortBranch(uint8_t opcode);
	StubBuilder& label(size_t position);

	std::span<const uint8_t> code() const { return {m_code.data(), m_code.size()}; }

private:
	std::vector<uint8_t> m_code;
};

struct Redirect {
	uint64_t site           = 0;
	uint64_t originalTarget = 0;
	uint8_t* stub           = nullptr;
};

// The site must hold a `call rel32` whose target is `image.base + expectedTargetOffset`.
// Anything else is Refused and nothing changes: the build differs from your sites file, so log
// it loudly and run without the patch. Otherwise the stub is copied near the image, sealed,
// and the call pointed at it.
Status redirectCall(const Image& image, uint64_t callOffset, uint64_t expectedTargetOffset,
                    std::span<const uint8_t> stub, Redirect* result);

} // namespace deunify::patch
