#include "gpuBackend.hpp"

#include "d3d12Backend.hpp"

#include <map>

namespace deunify::gpu {

namespace {

void addRun(std::vector<Run>& runs, uint64_t slot, std::optional<uint32_t> tile) {
	if (!runs.empty()) {
		Run&       last = runs.back();
		const bool follows =
		    last.slot + last.count == slot && last.tile.has_value() == tile.has_value();
		if (follows && (!tile || *last.tile + last.count == *tile)) {
			++last.count;
			return;
		}
	}
	runs.push_back(Run {slot, 1, tile});
}

} // namespace

std::vector<BindGroup> bindGroups(std::span<const SlotMove> moves) {
	std::map<uint32_t, std::vector<Run>> byBlock;
	std::vector<Run>                     unbound;
	for (const SlotMove& move: moves) {
		if (move.tile) {
			addRun(byBlock[move.tile->block], move.slot, move.tile->index);
		} else {
			addRun(unbound, move.slot, std::nullopt);
		}
	}
	std::vector<BindGroup> groups;
	for (auto& [block, runs]: byBlock) {
		groups.push_back(BindGroup {block, std::move(runs)});
	}
	if (unbound.empty()) {
		return groups;
	}
	if (groups.empty()) {
		groups.push_back(BindGroup {std::nullopt, std::move(unbound)});
	} else {
		groups.front().runs.insert(groups.front().runs.end(), unbound.begin(), unbound.end());
	}
	return groups;
}

std::unique_ptr<Backend> makeBackend(const Config&                   config, const Callbacks&,
                                     std::shared_ptr<detail::Ledger> ledger) {
	return makeD3d12Backend(config.device, config.queue, std::move(ledger));
}

} // namespace deunify::gpu
