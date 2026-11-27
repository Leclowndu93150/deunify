#include "staging.hpp"

#include <algorithm>
#include <cstring>

namespace deunify::detail {

namespace {

template <typename Interval>
void addInterval(std::vector<Interval>& left, uint64_t from, uint64_t to) {
	std::erase_if(left, [&](const Interval& interval) {
		if (interval.to < from || interval.from > to) {
			return false;
		}
		from = std::min(from, interval.from);
		to   = std::max(to, interval.to);
		return true;
	});
	auto at = std::partition_point(left.begin(), left.end(),
	                               [&](const Interval& interval) { return interval.from < from; });
	left.insert(at, Interval {from, to});
}

template <typename Interval>
void removeInterval(std::vector<Interval>& left, uint64_t from, uint64_t to) {
	std::vector<Interval> kept;
	kept.reserve(left.size() + 1);
	for (const Interval& interval : left) {
		if (interval.to <= from || interval.from >= to) {
			kept.push_back(interval);
			continue;
		}
		if (interval.from < from) {
			kept.push_back(Interval {interval.from, from});
		}
		if (interval.to > to) {
			kept.push_back(Interval {to, interval.to});
		}
	}
	left = std::move(kept);
}

} // namespace

uint64_t Staging::held() const {
	return m_pages.size() * kPageBytes;
}

void Staging::put(PageNumber page, uint64_t within, std::span<const uint8_t> bytes, uint64_t cap) {
	uint64_t age      = ++m_next;
	auto [entry, now] = m_pages.try_emplace(value(page));
	Staged& staged    = entry->second;
	if (now) {
		staged.bytes = std::make_unique<uint8_t[]>(kPageBytes);
	} else {
		m_ages.erase(staged.age);
	}
	staged.age = age;
	std::memcpy(staged.bytes.get() + within, bytes.data(), bytes.size());
	addInterval(staged.left, within, within + bytes.size());
	m_ages.emplace(age, page);
	dropOldestPast(cap);
}

void Staging::takenElsewhere(PageNumber page, uint64_t within, uint64_t bytes) {
	auto entry = m_pages.find(value(page));
	if (entry == m_pages.end()) {
		return;
	}
	removeInterval(entry->second.left, within, within + bytes);
	forgetIfGiven(page, entry->second);
}

bool Staging::covers(PageNumber page, uint64_t offset, uint64_t bytes) const {
	auto entry = m_pages.find(value(page));
	if (entry == m_pages.end()) {
		return false;
	}
	return std::any_of(entry->second.left.begin(), entry->second.left.end(), [&](const Interval& interval) {
		return interval.from <= offset && offset + bytes <= interval.to;
	});
}

bool Staging::take(PageNumber page, uint64_t offset, std::span<uint8_t> out) {
	if (!covers(page, offset, out.size())) {
		return false;
	}
	Staged& staged = m_pages.find(value(page))->second;
	std::memcpy(out.data(), staged.bytes.get() + offset, out.size());
	removeInterval(staged.left, offset, offset + out.size());
	forgetIfGiven(page, staged);
	return true;
}

bool Staging::takeWhole(PageNumber page, uint8_t* out) {
	bool whole = covers(page, 0, kPageBytes);
	if (whole) {
		std::memcpy(out, m_pages.find(value(page))->second.bytes.get(), kPageBytes);
	}
	forget(page);
	return whole;
}

void Staging::forget(PageNumber page) {
	auto entry = m_pages.find(value(page));
	if (entry == m_pages.end()) {
		return;
	}
	m_ages.erase(entry->second.age);
	m_pages.erase(entry);
}

void Staging::dropOldestPast(uint64_t cap) {
	while (held() > cap && !m_ages.empty()) {
		auto oldest = m_ages.begin();
		m_pages.erase(value(oldest->second));
		m_ages.erase(oldest);
	}
}

void Staging::forgetIfGiven(PageNumber page, const Staged& staged) {
	if (staged.left.empty()) {
		forget(page);
	}
}

} // namespace deunify::detail
