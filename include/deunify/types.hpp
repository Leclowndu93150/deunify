// Shared vocabulary. Plain values, no behaviour.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace deunify {

// Everything (homes, views, tiles, transfers) is done in whole pages.
inline constexpr uint64_t kPageBytes = 64ull * 1024;

inline constexpr uint64_t kFrameBytes    = 4ull * 1024;
inline constexpr uint64_t kFramesPerPage = kPageBytes / kFrameBytes;

// A title may store page numbers shifted right by 4 in 21 bits.
inline constexpr uint64_t kPageNumberLimit = 1ull << 25;

// The title's name for a physical page: the frame number of its first 4 KiB frame, so a
// non-zero multiple of kFramesPerPage below kPageNumberLimit. Zero is never a page; in page
// lists it means "nothing here".
//
// A number is reused only after its page was freed and its last CPU or GPU view is gone, so
// for as long as anything can reach a page, its number names that page and no other. All
// per-page state is keyed by it.
enum class PageNumber : uint64_t {};

inline constexpr PageNumber kNoPage = PageNumber {0};

constexpr uint64_t value(PageNumber page) {
	return static_cast<uint64_t>(page);
}

constexpr bool isValidPageNumber(uint64_t number) {
	return number != 0 && number % kFramesPerPage == 0 && number < kPageNumberLimit;
}

// Shared with the C ABI; never renumbered.
enum class Status : int32_t {
	Ok              = 0,
	InvalidArgument = 1,
	UnknownPage     = 2,  // not allocated, or no longer
	NotPlaceholder  = 3,  // map target is neither a placeholder nor one of our views
	OutOfMemory     = 4,
	HostFailure     = 5,  // see Context::lastHostError()
	NoSource        = 6,  // the page's load records cannot rebuild it
	Differs         = 7,  // the page's RAM differs from what its records rebuild
	InUse           = 8,
	TooLarge        = 9,
	NotSupported    = 10, // e.g. no tiled resources tier 2
	DecodeFailed    = 11,
	Unavailable     = 12, // component not enabled, e.g. no card view
	NotFound        = 13,
	Refused         = 14, // patching: the code is not what the caller expected
	VersionMismatch = 15, // C ABI struct with an unknown `version`
};

// CPU or GPU virtual addresses, depending on who asks.
struct Range {
	uint64_t start = 0;
	uint64_t bytes = 0;

	constexpr uint64_t end() const { return start + bytes; }
};

// Where a page's bytes live. Exactly one at a time; docs/ARCHITECTURE.md has the transitions.
enum class Home : uint8_t {
	Zero    = 0, // never written, nothing committed
	File    = 1, // rebuilt from its load records on demand, nothing committed
	Texture = 2, // like File, but a host texture already took the staged bytes
	Card    = 3, // a tile of the card view; RAM views closed
	// Committed and touched by the CPU. A tile, if it has one, is a copy we keep current; the
	// census counts those pages as held twice.
	Ram = 4,
};

// Who can reach an address range, as the title's own memory map says (Pages::setKind).
enum class Kind : uint8_t {
	Unknown = 0, // treated as CPU-reachable: loads go to RAM
	Shared  = 1,
	CpuOnly = 2,
	GpuOnly = 3,
};

enum class ReadPurpose : uint8_t {
	Inspect = 0,
	// The bytes become a host texture. A File page read whole this way moves to Texture.
	TextureUpload = 1,
};

// Zlib (RFC 1950) is built in. Anything from kFirstCustomCodec up goes to Callbacks::decode;
// no console codec ships with the library.
enum class Codec : uint32_t {
	None = 0,
	Zlib = 1,
};

inline constexpr uint32_t kFirstCustomCodec = 0x100;

constexpr Codec customCodec(uint32_t index) {
	return static_cast<Codec>(kFirstCustomCodec + index);
}

enum class ChangeReason : uint32_t {
	CpuWritten    = 0,
	ShaderWritten = 1, // declared through CardView::shaderWrites
	Loaded        = 2,
	Remapped      = 3, // the GPU page table shows other pages here now
};

} // namespace deunify
