#include "deunify/loads.hpp"

#include "loadBook.hpp"

#include "deunify/cardView.hpp"
#include "deunify/context.hpp"
#include "deunify/gpuPageTable.hpp"
#include "deunify/pages.hpp"
#include "deunify/physicalMemory.hpp"

#include <cstring>

namespace deunify {

namespace {

detail::LoadLimits limitsOf(const Config& config) {
	return detail::LoadLimits {config.stagingCapBytes, config.decodedCapBytes, config.maxRecordsPerPage,
	                           config.decodeWorkers};
}

detail::CustomDecoder decoderOf(const Callbacks& callbacks) {
	return detail::CustomDecoder {callbacks.decode, callbacks.context};
}

} // namespace

struct Loads::Impl {
	explicit Impl(Context& owner)
	    : context(owner), book(limitsOf(owner.config()), decoderOf(owner.callbacks())) {}

	Status checkPages(std::span<const detail::PagePiece> pieces) const {
		for (const detail::PagePiece& piece : pieces) {
			if (!isValidPageNumber(value(piece.page))) {
				return Status::InvalidArgument;
			}
			if (!context.physical().allocated(piece.page)) {
				return Status::UnknownPage;
			}
		}
		return Status::Ok;
	}

	Status place(std::span<const detail::PagePiece> pieces, const uint8_t* bytes) {
		Status   first = Status::Ok;
		uint64_t done  = 0;
		for (const detail::PagePiece& piece : pieces) {
			Status status = placePiece(piece, std::span<const uint8_t>(bytes + done, piece.length));
			first         = first == Status::Ok ? status : first;
			queueLoaded(piece);
			done += piece.length;
		}
		return first;
	}

	Status placePiece(const detail::PagePiece& piece, std::span<const uint8_t> bytes) {
		switch (context.pages().placeLoad(piece.page, piece.whole())) {
		case Pages::Placement::Ram: return writeRam(piece, bytes);
		case Pages::Placement::File:
		case Pages::Placement::Card: stageOrSend(piece, bytes); return Status::Ok;
		}
		return Status::InvalidArgument;
	}

	Status writeRam(const detail::PagePiece& piece, std::span<const uint8_t> bytes) {
		uint8_t* page = context.physical().bytesOf(piece.page, true);
		if (page == nullptr) {
			return Status::OutOfMemory;
		}
		std::memcpy(page + piece.within, bytes.data(), bytes.size());
		return Status::Ok;
	}

	void stageOrSend(const detail::PagePiece& piece, std::span<const uint8_t> bytes) {
		CardView* cardView = context.cardView();
		if (cardView != nullptr && cardView->loaded(piece.page, piece.within, bytes.data(), bytes.size())) {
			book.takenElsewhere(piece.page, piece.within, bytes.size());
			return;
		}
		book.stage(piece.page, piece.within, bytes);
	}

	void queueLoaded(const detail::PagePiece& piece) {
		std::vector<uint64_t> slots;
		context.gpuPages().slotsOf(piece.page, slots);
		for (uint64_t slot : slots) {
			context.queueChange(Range {slot + piece.within, piece.length}, ChangeReason::Loaded);
		}
	}

	Context&          context;
	detail::LoadBook  book;
};

Loads::Loads(Context& context) : m_context(context), m_impl(std::make_unique<Impl>(context)) {}

Loads::~Loads() = default;

Status Loads::openFile(void* handle, FileId* file) {
	return m_impl->book.openHandle(handle, file);
}

Status Loads::openFile(const wchar_t* path, FileId* file) {
	return m_impl->book.openPath(path, file);
}

void Loads::closeFile(FileId file) {
	m_impl->book.closeFile(file);
}

Status Loads::load(const LoadRequest& request) {
	std::vector<detail::PagePiece> pieces =
	    detail::piecesOf(request.pages, request.offsetInFirstPage, request.destinationBytes);
	if (pieces.empty()) {
		return Status::InvalidArgument;
	}
	Status status = m_impl->checkPages(pieces);
	if (status != Status::Ok) {
		return status;
	}
	detail::SharedFile file = m_impl->book.file(request.file);
	if (file == nullptr) {
		return Status::NotFound;
	}
	detail::RequestKey   key = detail::keyOf(request);
	std::vector<uint8_t> bytes;
	status = m_impl->book.produce(file, key, bytes);
	if (status != Status::Ok) {
		return status;
	}
	m_impl->book.record(file, key, request.offsetInFirstPage, pieces);
	status = m_impl->place(pieces, bytes.data());
	m_impl->book.countLoaded();
	return status;
}

Status Loads::write(std::span<const PageNumber> pages, uint32_t offsetInFirstPage, const void* bytes,
                    uint64_t byteCount) {
	std::vector<detail::PagePiece> pieces = detail::piecesOf(pages, offsetInFirstPage, byteCount);
	if (pieces.empty() || bytes == nullptr) {
		return Status::InvalidArgument;
	}
	Status status = m_impl->checkPages(pieces);
	if (status != Status::Ok) {
		return status;
	}
	m_impl->book.markUnknown(pieces);
	return m_impl->place(pieces, static_cast<const uint8_t*>(bytes));
}

bool Loads::rebuildable(PageNumber page) const {
	return m_impl->book.rebuildable(page);
}

Loads::Totals Loads::totals() const {
	return m_impl->book.totals();
}

bool Loads::takeStaged(PageNumber page, uint64_t offset, uint8_t* out, uint64_t bytes) {
	return m_impl->book.takeStaged(page, offset, out, bytes);
}

bool Loads::takeStagedPage(PageNumber page, uint8_t* out) {
	return m_impl->book.takeStagedPage(page, out);
}

bool Loads::replay(PageNumber page, uint8_t* out) {
	return m_impl->book.replay(page, out);
}

void Loads::prepare(std::span<const PageNumber> pages) {
	m_impl->book.prepare(pages);
}

void Loads::dropStaged(PageNumber page) {
	m_impl->book.dropStaged(page);
}

void Loads::forget(PageNumber page) {
	m_impl->book.forget(page);
}

} // namespace deunify
