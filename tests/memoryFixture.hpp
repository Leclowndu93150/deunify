// A context without a card view, placeholders to map into, and a vectored handler that sends
// faults to Pages the way an emulator would.

#pragma once

#include "deunify/context.hpp"
#include "deunify/gpuPageTable.hpp"
#include "deunify/pages.hpp"
#include "deunify/physicalMemory.hpp"

#include <windows.h>

#include <gtest/gtest.h>

#include <memory>
#include <utility>
#include <vector>

namespace memoryTest {

inline constexpr uint64_t P = deunify::kPageBytes;

inline deunify::Pages* g_faultPages = nullptr;

inline LONG CALLBACK sendFaultToPages(EXCEPTION_POINTERS* exception) {
	if (g_faultPages != nullptr && g_faultPages->handleFault(exception)) {
		return EXCEPTION_CONTINUE_EXECUTION;
	}
	return EXCEPTION_CONTINUE_SEARCH;
}

inline uint8_t readByte(uint64_t address) {
	return *reinterpret_cast<volatile uint8_t*>(address);
}

inline void writeByte(uint64_t address, uint8_t value) {
	*reinterpret_cast<volatile uint8_t*>(address) = value;
}

inline DWORD stateAt(uint64_t address) {
	MEMORY_BASIC_INFORMATION info {};
	VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info));
	return info.State;
}

inline void releasePlaceholders(uint64_t base, uint64_t bytes) {
	uint64_t at = base;
	while (at < base + bytes) {
		MEMORY_BASIC_INFORMATION info {};
		if (VirtualQuery(reinterpret_cast<void*>(at), &info, sizeof(info)) == 0) {
			return;
		}
		at = reinterpret_cast<uint64_t>(info.BaseAddress) + info.RegionSize;
		VirtualFree(info.AllocationBase, 0, MEM_RELEASE);
	}
}

struct Change {
	deunify::Range        range;
	deunify::ChangeReason reason;
};

class Fixture : public ::testing::Test {
protected:
	void SetUp() override {
		deunify::Callbacks callbacks;
		callbacks.context      = &m_changes;
		callbacks.pagesChanged = &Fixture::record;
		deunify::Status status = deunify::Status::HostFailure;
		m_context              = deunify::Context::create({}, callbacks, &status);
		ASSERT_NE(m_context, nullptr);
		ASSERT_EQ(status, deunify::Status::Ok);
		g_faultPages = &m_context->pages();
		m_handler    = AddVectoredExceptionHandler(1, sendFaultToPages);
	}

	void TearDown() override {
		RemoveVectoredExceptionHandler(m_handler);
		g_faultPages = nullptr;
		m_context.reset();
		for (const auto& [base, bytes]: m_reserved) {
			releasePlaceholders(base, bytes);
		}
	}

	uint64_t reserve(uint64_t pages) {
		void* base = VirtualAlloc2(nullptr, nullptr, pages * P, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
		                           PAGE_NOACCESS, nullptr, 0);
		EXPECT_NE(base, nullptr);
		m_reserved.emplace_back(reinterpret_cast<uint64_t>(base), pages * P);
		return reinterpret_cast<uint64_t>(base);
	}

	std::vector<deunify::PageNumber> allocate(uint64_t count) {
		std::vector<deunify::PageNumber> pages(count);
		EXPECT_EQ(physical().allocate(count, 0, pages.data()), deunify::Status::Ok);
		return pages;
	}

	deunify::PhysicalMemory& physical() { return m_context->physical(); }
	deunify::GpuPageTable&   gpu() { return m_context->gpuPages(); }

	std::vector<Change> delivered() {
		m_changes.clear();
		m_context->deliverChanges();
		return m_changes;
	}

	static bool covered(const std::vector<Change>& changes, uint64_t address,
	                    deunify::ChangeReason reason) {
		for (const Change& change: changes) {
			if (change.reason == reason && address >= change.range.start &&
			    address < change.range.end()) {
				return true;
			}
		}
		return false;
	}

	std::unique_ptr<deunify::Context> m_context;

private:
	static void record(void* context, const deunify::Range* ranges, size_t count,
	                   deunify::ChangeReason reason) {
		auto* changes = static_cast<std::vector<Change>*>(context);
		for (size_t i = 0; i < count; ++i) {
			changes->push_back(Change {ranges[i], reason});
		}
	}

	void*                                    m_handler = nullptr;
	std::vector<std::pair<uint64_t, uint64_t>> m_reserved;
	std::vector<Change>                      m_changes;
};

} // namespace memoryTest
