#pragma once

#include "deunify/types.hpp"

#include <memory>
#include <span>

namespace deunify {

class Context;

// A file we can read again later. We hold our own handle for as long as a record names it.
enum class FileId : uint64_t {};

inline constexpr FileId kNoFile = FileId {0};

// File bytes, decoded by up to two codecs, written into a list of physical pages.
struct LoadRequest {
	FileId   file        = kNoFile;
	uint64_t fileOffset  = 0;
	uint32_t sourceBytes = 0;
	Codec    firstCodec  = Codec::None;
	// If set, the first codec must produce exactly intermediateBytes.
	Codec    secondCodec       = Codec::None;
	uint32_t intermediateBytes = 0;
	uint32_t destinationBytes  = 0;
	// Written from offsetInFirstPage of pages[0], continuing at the start of each next page.
	// The pages need not be consecutive, or mapped anywhere at all.
	std::span<const PageNumber> pages;
	uint32_t                    offsetInFirstPage = 0;
};

// DirectStorage-style requests and what they wrote.
//
// A request is read and decoded once, on the caller's thread, then each destination page goes
// where Pages::placeLoad says: into RAM, into staging (File home), or to its tile. Every
// request that writes a page is recorded against it, so the page can be rebuilt later by
// replaying its records in order over zeros. A whole-page write starts the list afresh.
//
// Staged bytes are handed out once; after the first reader takes them, the page is replayed
// from the file. Concurrent replays of one request share a single decode.
//
// load() is meant to be called from many DirectStorage workers at once. No lock is held while
// decoding.
class Loads {
public:
	Loads(const Loads&)            = delete;
	Loads& operator=(const Loads&) = delete;
	~Loads();

	// The handle is duplicated, so you may close yours. Records name files by our duplicate,
	// never by your file object's address, which may be reused for another file.
	Status openFile(void* handle, FileId* file);
	Status openFile(const wchar_t* path, FileId* file);

	// The file stays open while records still name it.
	void closeFile(FileId file);

	// Returns once every page is placed. A read or decode failure changes no page, so a bad
	// stream fails here, at the request that caused it, and not later as lost bytes. A custom
	// codec without a DecodeFn is NotSupported.
	Status load(const LoadRequest& request);

	// Bytes from no file (a memory-source request, an emulator copy). These pages can't be
	// rebuilt until a later whole-page load, so they go to RAM, or to the tile of a Card page.
	Status write(std::span<const PageNumber> pages, uint32_t offsetInFirstPage, const void* bytes,
	             uint64_t byteCount);

	bool rebuildable(PageNumber page) const;

	// If requestsDecoded runs at a multiple of requestsLoaded, something decodes per page.
	struct Totals {
		uint64_t stagedBytes     = 0;
		uint64_t decodedBytes    = 0;
		uint64_t requestsLoaded  = 0;
		uint64_t requestsDecoded = 0;
		uint64_t pagesReplayed   = 0;
		uint64_t stagedReads     = 0;
	};
	Totals totals() const;

private:
	friend class Context;
	friend class Pages;
	friend class CardView;
	friend class GpuPageTable;

	explicit Loads(Context& context);

	// All or nothing: false if any byte of the range isn't staged. What is taken is gone.
	bool takeStaged(PageNumber page, uint64_t offset, uint8_t* out, uint64_t bytes);
	// Drops whatever was staged for the page if it isn't all there.
	bool takeStagedPage(PageNumber page, uint8_t* out);

	bool replay(PageNumber page, uint8_t* out);

	// Decodes the distinct requests behind `pages` on the worker pool and waits. Call it
	// before a batch of replays.
	void prepare(std::span<const PageNumber> pages);

	void dropStaged(PageNumber page);
	void forget(PageNumber page);

	struct Impl;
	Context&              m_context;
	std::unique_ptr<Impl> m_impl;
};

} // namespace deunify
