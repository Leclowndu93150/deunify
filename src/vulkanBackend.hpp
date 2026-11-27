// The Vulkan side: sparse buffers, VkDeviceMemory blocks bound with vkQueueBindSparse, copies on
// the transfer queue through host-visible staging, a timeline semaphore (or fences), and the
// census's numbers from VK_EXT_memory_budget.

#pragma once

#include "censusLedger.hpp"
#include "gpuBackend.hpp"

#include <memory>

namespace deunify::gpu {

std::unique_ptr<Backend> makeVulkanBackend(const VulkanSetup& setup, const Callbacks& callbacks,
                                           std::shared_ptr<detail::Ledger> ledger);

} // namespace deunify::gpu
