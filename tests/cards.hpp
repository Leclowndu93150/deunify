// Every card the card view tests run on: WARP for D3D12, and a real Vulkan device once with a
// timeline semaphore and once with fences only.

#pragma once

#include "testCard.hpp"
#include "vulkanCard.hpp"

#include <gtest/gtest.h>
#include <memory>
#include <string>

namespace deunify::test {

enum class Api {
	D3d12,
	Vulkan,
	VulkanFences,
};

inline const Api kApis[] = {Api::D3d12, Api::Vulkan, Api::VulkanFences};

inline std::string apiName(const ::testing::TestParamInfo<Api>& info) {
	switch (info.param) {
		case Api::D3d12: return "D3D12";
		case Api::Vulkan: return "Vulkan";
		case Api::VulkanFences: return "VulkanFences";
	}
	return "Unknown";
}

inline std::unique_ptr<TestCard> makeTestCard(Api api) {
	switch (api) {
		case Api::D3d12: return std::make_unique<WarpCard>();
		case Api::Vulkan: return std::make_unique<VulkanCard>(true);
		case Api::VulkanFences: return std::make_unique<VulkanCard>(false);
	}
	return nullptr;
}

} // namespace deunify::test
