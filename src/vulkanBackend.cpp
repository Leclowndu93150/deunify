#include "vulkanBackend.hpp"

#include "hostError.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>
#include <utility>

namespace deunify::gpu {

namespace {

constexpr uint64_t kTileBytes       = kPageBytes;
constexpr uint64_t kBlockBytes      = uint64_t {kHeapTiles} * kTileBytes;
constexpr uint64_t kMaxSegmentBytes = 4ull << 30;
constexpr uint64_t kForever         = UINT64_MAX;
constexpr uint64_t kGone            = UINT64_MAX;
constexpr uint32_t kNoType          = UINT32_MAX;
constexpr uint32_t kOneFamily       = 1;
constexpr uint32_t kTwoFamilies     = 2;

constexpr VkBufferCreateFlags kSparseFlags =
    VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT;
constexpr VkBufferUsageFlags kSparseUsage =
    VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
    VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT |
    VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
    VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
    VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
constexpr VkQueueFlags kCopyQueues =
    VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
constexpr VkMemoryPropertyFlags kHostMemory =
    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

Status statusOf(VkResult result) {
	switch (result) {
		case VK_SUCCESS: return Status::Ok;
		case VK_ERROR_OUT_OF_HOST_MEMORY:
		case VK_ERROR_OUT_OF_DEVICE_MEMORY: return Status::OutOfMemory;
		default:
			detail::setLastHostError(static_cast<uint32_t>(result));
			return Status::HostFailure;
	}
}

uint64_t alignDown(uint64_t value, uint64_t alignment) {
	return value / alignment * alignment;
}

struct Functions {
	PFN_vkGetDeviceProcAddr                      getDeviceProcAddr                      = nullptr;
	PFN_vkGetPhysicalDeviceProperties            getPhysicalDeviceProperties            = nullptr;
	PFN_vkGetPhysicalDeviceFeatures2             getPhysicalDeviceFeatures2             = nullptr;
	PFN_vkGetPhysicalDeviceMemoryProperties2     getPhysicalDeviceMemoryProperties2     = nullptr;
	PFN_vkGetPhysicalDeviceQueueFamilyProperties getPhysicalDeviceQueueFamilyProperties = nullptr;
	PFN_vkEnumerateDeviceExtensionProperties     enumerateDeviceExtensionProperties     = nullptr;
	PFN_vkCreateBuffer                           createBuffer                           = nullptr;
	PFN_vkDestroyBuffer                          destroyBuffer                          = nullptr;
	PFN_vkGetBufferMemoryRequirements            getBufferMemoryRequirements            = nullptr;
	PFN_vkGetBufferDeviceAddress                 getBufferDeviceAddress                 = nullptr;
	PFN_vkAllocateMemory                         allocateMemory                         = nullptr;
	PFN_vkFreeMemory                             freeMemory                             = nullptr;
	PFN_vkMapMemory                              mapMemory                              = nullptr;
	PFN_vkBindBufferMemory                       bindBufferMemory                       = nullptr;
	PFN_vkQueueBindSparse                        queueBindSparse                        = nullptr;
	PFN_vkQueueSubmit                            queueSubmit                            = nullptr;
	PFN_vkCreateCommandPool                      createCommandPool                      = nullptr;
	PFN_vkDestroyCommandPool                     destroyCommandPool                     = nullptr;
	PFN_vkResetCommandPool                       resetCommandPool                       = nullptr;
	PFN_vkAllocateCommandBuffers                 allocateCommandBuffers                 = nullptr;
	PFN_vkBeginCommandBuffer                     beginCommandBuffer                     = nullptr;
	PFN_vkEndCommandBuffer                       endCommandBuffer                       = nullptr;
	PFN_vkCmdCopyBuffer                          cmdCopyBuffer                          = nullptr;
	PFN_vkCmdPipelineBarrier                     cmdPipelineBarrier                     = nullptr;
	PFN_vkCreateSemaphore                        createSemaphore                        = nullptr;
	PFN_vkDestroySemaphore                       destroySemaphore                       = nullptr;
	PFN_vkGetSemaphoreCounterValue               getSemaphoreCounterValue               = nullptr;
	PFN_vkWaitSemaphores                         waitSemaphores                         = nullptr;
	PFN_vkCreateFence                            createFence                            = nullptr;
	PFN_vkDestroyFence                           destroyFence                           = nullptr;
	PFN_vkWaitForFences                          waitForFences                          = nullptr;
	PFN_vkGetFenceStatus                         getFenceStatus                         = nullptr;
};

class Loader {
public:
	explicit Loader(const VulkanSetup& setup): m_setup(setup) {}

	template <typename Fn>
	bool instance(Fn& fn, const char* name, const char* other = nullptr) {
		fn = reinterpret_cast<Fn>(m_setup.getInstanceProcAddr(m_setup.instance, name));
		if (fn == nullptr && other != nullptr) {
			fn = reinterpret_cast<Fn>(m_setup.getInstanceProcAddr(m_setup.instance, other));
		}
		return fn != nullptr;
	}

	template <typename Fn>
	bool device(PFN_vkGetDeviceProcAddr getDeviceProcAddr, Fn& fn, const char* name,
	            const char* other = nullptr) {
		fn = reinterpret_cast<Fn>(getDeviceProcAddr(m_setup.device, name));
		if (fn == nullptr && other != nullptr) {
			fn = reinterpret_cast<Fn>(getDeviceProcAddr(m_setup.device, other));
		}
		return fn != nullptr;
	}

private:
	const VulkanSetup& m_setup;
};

bool loadFunctions(const VulkanSetup& setup, Functions& f) {
	Loader     load(setup);
	const bool instance =
	    load.instance(f.getDeviceProcAddr, "vkGetDeviceProcAddr") &&
	    load.instance(f.getPhysicalDeviceProperties, "vkGetPhysicalDeviceProperties") &&
	    load.instance(f.getPhysicalDeviceFeatures2, "vkGetPhysicalDeviceFeatures2",
	                  "vkGetPhysicalDeviceFeatures2KHR") &&
	    load.instance(f.getPhysicalDeviceMemoryProperties2, "vkGetPhysicalDeviceMemoryProperties2",
	                  "vkGetPhysicalDeviceMemoryProperties2KHR") &&
	    load.instance(f.getPhysicalDeviceQueueFamilyProperties,
	                  "vkGetPhysicalDeviceQueueFamilyProperties") &&
	    load.instance(f.enumerateDeviceExtensionProperties, "vkEnumerateDeviceExtensionProperties");
	if (!instance) {
		return false;
	}
	const PFN_vkGetDeviceProcAddr at = f.getDeviceProcAddr;
	return load.device(at, f.createBuffer, "vkCreateBuffer") &&
	       load.device(at, f.destroyBuffer, "vkDestroyBuffer") &&
	       load.device(at, f.getBufferMemoryRequirements, "vkGetBufferMemoryRequirements") &&
	       load.device(at, f.getBufferDeviceAddress, "vkGetBufferDeviceAddress",
	                   "vkGetBufferDeviceAddressKHR") &&
	       load.device(at, f.allocateMemory, "vkAllocateMemory") &&
	       load.device(at, f.freeMemory, "vkFreeMemory") &&
	       load.device(at, f.mapMemory, "vkMapMemory") &&
	       load.device(at, f.bindBufferMemory, "vkBindBufferMemory") &&
	       load.device(at, f.queueBindSparse, "vkQueueBindSparse") &&
	       load.device(at, f.queueSubmit, "vkQueueSubmit") &&
	       load.device(at, f.createCommandPool, "vkCreateCommandPool") &&
	       load.device(at, f.destroyCommandPool, "vkDestroyCommandPool") &&
	       load.device(at, f.resetCommandPool, "vkResetCommandPool") &&
	       load.device(at, f.allocateCommandBuffers, "vkAllocateCommandBuffers") &&
	       load.device(at, f.beginCommandBuffer, "vkBeginCommandBuffer") &&
	       load.device(at, f.endCommandBuffer, "vkEndCommandBuffer") &&
	       load.device(at, f.cmdCopyBuffer, "vkCmdCopyBuffer") &&
	       load.device(at, f.cmdPipelineBarrier, "vkCmdPipelineBarrier") &&
	       load.device(at, f.createSemaphore, "vkCreateSemaphore") &&
	       load.device(at, f.destroySemaphore, "vkDestroySemaphore") &&
	       load.device(at, f.createFence, "vkCreateFence") &&
	       load.device(at, f.destroyFence, "vkDestroyFence") &&
	       load.device(at, f.waitForFences, "vkWaitForFences") &&
	       load.device(at, f.getFenceStatus, "vkGetFenceStatus");
}

bool loadTimelineFunctions(const VulkanSetup& setup, Functions& f) {
	Loader                        load(setup);
	const PFN_vkGetDeviceProcAddr at = f.getDeviceProcAddr;
	return load.device(at, f.getSemaphoreCounterValue, "vkGetSemaphoreCounterValue",
	                   "vkGetSemaphoreCounterValueKHR") &&
	       load.device(at, f.waitSemaphores, "vkWaitSemaphores", "vkWaitSemaphoresKHR");
}

class VulkanBackend;

class VulkanRegion final: public Region {
public:
	VulkanRegion(VulkanBackend& backend, VkBuffer buffer, uint64_t bytes, VkDeviceAddress address)
	    : m_backend(backend), m_buffer(buffer), m_bytes(bytes), m_address(address) {}
	~VulkanRegion() override;

	BufferView view(uint64_t offset, uint64_t bytes) const override {
		BufferView view;
		view.buffer        = m_buffer;
		view.deviceAddress = m_address + offset;
		view.offset        = offset;
		view.bytes         = bytes;
		return view;
	}

	VkBuffer buffer() const { return m_buffer; }

private:
	VulkanBackend&  m_backend;
	VkBuffer        m_buffer;
	uint64_t        m_bytes;
	VkDeviceAddress m_address;
};

class VulkanBlock final: public Block {
public:
	VulkanBlock(VulkanBackend& backend, VkDeviceMemory memory)
	    : m_backend(backend), m_memory(memory) {}
	~VulkanBlock() override;

	VkDeviceMemory memory() const { return m_memory; }

private:
	VulkanBackend& m_backend;
	VkDeviceMemory m_memory;
};

class VulkanHostBuffer final: public HostBuffer {
public:
	VulkanHostBuffer(VulkanBackend& backend, VkBuffer buffer, VkDeviceMemory memory, bool card,
	                 uint64_t allocated)
	    : m_backend(backend), m_buffer(buffer), m_memory(memory), m_card(card),
	      m_allocated(allocated) {}
	~VulkanHostBuffer() override;

	VkBuffer buffer() const { return m_buffer; }

private:
	VulkanBackend& m_backend;
	VkBuffer       m_buffer;
	VkDeviceMemory m_memory;
	bool           m_card;
	uint64_t       m_allocated;
};

class VulkanCommands final: public Commands {
public:
	VulkanCommands(VulkanBackend& backend, VkCommandPool pool, VkCommandBuffer buffer)
	    : m_backend(backend), m_pool(pool), m_buffer(buffer) {}
	~VulkanCommands() override;

	VkCommandPool   pool() const { return m_pool; }
	VkCommandBuffer buffer() const { return m_buffer; }

private:
	VulkanBackend&  m_backend;
	VkCommandPool   m_pool;
	VkCommandBuffer m_buffer;
};

class Signalled {
public:
	Signalled(const Functions& functions, VkDevice device, VkFence fence, VkSemaphore semaphore)
	    : m_functions(functions), m_device(device), m_fence(fence), m_semaphore(semaphore) {}
	Signalled(const Signalled&)            = delete;
	Signalled& operator=(const Signalled&) = delete;

	~Signalled() {
		m_functions.destroyFence(m_device, m_fence, nullptr);
		if (m_semaphore != VK_NULL_HANDLE) {
			m_functions.destroySemaphore(m_device, m_semaphore, nullptr);
		}
	}

	VkFence fence() const { return m_fence; }

private:
	const Functions& m_functions;
	VkDevice         m_device;
	VkFence          m_fence;
	VkSemaphore      m_semaphore;
};

struct Fenced {
	uint64_t                   value = 0;
	std::shared_ptr<Signalled> signalled;
};

class QueueGuard {
public:
	QueueGuard(const Callbacks& callbacks, VkQueue queue): m_callbacks(callbacks), m_queue(queue) {
		if (m_callbacks.queueLock != nullptr) {
			m_callbacks.queueLock(m_callbacks.context, m_queue, true);
		}
	}
	QueueGuard(const QueueGuard&)            = delete;
	QueueGuard& operator=(const QueueGuard&) = delete;

	~QueueGuard() {
		if (m_callbacks.queueLock != nullptr) {
			m_callbacks.queueLock(m_callbacks.context, m_queue, false);
		}
	}

private:
	const Callbacks& m_callbacks;
	VkQueue          m_queue;
};

struct PendingBind {
	VkBuffer                        buffer = VK_NULL_HANDLE;
	std::vector<VkSparseMemoryBind> binds;
};

class VulkanBackend final: public Backend {
public:
	VulkanBackend(const VulkanSetup& setup, const Callbacks& callbacks,
	              std::shared_ptr<detail::Ledger> ledger)
	    : m_setup(setup), m_callbacks(callbacks), m_ledger(std::move(ledger)) {}

	~VulkanBackend() override {
		{
			std::lock_guard lock(m_fenceMutex);
			m_fenced.clear();
		}
		if (m_timeline != VK_NULL_HANDLE) {
			m_f.destroySemaphore(m_setup.device, m_timeline, nullptr);
		}
	}

	Status start() override {
		if (!loadFunctions(m_setup, m_f)) {
			return Status::NotSupported;
		}
		VkPhysicalDeviceProperties properties {};
		m_f.getPhysicalDeviceProperties(m_setup.physicalDevice, &properties);
		VkPhysicalDeviceVulkan12Features features12 {};
		features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
		VkPhysicalDeviceFeatures2 features {};
		features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
		features.pNext = &features12;
		m_f.getPhysicalDeviceFeatures2(m_setup.physicalDevice, &features);
		if (properties.apiVersion < VK_API_VERSION_1_2 || !features.features.sparseBinding ||
		    !features.features.sparseResidencyBuffer || !features12.bufferDeviceAddress) {
			return Status::NotSupported;
		}
		if (!queuesFit()) {
			return Status::InvalidArgument;
		}
		m_strict      = properties.sparseProperties.residencyNonResidentStrict == VK_TRUE;
		m_sparseSpace = properties.limits.sparseAddressSpaceSize;
		m_shape.bytes =
		    alignDown(std::min<uint64_t>(kMaxSegmentBytes, properties.limits.maxStorageBufferRange),
		              kTileBytes);
		m_shape.step = alignDown(m_shape.bytes / 2, kTileBytes);
		if (m_shape.step < kLargestAskBytes) {
			return Status::NotSupported;
		}
		m_memory.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
		m_f.getPhysicalDeviceMemoryProperties2(m_setup.physicalDevice, &m_memory);
		m_budget      = hasExtension(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
		Status status = chooseBlockType();
		if (status != Status::Ok) {
			return status;
		}
		const bool timeline = m_setup.timelineSemaphores && features12.timelineSemaphore &&
		                      loadTimelineFunctions(m_setup, m_f);
		return timeline ? makeTimeline() : Status::Ok;
	}

	SegmentShape segmentShape() const override { return m_shape; }

	bool flatSpans() const override { return true; }

	bool unboundReadsZero() const override { return m_strict; }

	Status makeRegion(uint64_t bytes, std::unique_ptr<Region>* region) override {
		if (m_sparseUsed.fetch_add(bytes) + bytes > m_sparseSpace) {
			m_sparseUsed.fetch_sub(bytes);
			return Status::OutOfMemory;
		}
		VkBuffer     buffer = VK_NULL_HANDLE;
		const Status status = makeSparseBuffer(bytes, &buffer);
		if (status != Status::Ok) {
			m_sparseUsed.fetch_sub(bytes);
			return status;
		}
		VkBufferDeviceAddressInfo address {};
		address.sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
		address.buffer = buffer;
		*region        = std::make_unique<VulkanRegion>(
		    *this, buffer, bytes, m_f.getBufferDeviceAddress(m_setup.device, &address));
		return Status::Ok;
	}

	Status makeBlock(std::shared_ptr<Block>* block) override {
		VkMemoryAllocateFlagsInfo flags {};
		flags.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
		flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
		VkMemoryAllocateInfo info {};
		info.sType            = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		info.pNext            = &flags;
		info.allocationSize   = kBlockBytes;
		info.memoryTypeIndex  = m_blockType;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		const Status status = statusOf(m_f.allocateMemory(m_setup.device, &info, nullptr, &memory));
		if (status != Status::Ok) {
			return status;
		}
		m_ledger->count(kCardViewCategory, true, kBlockBytes);
		*block = std::make_shared<VulkanBlock>(*this, memory);
		return Status::Ok;
	}

	Status makeUpload(uint64_t bytes, std::unique_ptr<HostBuffer>* buffer) override {
		return makeHostBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 0, buffer);
	}

	Status makeReadback(uint64_t bytes, std::unique_ptr<HostBuffer>* buffer) override {
		return makeHostBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		                      VK_MEMORY_PROPERTY_HOST_CACHED_BIT, buffer);
	}

	Status makeCommands(std::unique_ptr<Commands>* commands) override {
		VkCommandPoolCreateInfo pool {};
		pool.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		pool.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
		pool.queueFamilyIndex = m_setup.transferFamily;
		VkCommandPool made    = VK_NULL_HANDLE;
		Status status = statusOf(m_f.createCommandPool(m_setup.device, &pool, nullptr, &made));
		if (status != Status::Ok) {
			return status;
		}
		VkCommandBufferAllocateInfo allocate {};
		allocate.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		allocate.commandPool        = made;
		allocate.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocate.commandBufferCount = 1;
		VkCommandBuffer buffer      = VK_NULL_HANDLE;
		status = statusOf(m_f.allocateCommandBuffers(m_setup.device, &allocate, &buffer));
		if (status != Status::Ok) {
			m_f.destroyCommandPool(m_setup.device, made, nullptr);
			return status;
		}
		*commands = std::make_unique<VulkanCommands>(*this, made, buffer);
		return Status::Ok;
	}

	void bind(Region& region, std::span<const BindGroup> groups,
	          std::span<Block* const> blocks) override {
		for (const BindGroup& group: groups) {
			PendingBind pending;
			pending.buffer = static_cast<VulkanRegion&>(region).buffer();
			const VkDeviceMemory memory =
			    group.block ? static_cast<VulkanBlock*>(blocks[*group.block])->memory()
			                : VK_NULL_HANDLE;
			for (const Run& run: group.runs) {
				VkSparseMemoryBind bind {};
				bind.resourceOffset = run.slot * kTileBytes;
				bind.size           = run.count * kTileBytes;
				bind.memory         = run.tile ? memory : VK_NULL_HANDLE;
				bind.memoryOffset   = run.tile.value_or(0) * kTileBytes;
				pending.binds.push_back(bind);
			}
			m_pending.push_back(std::move(pending));
		}
	}

	uint64_t submitBinds() override {
		std::vector<VkSparseBufferMemoryBindInfo> buffers;
		for (const PendingBind& pending: m_pending) {
			buffers.push_back(VkSparseBufferMemoryBindInfo {
			    pending.buffer, static_cast<uint32_t>(pending.binds.size()), pending.binds.data()});
		}
		VkBindSparseInfo info {};
		info.sType           = VK_STRUCTURE_TYPE_BIND_SPARSE_INFO;
		info.bufferBindCount = static_cast<uint32_t>(buffers.size());
		info.pBufferBinds    = buffers.data();
		const uint64_t value =
		    m_timeline != VK_NULL_HANDLE ? bindAfterTimeline(info) : bindAfterFence(info);
		m_pending.clear();
		return value;
	}

	Status begin(Commands& commands) override {
		auto&  set    = static_cast<VulkanCommands&>(commands);
		Status status = statusOf(m_f.resetCommandPool(m_setup.device, set.pool(), 0));
		if (status != Status::Ok) {
			return status;
		}
		VkCommandBufferBeginInfo info {};
		info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		status     = statusOf(m_f.beginCommandBuffer(set.buffer(), &info));
		if (status == Status::Ok) {
			barrier(set.buffer(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
			        VK_PIPELINE_STAGE_TRANSFER_BIT,
			        VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
		}
		return status;
	}

	void copyUp(Commands& commands, const Region& to, uint64_t toOffset, const HostBuffer& from,
	            uint64_t fromOffset, uint64_t bytes) override {
		const VkBufferCopy copy {fromOffset, toOffset, bytes};
		m_f.cmdCopyBuffer(static_cast<VulkanCommands&>(commands).buffer(),
		                  static_cast<const VulkanHostBuffer&>(from).buffer(),
		                  static_cast<const VulkanRegion&>(to).buffer(), 1, &copy);
	}

	void copyBack(Commands& commands, const HostBuffer& to, const Region& from, uint64_t fromOffset,
	              uint64_t bytes) override {
		const VkBufferCopy copy {fromOffset, 0, bytes};
		m_f.cmdCopyBuffer(static_cast<VulkanCommands&>(commands).buffer(),
		                  static_cast<const VulkanRegion&>(from).buffer(),
		                  static_cast<const VulkanHostBuffer&>(to).buffer(), 1, &copy);
	}

	Status end(Commands& commands) override {
		const VkCommandBuffer buffer = static_cast<VulkanCommands&>(commands).buffer();
		barrier(buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
		        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT,
		        VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_HOST_READ_BIT);
		return statusOf(m_f.endCommandBuffer(buffer));
	}

	uint64_t submit(Commands& commands) override {
		const VkCommandBuffer buffer = static_cast<VulkanCommands&>(commands).buffer();
		return submitTransfer(&buffer, 1);
	}

	uint64_t signal() override { return submitTransfer(nullptr, 0); }

	void fenceTitleWork() override {
		if (m_timeline == VK_NULL_HANDLE) {
			waitBinds();
			return;
		}
		if (m_titleFenced) {
			return;
		}
		const VkPipelineStageFlags    stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
		VkTimelineSemaphoreSubmitInfo values {};
		values.sType                   = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
		values.waitSemaphoreValueCount = 1;
		values.pWaitSemaphoreValues    = &m_value;
		VkSubmitInfo info {};
		info.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		info.pNext              = &values;
		info.waitSemaphoreCount = 1;
		info.pWaitSemaphores    = &m_timeline;
		info.pWaitDstStageMask  = &stage;
		queueSubmit(m_setup.transferQueue, info, VK_NULL_HANDLE);
		m_titleFenced = true;
	}

	uint64_t completed() const override {
		if (m_timeline != VK_NULL_HANDLE) {
			uint64_t value = 0;
			return m_f.getSemaphoreCounterValue(m_setup.device, m_timeline, &value) == VK_SUCCESS
			           ? value
			           : kGone;
		}
		std::lock_guard lock(m_fenceMutex);
		while (!m_fenced.empty()) {
			const VkResult result =
			    m_f.getFenceStatus(m_setup.device, m_fenced.front().signalled->fence());
			if (result == VK_NOT_READY) {
				break;
			}
			if (result != VK_SUCCESS) {
				return kGone;
			}
			m_done = m_fenced.front().value;
			m_fenced.pop_front();
		}
		return m_done;
	}

	bool wait(uint64_t value) const override {
		if (m_lost.load()) {
			return false;
		}
		if (m_timeline != VK_NULL_HANDLE) {
			VkSemaphoreWaitInfo info {};
			info.sType          = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
			info.semaphoreCount = 1;
			info.pSemaphores    = &m_timeline;
			info.pValues        = &value;
			return m_f.waitSemaphores(m_setup.device, &info, kForever) == VK_SUCCESS;
		}
		std::vector<std::shared_ptr<Signalled>> held;
		{
			std::lock_guard lock(m_fenceMutex);
			for (const Fenced& fenced: m_fenced) {
				if (fenced.value <= value) {
					held.push_back(fenced.signalled);
				}
			}
		}
		if (held.empty()) {
			return true;
		}
		std::vector<VkFence> fences;
		for (const auto& signalled: held) {
			fences.push_back(signalled->fence());
		}
		return m_f.waitForFences(m_setup.device, static_cast<uint32_t>(fences.size()),
		                         fences.data(), VK_TRUE, kForever) == VK_SUCCESS;
	}

	std::optional<bool> memoryTypeOnCard(uint32_t memoryType) const override {
		const VkPhysicalDeviceMemoryProperties& memory = m_memory.memoryProperties;
		if (memoryType >= memory.memoryTypeCount) {
			return std::nullopt;
		}
		return onCard(memoryType);
	}

	SystemMemory systemMemory(const std::vector<CategoryUse>& counted) const override {
		VkPhysicalDeviceMemoryBudgetPropertiesEXT budget {};
		budget.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
		VkPhysicalDeviceMemoryProperties2 properties {};
		properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
		properties.pNext = m_budget ? &budget : nullptr;
		m_f.getPhysicalDeviceMemoryProperties2(m_setup.physicalDevice, &properties);
		const VkPhysicalDeviceMemoryProperties& memory = properties.memoryProperties;
		SystemMemory                            system;
		system.known   = true;
		system.counted = !m_budget;
		for (uint32_t heap = 0; heap < memory.memoryHeapCount; ++heap) {
			const bool local =
			    (memory.memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0;
			(local ? system.localBudget : system.nonLocalBudget) +=
			    m_budget ? budget.heapBudget[heap] : memory.memoryHeaps[heap].size;
			if (m_budget) {
				(local ? system.localUsage : system.nonLocalUsage) += budget.heapUsage[heap];
			}
		}
		if (!m_budget) {
			for (const CategoryUse& use: counted) {
				system.localUsage += use.cardBytes;
				system.nonLocalUsage += use.sharedBytes;
			}
		}
		return system;
	}

	const Functions& functions() const { return m_f; }
	VkDevice         device() const { return m_setup.device; }

	void regionGone(uint64_t bytes) { m_sparseUsed.fetch_sub(bytes); }

	void memoryGone(bool card, uint64_t bytes) {
		m_ledger->uncount(kCardViewCategory, card, bytes);
	}

private:
	bool queuesFit() const {
		uint32_t count = 0;
		m_f.getPhysicalDeviceQueueFamilyProperties(m_setup.physicalDevice, &count, nullptr);
		std::vector<VkQueueFamilyProperties> families(count);
		m_f.getPhysicalDeviceQueueFamilyProperties(m_setup.physicalDevice, &count, families.data());
		return m_setup.sparseFamily < count && m_setup.transferFamily < count &&
		       (families[m_setup.sparseFamily].queueFlags & VK_QUEUE_SPARSE_BINDING_BIT) != 0 &&
		       (families[m_setup.transferFamily].queueFlags & kCopyQueues) != 0;
	}

	bool hasExtension(const char* name) const {
		uint32_t count = 0;
		m_f.enumerateDeviceExtensionProperties(m_setup.physicalDevice, nullptr, &count, nullptr);
		std::vector<VkExtensionProperties> extensions(count);
		m_f.enumerateDeviceExtensionProperties(m_setup.physicalDevice, nullptr, &count,
		                                       extensions.data());
		return std::any_of(extensions.begin(), extensions.end(), [name](const auto& extension) {
			return std::strcmp(extension.extensionName, name) == 0;
		});
	}

	bool onCard(uint32_t memoryType) const {
		const VkPhysicalDeviceMemoryProperties& memory = m_memory.memoryProperties;
		const uint32_t                          heap   = memory.memoryTypes[memoryType].heapIndex;
		return (memory.memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0;
	}

	uint32_t typeFor(uint32_t allowed, VkMemoryPropertyFlags required, VkMemoryPropertyFlags wanted,
	                 VkMemoryPropertyFlags unwanted) const {
		const VkPhysicalDeviceMemoryProperties& memory = m_memory.memoryProperties;
		uint32_t                                found  = kNoType;
		uint32_t                                best   = 0;
		for (uint32_t type = 0; type < memory.memoryTypeCount; ++type) {
			const VkMemoryPropertyFlags flags = memory.memoryTypes[type].propertyFlags;
			if ((allowed & (1u << type)) == 0 || (flags & required) != required) {
				continue;
			}
			const uint32_t score =
			    ((flags & wanted) == wanted ? 2u : 0u) + ((flags & unwanted) == 0 ? 1u : 0u);
			if (found == kNoType || score > best) {
				found = type;
				best  = score;
			}
		}
		return found;
	}

	Status makeSparseBuffer(uint64_t bytes, VkBuffer* buffer) {
		const uint32_t     families[] = {m_setup.sparseFamily, m_setup.transferFamily};
		const bool         shared     = m_setup.sparseFamily != m_setup.transferFamily;
		VkBufferCreateInfo info {};
		info.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		info.flags       = kSparseFlags;
		info.size        = bytes;
		info.usage       = kSparseUsage;
		info.sharingMode = shared ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE;
		info.queueFamilyIndexCount = shared ? kTwoFamilies : kOneFamily;
		info.pQueueFamilyIndices   = families;
		const Status status = statusOf(m_f.createBuffer(m_setup.device, &info, nullptr, buffer));
		if (status != Status::Ok) {
			return status;
		}
		VkMemoryRequirements requirements {};
		m_f.getBufferMemoryRequirements(m_setup.device, *buffer, &requirements);
		const bool fits =
		    kTileBytes % requirements.alignment == 0 &&
		    (m_blockType == kNoType || (requirements.memoryTypeBits & (1u << m_blockType)) != 0);
		if (!fits) {
			m_f.destroyBuffer(m_setup.device, *buffer, nullptr);
			*buffer = VK_NULL_HANDLE;
			return Status::NotSupported;
		}
		m_allowed = requirements.memoryTypeBits;
		return Status::Ok;
	}

	Status chooseBlockType() {
		VkBuffer     probe  = VK_NULL_HANDLE;
		const Status status = makeSparseBuffer(kBlockBytes, &probe);
		if (status != Status::Ok) {
			return status;
		}
		m_f.destroyBuffer(m_setup.device, probe, nullptr);
		m_blockType = typeFor(m_allowed, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0,
		                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
		return m_blockType == kNoType ? Status::NotSupported : Status::Ok;
	}

	Status makeTimeline() {
		VkSemaphoreTypeCreateInfo type {};
		type.sType         = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
		type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
		VkSemaphoreCreateInfo info {};
		info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
		info.pNext = &type;
		return statusOf(m_f.createSemaphore(m_setup.device, &info, nullptr, &m_timeline));
	}

	Status makeHostBuffer(uint64_t bytes, VkBufferUsageFlags usage, VkMemoryPropertyFlags wanted,
	                      std::unique_ptr<HostBuffer>* out) {
		VkBufferCreateInfo info {};
		info.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		info.size        = bytes;
		info.usage       = usage;
		info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		VkBuffer buffer  = VK_NULL_HANDLE;
		Status   status  = statusOf(m_f.createBuffer(m_setup.device, &info, nullptr, &buffer));
		if (status != Status::Ok) {
			return status;
		}
		VkMemoryRequirements requirements {};
		m_f.getBufferMemoryRequirements(m_setup.device, buffer, &requirements);
		const uint32_t type   = typeFor(requirements.memoryTypeBits, kHostMemory, wanted,
		                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		VkDeviceMemory memory = VK_NULL_HANDLE;
		void*          data   = nullptr;
		if (type == kNoType) {
			status = Status::NotSupported;
		} else {
			VkMemoryAllocateInfo allocate {};
			allocate.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
			allocate.allocationSize  = requirements.size;
			allocate.memoryTypeIndex = type;
			status = statusOf(m_f.allocateMemory(m_setup.device, &allocate, nullptr, &memory));
		}
		if (status == Status::Ok) {
			status = statusOf(m_f.bindBufferMemory(m_setup.device, buffer, memory, 0));
		}
		if (status == Status::Ok) {
			status = statusOf(m_f.mapMemory(m_setup.device, memory, 0, VK_WHOLE_SIZE, 0, &data));
		}
		if (status != Status::Ok) {
			if (memory != VK_NULL_HANDLE) {
				m_f.freeMemory(m_setup.device, memory, nullptr);
			}
			m_f.destroyBuffer(m_setup.device, buffer, nullptr);
			return status;
		}
		const bool card = onCard(type);
		m_ledger->count(kCardViewCategory, card, requirements.size);
		auto made =
		    std::make_unique<VulkanHostBuffer>(*this, buffer, memory, card, requirements.size);
		made->data  = static_cast<uint8_t*>(data);
		made->bytes = bytes;
		*out        = std::move(made);
		return Status::Ok;
	}

	void barrier(VkCommandBuffer buffer, VkPipelineStageFlags fromStages, VkAccessFlags fromAccess,
	             VkPipelineStageFlags toStages, VkAccessFlags toAccess) const {
		VkMemoryBarrier memory {};
		memory.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
		memory.srcAccessMask = fromAccess;
		memory.dstAccessMask = toAccess;
		m_f.cmdPipelineBarrier(buffer, fromStages, toStages, 0, 1, &memory, 0, nullptr, 0, nullptr);
	}

	void queueSubmit(VkQueue queue, const VkSubmitInfo& info, VkFence fence) {
		QueueGuard guard(m_callbacks, queue);
		if (m_f.queueSubmit(queue, 1, &info, fence) != VK_SUCCESS) {
			m_lost.store(true);
		}
	}

	void queueBindSparse(const VkBindSparseInfo& info, VkFence fence) {
		QueueGuard guard(m_callbacks, m_setup.sparseQueue);
		if (m_f.queueBindSparse(m_setup.sparseQueue, 1, &info, fence) != VK_SUCCESS) {
			m_lost.store(true);
		}
	}

	VkFence makeFence() {
		VkFenceCreateInfo info {};
		info.sType    = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		VkFence fence = VK_NULL_HANDLE;
		if (m_f.createFence(m_setup.device, &info, nullptr, &fence) != VK_SUCCESS) {
			m_lost.store(true);
		}
		return fence;
	}

	VkSemaphore makeSemaphore() {
		VkSemaphoreCreateInfo info {};
		info.sType            = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
		VkSemaphore semaphore = VK_NULL_HANDLE;
		if (m_f.createSemaphore(m_setup.device, &info, nullptr, &semaphore) != VK_SUCCESS) {
			m_lost.store(true);
		}
		return semaphore;
	}

	uint64_t record(VkFence fence, VkSemaphore semaphore) {
		std::lock_guard lock(m_fenceMutex);
		++m_value;
		m_fenced.push_back(
		    Fenced {m_value, std::make_shared<Signalled>(m_f, m_setup.device, fence, semaphore)});
		return m_value;
	}

	void waitBinds() {
		if (m_lastBind > completed()) {
			wait(m_lastBind);
		}
	}

	uint64_t submitTransfer(const VkCommandBuffer* buffers, uint32_t count) {
		VkSubmitInfo info {};
		info.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		info.commandBufferCount = count;
		info.pCommandBuffers    = buffers;
		if (m_timeline == VK_NULL_HANDLE) {
			waitBinds();
			const VkFence fence = makeFence();
			queueSubmit(m_setup.transferQueue, info, fence);
			return record(fence, VK_NULL_HANDLE);
		}
		const VkPipelineStageFlags    stage   = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
		const uint64_t                waitFor = m_value;
		const uint64_t                signal  = m_value + 1;
		VkTimelineSemaphoreSubmitInfo values {};
		values.sType                     = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
		values.waitSemaphoreValueCount   = 1;
		values.pWaitSemaphoreValues      = &waitFor;
		values.signalSemaphoreValueCount = 1;
		values.pSignalSemaphoreValues    = &signal;
		info.pNext                       = &values;
		info.waitSemaphoreCount          = 1;
		info.pWaitSemaphores             = &m_timeline;
		info.pWaitDstStageMask           = &stage;
		info.signalSemaphoreCount        = 1;
		info.pSignalSemaphores           = &m_timeline;
		queueSubmit(m_setup.transferQueue, info, VK_NULL_HANDLE);
		m_value       = signal;
		m_titleFenced = true;
		return m_value;
	}

	uint64_t bindAfterTimeline(VkBindSparseInfo& info) {
		const uint64_t                marked = submitTransfer(nullptr, 0);
		const uint64_t                signal = marked + 1;
		VkTimelineSemaphoreSubmitInfo values {};
		values.sType                     = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
		values.waitSemaphoreValueCount   = 1;
		values.pWaitSemaphoreValues      = &marked;
		values.signalSemaphoreValueCount = 1;
		values.pSignalSemaphoreValues    = &signal;
		info.pNext                       = &values;
		info.waitSemaphoreCount          = 1;
		info.pWaitSemaphores             = &m_timeline;
		info.signalSemaphoreCount        = 1;
		info.pSignalSemaphores           = &m_timeline;
		queueBindSparse(info, VK_NULL_HANDLE);
		m_value       = signal;
		m_titleFenced = false;
		return m_value;
	}

	uint64_t bindAfterFence(VkBindSparseInfo& info) {
		const VkSemaphore marker = makeSemaphore();
		VkSubmitInfo      mark {};
		mark.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		mark.signalSemaphoreCount = 1;
		mark.pSignalSemaphores    = &marker;
		queueSubmit(m_setup.transferQueue, mark, VK_NULL_HANDLE);
		info.waitSemaphoreCount = 1;
		info.pWaitSemaphores    = &marker;
		const VkFence fence     = makeFence();
		queueBindSparse(info, fence);
		m_lastBind = record(fence, marker);
		return m_lastBind;
	}

	VulkanSetup                       m_setup;
	Callbacks                         m_callbacks;
	std::shared_ptr<detail::Ledger>   m_ledger;
	Functions                         m_f;
	VkPhysicalDeviceMemoryProperties2 m_memory {};
	SegmentShape                      m_shape;
	uint32_t                          m_blockType   = kNoType;
	uint32_t                          m_allowed     = 0;
	bool                              m_strict      = false;
	bool                              m_budget      = false;
	uint64_t                          m_sparseSpace = 0;
	std::atomic<uint64_t>             m_sparseUsed {0};
	std::atomic<bool>                 m_lost {false};
	VkSemaphore                       m_timeline    = VK_NULL_HANDLE;
	uint64_t                          m_value       = 0;
	uint64_t                          m_lastBind    = 0;
	bool                              m_titleFenced = true;
	std::vector<PendingBind>          m_pending;
	mutable std::mutex                m_fenceMutex;
	mutable std::deque<Fenced>        m_fenced;
	mutable uint64_t                  m_done = 0;
};

VulkanRegion::~VulkanRegion() {
	m_backend.functions().destroyBuffer(m_backend.device(), m_buffer, nullptr);
	m_backend.regionGone(m_bytes);
}

VulkanBlock::~VulkanBlock() {
	m_backend.functions().freeMemory(m_backend.device(), m_memory, nullptr);
	m_backend.memoryGone(true, kBlockBytes);
}

VulkanHostBuffer::~VulkanHostBuffer() {
	m_backend.functions().destroyBuffer(m_backend.device(), m_buffer, nullptr);
	m_backend.functions().freeMemory(m_backend.device(), m_memory, nullptr);
	m_backend.memoryGone(m_card, m_allocated);
}

VulkanCommands::~VulkanCommands() {
	m_backend.functions().destroyCommandPool(m_backend.device(), m_pool, nullptr);
}

} // namespace

std::unique_ptr<Backend> makeVulkanBackend(const VulkanSetup& setup, const Callbacks& callbacks,
                                           std::shared_ptr<detail::Ledger> ledger) {
	return std::make_unique<VulkanBackend>(setup, callbacks, std::move(ledger));
}

} // namespace deunify::gpu
