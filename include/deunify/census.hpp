#pragma once

#include "deunify/cardView.hpp"
#include "deunify/loads.hpp"
#include "deunify/pages.hpp"
#include "deunify/physicalMemory.hpp"
#include "deunify/types.hpp"

#include <vulkan/vulkan_core.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct ID3D12Heap;
struct ID3D12Resource;
struct ID3D12Device;
struct IDXGIAdapter3;

namespace deunify::detail {
class Ledger;
}

namespace deunify::gpu {
class Backend;
}

namespace deunify {

class Context;

enum class Category : uint32_t {};

// Always there; the card view's tile heaps count here.
inline constexpr Category kCardViewCategory = Category {0};

struct CategoryUse {
	std::string name;
	uint64_t    cardBytes   = 0; // default heaps, L1 custom heaps
	uint64_t    cardCount   = 0;
	uint64_t    sharedBytes = 0; // system memory the card reads: upload, readback, L0
	uint64_t    sharedCount = 0;
};

// IDXGIAdapter3::QueryVideoMemoryInfo on D3D12, VK_EXT_memory_budget on Vulkan. A Vulkan device
// without that extension gives only its heap sizes: then the budgets are those sizes and the
// usages are what the census counted, and `counted` says so.
struct SystemMemory {
	bool     known          = false;
	bool     counted        = false;
	uint64_t localUsage     = 0;
	uint64_t localBudget    = 0;
	uint64_t nonLocalUsage  = 0;
	uint64_t nonLocalBudget = 0;
};

struct CensusSnapshot {
	std::vector<CategoryUse> categories;
	SystemMemory             system;
	HomeCounts               homes;
	PhysicalMemory::Totals   physical;
	Loads::Totals            loads;
	CardView::Totals         cardView;
	// What is still held twice. Both should be small; they measure how far an integration is
	// from one home per page. The second needs Callbacks::texturesOver.
	uint64_t ramAndTileBytes     = 0;
	uint64_t tileAndTextureBytes = 0;
};

// Video memory by category, next to what the system reports. Track your own heaps too, or the
// sum won't match and you won't see where the card went.
//
// On D3D12, tracking lasts until the object dies (we attach an object with
// SetPrivateDataInterface that uncounts it on release), so there is no untrack. Placed
// resources count through their heap; reserved resources have no memory and don't count. On
// Vulkan nothing tells us when memory goes, so untrack it before you free it.
class Census {
public:
	Census(const Census&)            = delete;
	Census& operator=(const Census&) = delete;
	~Census();

	Category addCategory(const char* name);

	Status track(ID3D12Heap* heap, Category category);
	Status track(ID3D12Device* device, ID3D12Resource* resource, Category category);

	// Vulkan memory from vkAllocateMemory, `bytes` long, of type `memoryType` of the card
	// view's physical device. Unavailable without a Vulkan card view or for a type that device
	// doesn't have. Tracking it again moves it to the new category.
	Status track(VkDeviceMemory memory, uint64_t bytes, uint32_t memoryType, Category category);
	void   untrack(VkDeviceMemory memory);

	// Calls Callbacks::texturesOver, with no lock held, for each tile's slots until one is
	// covered. Once a second is plenty.
	CensusSnapshot snapshot();

	// AddRef'd. Without one, SystemMemory::known stays false on D3D12. A Vulkan card view
	// asks its own device.
	void setAdapter(IDXGIAdapter3* adapter);

	// One log line.
	static std::string format(const CensusSnapshot& snapshot);

private:
	friend class Context;
	friend class CardView;

	explicit Census(Context& context);

	std::shared_ptr<detail::Ledger> ledger() const;
	void                            attach(gpu::Backend* backend);

	struct Impl;
	Context&              m_context;
	std::unique_ptr<Impl> m_impl;
};

} // namespace deunify
