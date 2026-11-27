// What the card view and the census need from a graphics API. The card view engine talks to
// this and nothing else; d3d12Backend and vulkanBackend implement it.
//
// Words: a region is a reserved (D3D12) or sparse (Vulkan) buffer over part of a span; a block
// is kHeapTiles tiles of card memory; a slot is 64 KiB of a region. Timeline values grow with
// every submission and complete in order.

#pragma once

#include "deunify/cardView.hpp"
#include "deunify/census.hpp"
#include "deunify/context.hpp"

#include <compare>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace deunify::detail {
class Ledger;
}

namespace deunify::gpu {

/// One 64 KiB tile of a block.
struct Tile {
	uint32_t block = 0;
	uint32_t index = 0;

	auto operator<=>(const Tile&) const = default;
};

/// A slot given a tile, or unbound.
struct SlotMove {
	uint64_t            slot = 0;
	std::optional<Tile> tile;

	bool operator==(const SlotMove&) const = default;
};

/// `count` slots from `slot` on, bound to the tiles from `tile` on in the group's block, or
/// unbound if `tile` is empty.
struct Run {
	uint64_t                slot  = 0;
	uint64_t                count = 0;
	std::optional<uint32_t> tile;

	bool operator==(const Run&) const = default;
};

/// The runs that go up together: those of one block, with the unbindings riding along with the
/// first block's, or alone when no tile is given.
struct BindGroup {
	std::optional<uint32_t> block;
	std::vector<Run>        runs;
};

/// Moves (each slot once) grouped by block in block order; one group is one mapping call.
std::vector<BindGroup> bindGroups(std::span<const SlotMove> moves);

/// Regions are `bytes` long and start every `step`, so any view up to `step` lies wholly in
/// one of them.
struct SegmentShape {
	uint64_t bytes = kSegmentBytes;
	uint64_t step  = kSegmentStep;
};

class Region {
public:
	virtual ~Region() = default;

	virtual BufferView view(uint64_t offset, uint64_t bytes) const = 0;
};

class Block {
public:
	virtual ~Block() = default;
};

/// Mapped for the CPU for as long as it lives.
class HostBuffer {
public:
	virtual ~HostBuffer() = default;

	uint8_t* data  = nullptr;
	uint64_t bytes = 0;
};

/// A recording of copies, reused after the timeline value of its submission completed.
class Commands {
public:
	virtual ~Commands() = default;
};

/// The system's numbers next to what we counted ourselves.
class MemoryReport {
public:
	virtual ~MemoryReport() = default;

	virtual SystemMemory systemMemory(const std::vector<CategoryUse>& counted) const = 0;
};

class Backend: public MemoryReport {
public:
	/// NotSupported if the device can't hold a card view.
	virtual Status start() = 0;

	virtual SegmentShape segmentShape() const = 0;

	/// Whether each span also gets one region over all of it, for a flat device address.
	virtual bool flatSpans() const = 0;

	/// Whether a read of an unbound slot returns zeros.
	virtual bool unboundReadsZero() const = 0;

	virtual Status makeRegion(uint64_t bytes, std::unique_ptr<Region>* region)       = 0;
	virtual Status makeBlock(std::shared_ptr<Block>* block)                          = 0;
	virtual Status makeUpload(uint64_t bytes, std::unique_ptr<HostBuffer>* buffer)   = 0;
	virtual Status makeReadback(uint64_t bytes, std::unique_ptr<HostBuffer>* buffer) = 0;
	virtual Status makeCommands(std::unique_ptr<Commands>* commands)                 = 0;

	/// The rest put work on the queues: call them under the card view's queue order lock.
	/// `blocks` is indexed by BindGroup::block.
	virtual void bind(Region& region, std::span<const BindGroup> groups,
	                  std::span<Block* const> blocks) = 0;
	/// Sends what bind collected, after everything submitted before.
	virtual uint64_t submitBinds()                                                       = 0;
	virtual Status   begin(Commands& commands)                                           = 0;
	virtual void     copyUp(Commands& commands, const Region& to, uint64_t toOffset,
	                        const HostBuffer& from, uint64_t fromOffset, uint64_t bytes) = 0;
	virtual void     copyBack(Commands& commands, const HostBuffer& to, const Region& from,
	                          uint64_t fromOffset, uint64_t bytes)                       = 0;
	virtual Status   end(Commands& commands)                                             = 0;
	virtual uint64_t submit(Commands& commands)                                          = 0;
	virtual uint64_t signal()                                                            = 0;
	/// Title work submitted later on the title's queue starts after everything sent so far.
	virtual void fenceTitleWork() = 0;

	/// Any thread, no lock needed. wait is false if the device is gone.
	virtual uint64_t completed() const          = 0;
	virtual bool     wait(uint64_t value) const = 0;

	/// Whether memory of this Vulkan memory type sits on the card; none on D3D12.
	virtual std::optional<bool> memoryTypeOnCard(uint32_t memoryType) const = 0;
};

/// The backend `config` asks for. Its own allocations count in the ledger's card view
/// category.
std::unique_ptr<Backend> makeBackend(const Config& config, const Callbacks& callbacks,
                                     std::shared_ptr<detail::Ledger> ledger);

} // namespace deunify::gpu
