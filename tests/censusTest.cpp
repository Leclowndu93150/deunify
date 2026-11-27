#include "deunify/census.hpp"

#include "d3d12Backend.hpp"

#include "deunify/context.hpp"

#include <gtest/gtest.h>

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

namespace {

using namespace deunify;
using Microsoft::WRL::ComPtr;

constexpr uint64_t kMegabyte = 1ull << 20;

struct Warp {
	ComPtr<IDXGIFactory4> factory;
	ComPtr<IDXGIAdapter3> adapter;
	ComPtr<ID3D12Device>  device;
};

bool makeWarp(Warp& warp) {
	return SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&warp.factory))) &&
	       SUCCEEDED(warp.factory->EnumWarpAdapter(IID_PPV_ARGS(&warp.adapter))) &&
	       SUCCEEDED(D3D12CreateDevice(warp.adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&warp.device)));
}

D3D12_HEAP_PROPERTIES heapProperties(D3D12_HEAP_TYPE type, D3D12_MEMORY_POOL pool = D3D12_MEMORY_POOL_UNKNOWN) {
	D3D12_HEAP_PROPERTIES properties {};
	properties.Type                 = type;
	properties.MemoryPoolPreference = pool;
	return properties;
}

ComPtr<ID3D12Heap> makeHeap(ID3D12Device* device, uint64_t bytes) {
	D3D12_HEAP_DESC desc {};
	desc.SizeInBytes = bytes;
	desc.Properties  = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
	desc.Flags       = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
	ComPtr<ID3D12Heap> heap;
	device->CreateHeap(&desc, IID_PPV_ARGS(&heap));
	return heap;
}

ComPtr<ID3D12Resource> makeBuffer(ID3D12Device* device, uint64_t bytes) {
	D3D12_HEAP_PROPERTIES properties = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
	D3D12_RESOURCE_DESC   desc {};
	desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
	desc.Width            = bytes;
	desc.Height           = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels        = 1;
	desc.SampleDesc.Count = 1;
	desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	ComPtr<ID3D12Resource> buffer;
	device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
	                                IID_PPV_ARGS(&buffer));
	return buffer;
}

TEST(Census, HeapsAndCommittedResourcesCountWhileTheyLive) {
	constexpr uint64_t kHeap   = 4 * kMegabyte;
	constexpr uint64_t kBuffer = 1 * kMegabyte;
	Warp               warp;
	if (!makeWarp(warp)) {
		GTEST_SKIP() << "no WARP device";
	}
	auto     ledger   = std::make_shared<detail::Ledger>();
	Category category = ledger->addCategory("buffers");
	ComPtr<ID3D12Heap>     heap   = makeHeap(warp.device.Get(), kHeap);
	ComPtr<ID3D12Resource> buffer = makeBuffer(warp.device.Get(), kBuffer);
	ASSERT_TRUE(heap && buffer);
	ASSERT_EQ(detail::trackHeap(ledger, heap.Get(), category), Status::Ok);
	ASSERT_EQ(detail::trackResource(ledger, warp.device.Get(), buffer.Get(), category), Status::Ok);
	CategoryUse use = ledger->uses()[0];
	EXPECT_EQ(use.cardBytes, kHeap + kBuffer);
	EXPECT_EQ(use.cardCount, 2u);
	buffer.Reset();
	use = ledger->uses()[0];
	EXPECT_EQ(use.cardBytes, kHeap) << "the buffer is uncounted when it goes, the heap still held is not";
	EXPECT_EQ(use.cardCount, 1u);
	ASSERT_EQ(detail::trackHeap(ledger, heap.Get(), category), Status::Ok);
	EXPECT_EQ(ledger->uses()[0].cardCount, 1u) << "tracking again replaces the earlier charge";
	heap.Reset();
	use = ledger->uses()[0];
	EXPECT_EQ(use.cardBytes, 0u) << "the heap is uncounted with its last reference";
	EXPECT_EQ(use.cardCount, 0u);
}

TEST(Census, ACategoryThatWasNotAddedIsRefused) {
	auto ledger = std::make_shared<detail::Ledger>();
	EXPECT_EQ(ledger->addCategory("first"), Category {0});
	EXPECT_TRUE(ledger->known(Category {0}));
	EXPECT_FALSE(ledger->known(Category {1}));
	EXPECT_EQ(detail::trackHeap(ledger, nullptr, Category {0}), Status::InvalidArgument);
}

TEST(Census, MemoryThePcHoldsIsCountedApartFromTheCards) {
	EXPECT_TRUE(detail::onCard(heapProperties(D3D12_HEAP_TYPE_DEFAULT)));
	EXPECT_TRUE(detail::onCard(heapProperties(D3D12_HEAP_TYPE_CUSTOM, D3D12_MEMORY_POOL_L1)));
	EXPECT_FALSE(detail::onCard(heapProperties(D3D12_HEAP_TYPE_CUSTOM, D3D12_MEMORY_POOL_L0)));
	EXPECT_FALSE(detail::onCard(heapProperties(D3D12_HEAP_TYPE_UPLOAD)));
	EXPECT_FALSE(detail::onCard(heapProperties(D3D12_HEAP_TYPE_READBACK)));
}

TEST(Census, TheLineSumsTheCardAndSaysWhatIsHeldTwice) {
	CensusSnapshot snapshot;
	snapshot.categories = {
	    CategoryUse {"card view", 640 * kMegabyte, 10, 0, 0},
	    CategoryUse {"textures", 2048 * kMegabyte, 32, 256 * kMegabyte, 4},
	};
	snapshot.system              = SystemMemory {true, false, 3000 * kMegabyte, 4000 * kMegabyte, 300 * kMegabyte, 6000 * kMegabyte};
	snapshot.homes.ram           = 10;
	snapshot.homes.ramTiled      = 3;
	snapshot.ramAndTileBytes     = 3 * kPageBytes;
	snapshot.tileAndTextureBytes = 20 * kMegabyte;
	std::string line             = Census::format(snapshot);
	EXPECT_NE(line.find("card 2688 MB in 42 (card view 640 MB in 10, textures 2048 MB in 32)"), std::string::npos) << line;
	EXPECT_NE(line.find("shared 256 MB in 4 (textures 256 MB in 4)"), std::string::npos) << line;
	EXPECT_NE(line.find("system: card 3000 MB of 4000 MB budget, shared 300 MB of 6000 MB budget"), std::string::npos)
	    << line;
	EXPECT_NE(line.find("ram 10 (3 with a tile)"), std::string::npos) << line;
	EXPECT_NE(line.find("0 MB in RAM and a tile, 20 MB in a tile and a texture"), std::string::npos) << line;
	snapshot.system.counted = true;
	line                    = Census::format(snapshot);
	EXPECT_NE(line.find("system (heap sizes, our counts): card 3000 MB of 4000 MB"), std::string::npos)
	    << line;
}

TEST(Census, EveryContextCountsTheCardViewAndAsksTheSystem) {
	Warp warp;
	ASSERT_TRUE(makeWarp(warp));
	Status                   status  = Status::Ok;
	std::unique_ptr<Context> context = Context::create(Config {}, Callbacks {}, &status);
	ASSERT_NE(context, nullptr);
	Census& census = context->census();
	census.setAdapter(warp.adapter.Get());
	Category               buffers = census.addCategory("buffers");
	ComPtr<ID3D12Resource> buffer  = makeBuffer(warp.device.Get(), kMegabyte);
	ASSERT_EQ(census.track(warp.device.Get(), buffer.Get(), buffers), Status::Ok);
	CensusSnapshot snapshot = census.snapshot();
	ASSERT_EQ(snapshot.categories.size(), 2u);
	EXPECT_EQ(snapshot.categories[static_cast<uint32_t>(kCardViewCategory)].name, "card view");
	EXPECT_EQ(snapshot.categories[1].cardBytes, kMegabyte);
	EXPECT_TRUE(snapshot.system.known);
	EXPECT_EQ(snapshot.cardView.tiles, 0u) << "no card view without a device";
	EXPECT_FALSE(Census::format(snapshot).empty());
}

} // namespace
