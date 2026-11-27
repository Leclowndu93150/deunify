#include "deunify/pages.hpp"

#include "deunify/cardView.hpp"
#include "deunify/context.hpp"
#include "deunify/gpuPageTable.hpp"
#include "deunify/loads.hpp"
#include "deunify/physicalMemory.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <utility>

namespace deunify {

namespace {

constexpr ULONG_PTR kWriteAccess      = 1;
constexpr DWORD     kAccessParameters = 2;
constexpr uint32_t  kMostTouches      = 3;
constexpr auto      kRemapWaitBound   = std::chrono::seconds(10);

struct Entry {
	Home     home    = Home::Zero;
	bool     tile    = false;
	bool     armed   = false;
	bool     written = false;
	bool     listed  = false;
	uint32_t touches = 0;
	uint64_t version = 0;
};

struct Bracket {
	uint64_t ticket = 0;
	Range    range;
	bool     writes = false;
};

struct KindSpan {
	uint64_t end  = 0;
	Kind     kind = Kind::Unknown;
};

struct Fetch {
	Home     home    = Home::Zero;
	uint64_t version = 0;
};

using PageBytes = std::vector<uint8_t>;

uint64_t key(PageNumber page) {
	return value(page);
}

PageNumber pageInView(const View& view, uint64_t index) {
	return PageNumber {value(view.firstPage) + index * kFramesPerPage};
}

bool overlaps(Range a, Range b) {
	return a.start < b.end() && b.start < a.end();
}

bool cpuReachable(Kind kind) {
	return kind == Kind::Unknown || kind == Kind::CpuOnly;
}

bool rebuiltFromRecords(Home home) {
	return home == Home::File || home == Home::Texture;
}

bool heldElsewhere(Home home) {
	return rebuiltFromRecords(home) || home == Home::Card;
}

bool viewAllows(uint64_t address, bool write) {
	MEMORY_BASIC_INFORMATION info {};
	if (VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) == 0) {
		return false;
	}
	if (info.State != MEM_COMMIT) {
		return false;
	}
	return info.Protect == PAGE_READWRITE || (!write && info.Protect == PAGE_READONLY);
}

void pagesIn(const std::vector<View>& views, Range range, std::vector<PageNumber>& out) {
	for (const View& view : views) {
		const uint64_t first = std::max(range.start, view.address) - view.address;
		const uint64_t last  = std::min(range.end(), view.address + view.bytes) - view.address;
		for (uint64_t index = first / kPageBytes; index * kPageBytes < last; ++index) {
			out.push_back(pageInView(view, index));
		}
	}
	std::sort(out.begin(), out.end());
	out.erase(std::unique(out.begin(), out.end()), out.end());
}

} // namespace

struct Pages::Impl {
	explicit Impl(Context& owner) : context(owner) {}

	Context&                            context;
	mutable std::mutex                  lock;
	std::unordered_map<uint64_t, Entry> entries;
	std::map<uint64_t, KindSpan>        kinds;
	std::vector<PageNumber>             opened;
	std::vector<Bracket>                brackets;
	uint64_t                            nextTicket = 1;
	uint64_t                            versions   = 0;
	std::atomic<uint64_t>               parked     = 0;
	std::atomic<uint64_t>               faults     = 0;
	std::atomic<uint64_t>               readBacks  = 0;
	std::atomic<uint64_t>               replays    = 0;

	PhysicalMemory& physical() const { return context.physical(); }
	Loads&          loads() { return context.loads(); }

	Entry* find(PageNumber page) {
		const auto found = entries.find(key(page));
		return found == entries.end() ? nullptr : &found->second;
	}

	void changed(Entry& entry) { entry.version = ++versions; }

	void moveHome(Entry& entry, Home home) {
		entry.home = home;
		changed(entry);
	}

	Kind kindAt(uint64_t address) const {
		auto after = kinds.upper_bound(address);
		if (after == kinds.begin()) {
			return Kind::Unknown;
		}
		--after;
		return address < after->second.end ? after->second.kind : Kind::Unknown;
	}

	void setKind(uint64_t address, uint64_t bytes, Kind kind) {
		const uint64_t end = address + bytes;
		cutAt(address);
		cutAt(end);
		kinds.erase(kinds.lower_bound(address), kinds.lower_bound(end));
		if (kind != Kind::Unknown) {
			kinds[address] = KindSpan {end, kind};
		}
	}

	void cutAt(uint64_t address) {
		auto after = kinds.upper_bound(address);
		if (after == kinds.begin()) {
			return;
		}
		--after;
		if (after->first < address && address < after->second.end) {
			kinds[address]    = after->second;
			after->second.end = address;
		}
	}

	std::vector<uint64_t> viewsOf(PageNumber page) {
		std::vector<uint64_t> views;
		physical().viewsOf(page, views);
		return views;
	}

	std::vector<PageNumber> pagesShownIn(Range range) {
		std::vector<View> views;
		physical().viewsIn(range.start, range.bytes, views);
		std::vector<PageNumber> pages;
		pagesIn(views, range, pages);
		return pages;
	}

	bool underWriteBracket(const std::vector<uint64_t>& views) const {
		for (const Bracket& bracket : brackets) {
			if (!bracket.writes) {
				continue;
			}
			for (uint64_t view : views) {
				if (overlaps(bracket.range, Range {view, kPageBytes})) {
					return true;
				}
			}
		}
		return false;
	}

	bool shownOnlyToTheGpu(const std::vector<uint64_t>& views) const {
		return std::none_of(views.begin(), views.end(),
		                    [this](uint64_t view) { return cpuReachable(kindAt(view)); });
	}

	void open(PageNumber page, Entry& entry) {
		entry.written = true;
		if (!entry.listed) {
			entry.listed = true;
			opened.push_back(page);
		}
	}

	void disarm(PageNumber page, Entry& entry) {
		if (entry.armed) {
			physical().protectViews(page, PAGE_READWRITE);
			entry.armed = false;
		}
	}

	bool arm(PageNumber page, Entry& entry) {
		if (entry.home != Home::Ram) {
			return true;
		}
		if (underWriteBracket(viewsOf(page))) {
			return false;
		}
		entry.armed = true;
		if (physical().protectViews(page, PAGE_READONLY)) {
			return true;
		}
		physical().protectViews(page, PAGE_READWRITE);
		entry.armed = false;
		return false;
	}

	bool writeAt(PageNumber page, Entry& entry) {
		disarm(page, entry);
		open(page, entry);
		++faults;
		return true;
	}

	void becomeRam(PageNumber page, Entry& entry) {
		entry.armed = false;
		moveHome(entry, Home::Ram);
		open(page, entry);
	}

	void closeRam(PageNumber page, Entry& entry) {
		entry.armed   = false;
		entry.written = false;
		if (physical().committed(page)) {
			physical().protectViews(page, PAGE_NOACCESS);
		}
	}

	void leaveRam(PageNumber page) {
		if (physical().committed(page)) {
			physical().reset(page);
		}
	}

	bool copyRam(PageNumber page, uint8_t* out) {
		const uint8_t* ram = physical().bytesOf(page, false);
		if (ram == nullptr) {
			std::memset(out, 0, kPageBytes);
		} else {
			std::memcpy(out, ram, kPageBytes);
		}
		return true;
	}

	bool replay(PageNumber page, uint8_t* out) {
		if (!loads().replay(page, out)) {
			return false;
		}
		++replays;
		return true;
	}

	bool readBack(PageNumber page, uint8_t* out) {
		CardView* cardView = context.cardView();
		return cardView != nullptr && cardView->readBack(page, out);
	}

	bool fetchUnlocked(PageNumber page, Home home, uint8_t* out) {
		switch (home) {
		case Home::File:
			return loads().takeStagedPage(page, out) || replay(page, out);
		case Home::Texture:
			return replay(page, out);
		case Home::Card:
			return readBack(page, out);
		case Home::Zero:
		case Home::Ram:
			break;
		}
		return false;
	}

	bool bytesHeldHere(PageNumber page, const Entry& entry, uint8_t* out) {
		if (entry.home == Home::Ram) {
			return copyRam(page, out);
		}
		std::memset(out, 0, kPageBytes);
		return true;
	}

	bool unchanged(PageNumber page, const Fetch& fetch) {
		const Entry* entry = find(page);
		return entry != nullptr && entry->version == fetch.version;
	}

	Status settleInRam(PageNumber page, Entry& entry) {
		if (physical().bytesOf(page, true) == nullptr) {
			return Status::OutOfMemory;
		}
		if (entry.home == Home::Zero) {
			becomeRam(page, entry);
		}
		return Status::Ok;
	}

	Status landInRam(PageNumber page, Entry& entry, const uint8_t* bytes) {
		uint8_t* ram = physical().bytesOf(page, true);
		if (ram == nullptr) {
			return Status::OutOfMemory;
		}
		std::memcpy(ram, bytes, kPageBytes);
		physical().protectViews(page, PAGE_READWRITE);
		becomeRam(page, entry);
		return Status::Ok;
	}

	Status touchedHome(PageNumber page, Entry& entry, const uint8_t* bytes) {
		const Home   from   = entry.home;
		const Status landed = landInRam(page, entry, bytes);
		if (landed == Status::Ok && from == Home::Card) {
			++readBacks;
			context.cardView()->cardHomeLeft(page);
		}
		return landed;
	}

	Status bringToRam(PageNumber page) {
		PageBytes bytes(kPageBytes);
		for (;;) {
			Fetch fetch;
			{
				std::lock_guard guard(lock);
				Entry*          entry = find(page);
				if (entry == nullptr) {
					return Status::UnknownPage;
				}
				if (!heldElsewhere(entry->home)) {
					return settleInRam(page, *entry);
				}
				fetch = Fetch {entry->home, entry->version};
			}
			const bool fetched = fetchUnlocked(page, fetch.home, bytes.data());
			std::lock_guard guard(lock);
			if (!unchanged(page, fetch)) {
				continue;
			}
			if (!fetched) {
				context.log(LogLevel::Error, "a page's bytes are lost: its home cannot give them");
				return Status::NoSource;
			}
			return touchedHome(page, *find(page), bytes.data());
		}
	}

	bool fault(uint64_t address, bool write) {
		const std::optional<PageAt> at = physical().pageAt(address);
		if (!at) {
			return false;
		}
		bool touch = false;
		{
			std::lock_guard guard(lock);
			Entry*          entry = find(at->page);
			if (entry == nullptr) {
				return false;
			}
			if (entry->home == Home::Ram) {
				return faultInRam(at->page, *entry, address, write);
			}
			touch = entry->home != Home::Zero;
		}
		if (bringToRam(at->page) != Status::Ok) {
			return false;
		}
		std::lock_guard guard(lock);
		if (Entry* entry = find(at->page); entry != nullptr && touch) {
			++entry->touches;
		}
		++faults;
		return true;
	}

	bool faultInRam(PageNumber page, Entry& entry, uint64_t address, bool write) {
		if (!physical().committed(page)) {
			++faults;
			return physical().commit(page);
		}
		if (entry.armed && write) {
			return writeAt(page, entry);
		}
		return viewAllows(address, write);
	}

	bool remapSettled(std::chrono::steady_clock::time_point deadline) const {
		while (physical().remapGeneration() % 2 != 0) {
			if (std::chrono::steady_clock::now() >= deadline) {
				context.log(LogLevel::Error, "a fault gave up waiting for a view change to finish");
				return false;
			}
			std::this_thread::yield();
		}
		return true;
	}

	bool faultAcrossRemaps(uint64_t address, bool write) {
		const auto deadline = std::chrono::steady_clock::now() + kRemapWaitBound;
		for (;;) {
			const uint64_t before  = physical().remapGeneration();
			const bool     handled = fault(address, write);
			const uint64_t after   = physical().remapGeneration();
			if (handled || (before == after && before % 2 == 0)) {
				return handled;
			}
			if (!remapSettled(deadline)) {
				return false;
			}
		}
	}

	std::vector<PageNumber> takeOpened() {
		std::vector<PageNumber> taken;
		for (PageNumber page : std::exchange(opened, {})) {
			Entry* entry = find(page);
			if (entry == nullptr) {
				continue;
			}
			entry->listed = false;
			if (entry->home != Home::Ram || !entry->written) {
				continue;
			}
			entry->written = false;
			taken.push_back(page);
		}
		for (PageNumber page : taken) {
			Entry& entry = *find(page);
			if (!arm(page, entry)) {
				open(page, entry);
			}
		}
		return taken;
	}

	std::optional<ParkResult> refusal(PageNumber page) {
		const Entry* entry = find(page);
		if (entry == nullptr) {
			return ParkResult::Unknown;
		}
		if (rebuiltFromRecords(entry->home)) {
			return ParkResult::Parked;
		}
		if (entry->home != Home::Ram || !physical().committed(page)) {
			return ParkResult::NotWritten;
		}
		if (entry->touches >= kMostTouches) {
			return ParkResult::Touched;
		}
		return std::nullopt;
	}

	ParkResult parkWith(PageNumber page, const uint8_t* rebuilt) {
		if (const std::optional<ParkResult> refused = refusal(page)) {
			return *refused;
		}
		Entry& entry = *find(page);
		if (entry.armed || underWriteBracket(viewsOf(page))) {
			return ParkResult::InUse;
		}
		physical().protectViews(page, PAGE_NOACCESS);
		const uint8_t* ram = physical().bytesOf(page, false);
		if (ram == nullptr || std::memcmp(ram, rebuilt, kPageBytes) != 0) {
			physical().protectViews(page, PAGE_READWRITE);
			return ParkResult::Differs;
		}
		entry.written = false;
		leaveRam(page);
		moveHome(entry, Home::File);
		++parked;
		return ParkResult::Parked;
	}

	Placement placeInFile(PageNumber page, Entry& entry) {
		if (entry.home == Home::Ram) {
			closeRam(page, entry);
			leaveRam(page);
		}
		moveHome(entry, Home::File);
		return Placement::File;
	}

	bool fileEligible(PageNumber page, const Entry& entry, bool whole) {
		const bool earlierBytesKnown = entry.home == Home::Zero || rebuiltFromRecords(entry.home);
		return (whole || earlierBytesKnown) && loads().rebuildable(page);
	}

	std::optional<Placement> placedAtOnce(PageNumber page, bool whole,
	                                      const std::vector<uint64_t>& views) {
		Entry* entry = find(page);
		if (entry == nullptr) {
			return Placement::Ram;
		}
		changed(*entry);
		if (entry->home == Home::Card) {
			return Placement::Card;
		}
		if (shownOnlyToTheGpu(views) && !underWriteBracket(views) &&
		    fileEligible(page, *entry, whole)) {
			return placeInFile(page, *entry);
		}
		if (entry->home == Home::Ram) {
			open(page, *entry);
			return Placement::Ram;
		}
		return std::nullopt;
	}

	void placedInRam(PageNumber page, Status brought) {
		std::lock_guard guard(lock);
		Entry*          entry = find(page);
		if (entry == nullptr) {
			return;
		}
		if (brought != Status::Ok && entry->home != Home::Ram) {
			context.log(LogLevel::Error, "a partial load lands on a page whose earlier bytes are lost");
			physical().protectViews(page, PAGE_READWRITE);
			becomeRam(page, *entry);
		}
		open(page, *entry);
	}

	std::optional<Status> readHere(PageNumber page, uint64_t offset, uint8_t* out, uint64_t bytes,
	                               ReadPurpose purpose, Fetch& fetch) {
		Entry* entry = find(page);
		if (entry == nullptr) {
			return Status::UnknownPage;
		}
		if (!heldElsewhere(entry->home)) {
			PageBytes held(kPageBytes);
			bytesHeldHere(page, *entry, held.data());
			std::memcpy(out, held.data() + offset, bytes);
			return Status::Ok;
		}
		if (entry->home == Home::File && loads().takeStaged(page, offset, out, bytes)) {
			consumeForTexture(page, *entry, purpose, offset, bytes);
			return Status::Ok;
		}
		fetch = Fetch {entry->home, entry->version};
		return std::nullopt;
	}

	bool fetchForRead(PageNumber page, Home home, uint8_t* out) {
		return home == Home::File ? replay(page, out) : fetchUnlocked(page, home, out);
	}

	Status readPage(PageNumber page, uint64_t offset, uint8_t* out, uint64_t bytes,
	                ReadPurpose purpose) {
		PageBytes held(kPageBytes);
		for (;;) {
			Fetch fetch;
			{
				std::lock_guard guard(lock);
				if (const std::optional<Status> done =
				        readHere(page, offset, out, bytes, purpose, fetch)) {
					return *done;
				}
			}
			const bool      fetched = fetchForRead(page, fetch.home, held.data());
			std::lock_guard guard(lock);
			if (!unchanged(page, fetch)) {
				continue;
			}
			if (!fetched) {
				return Status::NoSource;
			}
			std::memcpy(out, held.data() + offset, bytes);
			consumeForTexture(page, *find(page), purpose, offset, bytes);
			return Status::Ok;
		}
	}

	void consumeForTexture(PageNumber page, Entry& entry, ReadPurpose purpose, uint64_t offset,
	                       uint64_t bytes) {
		const bool whole = offset == 0 && bytes == kPageBytes;
		if (entry.home != Home::File || purpose != ReadPurpose::TextureUpload || !whole) {
			return;
		}
		loads().dropStaged(page);
		moveHome(entry, Home::Texture);
	}

	std::optional<bool> givenHere(PageNumber page, uint8_t* out, bool forCard, Fetch& fetch) {
		Entry* entry = find(page);
		if (entry == nullptr || entry->home == Home::Card) {
			return false;
		}
		if (rebuiltFromRecords(entry->home)) {
			fetch = Fetch {entry->home, entry->version};
			return std::nullopt;
		}
		const bool ram = entry->home == Home::Ram;
		if (ram && forCard) {
			closeRam(page, *entry);
		}
		bytesHeldHere(page, *entry, out);
		if (forCard) {
			if (ram) {
				leaveRam(page);
			}
			moveHome(*entry, Home::Card);
		}
		return true;
	}

	bool give(PageNumber page, uint8_t* out, bool forCard) {
		for (;;) {
			Fetch fetch;
			{
				std::lock_guard guard(lock);
				if (const std::optional<bool> done = givenHere(page, out, forCard, fetch)) {
					return *done;
				}
			}
			const bool      fetched = fetchUnlocked(page, fetch.home, out);
			std::lock_guard guard(lock);
			if (!unchanged(page, fetch)) {
				continue;
			}
			if (!forCard) {
				return fetched;
			}
			if (!fetched) {
				context.log(LogLevel::Error, "a page goes home to the card with its bytes lost");
				std::memset(out, 0, kPageBytes);
			}
			moveHome(*find(page), Home::Card);
			return true;
		}
	}

	std::vector<PageNumber> rebuiltAmong(const std::vector<PageNumber>& pages) {
		std::lock_guard         guard(lock);
		std::vector<PageNumber> rebuilt;
		for (PageNumber page : pages) {
			const Entry* entry = find(page);
			if (entry != nullptr && rebuiltFromRecords(entry->home)) {
				rebuilt.push_back(page);
			}
		}
		return rebuilt;
	}

	void prepareReads(uint64_t address, uint64_t bytes) {
		if (address / kPageBytes == (address + bytes - 1) / kPageBytes) {
			return;
		}
		const std::vector<PageNumber> rebuilt = rebuiltAmong(pagesShownIn(Range {address, bytes}));
		if (!rebuilt.empty()) {
			loads().prepare(rebuilt);
		}
	}

	void reopen(const std::vector<PageNumber>& pages) {
		std::lock_guard guard(lock);
		for (PageNumber page : pages) {
			Entry* entry = find(page);
			if (entry != nullptr && entry->home == Home::Ram) {
				disarm(page, *entry);
				open(page, *entry);
			}
		}
	}

	void dropBracket(uint64_t ticket) {
		std::lock_guard guard(lock);
		std::erase_if(brackets, [ticket](const Bracket& b) { return b.ticket == ticket; });
	}

	std::optional<Bracket> endBracket(uint64_t ticket) {
		std::lock_guard guard(lock);
		const auto      found = std::find_if(brackets.begin(), brackets.end(),
		                                     [ticket](const Bracket& b) { return b.ticket == ticket; });
		if (found == brackets.end()) {
			return std::nullopt;
		}
		const Bracket ended = *found;
		brackets.erase(found);
		return ended;
	}
};

Pages::Pages(Context& context) : m_context(context), m_impl(std::make_unique<Impl>(context)) {}

Pages::~Pages() = default;

void Pages::setKind(uint64_t address, uint64_t bytes, Kind kind) {
	if (bytes == 0) {
		return;
	}
	std::lock_guard guard(m_impl->lock);
	m_impl->setKind(address, bytes, kind);
}

Kind Pages::kindAt(uint64_t address) const {
	std::lock_guard guard(m_impl->lock);
	return m_impl->kindAt(address);
}

PageState Pages::state(PageNumber page) const {
	PageState state;
	{
		std::lock_guard guard(m_impl->lock);
		const Entry*    entry = m_impl->find(page);
		if (entry == nullptr) {
			return state;
		}
		state.home    = entry->home;
		state.tile    = entry->tile;
		state.armed   = entry->armed;
		state.written = entry->written;
	}
	state.committed = m_context.physical().committed(page);
	state.views     = static_cast<uint32_t>(m_impl->viewsOf(page).size());
	return state;
}

HomeCounts Pages::counts() const {
	std::lock_guard guard(m_impl->lock);
	HomeCounts      counts;
	for (const auto& [number, entry] : m_impl->entries) {
		switch (entry.home) {
		case Home::Zero: ++counts.zero; break;
		case Home::File: ++counts.file; break;
		case Home::Texture: ++counts.texture; break;
		case Home::Card: ++counts.card; break;
		case Home::Ram:
			++counts.ram;
			counts.ramTiled += entry.tile ? 1 : 0;
			break;
		}
	}
	counts.parked    = m_impl->parked;
	counts.faults    = m_impl->faults;
	counts.readBacks = m_impl->readBacks;
	counts.replays   = m_impl->replays;
	return counts;
}

bool Pages::handleFault(_EXCEPTION_POINTERS* exception) {
	if (exception == nullptr || exception->ExceptionRecord == nullptr) {
		return false;
	}
	const EXCEPTION_RECORD& record = *exception->ExceptionRecord;
	if (record.ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
	    record.NumberParameters < kAccessParameters ||
	    record.ExceptionInformation[0] > kWriteAccess) {
		return false;
	}
	const uint64_t address = record.ExceptionInformation[1];
	const bool     write   = record.ExceptionInformation[0] == kWriteAccess;
	return m_impl->faultAcrossRemaps(address, write);
}

void Pages::takeWritten(std::vector<PageNumber>& out) {
	std::vector<PageNumber> taken;
	{
		std::lock_guard guard(m_impl->lock);
		taken = m_impl->takeOpened();
	}
	std::vector<uint64_t> slots;
	for (PageNumber page : taken) {
		slots.clear();
		m_context.gpuPages().slotsOf(page, slots);
		for (uint64_t slot : slots) {
			m_context.queueChange(Range {slot, kPageBytes}, ChangeReason::CpuWritten);
		}
	}
	out.insert(out.end(), taken.begin(), taken.end());
}

ParkResult Pages::park(PageNumber page) {
	{
		std::lock_guard guard(m_impl->lock);
		if (const std::optional<ParkResult> refused = m_impl->refusal(page)) {
			return *refused;
		}
	}
	PageBytes rebuilt(kPageBytes);
	if (!m_context.loads().replay(page, rebuilt.data())) {
		return ParkResult::NoSource;
	}
	std::lock_guard guard(m_impl->lock);
	return m_impl->parkWith(page, rebuilt.data());
}

Status Pages::readTitle(uint64_t address, void* out, uint64_t bytes, ReadPurpose purpose) {
	if (bytes == 0) {
		return Status::Ok;
	}
	if (out == nullptr) {
		return Status::InvalidArgument;
	}
	m_impl->prepareReads(address, bytes);
	auto*  bytesOut = static_cast<uint8_t*>(out);
	Status result   = Status::Ok;
	for (uint64_t done = 0; done < bytes;) {
		const uint64_t              at    = address + done;
		const uint64_t              count = std::min(kPageBytes - at % kPageBytes, bytes - done);
		const std::optional<PageAt> where = m_context.physical().pageAt(at);
		const Status                read  = where ? m_impl->readPage(where->page, where->offset,
		                                                             bytesOut + done, count, purpose)
		                                          : Status::UnknownPage;
		if (read != Status::Ok) {
			std::memset(bytesOut + done, 0, count);
			result = result == Status::Ok ? read : result;
		}
		done += count;
	}
	return result;
}

Status Pages::readPage(PageNumber page, uint64_t offset, void* out, uint64_t bytes,
                       ReadPurpose purpose) {
	if (out == nullptr || offset >= kPageBytes || bytes > kPageBytes - offset) {
		return Status::InvalidArgument;
	}
	return m_impl->readPage(page, offset, static_cast<uint8_t*>(out), bytes, purpose);
}

void Pages::pagesAllocated(std::span<const PageNumber> pages) {
	std::lock_guard guard(m_impl->lock);
	for (PageNumber page : pages) {
		Entry& entry = m_impl->entries[key(page)];
		entry        = Entry {};
		m_impl->changed(entry);
	}
}

void Pages::pageFreed(PageNumber) {}

void Pages::pageForgotten(PageNumber page) {
	{
		std::lock_guard guard(m_impl->lock);
		m_impl->entries.erase(key(page));
	}
	m_context.loads().dropStaged(page);
	m_context.loads().forget(page);
	if (CardView* cardView = m_context.cardView()) {
		cardView->pageForgotten(page);
	}
}

void Pages::viewMapped(const View& view) {
	std::lock_guard guard(m_impl->lock);
	for (uint64_t index = 0; index * kPageBytes < view.bytes; ++index) {
		const PageNumber page    = pageInView(view, index);
		const uint64_t   address = view.address + index * kPageBytes;
		Entry*           entry   = m_impl->find(page);
		if (entry == nullptr) {
			continue;
		}
		if (entry->home != Home::Ram) {
			if (m_context.physical().committed(page) &&
			    !m_context.physical().protectView(address, PAGE_NOACCESS)) {
				m_context.log(LogLevel::Error, "a new view of a page outside RAM stays open");
			}
			continue;
		}
		if (entry->armed && !m_context.physical().protectView(address, PAGE_READONLY)) {
			m_impl->disarm(page, *entry);
			m_impl->open(page, *entry);
		}
	}
}

void Pages::viewUnmapping(uint64_t, uint64_t) {}

Pages::Placement Pages::placeLoad(PageNumber page, bool whole) {
	const std::vector<uint64_t> views = m_impl->viewsOf(page);
	{
		std::lock_guard guard(m_impl->lock);
		if (const std::optional<Placement> placed = m_impl->placedAtOnce(page, whole, views)) {
			return *placed;
		}
	}
	m_impl->placedInRam(page, m_impl->bringToRam(page));
	return Placement::Ram;
}

bool Pages::bytesForTile(PageNumber page, uint8_t* out) {
	return m_impl->give(page, out, false);
}

bool Pages::becomeCardHome(PageNumber page, uint8_t* out) {
	return m_impl->give(page, out, true);
}

void Pages::cardHomeLost(PageNumber page, const uint8_t* bytes) {
	std::lock_guard guard(m_impl->lock);
	Entry*          entry = m_impl->find(page);
	if (entry == nullptr || entry->home != Home::Card) {
		return;
	}
	if (m_impl->landInRam(page, *entry, bytes) != Status::Ok) {
		m_context.log(LogLevel::Error, "a page coming home from the card cannot be committed");
	}
}

void Pages::tileChanged(PageNumber page, bool tile) {
	std::lock_guard guard(m_impl->lock);
	if (Entry* entry = m_impl->find(page)) {
		entry->tile = tile;
	}
}

Status Pages::beginKernelAccess(uint64_t address, uint64_t bytes, bool writes, uint64_t& ticket) {
	ticket = 0;
	if (bytes == 0) {
		return Status::Ok;
	}
	const std::vector<PageNumber> pages = m_impl->pagesShownIn(Range {address, bytes});
	uint64_t                      opened = 0;
	{
		std::lock_guard guard(m_impl->lock);
		opened = m_impl->nextTicket++;
		m_impl->brackets.push_back(Bracket {opened, Range {address, bytes}, writes});
	}
	for (PageNumber page : pages) {
		const Status status = m_impl->bringToRam(page);
		if (status != Status::Ok && status != Status::UnknownPage) {
			m_impl->dropBracket(opened);
			return status;
		}
	}
	if (writes) {
		m_impl->reopen(pages);
	}
	ticket = opened;
	return Status::Ok;
}

void Pages::endKernelAccess(uint64_t ticket) {
	const std::optional<Bracket> ended = m_impl->endBracket(ticket);
	if (!ended || !ended->writes) {
		return;
	}
	m_impl->reopen(m_impl->pagesShownIn(ended->range));
}

KernelWrite::KernelWrite(Pages& pages, uint64_t address, uint64_t bytes, Access access)
    : m_pages(&pages) {
	m_status = pages.beginKernelAccess(address, bytes, access == Access::Write, m_ticket);
}

KernelWrite::~KernelWrite() {
	if (m_status == Status::Ok) {
		m_pages->endKernelAccess(m_ticket);
	}
}

} // namespace deunify
