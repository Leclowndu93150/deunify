// The D3D12 side: reserved buffers, tile heaps, UpdateTileMappings, copies on the given queue,
// a fence, and the census's way of counting D3D12 objects and asking DXGI.

#pragma once

#include "censusLedger.hpp"
#include "gpuBackend.hpp"

#include <d3d12.h>
#include <dxgi1_4.h>

#include <memory>

namespace deunify::detail {

/// Whether memory with `properties` lies in the card's own memory (default and GPU upload heaps,
/// custom heaps in pool L1) rather than in system memory the card reads.
bool onCard(const D3D12_HEAP_PROPERTIES& properties);

/// Counts `heap` in `category` until the runtime destroys it.
Status trackHeap(const std::shared_ptr<Ledger>& ledger, ID3D12Heap* heap, Category category);

/// Counts committed resource `resource` in `category` until the runtime destroys it. A reserved
/// resource (no heap properties) is not counted.
Status trackResource(const std::shared_ptr<Ledger>& ledger, ID3D12Device* device,
                     ID3D12Resource* resource, Category category);

} // namespace deunify::detail

namespace deunify::gpu {

/// IDXGIAdapter3::QueryVideoMemoryInfo, on an adapter we hold a reference to.
std::shared_ptr<MemoryReport> dxgiReport(IDXGIAdapter3* adapter);

std::unique_ptr<Backend> makeD3d12Backend(ID3D12Device* device, ID3D12CommandQueue* queue,
                                          std::shared_ptr<detail::Ledger> ledger);

} // namespace deunify::gpu
