// A real Vulkan device for the card view tests, made through vulkan-1.dll at run time (the
// library never links the loader, and neither do the tests). One queue does sparse binding and
// copies. With DEUNIFY_VULKAN_VALIDATION=1 in the environment the Khronos validation layer is
// on and anything it reports fails the test.

#pragma once

#include <windows.h>

#include "testCard.hpp"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <gtest/gtest.h>
#include <string>
#include <vector>
#include <vulkan/vulkan_core.h>

namespace deunify::test {

inline std::atomic<int> g_vulkanComplaints {0};

inline VKAPI_ATTR VkBool32 VKAPI_CALL
vulkanComplaint(VkDebugUtilsMessageSeverityFlagBitsEXT, VkDebugUtilsMessageTypeFlagsEXT,
                const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
	++g_vulkanComplaints;
	ADD_FAILURE() << "Vulkan validation: " << data->pMessage;
	return VK_FALSE;
}

class VulkanCard final: public TestCard {
public:
	explicit VulkanCard(bool timeline): m_timeline(timeline) {
		m_loader = LoadLibraryW(L"vulkan-1.dll");
		if (m_loader == nullptr) {
			m_missing = "no Vulkan loader (vulkan-1.dll)";
			return;
		}
		m_getInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
		    reinterpret_cast<void*>(GetProcAddress(m_loader, "vkGetInstanceProcAddr")));
		if (m_getInstanceProcAddr == nullptr || !makeInstance()) {
			m_missing = "no Vulkan 1.2 instance";
			return;
		}
		if (!pickDevice()) {
			m_missing = "no Vulkan device offers sparseBinding, sparseResidencyBuffer and "
			            "bufferDeviceAddress with a sparse binding queue";
			return;
		}
		if (!makeDevice()) {
			m_missing = "the Vulkan device could not be created";
		}
	}

	~VulkanCard() override {
		if (m_device != VK_NULL_HANDLE) {
			m_deviceWaitIdle(m_device);
			m_destroyCommandPool(m_device, m_pool, nullptr);
			m_destroyFence(m_device, m_fence, nullptr);
			m_destroyDevice(m_device, nullptr);
		}
		if (m_messenger != VK_NULL_HANDLE) {
			instanceFn<PFN_vkDestroyDebugUtilsMessengerEXT>("vkDestroyDebugUtilsMessengerEXT")(
			    m_instance, m_messenger, nullptr);
		}
		if (m_instance != VK_NULL_HANDLE) {
			instanceFn<PFN_vkDestroyInstance>("vkDestroyInstance")(m_instance, nullptr);
		}
		if (m_loader != nullptr) {
			FreeLibrary(m_loader);
		}
	}

	std::string missing() const override { return m_missing; }

	Config config() const override {
		Config config;
		config.vulkan.instance            = m_instance;
		config.vulkan.physicalDevice      = m_physical;
		config.vulkan.device              = m_device;
		config.vulkan.getInstanceProcAddr = m_getInstanceProcAddr;
		config.vulkan.sparseQueue         = m_queue;
		config.vulkan.sparseFamily        = m_family;
		config.vulkan.transferQueue       = m_queue;
		config.vulkan.transferFamily      = m_family;
		config.vulkan.timelineSemaphores  = m_timeline;
		return config;
	}

	void store(const BufferView& view, uint64_t at, const std::vector<uint8_t>& bytes) override {
		Staging staging = makeStaging(bytes.size());
		std::memcpy(staging.data, bytes.data(), bytes.size());
		copy(staging.buffer, 0, view.buffer, at, bytes.size());
		dropStaging(staging);
	}

	std::vector<uint8_t> fetch(const BufferView& view, uint64_t at, uint64_t bytes) override {
		Staging staging = makeStaging(bytes);
		copy(view.buffer, at, staging.buffer, 0, bytes);
		std::vector<uint8_t> got(staging.data, staging.data + bytes);
		dropStaging(staging);
		return got;
	}

	void drain() override { m_deviceWaitIdle(m_device); }

private:
	struct Staging {
		VkBuffer       buffer = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		uint8_t*       data   = nullptr;
	};

	template <typename Fn>
	Fn instanceFn(const char* name) const {
		return reinterpret_cast<Fn>(m_getInstanceProcAddr(m_instance, name));
	}

	template <typename Fn>
	Fn deviceFn(const char* name) const {
		return reinterpret_cast<Fn>(m_getDeviceProcAddr(m_device, name));
	}

	static bool validationWanted() {
		char*  value = nullptr;
		size_t size  = 0;
		_dupenv_s(&value, &size, "DEUNIFY_VULKAN_VALIDATION");
		const bool wanted = value != nullptr && std::strcmp(value, "1") == 0;
		std::free(value);
		return wanted;
	}

	bool makeInstance() {
		VkApplicationInfo app {};
		app.sType                       = VK_STRUCTURE_TYPE_APPLICATION_INFO;
		app.pApplicationName            = "deunify tests";
		app.apiVersion                  = VK_API_VERSION_1_2;
		const char*          layer      = "VK_LAYER_KHRONOS_validation";
		const char*          extension  = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
		const bool           validation = validationWanted();
		VkInstanceCreateInfo info {};
		info.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
		info.pApplicationInfo        = &app;
		info.enabledLayerCount       = validation ? 1 : 0;
		info.ppEnabledLayerNames     = &layer;
		info.enabledExtensionCount   = validation ? 1 : 0;
		info.ppEnabledExtensionNames = &extension;
		auto create                  = instanceFn<PFN_vkCreateInstance>("vkCreateInstance");
		if (create == nullptr || create(&info, nullptr, &m_instance) != VK_SUCCESS) {
			m_instance = VK_NULL_HANDLE;
			return false;
		}
		if (validation) {
			VkDebugUtilsMessengerCreateInfoEXT messenger {};
			messenger.sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
			messenger.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
			                            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
			messenger.messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
			messenger.pfnUserCallback = vulkanComplaint;
			instanceFn<PFN_vkCreateDebugUtilsMessengerEXT>("vkCreateDebugUtilsMessengerEXT")(
			    m_instance, &messenger, nullptr, &m_messenger);
		}
		return true;
	}

	bool pickDevice() {
		auto enumerate = instanceFn<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices");
		auto properties =
		    instanceFn<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties");
		auto features =
		    instanceFn<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2");
		auto families = instanceFn<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
		    "vkGetPhysicalDeviceQueueFamilyProperties");
		uint32_t count = 0;
		enumerate(m_instance, &count, nullptr);
		std::vector<VkPhysicalDevice> devices(count);
		enumerate(m_instance, &count, devices.data());
		for (VkPhysicalDevice device: devices) {
			VkPhysicalDeviceProperties props {};
			properties(device, &props);
			VkPhysicalDeviceVulkan12Features features12 {};
			features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
			VkPhysicalDeviceFeatures2 all {};
			all.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
			all.pNext = &features12;
			features(device, &all);
			if (props.apiVersion < VK_API_VERSION_1_2 || !all.features.sparseBinding ||
			    !all.features.sparseResidencyBuffer || !features12.bufferDeviceAddress) {
				continue;
			}
			uint32_t familyCount = 0;
			families(device, &familyCount, nullptr);
			std::vector<VkQueueFamilyProperties> found(familyCount);
			families(device, &familyCount, found.data());
			for (uint32_t family = 0; family < familyCount; ++family) {
				const VkQueueFlags flags = found[family].queueFlags;
				if ((flags & VK_QUEUE_SPARSE_BINDING_BIT) != 0 &&
				    (flags & VK_QUEUE_GRAPHICS_BIT) != 0) {
					m_physical = device;
					m_family   = family;
					m_timeline = m_timeline && features12.timelineSemaphore;
					return true;
				}
			}
		}
		return false;
	}

	bool makeDevice() {
		const float             priority = 1.0f;
		VkDeviceQueueCreateInfo queue {};
		queue.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
		queue.queueFamilyIndex = m_family;
		queue.queueCount       = 1;
		queue.pQueuePriorities = &priority;
		VkPhysicalDeviceVulkan12Features features12 {};
		features12.sType               = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
		features12.bufferDeviceAddress = VK_TRUE;
		features12.timelineSemaphore   = m_timeline ? VK_TRUE : VK_FALSE;
		VkPhysicalDeviceFeatures2 features {};
		features.sType                          = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
		features.pNext                          = &features12;
		features.features.sparseBinding         = VK_TRUE;
		features.features.sparseResidencyBuffer = VK_TRUE;
		VkDeviceCreateInfo info {};
		info.sType                = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
		info.pNext                = &features;
		info.queueCreateInfoCount = 1;
		info.pQueueCreateInfos    = &queue;
		auto create               = instanceFn<PFN_vkCreateDevice>("vkCreateDevice");
		if (create(m_physical, &info, nullptr, &m_device) != VK_SUCCESS) {
			m_device = VK_NULL_HANDLE;
			return false;
		}
		m_getDeviceProcAddr = instanceFn<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
		m_destroyDevice     = deviceFn<PFN_vkDestroyDevice>("vkDestroyDevice");
		m_deviceWaitIdle    = deviceFn<PFN_vkDeviceWaitIdle>("vkDeviceWaitIdle");
		m_createBuffer      = deviceFn<PFN_vkCreateBuffer>("vkCreateBuffer");
		m_destroyBuffer     = deviceFn<PFN_vkDestroyBuffer>("vkDestroyBuffer");
		m_requirements =
		    deviceFn<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements");
		m_allocateMemory     = deviceFn<PFN_vkAllocateMemory>("vkAllocateMemory");
		m_freeMemory         = deviceFn<PFN_vkFreeMemory>("vkFreeMemory");
		m_bindBufferMemory   = deviceFn<PFN_vkBindBufferMemory>("vkBindBufferMemory");
		m_mapMemory          = deviceFn<PFN_vkMapMemory>("vkMapMemory");
		m_destroyCommandPool = deviceFn<PFN_vkDestroyCommandPool>("vkDestroyCommandPool");
		m_resetCommandPool   = deviceFn<PFN_vkResetCommandPool>("vkResetCommandPool");
		m_beginCommandBuffer = deviceFn<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer");
		m_endCommandBuffer   = deviceFn<PFN_vkEndCommandBuffer>("vkEndCommandBuffer");
		m_cmdCopyBuffer      = deviceFn<PFN_vkCmdCopyBuffer>("vkCmdCopyBuffer");
		m_cmdPipelineBarrier = deviceFn<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
		m_queueSubmit        = deviceFn<PFN_vkQueueSubmit>("vkQueueSubmit");
		m_destroyFence       = deviceFn<PFN_vkDestroyFence>("vkDestroyFence");
		m_waitForFences      = deviceFn<PFN_vkWaitForFences>("vkWaitForFences");
		m_resetFences        = deviceFn<PFN_vkResetFences>("vkResetFences");
		deviceFn<PFN_vkGetDeviceQueue>("vkGetDeviceQueue")(m_device, m_family, 0, &m_queue);
		VkCommandPoolCreateInfo pool {};
		pool.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		pool.queueFamilyIndex = m_family;
		deviceFn<PFN_vkCreateCommandPool>("vkCreateCommandPool")(m_device, &pool, nullptr, &m_pool);
		VkCommandBufferAllocateInfo allocate {};
		allocate.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		allocate.commandPool        = m_pool;
		allocate.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocate.commandBufferCount = 1;
		deviceFn<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(m_device, &allocate,
		                                                                   &m_commands);
		VkFenceCreateInfo fence {};
		fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		deviceFn<PFN_vkCreateFence>("vkCreateFence")(m_device, &fence, nullptr, &m_fence);
		return true;
	}

	Staging makeStaging(uint64_t bytes) {
		Staging            staging;
		VkBufferCreateInfo info {};
		info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		info.size  = bytes;
		info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		m_createBuffer(m_device, &info, nullptr, &staging.buffer);
		VkMemoryRequirements requirements {};
		m_requirements(m_device, staging.buffer, &requirements);
		VkPhysicalDeviceMemoryProperties memory {};
		instanceFn<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(
		    m_physical, &memory);
		const VkMemoryPropertyFlags wanted =
		    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
		uint32_t type = 0;
		while ((requirements.memoryTypeBits & (1u << type)) == 0 ||
		       (memory.memoryTypes[type].propertyFlags & wanted) != wanted) {
			++type;
		}
		VkMemoryAllocateInfo allocate {};
		allocate.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		allocate.allocationSize  = requirements.size;
		allocate.memoryTypeIndex = type;
		m_allocateMemory(m_device, &allocate, nullptr, &staging.memory);
		m_bindBufferMemory(m_device, staging.buffer, staging.memory, 0);
		void* data = nullptr;
		m_mapMemory(m_device, staging.memory, 0, VK_WHOLE_SIZE, 0, &data);
		staging.data = static_cast<uint8_t*>(data);
		return staging;
	}

	void dropStaging(const Staging& staging) {
		m_destroyBuffer(m_device, staging.buffer, nullptr);
		m_freeMemory(m_device, staging.memory, nullptr);
	}

	void barrier(VkPipelineStageFlags from, VkPipelineStageFlags to) {
		VkMemoryBarrier memory {};
		memory.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
		memory.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
		memory.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
		m_cmdPipelineBarrier(m_commands, from, to, 0, 1, &memory, 0, nullptr, 0, nullptr);
	}

	void copy(VkBuffer from, uint64_t fromOffset, VkBuffer to, uint64_t toOffset, uint64_t bytes) {
		m_deviceWaitIdle(m_device);
		m_resetCommandPool(m_device, m_pool, 0);
		VkCommandBufferBeginInfo begin {};
		begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		m_beginCommandBuffer(m_commands, &begin);
		barrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
		const VkBufferCopy region {fromOffset, toOffset, bytes};
		m_cmdCopyBuffer(m_commands, from, to, 1, &region);
		barrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
		        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT);
		m_endCommandBuffer(m_commands);
		VkSubmitInfo submit {};
		submit.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submit.commandBufferCount = 1;
		submit.pCommandBuffers    = &m_commands;
		m_queueSubmit(m_queue, 1, &submit, m_fence);
		m_waitForFences(m_device, 1, &m_fence, VK_TRUE, UINT64_MAX);
		m_resetFences(m_device, 1, &m_fence);
	}

	bool                      m_timeline;
	std::string               m_missing;
	HMODULE                   m_loader              = nullptr;
	PFN_vkGetInstanceProcAddr m_getInstanceProcAddr = nullptr;
	PFN_vkGetDeviceProcAddr   m_getDeviceProcAddr   = nullptr;
	VkInstance                m_instance            = VK_NULL_HANDLE;
	VkDebugUtilsMessengerEXT  m_messenger           = VK_NULL_HANDLE;
	VkPhysicalDevice          m_physical            = VK_NULL_HANDLE;
	uint32_t                  m_family              = 0;
	VkDevice                  m_device              = VK_NULL_HANDLE;
	VkQueue                   m_queue               = VK_NULL_HANDLE;
	VkCommandPool             m_pool                = VK_NULL_HANDLE;
	VkCommandBuffer           m_commands            = VK_NULL_HANDLE;
	VkFence                   m_fence               = VK_NULL_HANDLE;

	PFN_vkDestroyDevice               m_destroyDevice      = nullptr;
	PFN_vkDeviceWaitIdle              m_deviceWaitIdle     = nullptr;
	PFN_vkCreateBuffer                m_createBuffer       = nullptr;
	PFN_vkDestroyBuffer               m_destroyBuffer      = nullptr;
	PFN_vkGetBufferMemoryRequirements m_requirements       = nullptr;
	PFN_vkAllocateMemory              m_allocateMemory     = nullptr;
	PFN_vkFreeMemory                  m_freeMemory         = nullptr;
	PFN_vkBindBufferMemory            m_bindBufferMemory   = nullptr;
	PFN_vkMapMemory                   m_mapMemory          = nullptr;
	PFN_vkDestroyCommandPool          m_destroyCommandPool = nullptr;
	PFN_vkResetCommandPool            m_resetCommandPool   = nullptr;
	PFN_vkBeginCommandBuffer          m_beginCommandBuffer = nullptr;
	PFN_vkEndCommandBuffer            m_endCommandBuffer   = nullptr;
	PFN_vkCmdCopyBuffer               m_cmdCopyBuffer      = nullptr;
	PFN_vkCmdPipelineBarrier          m_cmdPipelineBarrier = nullptr;
	PFN_vkQueueSubmit                 m_queueSubmit        = nullptr;
	PFN_vkDestroyFence                m_destroyFence       = nullptr;
	PFN_vkWaitForFences               m_waitForFences      = nullptr;
	PFN_vkResetFences                 m_resetFences        = nullptr;
};

} // namespace deunify::test
