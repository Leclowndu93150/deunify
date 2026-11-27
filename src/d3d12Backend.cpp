#include "d3d12Backend.hpp"

#include "hostError.hpp"

#include <atomic>
#include <wrl/client.h>

namespace deunify::detail {

namespace {

constexpr D3D12_HEAP_TYPE kHeapTypeGpuUpload = static_cast<D3D12_HEAP_TYPE>(5);
constexpr UINT64          kUnknownSize       = ~0ull;

constexpr GUID kChargeKey = {
    0x6f0b2d4e, 0x93a1, 0x4c57, {0x8e, 0x2d, 0x51, 0xb7, 0xc4, 0x09, 0xa3, 0x6e}};

class Charge final: public IUnknown {
public:
	Charge(std::shared_ptr<Ledger> ledger, Category category, bool card, uint64_t bytes)
	    : m_ledger(std::move(ledger)), m_category(category), m_card(card), m_bytes(bytes) {}

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
		if (out == nullptr) {
			return E_POINTER;
		}
		if (iid != __uuidof(IUnknown)) {
			*out = nullptr;
			return E_NOINTERFACE;
		}
		AddRef();
		*out = static_cast<IUnknown*>(this);
		return S_OK;
	}

	ULONG STDMETHODCALLTYPE AddRef() override {
		return m_references.fetch_add(1, std::memory_order_relaxed) + 1;
	}

	ULONG STDMETHODCALLTYPE Release() override {
		ULONG left = m_references.fetch_sub(1, std::memory_order_acq_rel) - 1;
		if (left == 0) {
			m_ledger->uncount(m_category, m_card, m_bytes);
			delete this;
		}
		return left;
	}

private:
	std::atomic<ULONG>      m_references {1};
	std::shared_ptr<Ledger> m_ledger;
	Category                m_category;
	bool                    m_card;
	uint64_t                m_bytes;
};

Status attach(ID3D12Object* object, const std::shared_ptr<Ledger>& ledger, Category category,
              bool card, uint64_t bytes) {
	ledger->count(category, card, bytes);
	Charge* charge = new Charge(ledger, category, card, bytes);
	HRESULT result = object->SetPrivateDataInterface(kChargeKey, charge);
	charge->Release();
	if (FAILED(result)) {
		setLastHostError(static_cast<uint32_t>(result));
		return Status::HostFailure;
	}
	return Status::Ok;
}

} // namespace

bool onCard(const D3D12_HEAP_PROPERTIES& properties) {
	switch (properties.Type) {
		case D3D12_HEAP_TYPE_DEFAULT: return true;
		case D3D12_HEAP_TYPE_CUSTOM: return properties.MemoryPoolPreference == D3D12_MEMORY_POOL_L1;
		default: return properties.Type == kHeapTypeGpuUpload;
	}
}

Status trackHeap(const std::shared_ptr<Ledger>& ledger, ID3D12Heap* heap, Category category) {
	if (heap == nullptr || !ledger->known(category)) {
		return Status::InvalidArgument;
	}
	D3D12_HEAP_DESC desc = heap->GetDesc();
	return attach(heap, ledger, category, onCard(desc.Properties), desc.SizeInBytes);
}

Status trackResource(const std::shared_ptr<Ledger>& ledger, ID3D12Device* device,
                     ID3D12Resource* resource, Category category) {
	if (device == nullptr || resource == nullptr || !ledger->known(category)) {
		return Status::InvalidArgument;
	}
	D3D12_HEAP_PROPERTIES properties {};
	if (FAILED(resource->GetHeapProperties(&properties, nullptr))) {
		return Status::Ok;
	}
	D3D12_RESOURCE_DESC desc = resource->GetDesc();
	UINT64              size = device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
	if (size == kUnknownSize) {
		return Status::Ok;
	}
	return attach(resource, ledger, category, onCard(properties), size);
}

} // namespace deunify::detail

namespace deunify::gpu {

namespace {

using Microsoft::WRL::ComPtr;

constexpr uint64_t kTileBytes      = kPageBytes;
constexpr uint64_t kDeviceRemoved  = ~0ull;
constexpr UINT     kFirstNode      = 0;
constexpr wchar_t  kSegmentName[]  = L"deunify card view segment";
constexpr wchar_t  kHeapName[]     = L"deunify card view tiles";
constexpr wchar_t  kUploadName[]   = L"deunify card view upload";
constexpr wchar_t  kReadbackName[] = L"deunify card view read-back";

Status statusOf(HRESULT result) {
	return result == E_OUTOFMEMORY ? Status::OutOfMemory : Status::HostFailure;
}

Status checked(HRESULT result) {
	return FAILED(result) ? statusOf(result) : Status::Ok;
}

D3D12_RESOURCE_DESC bufferDesc(uint64_t bytes, D3D12_RESOURCE_FLAGS flags) {
	D3D12_RESOURCE_DESC desc {};
	desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
	desc.Width            = bytes;
	desc.Height           = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels        = 1;
	desc.SampleDesc.Count = 1;
	desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	desc.Flags            = flags;
	return desc;
}

bool queryGroup(IDXGIAdapter3* adapter, DXGI_MEMORY_SEGMENT_GROUP group, uint64_t& usage,
                uint64_t& budget) {
	DXGI_QUERY_VIDEO_MEMORY_INFO info {};
	if (FAILED(adapter->QueryVideoMemoryInfo(kFirstNode, group, &info))) {
		return false;
	}
	usage  = info.CurrentUsage;
	budget = info.Budget;
	return true;
}

class DxgiReport final: public MemoryReport {
public:
	explicit DxgiReport(IDXGIAdapter3* adapter): m_adapter(adapter) {}

	SystemMemory systemMemory(const std::vector<CategoryUse>&) const override {
		SystemMemory system;
		system.known = queryGroup(m_adapter.Get(), DXGI_MEMORY_SEGMENT_GROUP_LOCAL,
		                          system.localUsage, system.localBudget) &&
		               queryGroup(m_adapter.Get(), DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL,
		                          system.nonLocalUsage, system.nonLocalBudget);
		return system;
	}

private:
	ComPtr<IDXGIAdapter3> m_adapter;
};

class D3d12Region final: public Region {
public:
	BufferView view(uint64_t offset, uint64_t bytes) const override {
		BufferView view;
		view.resource = resource.Get();
		view.offset   = offset;
		view.bytes    = bytes;
		return view;
	}

	ComPtr<ID3D12Resource> resource;
};

class D3d12Block final: public Block {
public:
	ComPtr<ID3D12Heap> heap;
};

class D3d12HostBuffer final: public HostBuffer {
public:
	ComPtr<ID3D12Resource> resource;
};

class D3d12Commands final: public Commands {
public:
	ComPtr<ID3D12CommandAllocator>    allocator;
	ComPtr<ID3D12GraphicsCommandList> list;
};

ID3D12Resource* resourceOf(const Region& region) {
	return static_cast<const D3d12Region&>(region).resource.Get();
}

ID3D12Resource* resourceOf(const HostBuffer& buffer) {
	return static_cast<const D3d12HostBuffer&>(buffer).resource.Get();
}

ID3D12GraphicsCommandList* listOf(Commands& commands) {
	return static_cast<D3d12Commands&>(commands).list.Get();
}

class TileRanges {
public:
	explicit TileRanges(const BindGroup& group) {
		for (const Run& run: group.runs) {
			D3D12_TILED_RESOURCE_COORDINATE coordinate {};
			coordinate.X = static_cast<UINT>(run.slot);
			D3D12_TILE_REGION_SIZE size {};
			size.NumTiles = static_cast<UINT>(run.count);
			m_coordinates.push_back(coordinate);
			m_sizes.push_back(size);
			m_flags.push_back(run.tile ? D3D12_TILE_RANGE_FLAG_NONE : D3D12_TILE_RANGE_FLAG_NULL);
			m_starts.push_back(run.tile.value_or(0));
			m_counts.push_back(static_cast<UINT>(run.count));
		}
	}

	void send(ID3D12CommandQueue* queue, ID3D12Resource* buffer, ID3D12Heap* heap) const {
		const UINT ranges = static_cast<UINT>(m_coordinates.size());
		queue->UpdateTileMappings(buffer, ranges, m_coordinates.data(), m_sizes.data(), heap,
		                          ranges, m_flags.data(), m_starts.data(), m_counts.data(),
		                          D3D12_TILE_MAPPING_FLAG_NONE);
	}

private:
	std::vector<D3D12_TILED_RESOURCE_COORDINATE> m_coordinates;
	std::vector<D3D12_TILE_REGION_SIZE>          m_sizes;
	std::vector<D3D12_TILE_RANGE_FLAGS>          m_flags;
	std::vector<UINT>                            m_starts;
	std::vector<UINT>                            m_counts;
};

class D3d12Backend final: public Backend {
public:
	D3d12Backend(ID3D12Device* device, ID3D12CommandQueue* queue,
	             std::shared_ptr<detail::Ledger> ledger)
	    : m_device(device), m_queue(queue), m_ledger(std::move(ledger)) {}

	Status start() override {
		D3D12_FEATURE_DATA_D3D12_OPTIONS options {};
		if (FAILED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options,
		                                         sizeof(options))) ||
		    options.TiledResourcesTier < D3D12_TILED_RESOURCES_TIER_2) {
			return Status::NotSupported;
		}
		return checked(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));
	}

	SegmentShape segmentShape() const override { return SegmentShape {}; }

	bool flatSpans() const override { return false; }

	bool unboundReadsZero() const override { return true; }

	Status makeRegion(uint64_t bytes, std::unique_ptr<Region>* region) override {
		auto                      made = std::make_unique<D3d12Region>();
		const D3D12_RESOURCE_DESC desc =
		    bufferDesc(bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
		const HRESULT result = m_device->CreateReservedResource(
		    &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&made->resource));
		if (FAILED(result)) {
			return statusOf(result);
		}
		made->resource->SetName(kSegmentName);
		*region = std::move(made);
		return Status::Ok;
	}

	Status makeBlock(std::shared_ptr<Block>* block) override {
		auto            made = std::make_shared<D3d12Block>();
		D3D12_HEAP_DESC desc {};
		desc.SizeInBytes     = uint64_t {kHeapTiles} * kTileBytes;
		desc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
		desc.Alignment       = kTileBytes;
		desc.Flags           = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
		const HRESULT result = m_device->CreateHeap(&desc, IID_PPV_ARGS(&made->heap));
		if (FAILED(result)) {
			return statusOf(result);
		}
		made->heap->SetName(kHeapName);
		static_cast<void>(detail::trackHeap(m_ledger, made->heap.Get(), kCardViewCategory));
		*block = std::move(made);
		return Status::Ok;
	}

	Status makeUpload(uint64_t bytes, std::unique_ptr<HostBuffer>* buffer) override {
		return makeHostBuffer(D3D12_HEAP_TYPE_UPLOAD, bytes, buffer);
	}

	Status makeReadback(uint64_t bytes, std::unique_ptr<HostBuffer>* buffer) override {
		return makeHostBuffer(D3D12_HEAP_TYPE_READBACK, bytes, buffer);
	}

	Status makeCommands(std::unique_ptr<Commands>* commands) override {
		auto    made   = std::make_unique<D3d12Commands>();
		HRESULT result = m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
		                                                  IID_PPV_ARGS(&made->allocator));
		if (SUCCEEDED(result)) {
			result = m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
			                                     made->allocator.Get(), nullptr,
			                                     IID_PPV_ARGS(&made->list));
		}
		if (SUCCEEDED(result)) {
			result = made->list->Close();
		}
		if (FAILED(result)) {
			return statusOf(result);
		}
		*commands = std::move(made);
		return Status::Ok;
	}

	void bind(Region& region, std::span<const BindGroup> groups,
	          std::span<Block* const> blocks) override {
		for (const BindGroup& group: groups) {
			ID3D12Heap* heap =
			    group.block ? static_cast<D3d12Block*>(blocks[*group.block])->heap.Get() : nullptr;
			TileRanges(group).send(m_queue.Get(), resourceOf(region), heap);
		}
	}

	uint64_t submitBinds() override { return signal(); }

	Status begin(Commands& commands) override {
		auto&   set    = static_cast<D3d12Commands&>(commands);
		HRESULT result = set.allocator->Reset();
		if (SUCCEEDED(result)) {
			result = set.list->Reset(set.allocator.Get(), nullptr);
		}
		return checked(result);
	}

	void copyUp(Commands& commands, const Region& to, uint64_t toOffset, const HostBuffer& from,
	            uint64_t fromOffset, uint64_t bytes) override {
		listOf(commands)->CopyBufferRegion(resourceOf(to), toOffset, resourceOf(from), fromOffset,
		                                   bytes);
	}

	void copyBack(Commands& commands, const HostBuffer& to, const Region& from, uint64_t fromOffset,
	              uint64_t bytes) override {
		listOf(commands)->CopyBufferRegion(resourceOf(to), 0, resourceOf(from), fromOffset, bytes);
	}

	Status end(Commands& commands) override { return checked(listOf(commands)->Close()); }

	uint64_t submit(Commands& commands) override {
		ID3D12CommandList* lists[] = {listOf(commands)};
		m_queue->ExecuteCommandLists(1, lists);
		return signal();
	}

	uint64_t signal() override {
		++m_fenced;
		m_queue->Signal(m_fence.Get(), m_fenced);
		return m_fenced;
	}

	void fenceTitleWork() override {}

	uint64_t completed() const override { return m_fence->GetCompletedValue(); }

	bool wait(uint64_t value) const override {
		if (m_fence->GetCompletedValue() < value &&
		    FAILED(m_fence->SetEventOnCompletion(value, nullptr))) {
			return false;
		}
		return m_fence->GetCompletedValue() != kDeviceRemoved;
	}

	std::optional<bool> memoryTypeOnCard(uint32_t) const override { return std::nullopt; }

	SystemMemory systemMemory(const std::vector<CategoryUse>&) const override { return {}; }

private:
	Status makeHostBuffer(D3D12_HEAP_TYPE type, uint64_t bytes,
	                      std::unique_ptr<HostBuffer>* buffer) {
		auto                  made = std::make_unique<D3d12HostBuffer>();
		D3D12_HEAP_PROPERTIES properties {};
		properties.Type                   = type;
		const D3D12_RESOURCE_DESC   desc  = bufferDesc(bytes, D3D12_RESOURCE_FLAG_NONE);
		const D3D12_RESOURCE_STATES state = type == D3D12_HEAP_TYPE_UPLOAD
		                                        ? D3D12_RESOURCE_STATE_GENERIC_READ
		                                        : D3D12_RESOURCE_STATE_COPY_DEST;
		HRESULT                     result =
		    m_device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc, state,
		                                      nullptr, IID_PPV_ARGS(&made->resource));
		void* data = nullptr;
		if (SUCCEEDED(result)) {
			result = made->resource->Map(0, nullptr, &data);
		}
		if (FAILED(result)) {
			return statusOf(result);
		}
		made->resource->SetName(type == D3D12_HEAP_TYPE_UPLOAD ? kUploadName : kReadbackName);
		made->data  = static_cast<uint8_t*>(data);
		made->bytes = bytes;
		static_cast<void>(detail::trackResource(m_ledger, m_device.Get(), made->resource.Get(),
		                                        kCardViewCategory));
		*buffer = std::move(made);
		return Status::Ok;
	}

	ComPtr<ID3D12Device>            m_device;
	ComPtr<ID3D12CommandQueue>      m_queue;
	std::shared_ptr<detail::Ledger> m_ledger;
	ComPtr<ID3D12Fence>             m_fence;
	uint64_t                        m_fenced = 0;
};

} // namespace

std::shared_ptr<MemoryReport> dxgiReport(IDXGIAdapter3* adapter) {
	return std::make_shared<DxgiReport>(adapter);
}

std::unique_ptr<Backend> makeD3d12Backend(ID3D12Device* device, ID3D12CommandQueue* queue,
                                          std::shared_ptr<detail::Ledger> ledger) {
	return std::make_unique<D3d12Backend>(device, queue, std::move(ledger));
}

} // namespace deunify::gpu
