#pragma once

#include "deunify/census.hpp"

#include <cstdint>
#include <mutex>
#include <vector>

namespace deunify::detail {

/// Bytes and allocations by category. Shared with whatever uncounts an allocation when it goes
/// (the objects attached to counted D3D12 objects, the Vulkan backend), so that can happen
/// safely after the census is gone.
class Ledger {
public:
	Category addCategory(const char* name);
	bool     known(Category category) const;
	void     count(Category category, bool card, uint64_t bytes);
	void     uncount(Category category, bool card, uint64_t bytes);
	std::vector<CategoryUse> uses() const;

private:
	mutable std::mutex       m_mutex;
	std::vector<CategoryUse> m_uses;
};

} // namespace deunify::detail
