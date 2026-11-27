#pragma once

#include "deunify/types.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

namespace deunify::detail {

/// Decoded load bytes kept per page for the page's first reader. Each staged byte is given once:
/// take() hands out a range and forgets it. Pages are dropped oldest first (by their last put)
/// while more than `cap` bytes of whole pages are held. Not synchronized.
class Staging {
public:
	/// Bytes held: one whole page for every page with anything staged.
	uint64_t held() const;

	/// Stages `bytes` at `within` of `page` (over zeros, or over what is staged), making the page
	/// the newest, then drops the oldest pages past `cap`.
	void put(PageNumber page, uint64_t within, std::span<const uint8_t> bytes, uint64_t cap);

	/// `[within, within + bytes)` of `page` reached its reader another way: it is not given.
	void takenElsewhere(PageNumber page, uint64_t within, uint64_t bytes);

	/// Whether all of `[offset, offset + bytes)` of `page` is staged in one piece.
	bool covers(PageNumber page, uint64_t offset, uint64_t bytes) const;

	/// Gives `[offset, offset + out.size())` of `page` if all of it is staged, and forgets it.
	bool take(PageNumber page, uint64_t offset, std::span<uint8_t> out);

	/// Gives the whole page (kPageBytes into `out`) if all of it is staged. The page is
	/// forgotten either way.
	bool takeWhole(PageNumber page, uint8_t* out);

	/// Drops whatever is staged for `page`.
	void forget(PageNumber page);

private:
	struct Interval {
		uint64_t from = 0;
		uint64_t to   = 0;
	};

	struct Staged {
		std::unique_ptr<uint8_t[]> bytes;
		std::vector<Interval>      left;
		uint64_t                   age = 0;
	};

	void dropOldestPast(uint64_t cap);
	void forgetIfGiven(PageNumber page, const Staged& staged);

	std::unordered_map<uint64_t, Staged> m_pages;
	std::map<uint64_t, PageNumber>       m_ages;
	uint64_t                             m_next = 0;
};

} // namespace deunify::detail
