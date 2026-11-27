#pragma once

#include "deunify/census.hpp"

#include <d3d12.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace deunify::detail {

/// Whether memory with `properties` lies in the card's own memory (default and GPU upload heaps,
/// custom heaps in pool L1) rather than in system memory the card reads.
bool onCard(const D3D12_HEAP_PROPERTIES& properties);

/// Bytes and allocations by category. Shared with the objects attached to counted D3D12 objects,
/// so an object released after the census is gone still uncounts safely.
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

/// Counts `heap` in `category` until the runtime destroys it.
Status trackHeap(const std::shared_ptr<Ledger>& ledger, ID3D12Heap* heap, Category category);

/// Counts committed resource `resource` in `category` until the runtime destroys it. A reserved
/// resource (no heap properties) is not counted.
Status trackResource(const std::shared_ptr<Ledger>& ledger, ID3D12Device* device,
                     ID3D12Resource* resource, Category category);

} // namespace deunify::detail
