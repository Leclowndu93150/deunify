#include "deunify/context.hpp"

#include "deunify/cardView.hpp"
#include "deunify/census.hpp"
#include "deunify/gpuPageTable.hpp"
#include "deunify/loads.hpp"
#include "deunify/pages.hpp"
#include "deunify/physicalMemory.hpp"

#include "hostError.hpp"

#include <d3d12.h>
#include <wrl/client.h>

#include <algorithm>
#include <mutex>
#include <vector>

namespace deunify {

namespace {

struct Change {
	Range        range;
	ChangeReason reason = ChangeReason::CpuWritten;
};

constexpr ChangeReason kReasons[] = {
    ChangeReason::CpuWritten,
    ChangeReason::ShaderWritten,
    ChangeReason::Loaded,
    ChangeReason::Remapped,
};

void report(Status* status, Status value) {
	if (status != nullptr) {
		*status = value;
	}
}

bool wantsVulkan(const Config& config) {
	return config.vulkan.device != VK_NULL_HANDLE;
}

bool completeVulkan(const VulkanSetup& vulkan) {
	return vulkan.instance != VK_NULL_HANDLE && vulkan.physicalDevice != VK_NULL_HANDLE &&
	       vulkan.getInstanceProcAddr != nullptr && vulkan.sparseQueue != VK_NULL_HANDLE &&
	       vulkan.transferQueue != VK_NULL_HANDLE;
}

Status checked(const Config& config) {
	const bool device = config.device != nullptr;
	const bool queue  = config.queue != nullptr;
	if (device != queue) {
		return Status::InvalidArgument;
	}
	if (wantsVulkan(config) && (device || !completeVulkan(config.vulkan))) {
		return Status::InvalidArgument;
	}
	return Status::Ok;
}

std::vector<Range> merged(const std::vector<Change>& changes, ChangeReason reason) {
	std::vector<Range> ranges;
	for (const Change& change : changes) {
		if (change.reason == reason && change.range.bytes != 0) {
			ranges.push_back(change.range);
		}
	}
	std::sort(ranges.begin(), ranges.end(),
	          [](const Range& a, const Range& b) { return a.start < b.start; });
	std::vector<Range> out;
	for (const Range& range : ranges) {
		if (!out.empty() && range.start <= out.back().end()) {
			out.back().bytes = std::max(out.back().end(), range.end()) - out.back().start;
		} else {
			out.push_back(range);
		}
	}
	return out;
}

} // namespace

struct Context::Impl {
	Microsoft::WRL::ComPtr<ID3D12Device>       device;
	Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
	Config                                     config;
	Callbacks                                  callbacks;
	std::unique_ptr<PhysicalMemory>            physical;
	std::unique_ptr<Loads>                     loads;
	std::unique_ptr<Pages>                     pages;
	std::unique_ptr<GpuPageTable>              gpuPages;
	std::unique_ptr<CardView>                  cardView;
	std::unique_ptr<Census>                    census;
	std::mutex                                 changesLock;
	std::vector<Change>                        changes;
};

std::unique_ptr<Context> Context::create(const Config& config, const Callbacks& callbacks,
                                         Status* status) {
	const Status valid = checked(config);
	if (valid != Status::Ok) {
		report(status, valid);
		return nullptr;
	}
	if (wantsVulkan(config)) {
		report(status, Status::NotSupported);
		return nullptr;
	}
	std::unique_ptr<Context> context(new Context(config, callbacks));
	if (CardView* cardView = context->cardView()) {
		const Status started = cardView->start();
		if (started != Status::Ok) {
			report(status, started);
			return nullptr;
		}
	}
	report(status, Status::Ok);
	return context;
}

Context::Context(const Config& config, const Callbacks& callbacks)
    : m_impl(std::make_unique<Impl>()) {
	m_impl->device    = config.device;
	m_impl->queue     = config.queue;
	m_impl->config    = config;
	m_impl->callbacks = callbacks;
	m_impl->physical.reset(new PhysicalMemory(*this));
	m_impl->loads.reset(new Loads(*this));
	m_impl->pages.reset(new Pages(*this));
	m_impl->gpuPages.reset(new GpuPageTable(*this));
	m_impl->census.reset(new Census(*this));
	if (config.device != nullptr || wantsVulkan(config)) {
		m_impl->cardView.reset(new CardView(*this));
	}
}

Context::~Context() {
	m_impl->census.reset();
	m_impl->cardView.reset();
	m_impl->gpuPages.reset();
	m_impl->pages.reset();
	m_impl->loads.reset();
	m_impl->physical.reset();
}

PhysicalMemory& Context::physical() {
	return *m_impl->physical;
}

GpuPageTable& Context::gpuPages() {
	return *m_impl->gpuPages;
}

Pages& Context::pages() {
	return *m_impl->pages;
}

Loads& Context::loads() {
	return *m_impl->loads;
}

CardView* Context::cardView() {
	return m_impl->cardView.get();
}

Census& Context::census() {
	return *m_impl->census;
}

const Config& Context::config() const {
	return m_impl->config;
}

const Callbacks& Context::callbacks() const {
	return m_impl->callbacks;
}

void Context::deliverChanges() {
	std::vector<Change> changes;
	{
		std::lock_guard guard(m_impl->changesLock);
		changes.swap(m_impl->changes);
	}
	const Callbacks& callbacks = m_impl->callbacks;
	if (callbacks.pagesChanged == nullptr || changes.empty()) {
		return;
	}
	for (ChangeReason reason : kReasons) {
		const std::vector<Range> ranges = merged(changes, reason);
		if (!ranges.empty()) {
			callbacks.pagesChanged(callbacks.context, ranges.data(), ranges.size(), reason);
		}
	}
}

uint32_t Context::lastHostError() {
	return detail::lastHostError();
}

void Context::log(LogLevel level, const char* text) const {
	if (m_impl->callbacks.log != nullptr) {
		m_impl->callbacks.log(m_impl->callbacks.context, level, text);
	}
}

void Context::queueChange(Range gpuRange, ChangeReason reason) {
	std::lock_guard guard(m_impl->changesLock);
	m_impl->changes.push_back(Change {gpuRange, reason});
}

} // namespace deunify
