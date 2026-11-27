// A card for the tests that need one, on each backend: WARP for D3D12. Copies in and out of
// the card view's buffers go on the queue the card view uses.

#pragma once

#include "deunify/cardView.hpp"
#include "deunify/context.hpp"
#include "warp.hpp"

#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <vector>

namespace deunify::test {

enum class Api {
	D3d12,
};

inline const Api kApis[] = {Api::D3d12};

inline std::string apiName(const ::testing::TestParamInfo<Api>& info) {
	switch (info.param) {
		case Api::D3d12: return "D3D12";
	}
	return "Unknown";
}

class TestCard {
public:
	virtual ~TestCard() = default;

	// Why this card can't hold a card view; empty if it can.
	virtual std::string missing() const = 0;

	virtual Config config() const = 0;

	// `at` counts from the start of the view's buffer, not from the view.
	virtual void store(const BufferView& view, uint64_t at, const std::vector<uint8_t>& bytes) = 0;
	virtual std::vector<uint8_t> fetch(const BufferView& view, uint64_t at, uint64_t bytes)    = 0;

	// Waits until the queue is idle.
	virtual void drain() = 0;
};

class WarpCard final: public TestCard {
public:
	std::string missing() const override {
		return m_warp.tiled() ? std::string {} : "WARP without tiled resources tier 2";
	}

	Config config() const override {
		Config config;
		config.device = m_warp.device.Get();
		config.queue  = m_warp.queue.Get();
		return config;
	}

	void store(const BufferView& view, uint64_t at, const std::vector<uint8_t>& bytes) override {
		m_warp.store(view.resource, at, bytes);
	}

	std::vector<uint8_t> fetch(const BufferView& view, uint64_t at, uint64_t bytes) override {
		return m_warp.fetch(view.resource, at, bytes);
	}

	void drain() override { m_warp.drain(); }

private:
	Warp m_warp;
};

inline std::unique_ptr<TestCard> makeTestCard(Api api) {
	switch (api) {
		case Api::D3d12: return std::make_unique<WarpCard>();
	}
	return nullptr;
}

} // namespace deunify::test
