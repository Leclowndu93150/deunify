#include "censusLedger.hpp"

namespace deunify::detail {

Category Ledger::addCategory(const char* name) {
	std::lock_guard lock(m_mutex);
	m_uses.push_back(CategoryUse {.name = name});
	return Category {static_cast<uint32_t>(m_uses.size() - 1)};
}

bool Ledger::known(Category category) const {
	std::lock_guard lock(m_mutex);
	return static_cast<uint32_t>(category) < m_uses.size();
}

void Ledger::count(Category category, bool card, uint64_t bytes) {
	std::lock_guard lock(m_mutex);
	CategoryUse&    use = m_uses[static_cast<uint32_t>(category)];
	(card ? use.cardBytes : use.sharedBytes) += bytes;
	++(card ? use.cardCount : use.sharedCount);
}

void Ledger::uncount(Category category, bool card, uint64_t bytes) {
	std::lock_guard lock(m_mutex);
	CategoryUse&    use = m_uses[static_cast<uint32_t>(category)];
	(card ? use.cardBytes : use.sharedBytes) -= bytes;
	--(card ? use.cardCount : use.sharedCount);
}

std::vector<CategoryUse> Ledger::uses() const {
	std::lock_guard lock(m_mutex);
	return m_uses;
}

} // namespace deunify::detail
