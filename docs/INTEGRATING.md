# Integrating deunify

This is for whoever wires deunify into an emulator. Read ARCHITECTURE.md first. The model and
its invariants are the contract; the headers in `include/deunify/` add what each call needs from
you, and stay quiet where it needs nothing. deunify is a reference implementation. Change it to
fit your engine where you must, but keep the invariants. Most of them are there because breaking
them cost us something (LESSONS.md has the receipts).

## What it replaces

Your emulator's mirror of title memory: the RAM copies watched for CPU writes, the uploads of
whole ranges to the card, the read-backs of GPU writes, and the commit of every physical page at
map time. It does not replace your renderer, texture cache, shader translation or file system.
It also doesn't cover the title's committed engine heaps, only physical pages (CONSOLE.md says
what we did with those).

## Where each call goes

| Emulator function | deunify call | Notes |
|---|---|---|
| Start of the title process | `Context::create(config, callbacks)` | `config.device`/`queue`: the D3D12 device and the queue title work is submitted to; or `config.vulkan` (see "On Vulkan"); or neither, for no card view |
| Reserve of the title address space | (emulator) placeholders with `VirtualAlloc2(MEM_RESERVE_PLACEHOLDER)` | deunify maps views into placeholders; it does not reserve address space |
| Allocate physical pages | `PhysicalMemory::allocate` | numbers returned are the title's page numbers |
| Free physical pages | `PhysicalMemory::free` | |
| Map physical pages at an address | `PhysicalMemory::map` | |
| Scatter-map physical pages | `PhysicalMemory::mapScatter` | |
| Unmap | `PhysicalMemory::unmap` | |
| Title allocator metadata (CPU-only, GPU-only, shared memory) | `Pages::setKind` | unknown kinds keep loads in RAM: declare GPU-only memory or nothing is saved |
| Address ranges where the GPU sees what the CPU sees | `GpuPageTable::followCpuViews` | at start-up |
| GPU-only address ranges (tiled heaps, pools mapped for the GPU) | `GpuPageTable::addGpuRange`, then `CardView::addSpan` | |
| The queue's page-mapping copy | `GpuPageTable::copyPageMappings` | on the queue thread, at that point of the submission order |
| DirectStorage: open a file | `Loads::openFile` | |
| DirectStorage: a request with a file source | `Loads::load` | console codecs other than zlib go through `Callbacks::decode` |
| DirectStorage: a request with a memory source | `Loads::write` | |
| Vectored exception handler | `Pages::handleFault` first | return `EXCEPTION_CONTINUE_EXECUTION` when it returns true |
| Kernel file reads into title memory (`ReadFile`, sockets) | `KernelWrite` around the call | `Access::Read` for kernel reads from title memory (`WriteFile`) |
| Renderer: a buffer view (SRV, UAV, CBV, raw address) of title memory | `CardView::bufferFor` | fall back to the emulator's own path on `TooLarge` or `NotFound` |
| Renderer: a view shaders may write | `CardView::shaderWrites` | every UAV and copy destination over title memory |
| Queue: before `ExecuteCommandLists` or `vkQueueSubmit` of title work | `CardView::beforeSubmit` | without a card view: `Pages::takeWritten` for your uploads and `Context::deliverChanges` |
| Texture cache: read bytes to make a texture | `GpuPageTable::read(..., ReadPurpose::TextureUpload)` | never read title memory at its address for this |
| Texture cache: invalidation | `Callbacks::pagesChanged` | |
| Emulator reads of title memory for its own use | `Pages::readTitle` or `GpuPageTable::read` | reading at the title address works too, but brings the page to RAM |
| Memory statistics | `Census::snapshot`, `Census::format` | track your own heaps (or `VkDeviceMemory`) with `Census::track` to see the whole card |
| Title code patches | `patch::Sites`, `patch::redirectCall` | sites files stay in the emulator project |

C users get the same through `include/deunify.h`, and Rust users raw declarations of it in
`bindings/rust/deunify-sys`.

## On Vulkan

Many emulators translate the console's shaders to SPIR-V and render with Vulkan. The card view
works there too. Everything in this file holds, with these additions.

**What to pass.** `Config::vulkan`, and leave `device` and `queue` null. The instance, the
physical device, the device and `vkGetInstanceProcAddr`; deunify loads every other function
itself and never links `vulkan-1.lib`. Then two queues:

- `transferQueue` is the queue your title work over card-view memory runs on. Fills and
  read-backs go there, and that is what orders them against the title's work, as
  `Config::queue` does on D3D12. Its family needs graphics, compute or transfer. Passing some
  other queue loses that order.
- `sparseQueue` is any queue whose family has `VK_QUEUE_SPARSE_BINDING_BIT`. It may be the same
  queue. On most desktop cards the graphics family has the bit.

A `VkQueue` takes one thread at a time, and a read-back submits from whichever thread touched
the page. If anything else submits to these queues, set `Callbacks::queueLock` and take your
own lock in it.

**Features and extensions.** Vulkan 1.2 or later, with `sparseBinding`,
`sparseResidencyBuffer` and `bufferDeviceAddress` enabled when you create the device.
`Context::create` says `NotSupported` if the physical device lacks them, and we can't tell
whether you enabled them, so do. Enable `timelineSemaphore` too and set
`VulkanSetup::timelineSemaphores`. Without it every batch of bindings is waited for on the
CPU, which works and costs a stall per submission that moves tiles. `VK_EXT_memory_budget`
only has to be supported by the device for the census to read the system's budget; without
it the census shows heap sizes and its own counts, and says so. Track your own allocations
with `Census::track(VkDeviceMemory, ...)` and untrack them before you free them.

**Views.** `bufferFor` gives a `VkBuffer`, an offset in it and the device address of that
offset. A descriptor takes the buffer, the offset and `bytes`. A view never crosses the end of
its buffer, and the buffers fit `maxStorageBufferRange`. The offset is wherever the title's view
starts, so mind `minStorageBufferOffsetAlignment`: round down and add the rest in the shader,
as you would without deunify.

**Console pointers.** Translated RDNA2 code reaches memory through 64-bit addresses (the base
of a buffer resource, scalar loads from raw addresses), and the usual way to keep that on
Vulkan is a device address plus an offset. `CardView::spanAddress` gives each span as one flat
range: GPU address `gpuBase + n` is device address `deviceAddress + n`. Hand the shader
`deviceAddress - gpuBase` once per span (a push constant does it) and add it to the console
pointer; the wrap-around of 64-bit addition takes care of the sign. Only pages that got tiles
are backed, so ask `bufferFor` for what a draw or a dispatch reads before you submit it, as you
would for a descriptor. The flat range is one more sparse buffer, bound like the segments, so
it costs address space and bindings. A span that fits in one segment uses that segment for it.

**Without `residencyNonResidentStrict`.** Then a read of a slot that holds no tile is
undefined: zeros, stale bytes of another tile, or anything else. `CardView::unboundReadsZero`
tells you which kind of device you have. The driver we tested on has the property; check
yours. deunify binds nothing in place of a missing tile. A slot has no tile when nobody asked
for it or when the title's page table maps nothing there. The first is yours to avoid: ask for
every range a shader can read. For the second, if the title reads unmapped memory and counts
on zeros, give it a page of zeros: allocate one physical page, never write it, and map it into
those slots with `GpuPageTable::map` (in a GPU-only range). Every slot showing it shares one
tile. Never do that where shaders may write.

## Rules that don't bend

- One home per page. Never keep a second copy "just in case". Just-in-case copies are how
  the mirror grows back.
- Never read a page's RAM when its home is elsewhere. Read through `GpuPageTable::read` or
  `Pages::readTitle`; a direct read at the title address faults and moves the page.
- Every kernel call that touches title memory is inside a `KernelWrite` bracket. The kernel
  doesn't fault on a closed page, it fails, and the title gets the failure.
- Respect the lock order in `context.hpp`. Never call deunify from `Callbacks::log`. Never call
  `Pages::handleFault` while holding a lock that a deunify call on another thread might wait for.
- `CardView::beforeSubmit` runs on the submitting thread right before the title's lists go to
  `Config::queue` (on Vulkan, the transfer queue). Work on other queues that touches card-view
  memory waits for that queue.
- The library never touches title addresses itself. Keep it that way when you adapt it.

## Bring-up order

Each step builds, passes its tests and is measured before the next one starts. Doing three at
once saves a day and costs a week.

1. Physical memory and views through `PhysicalMemory`, card view off, kinds unset. Measure
   commit charge at boot; it should drop by roughly the size of the pools.
2. Fault handler and lazy commit (`Pages::handleFault`), kernel brackets. Measure committed
   pages after the first scene and faults per second.
3. Loads through `Loads::load`, kinds declared (`Pages::setKind`). Measure pages by home
   (`HomeCounts`), staged bytes and replays per second.
4. GPU page table (`GpuPageTable`), still without a card view, so GPU mappings become RAM views.
   Check every scene still looks right, and measure mapping time per frame.
5. Card view: `CardView::addSpan`, `bufferFor`, `shaderWrites`, `beforeSubmit`, and texture
   reads through `GpuPageTable::read`. Measure bytes over the bus per frame, read-backs per
   frame, card memory against the system budget (census), frame time and time spent waiting for
   the card.
6. Census: what is still held twice (`ramTiled`, tiles under textures), and what your own heaps
   hold.

## Pitfalls

The short version of LESSONS.md, for people who won't read LESSONS.md.

- Don't hold the page lock while waiting for the card. A read-back waits for everything queued,
  and the submitting thread needs that lock every submission. It cost us most of a frame.
- Close a page's views before you take its bytes, and set the armed bit before a view goes
  read-only (clear it after it is writable again). Both gaps lose writes or crash the title,
  rarely enough to be blamed on the title.
- Let `Pages::handleFault` see every access violation, including after your own handler waited
  out a remap.
- Pass lengths where lengths are expected, and test every range loop with a huge length. One
  underflow froze the whole title under a lock.
- Decode each load once. Count requests decoded against requests loaded.
- Don't ask the kernel what you already know. `GetWriteWatch` refuses views, uncommitted pages
  look like holes, and `VirtualQuery` on a multi-gigabyte region walks it to the end.
- Every writable view a shader can pick at run time goes through `shaderWrites`. Ask for tiles by
  the view's own range, never the whole buffer.
- A load into a Card page goes to its tile. A partial load into a page the CPU wrote stays in RAM.
- Track your texture allocator in the census. Ours sat on empty blocks and ate the gain.

## Measure before you blame

When something breaks after you wire deunify in, it is tempting to blame deunify, because it is
the new thing. Sometimes that's right. The one time we were sure it was, a shader translator
change had refused hundreds of shaders and the memory layer was innocent. Two habits would have
saved us the most time.

**Per-frame counters, in every build, the baseline included.** A counter that only exists on the
new path can't be compared with anything. Print one line a frame, or one a second, with at least:
frame time and draws per frame; shaders refused; the share of the frame the card was busy; the
submitting thread's time split into prep work (taking CPU writes, tile mapping changes, fills)
and waiting for the card; faults, read-backs and replays since the last line (`HomeCounts`);
requests loaded and decoded (`Loads::Totals`); fill bytes and mapping calls
(`CardView::Totals`); and `VirtualQuery` calls, if your emulator makes any. Add a census line
once a second. Compare runs at the same point in the same scene with the same script, one
emulator at a time. A scene usually takes a few minutes to settle, and earlier readings compare
badly with anything.

Memory needs one warning of its own: commit of a pagefile-backed section does not show in a
process's private bytes. Use the system commit charge, free RAM and
`PhysicalMemory::Totals::committed`, or you will report savings that aren't there, or miss the
ones that are.

**Stack samples of the submitting thread.** Counters tell you that the frame got slower, and
stacks tell you why. A sampling profiler with stacks (Windows Performance Recorder, or whatever
you like) does it. So does a small in-process sampler: a thread that every few milliseconds
suspends the submitting thread, reads its context, unwinds it and counts the top frames, then
prints the ten most common once a second. Don't allocate or take locks in the sampler while the
other thread is suspended, or you'll deadlock on the heap lock it happens to hold, and then you'll
have a new story for LESSONS.md. These signatures are worth knowing:

- the submitting thread waiting on a lock whose owner is a fault handler waiting on a fence: a
  read-back under a lock;
- frames ending in `NtQueryVirtualMemory`: the kernel being asked what you already know;
- inflate or the console codec on the submitting thread: something decodes per page;
- far more `UpdateTileMappings` calls than heaps and buffers per submission: mapping changes not
  batched.

For a hang, take every thread's stack before you kill the process (a full dump, or the debugger's
all-threads stack command). That is how we found the freeze in LESSONS.md, once we stopped
guessing.
