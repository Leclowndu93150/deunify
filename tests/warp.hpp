// A WARP device and queue for tests that need a card, with copies in and out of buffers.

#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace deunify::test {

using Microsoft::WRL::ComPtr;

inline D3D12_RESOURCE_DESC bufferDesc(uint64_t bytes) {
	D3D12_RESOURCE_DESC desc {};
	desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
	desc.Width            = bytes;
	desc.Height           = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels        = 1;
	desc.SampleDesc.Count = 1;
	desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	return desc;
}

class Warp {
public:
	Warp() {
		ComPtr<IDXGIFactory4> factory;
		ComPtr<IDXGIAdapter>  adapter;
		if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
		    FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))) ||
		    FAILED(
		        D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
			device.Reset();
			return;
		}
		D3D12_COMMAND_QUEUE_DESC desc {};
		desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue));
		device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
		device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
		device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
		                          IID_PPV_ARGS(&list));
		list->Close();
	}

	bool tiled() const {
		D3D12_FEATURE_DATA_D3D12_OPTIONS options {};
		return device &&
		       SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options,
		                                             sizeof(options))) &&
		       options.TiledResourcesTier >= D3D12_TILED_RESOURCES_TIER_2;
	}

	void drain() {
		queue->Signal(fence.Get(), ++fenced);
		fence->SetEventOnCompletion(fenced, nullptr);
	}

	ComPtr<ID3D12Resource> hostBuffer(D3D12_HEAP_TYPE type, uint64_t bytes) {
		D3D12_HEAP_PROPERTIES properties {};
		properties.Type                = type;
		const D3D12_RESOURCE_DESC desc = bufferDesc(bytes);
		ComPtr<ID3D12Resource>    buffer;
		device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc,
		                                type == D3D12_HEAP_TYPE_UPLOAD
		                                    ? D3D12_RESOURCE_STATE_GENERIC_READ
		                                    : D3D12_RESOURCE_STATE_COPY_DEST,
		                                nullptr, IID_PPV_ARGS(&buffer));
		return buffer;
	}

	void copy(ID3D12Resource* to, uint64_t toOffset, ID3D12Resource* from, uint64_t fromOffset,
	          uint64_t bytes) {
		allocator->Reset();
		list->Reset(allocator.Get(), nullptr);
		list->CopyBufferRegion(to, toOffset, from, fromOffset, bytes);
		list->Close();
		ID3D12CommandList* lists[] = {list.Get()};
		queue->ExecuteCommandLists(1, lists);
		drain();
	}

	void store(ID3D12Resource* buffer, uint64_t offset, const std::vector<uint8_t>& bytes) {
		ComPtr<ID3D12Resource> upload = hostBuffer(D3D12_HEAP_TYPE_UPLOAD, bytes.size());
		void*                  data   = nullptr;
		upload->Map(0, nullptr, &data);
		std::memcpy(data, bytes.data(), bytes.size());
		upload->Unmap(0, nullptr);
		copy(buffer, offset, upload.Get(), 0, bytes.size());
	}

	std::vector<uint8_t> fetch(ID3D12Resource* buffer, uint64_t offset, uint64_t bytes) {
		ComPtr<ID3D12Resource> back = hostBuffer(D3D12_HEAP_TYPE_READBACK, bytes);
		copy(back.Get(), 0, buffer, offset, bytes);
		void* data = nullptr;
		back->Map(0, nullptr, &data);
		std::vector<uint8_t> got(static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + bytes);
		back->Unmap(0, nullptr);
		return got;
	}

	ComPtr<ID3D12Device>              device;
	ComPtr<ID3D12CommandQueue>        queue;
	ComPtr<ID3D12Fence>               fence;
	ComPtr<ID3D12CommandAllocator>    allocator;
	ComPtr<ID3D12GraphicsCommandList> list;
	uint64_t                          fenced = 0;
};

} // namespace deunify::test
