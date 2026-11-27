# Title patches

deunify can only give a page one home if the title lets it. Most of the time it does: the
platform makes titles say a lot about their memory, and an emulator that implements the platform
hears it. Sometimes the title says something unhelpful. It puts a buffer that shaders write in
the same 64 KiB page as data the CPU writes. Or it marks memory as shared that the CPU never
looks at. For those cases we patch the title's code in memory at load.

A patch changes how the title does something real. It never fakes a result, hides an error or
skips work the title needs. If you catch yourself writing a patch that returns success, stop and
implement the thing instead. Your future self, debugging a black screen at 2am, will thank you.

Nothing here contains addresses, and nothing in this repository ever should. The patterns are
generic. Where they come from one shipping title we say so, and you should treat it as a lead.
Your title may not do any of this. Worth checking before you write a single byte.

## The patterns

### UAV buffers get pages of their own

Engines allocate GPU buffers from a general allocator that hands out small alignment, often 16
or 256 bytes. A buffer that shaders write ends up sharing a 64 KiB page with data the CPU
writes, and that page ping-pongs: each submission uploads the CPU's writes, each CPU touch reads
back the shaders' writes, and the read-back waits for the queue. See CONSOLE.md, "UAV buffers
next to CPU data".

The fix is at the engine's allocator. The call that allocates a GPU buffer usually has the
buffer's flags at hand, and one of the bits says unordered access. When it is set, round the
size up to 64 KiB and raise the alignment to 64 KiB. The buffer then starts on a page and covers
whole pages, and those pages can be Card home without dragging CPU data along. A small stub in
front of the allocator does it. There is an example at the end of this file.

If UAV buffers sit on 16-byte boundaries next to CPU data in your title, worth checking for a
flags word nearby that says UAV. In the title we patched, the flags were available at the
allocator call, so the stub could decide per buffer. Rounding costs up to 64 KiB per buffer,
which is nothing for a few hundred large buffers and very noticeable for ten thousand small
ones. Count them before you decide.

### The boot mapping of the whole pool must not commit it

A title may map its entire physical pool once at boot just to register it with the graphics
driver. Observed in a shipping title: nothing ever read through that mapping, but an emulator
whose map call committed pages committed the whole pool there, gigabytes, before the first load.

With deunify there is nothing to patch. `PhysicalMemory::map` never commits, and a page is
committed only when the CPU touches it or a load writes it into RAM. If your own memory code
commits at map time and can't tell this mapping from a real one, the patch is to skip committing
at that one call site, not to skip the mapping. The title may still use the base address as a
number (the one we looked at did), and the GPU registration must still happen.

To see whether you need it, read committed pages right after boot (`PhysicalMemory::Totals`, or
the system commit charge). Process private bytes will lie to you here: commit of a pagefile-backed
section does not show up in them.

### Graphics chunks classified GPU-only where the engine marks them shared

Engines that classify their pool (CPU view only, GPU view only, both) are sometimes generous
with "both". Observed in a shipping title: once the opening scene settled, the CPU had left
about three quarters of the "both" slots untouched. deunify handles that without a patch: shared
pages that were loaded stay File home until touched. A patch can still go further. If big
graphics chunks are classified GPU-only at the allocator, they get no CPU view at all, the CPU
view's page table shrinks, and nobody needs to wonder whether a touch is coming.

This is the riskiest of the four and the one we have not applied. In the title we studied, the
chunk class is picked by one small comparison in the allocator, and flipping it is a byte or two.
But the same path also serves textures the title creates and fills on the CPU at run time, and
those must stay shared. So the patch needs a gate: only on the streaming path, only for chunks
loaded from files. Without the gate, the first runtime texture the CPU writes hits an unmapped
address. Measure the touch map first (see below) and patch only what it proves untouched.

### Engine hooks to take over memory pools

deunify needs three facts from the emulator: the class of each address range (`Pages::setKind`),
the bounds of the allocation around an address (`Callbacks::regionOf`), and the pool's GPU range
(`CardView::addSpan`). Get them from your own implementation of the platform's memory calls
first. Your emulator sees the pool being allocated and both views being mapped, and the platform
lets titles install allocation hooks that your implementation then calls for every allocation.

When the facts live only in the engine's own allocator (block bounds and per-slot classes in a
table it keeps), hook the allocator. The simplest hook is a redirected call whose stub is a
single absolute jump into your emulator. Your function has the allocator's signature, calls the
original (`Redirect::originalTarget`), looks at what came back, records it, and returns it
unchanged. That is a patch in the sense above: the title still allocates exactly what it asked
for, and you just get to watch. Once you trust the hook, the same place lets you take the pool
over, for example by keeping GPU-only blocks in a range of their own.

The earlier implementation once read the engine's allocator tables straight out of title
memory from a separate module. It worked and we would not do it again. It needed the table
addresses for one build, its touch-map tool raced with the allocator and ended runs, and the
emulator's own memory code knew most of the same things already.

## Finding the sites

Start from the platform calls your emulator already implements and walk up. Put a breakpoint on
the call with the distinctive arguments and look at the title's stack:

- the physical-page allocation whose size matches the pool, and the map call that maps every
  page of it in order, for the boot mapping;
- the queue's page-mapping copy, for the GPU view and the pool base it reads from;
- placed buffer creation at a title address with the UAV flag set, for the UAV allocator (two or
  three frames up there is usually a call into the engine's allocator with a flags argument);
- DirectStorage requests whose destination is a page list, for the streaming path and its chunk
  classes.

deunify's fault path doubles as a touch map. Declare the pool's shared ranges as `Kind::Shared`,
let a long session run, and every loaded page the CPU ever touched has faulted once and left File
home. `Pages::state` on the rest tells you what the CPU never looked at. Run it longer than you
think you need to; the title will touch something on the loading screen of the second chapter
that it never touched in the first.

Record each site as a name and an offset in a sites file that lives with your emulator, one file
per title build (`patch::Sites`). If you need to survive builds, look the site up by bytes with
`patch::matches` and a mask, and keep the pattern in the same file.

## Verifying at run time

Every patch checks the code it replaces before it writes anything. `patch::redirectCall` does it
for you: the bytes at the site must be a `call rel32`, and its target must be the one you expected,
or it returns `Status::Refused` and changes nothing. A refusal means the build differs from the one
your sites file describes. Log it loudly and run without the patch. Never patch a guess.

Then check that the patch does what you think, with numbers:

- UAV buffers: every buffer created with the UAV flag starts on a 64 KiB boundary and covers
  whole pages; read-backs per frame (`HomeCounts::readBacks`) and fill bytes per submission
  (`CardView::Totals::fillBytes`) drop.
- Boot mapping: committed pages right after boot are close to zero.
- GPU-only chunks: no access violations at unmapped addresses in the pool's CPU view, over a long
  session, including the parts of the game you didn't think to test.
- Hooks: the regions the hook reports match `PhysicalMemory::viewsIn` over the same addresses.

## The helpers

`include/deunify/patch.hpp` has what we needed and no more.

- `patch::Sites` parses a sites file: `name hex-offset` per line, `#` comments.
- `patch::Image` is the loaded title image (base and size).
- `patch::allocateNear` finds executable memory within rel32 reach of an address, for stubs.
  `releaseNear` frees it, `protectExecutable` seals it.
- `patch::writeCode` overwrites title code and restores its protection. Only call it while no
  title thread can be running those bytes, which in practice means at load, before the entry
  point.
- `patch::matches` compares bytes under a mask. `patch::rel32` computes a displacement if it
  reaches.
- `patch::StubBuilder` assembles a stub from raw bytes, short forward branches with labels, and
  an absolute jump (`FF 25` followed by the 64-bit target, 14 bytes, any distance).
- `patch::redirectCall` verifies a `call rel32`, copies your stub next to the image, seals it and
  points the call at it.

Here is the UAV rounding as a stub. The register assignment is an assumption: in this example
the allocator takes the size in `rdx`, the alignment in bytes in `r8` and the flags in `r9d`.
Read your allocator before you trust any of that. The stub also sets the alignment to exactly
64 KiB, which is wrong if the allocator was ever asked for more. Check that too.

```cpp
#include <deunify/patch.hpp>

using namespace deunify;

constexpr uint32_t kUavFlag = 0;  // the title's bit, found by reading its allocator

static constexpr uint8_t kTestFlags[] = {0x41, 0xF7, 0xC1};  // test r9d, imm32
static constexpr uint8_t kRoundSize[] = {
	0x48, 0x81, 0xC2, 0xFF, 0xFF, 0x00, 0x00,  // add rdx, 0xFFFF
	0x48, 0x81, 0xE2, 0x00, 0x00, 0xFF, 0xFF,  // and rdx, -0x10000
};
static constexpr uint8_t kAlignment[] = {0x41, 0xB8, 0x00, 0x00, 0x01, 0x00};  // mov r8d, 0x10000

Status roundUavBuffers(const patch::Image& image, const patch::Sites& sites)
{
	std::optional<uint64_t> call   = sites.offset("uav_buffer_allocation_call");
	std::optional<uint64_t> target = sites.offset("general_allocator");
	if (!call || !target)
		return Status::NotFound;

	patch::StubBuilder stub;
	stub.bytes(kTestFlags).dword(kUavFlag);
	size_t notUav = stub.shortBranch(0x74);  // jz
	stub.bytes(kRoundSize).bytes(kAlignment);
	stub.label(notUav).jumpAbsolute(reinterpret_cast<uint64_t>(image.base) + *target);

	patch::Redirect redirect;
	return patch::redirectCall(image, *call, *target, stub.code(), &redirect);
}
```

The stub is entered through the title's own `call`, so the return address is already on the
stack. It must not touch `rsp` and must end in a jump, never a call. It clobbers the flags
register, which no calling convention asks a callee to preserve.

Sites files are yours. Keep them in your emulator's repository, or better, out of every
repository. deunify ships none and knows no title.
