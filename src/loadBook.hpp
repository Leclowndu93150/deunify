#pragma once

#include "codecs.hpp"
#include "decodedStore.hpp"
#include "staging.hpp"
#include "workerPool.hpp"

#include "deunify/loads.hpp"
#include "deunify/types.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>

namespace deunify::detail {

/// A file handle the library owns; closed when the last owner lets it go.
class SourceFile {
public:
	explicit SourceFile(void* handle);
	~SourceFile();
	SourceFile(const SourceFile&)            = delete;
	SourceFile& operator=(const SourceFile&) = delete;

	/// Reads exactly `bytes` at `offset` (positional, on synchronous and overlapped handles).
	/// False if the host failed or the file ended first.
	bool read(uint64_t offset, uint8_t* out, uint32_t bytes) const;

private:
	void* m_handle;
};

using SharedFile = std::shared_ptr<const SourceFile>;

/// What identifies a request's decoded result: the file, where and how much is read, and how
/// it is decoded into how many bytes.
struct RequestKey {
	uint64_t file              = 0;
	uint64_t offset            = 0;
	uint32_t sourceBytes       = 0;
	uint32_t intermediateBytes = 0;
	uint32_t destinationBytes  = 0;
	Codec    firstCodec        = Codec::None;
	Codec    secondCodec       = Codec::None;

	bool operator==(const RequestKey&) const = default;
};

struct RequestKeyHash {
	size_t operator()(const RequestKey& key) const;
};

RequestKey keyOf(const LoadRequest& request);

/// One page's share of a request's destination.
struct PagePiece {
	PageNumber page   = kNoPage;
	uint32_t   index  = 0; ///< The page's place in the request's page list.
	uint32_t   within = 0; ///< Where the share begins in the page.
	uint32_t   length = 0;

	bool whole() const { return within == 0 && length == kPageBytes; }
};

/// The shares of `bytes` from `offsetInFirstPage` of pages[0] on. Empty if `bytes` is zero,
/// the offset is not inside a page or there are too few pages.
std::vector<PagePiece> piecesOf(std::span<const PageNumber> pages, uint32_t offsetInFirstPage,
                                uint64_t bytes);

struct LoadLimits {
	uint64_t stagingCapBytes   = 0;
	uint64_t decodedCapBytes   = 0;
	uint32_t maxRecordsPerPage = 0;
	uint32_t decodeWorkers     = 0; ///< 0: half the logical processors, clamped to 2..8.
};

/// Everything of Loads that does not involve another component: open files, per-page records
/// of the requests that wrote each page, staged bytes, decoded requests and replay.
///
/// Locks, in order: records (with the open files), staging, decoded. Decoding runs unlocked.
class LoadBook {
public:
	LoadBook(const LoadLimits& limits, const CustomDecoder& decoder);
	~LoadBook();
	LoadBook(const LoadBook&)            = delete;
	LoadBook& operator=(const LoadBook&) = delete;

	Status openHandle(void* handle, FileId* file);
	Status openPath(const wchar_t* path, FileId* file);
	void   closeFile(FileId file);

	/// The open file `id`, null if it is not open.
	SharedFile file(FileId id) const;

	/// Reads and decodes the request `key` from `file` into `out` (not kept).
	Status produce(const SharedFile& file, const RequestKey& key, std::vector<uint8_t>& out);

	/// Records the request for each piece's page: a whole piece starts the page's records anew;
	/// a page past the record limit becomes not rebuildable.
	void record(const SharedFile& file, const RequestKey& key, uint32_t offsetInFirstPage,
	            std::span<const PagePiece> pieces);

	/// The pieces' pages were written with bytes from no file: not rebuildable.
	void markUnknown(std::span<const PagePiece> pieces);

	bool rebuildable(PageNumber page) const;

	/// Rebuilds `page` (kPageBytes into `out`) from its records over zeros.
	bool replay(PageNumber page, uint8_t* out);

	/// Decodes the not yet decoded requests of `pages` on the worker pool, each once.
	void prepare(std::span<const PageNumber> pages);

	/// Drops `page`'s records and staged bytes.
	void forget(PageNumber page);

	void stage(PageNumber page, uint32_t within, std::span<const uint8_t> bytes);
	void takenElsewhere(PageNumber page, uint64_t offset, uint64_t bytes);
	bool takeStaged(PageNumber page, uint64_t offset, uint8_t* out, uint64_t bytes);
	bool takeStagedPage(PageNumber page, uint8_t* out);
	void dropStaged(PageNumber page);

	void          countLoaded();
	Loads::Totals totals() const;

private:
	struct Record {
		SharedFile file;
		RequestKey key;
		uint32_t   index  = 0;
		uint32_t   begins = 0;
		uint32_t   length = 0;
	};

	struct PageRecords {
		std::vector<Record> records;
		bool                unknown = false;
	};

	Status  adopt(void* handle, FileId* file);
	void    recordPiece(PageRecords& page, Record record, bool whole);
	Decoded decoded(const Record& record);
	bool    copyRecord(const Record& record, uint8_t* out);
	std::vector<Record> undecoded(std::span<const PageNumber> pages) const;
	WorkerPool&         pool();

	const LoadLimits    m_limits;
	const CustomDecoder m_decoder;

	mutable std::mutex                                m_recordsMutex;
	std::unordered_map<uint64_t, SharedFile>          m_files;
	std::unordered_map<uint64_t, PageRecords>         m_pages;
	std::atomic<uint64_t>                             m_nextFile {1};

	mutable std::mutex m_stagingMutex;
	Staging            m_staging;

	DecodedStore<RequestKey, RequestKeyHash> m_decoded;

	std::once_flag              m_poolOnce;
	std::unique_ptr<WorkerPool> m_pool;

	std::atomic<uint64_t> m_requestsLoaded {0};
	std::atomic<uint64_t> m_requestsDecoded {0};
	std::atomic<uint64_t> m_pagesReplayed {0};
	std::atomic<uint64_t> m_stagedReads {0};
};

} // namespace deunify::detail
