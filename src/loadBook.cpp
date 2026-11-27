#include "loadBook.hpp"

#include "hostError.hpp"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <unordered_set>

namespace deunify::detail {

namespace {

constexpr uintptr_t kNoCompletionPacket = 1;
constexpr uint32_t  kSmallestPool       = 2;
constexpr uint32_t  kLargestPool        = 8;
constexpr size_t    kHashMultiplier     = 0x9E3779B97F4A7C15ull;

size_t mix(size_t seed, uint64_t value) {
	return (seed ^ value) * kHashMultiplier + (seed >> 29);
}

DWORD readOnce(HANDLE file, HANDLE event, uint64_t at, uint8_t* out, DWORD bytes) {
	OVERLAPPED overlapped {};
	overlapped.Offset     = static_cast<DWORD>(at);
	overlapped.OffsetHigh = static_cast<DWORD>(at >> 32);
	overlapped.hEvent     = reinterpret_cast<HANDLE>(reinterpret_cast<uintptr_t>(event) | kNoCompletionPacket);
	DWORD got = 0;
	if (ReadFile(file, out, bytes, &got, &overlapped)) {
		return got;
	}
	if (GetLastError() != ERROR_IO_PENDING || !GetOverlappedResult(file, &overlapped, &got, TRUE)) {
		setLastHostError(GetLastError());
		return 0;
	}
	return got;
}

uint32_t workersFor(uint32_t asked) {
	if (asked != 0) {
		return asked;
	}
	return std::clamp(std::thread::hardware_concurrency() / 2, kSmallestPool, kLargestPool);
}

Status openFailure() {
	DWORD error = GetLastError();
	setLastHostError(error);
	return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? Status::NotFound
	                                                                      : Status::HostFailure;
}

} // namespace

SourceFile::SourceFile(void* handle) : m_handle(handle) {}

SourceFile::~SourceFile() {
	CloseHandle(m_handle);
}

bool SourceFile::read(uint64_t offset, uint8_t* out, uint32_t bytes) const {
	HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (event == nullptr) {
		setLastHostError(GetLastError());
		return false;
	}
	uint32_t done = 0;
	while (done < bytes) {
		DWORD got = readOnce(m_handle, event, offset + done, out + done, bytes - done);
		if (got == 0) {
			break;
		}
		done += got;
	}
	CloseHandle(event);
	return done == bytes;
}

size_t RequestKeyHash::operator()(const RequestKey& key) const {
	size_t seed = mix(0, key.file);
	seed        = mix(seed, key.offset);
	seed        = mix(seed, key.sourceBytes);
	seed        = mix(seed, key.intermediateBytes);
	seed        = mix(seed, key.destinationBytes);
	seed        = mix(seed, static_cast<uint32_t>(key.firstCodec));
	return mix(seed, static_cast<uint32_t>(key.secondCodec));
}

RequestKey keyOf(const LoadRequest& request) {
	return RequestKey {
	    .file              = static_cast<uint64_t>(request.file),
	    .offset            = request.fileOffset,
	    .sourceBytes       = request.sourceBytes,
	    .intermediateBytes = request.secondCodec == Codec::None ? 0 : request.intermediateBytes,
	    .destinationBytes  = request.destinationBytes,
	    .firstCodec        = request.firstCodec,
	    .secondCodec       = request.secondCodec,
	};
}

std::vector<PagePiece> piecesOf(std::span<const PageNumber> pages, uint32_t offsetInFirstPage,
                                uint64_t bytes) {
	uint64_t end   = offsetInFirstPage + bytes;
	uint64_t count = (end + kPageBytes - 1) / kPageBytes;
	if (bytes == 0 || offsetInFirstPage >= kPageBytes || count > pages.size()) {
		return {};
	}
	std::vector<PagePiece> pieces;
	pieces.reserve(count);
	uint64_t done = 0;
	for (uint32_t index = 0; index < count; ++index) {
		uint32_t within = index == 0 ? offsetInFirstPage : 0;
		uint64_t length = std::min(kPageBytes - within, bytes - done);
		pieces.push_back(PagePiece {pages[index], index, within, static_cast<uint32_t>(length)});
		done += length;
	}
	return pieces;
}

LoadBook::LoadBook(const LoadLimits& limits, const CustomDecoder& decoder)
    : m_limits(limits), m_decoder(decoder) {}

LoadBook::~LoadBook() = default;

Status LoadBook::openHandle(void* handle, FileId* file) {
	if (handle == nullptr || handle == INVALID_HANDLE_VALUE || file == nullptr) {
		return Status::InvalidArgument;
	}
	HANDLE copy    = nullptr;
	HANDLE process = GetCurrentProcess();
	if (!DuplicateHandle(process, handle, process, &copy, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
		setLastHostError(GetLastError());
		return Status::HostFailure;
	}
	return adopt(copy, file);
}

Status LoadBook::openPath(const wchar_t* path, FileId* file) {
	if (path == nullptr || file == nullptr) {
		return Status::InvalidArgument;
	}
	HANDLE handle = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
	                            FILE_ATTRIBUTE_NORMAL, nullptr);
	if (handle == INVALID_HANDLE_VALUE) {
		return openFailure();
	}
	return adopt(handle, file);
}

Status LoadBook::adopt(void* handle, FileId* file) {
	uint64_t        id = m_nextFile.fetch_add(1);
	std::lock_guard lock(m_recordsMutex);
	m_files.emplace(id, std::make_shared<const SourceFile>(handle));
	*file = FileId {id};
	return Status::Ok;
}

void LoadBook::closeFile(FileId file) {
	SharedFile closing;
	std::lock_guard lock(m_recordsMutex);
	auto at = m_files.find(static_cast<uint64_t>(file));
	if (at != m_files.end()) {
		closing = std::move(at->second);
		m_files.erase(at);
	}
}

SharedFile LoadBook::file(FileId id) const {
	std::lock_guard lock(m_recordsMutex);
	auto            at = m_files.find(static_cast<uint64_t>(id));
	return at == m_files.end() ? nullptr : at->second;
}

Status LoadBook::produce(const SharedFile& file, const RequestKey& key, std::vector<uint8_t>& out) {
	m_requestsDecoded.fetch_add(1, std::memory_order_relaxed);
	out.resize(key.sourceBytes);
	if (!file->read(key.offset, out.data(), key.sourceBytes)) {
		return Status::HostFailure;
	}
	CodecSteps steps {key.firstCodec, key.secondCodec, key.intermediateBytes, key.destinationBytes};
	return decode(steps, m_decoder, out);
}

void LoadBook::record(const SharedFile& file, const RequestKey& key, uint32_t offsetInFirstPage,
                      std::span<const PagePiece> pieces) {
	std::lock_guard lock(m_recordsMutex);
	for (const PagePiece& piece : pieces) {
		Record record {file, key, piece.index, offsetInFirstPage, piece.length};
		recordPiece(m_pages[value(piece.page)], std::move(record), piece.whole());
	}
}

void LoadBook::recordPiece(PageRecords& page, Record record, bool whole) {
	if (whole) {
		page.records.clear();
		page.unknown = false;
	}
	if (page.records.size() >= m_limits.maxRecordsPerPage) {
		page.unknown = true;
	}
	if (page.unknown) {
		page.records.clear();
		return;
	}
	page.records.push_back(std::move(record));
}

void LoadBook::markUnknown(std::span<const PagePiece> pieces) {
	std::lock_guard lock(m_recordsMutex);
	for (const PagePiece& piece : pieces) {
		PageRecords& page = m_pages[value(piece.page)];
		page.records.clear();
		page.unknown = true;
	}
}

bool LoadBook::rebuildable(PageNumber page) const {
	std::lock_guard lock(m_recordsMutex);
	auto            at = m_pages.find(value(page));
	return at != m_pages.end() && !at->second.unknown && !at->second.records.empty();
}

bool LoadBook::replay(PageNumber page, uint8_t* out) {
	std::vector<Record> records;
	{
		std::lock_guard lock(m_recordsMutex);
		auto            at = m_pages.find(value(page));
		if (at == m_pages.end() || at->second.unknown || at->second.records.empty()) {
			return false;
		}
		records = at->second.records;
	}
	m_pagesReplayed.fetch_add(1, std::memory_order_relaxed);
	std::memset(out, 0, kPageBytes);
	return std::all_of(records.begin(), records.end(),
	                   [&](const Record& record) { return copyRecord(record, out); });
}

bool LoadBook::copyRecord(const Record& record, uint8_t* out) {
	Decoded made = decoded(record);
	if (made == nullptr) {
		return false;
	}
	uint64_t from = record.index == 0 ? 0 : record.index * kPageBytes - record.begins;
	uint64_t to   = record.index == 0 ? record.begins : 0;
	if (from + record.length > made->size()) {
		return false;
	}
	std::memcpy(out + to, made->data() + from, record.length);
	return true;
}

Decoded LoadBook::decoded(const Record& record) {
	return m_decoded.kept(record.key, m_limits.decodedCapBytes, [&]() -> Decoded {
		std::vector<uint8_t> bytes;
		if (produce(record.file, record.key, bytes) != Status::Ok) {
			return nullptr;
		}
		return std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
	});
}

void LoadBook::prepare(std::span<const PageNumber> pages) {
	std::vector<Record> wanted = undecoded(pages);
	if (wanted.empty()) {
		return;
	}
	pool().each(wanted.size(), [&](size_t i) { decoded(wanted[i]); });
}

std::vector<LoadBook::Record> LoadBook::undecoded(std::span<const PageNumber> pages) const {
	std::vector<Record>                            wanted;
	std::unordered_set<RequestKey, RequestKeyHash> seen;
	std::lock_guard                                lock(m_recordsMutex);
	for (PageNumber page : pages) {
		auto at = m_pages.find(value(page));
		if (at == m_pages.end() || at->second.unknown) {
			continue;
		}
		for (const Record& record : at->second.records) {
			if (!m_decoded.has(record.key) && seen.insert(record.key).second) {
				wanted.push_back(record);
			}
		}
	}
	return wanted;
}

WorkerPool& LoadBook::pool() {
	std::call_once(m_poolOnce, [this] { m_pool = std::make_unique<WorkerPool>(workersFor(m_limits.decodeWorkers)); });
	return *m_pool;
}

void LoadBook::forget(PageNumber page) {
	PageRecords gone;
	{
		std::lock_guard lock(m_recordsMutex);
		auto            at = m_pages.find(value(page));
		if (at != m_pages.end()) {
			gone = std::move(at->second);
			m_pages.erase(at);
		}
	}
	dropStaged(page);
}

void LoadBook::stage(PageNumber page, uint32_t within, std::span<const uint8_t> bytes) {
	std::lock_guard lock(m_stagingMutex);
	m_staging.put(page, within, bytes, m_limits.stagingCapBytes);
}

void LoadBook::takenElsewhere(PageNumber page, uint64_t offset, uint64_t bytes) {
	std::lock_guard lock(m_stagingMutex);
	m_staging.takenElsewhere(page, offset, bytes);
}

bool LoadBook::takeStaged(PageNumber page, uint64_t offset, uint8_t* out, uint64_t bytes) {
	std::lock_guard lock(m_stagingMutex);
	bool            taken = m_staging.take(page, offset, std::span<uint8_t>(out, bytes));
	m_stagedReads.fetch_add(taken ? 1 : 0, std::memory_order_relaxed);
	return taken;
}

bool LoadBook::takeStagedPage(PageNumber page, uint8_t* out) {
	std::lock_guard lock(m_stagingMutex);
	bool            taken = m_staging.takeWhole(page, out);
	m_stagedReads.fetch_add(taken ? 1 : 0, std::memory_order_relaxed);
	return taken;
}

void LoadBook::dropStaged(PageNumber page) {
	std::lock_guard lock(m_stagingMutex);
	m_staging.forget(page);
}

void LoadBook::countLoaded() {
	m_requestsLoaded.fetch_add(1, std::memory_order_relaxed);
}

Loads::Totals LoadBook::totals() const {
	Loads::Totals totals;
	{
		std::lock_guard lock(m_stagingMutex);
		totals.stagedBytes = m_staging.held();
	}
	totals.decodedBytes    = m_decoded.bytes();
	totals.requestsLoaded  = m_requestsLoaded.load(std::memory_order_relaxed);
	totals.requestsDecoded = m_requestsDecoded.load(std::memory_order_relaxed);
	totals.pagesReplayed   = m_pagesReplayed.load(std::memory_order_relaxed);
	totals.stagedReads     = m_stagedReads.load(std::memory_order_relaxed);
	return totals;
}

} // namespace deunify::detail
