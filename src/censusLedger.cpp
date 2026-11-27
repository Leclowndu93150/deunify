#include "censusLedger.hpp"

#include "hostError.hpp"

#include <atomic>

namespace deunify::detail {

namespace {

constexpr D3D12_HEAP_TYPE kHeapTypeGpuUpload = static_cast<D3D12_HEAP_TYPE>(5);
constexpr UINT64          kUnknownSize       = ~0ull;

constexpr GUID kChargeKey = {0x6f0b2d4e, 0x93a1, 0x4c57, {0x8e, 0x2d, 0x51, 0xb7, 0xc4, 0x09, 0xa3, 0x6e}};

class Charge final : public IUnknown {
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

	ULONG STDMETHODCALLTYPE AddRef() override { return m_references.fetch_add(1, std::memory_order_relaxed) + 1; }

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

Status attach(ID3D12Object* object, const std::shared_ptr<Ledger>& ledger, Category category, bool card,
              uint64_t bytes) {
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
