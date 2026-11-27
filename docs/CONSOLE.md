# What Xbox Series titles do with memory

This is what matters for deunify, written from what is public and from what open-source
emulators document. Where we say "observed in a shipping title", one big open-world title did it
in one build and we watched it happen. Treat every such line as a lead. Your title may do the
same, something close, or nothing of the kind, and the only way to know is to look. We say
"worth checking" a lot below. We mean it every time.

The console has one memory. The CPU and the GPU read and write the same bytes at the same
virtual addresses, GPU addresses stay below 1 TiB, and nothing needs copying from one side to
the other. A title written for that machine has no reason to be careful about who touches what,
and most aren't. The interesting part is that the engines that ship on it often know exactly
who touches what anyway, because the platform makes it cheap to say so.

## Physical pages, mapped twice

Titles allocate physical memory in 64 KiB pages and map those pages wherever they like, as
often as they like. A page is named by the frame number of its first 4 KiB frame, so page
numbers are non-zero multiples of 16 below 2^25. A title may pack them into 21 bits by shifting
right by 4 and rely on that. Observed in a shipping title: numbers handed out lowest-first and
reused after a free, and the title relying on both. That is why `PhysicalMemory::allocate` hands
out the lowest free numbers and why a number is reused only after its page's last view is gone.

The common pattern for streaming is a pool. Observed in a shipping title: a few gigabytes of
physical pages allocated in one call at boot, behind a much larger address range of 64 KiB
slots, shown twice. One copy of the range is the CPU view, the other the GPU view.
An allocator hands out blocks of slots and decides, per block, which views get a mapping. The
CPU view is updated with ordinary map calls, usually the scatter form. The GPU view is updated
some other way (next section).

The same title also mapped the entire pool once at boot, in order, to register it with the
graphics driver as a page pool. If your title does that, worth checking whether anything ever
reads through that mapping. In the one we looked at nothing did; the base address was used as
a number and never dereferenced. An emulator whose map call commits pages turned that mapping
into gigabytes of committed RAM before the first asset loaded. deunify never commits at map
time, which makes this harmless. If your own memory code commits at map, see TITLE-PATCHES.md.

## The GPU page table

On the console, a title can change what the GPU sees without touching the CPU's page tables.
It does so through the queue: a page-mapping copy that takes runs of destination pages, each run
consecutive pool pages, one pool page repeated, unmapped, or left as it was. It executes in
queue order, between the title's command lists, so a mapping change only takes effect for work
submitted after it.

Open-source emulators that implement this usually turn it into host views at the GPU
addresses, which costs a view per run and a RAM copy of everything the GPU can see. If your
title's GPU view arrives through the queue's page-mapping copy instead of the CPU's map call,
that is your GPU page table on a plate. Feed it to `GpuPageTable::copyPageMappings` at the
point in your submission order where the title put it, and with a card view no RAM view is made
at all.

## Who touches what

Engines that use a pool tend to classify every block: mapped in both views (the CPU and the
GPU both use it), CPU view only, or GPU view only. GPU-only memory has no CPU address. Title code
cannot read it, so nothing on the CPU side ever has to see those bytes. CPU-only memory is never
seen by the GPU.

In the off chance your title's engine tags every chunk of its streaming pool as CPU-only,
GPU-only or both, believe it. Worth checking, and cheap: break where the allocator picks the
views, or read the per-slot entries it keeps. Declare the result with `Pages::setKind`. Without
it, deunify treats memory as CPU-reachable and loads go to RAM, which is correct and saves
nothing.

What "both" means in practice is another matter. Observed in a shipping title, in its opening
scene: the three classes were of the same order of size, none of them small. Once the scene
settled we counted CPU touches on the "both" slots. About three quarters of them the CPU never
touched at all, most of the rest it only read, and it wrote very few. "Both" mostly meant "the
GPU, and the CPU now and then". That is the case deunify's File home is for. A page that nobody
on the CPU side touches stays in its load records, and the rare touch rebuilds it. It is also a
hint that some of those chunks could have been GPU-only all along (TITLE-PATCHES.md).

## Loads into page lists

The console's DirectStorage can take a request whose destination is a list of physical pages
rather than an address, and open-source emulators that implement it document such requests.
Observed in a shipping title: graphics chunks of the streaming pool are loaded exactly that way,
into the page list behind the block, so the bytes land in pages that may have no CPU view at all.

This is the best news in the whole document. A request names a file, an offset and a codec, so
a page written only by requests can be rebuilt from them at any time. deunify records them
(`Loads::load`) and keeps such pages out of RAM until something on the CPU side touches them. The
console's own codecs other than zlib are the emulator's business, through `Callbacks::decode`.

## Engine heaps: write-combined and cached

Besides the pool, titles commit large heaps up front with GPU access and pick a memory type
for each. Write-combined memory is for data the CPU writes and the GPU reads: per-frame
constants, dynamic vertex data, skinning output. Cached memory is the engine's general memory,
and it also holds GPU resources whose flags put them there. Observed in a shipping title on the
Series S profile: one large write-combined heap and one large cached heap, never decommitted or
reprotected inside, released only as whole allocations.

These heaps are ordinary committed memory, not pool pages, and deunify's page model does not
cover them. The right home for CPU-written per-frame data on a PC is RAM that the card reads
over the bus, which is what an upload heap is. The emulator we built this in opened host heaps
directly on the title's memory (`OpenExistingHeapFromAddress`) and placed buffers in them, so
there was one copy and no watch. Worth checking first that your title never reprotects or
decommits inside such a heap. The one we watched didn't; a host heap whose memory changes under
it is not a fun afternoon.

Constant buffers are a small trap in there. The console accepts constant buffer views on 16-byte
boundaries; D3D12 wants 256. A constant buffer that is not on 256 bytes needs a copy, a root
view, or raw loads in the translated shader, and the copy is the one that quietly brings back
the mirror you were removing.

## Transient pages under the render graph

Render graphs alias memory: several resources whose lifetimes don't overlap share the same
physical pages. Observed in a shipping title: a large range of address space reserved in 64 KiB
pages, physical pages mapped into it one at a time, and render targets and UAV buffers placed
over it, with one page under several resources. The CPU barely touches any of it.

On a PC that is one tile per physical page, shared by every address that shows the page. Add
the range as a `CardView::addSpan(..., SpanHome::Card)` span: every page the GPU asks for
becomes Card home at once, and the rare CPU touch reads back.

## UAV buffers next to CPU data

A general allocator does not care that a buffer will be written by shaders. It hands out
16-byte or 256-byte alignment and puts the buffer beside whatever came before it. Then one
64 KiB page holds a buffer that shaders write and data the CPU writes, and the page has no good
single home: every submission uploads what the CPU wrote and every CPU touch reads back what
shaders wrote, in a loop.

If UAV buffers in your title sit on 16-byte boundaries next to CPU data, worth checking for a
flags word nearby that says UAV. Round them to 64 KiB and they move out. A buffer that starts on
a page and covers whole pages can be Card home on its own. TITLE-PATCHES.md has the pattern.

## Descriptor heaps the title writes itself

Console titles may own the memory of their descriptor heaps and write descriptors into it
directly, 32 bytes each (16 for samplers), with GPU virtual addresses of title memory inside.
Shaders then index those heaps at run time. Open-source emulators report shaders that hold a
descriptor table address in 32 bits and zero-extend it, so those heaps have to live in the low
4 GiB. Worth checking where yours put them.

Two things follow for the memory layer. First, the emulator only learns which memory a view
covers when it decodes the descriptor at use, so `CardView::bufferFor` is called from the
renderer, not from a creation hook. Second, a shader can write through any writable view it can
reach in the heap, and nobody tells you which it picked. Every writable view a shader could
pick has to go through `CardView::shaderWrites`, or its writes stay on the card while the CPU
side believes RAM (the missed shader writes in LESSONS.md). Declare the view's own range, never the
whole buffer behind it.

The heap memory itself is CPU-written and GPU-read. Leave it in RAM.

## Things that bite regardless

Some titles take it personally when a frame takes half a minute. Observed in a shipping title:
a watchdog thread that counts one-second sleeps and writes to address zero after about half a
minute without progress on the main thread. A round whose own sleep overran doesn't count, which
is why it never fires while you sit in a debugger and always fires when the emulator stalls by
itself. A lock held across a long read-back counts, and so does a stall while the emulator
compiles shaders on the title's thread. If your title crashes at a null address after a long
hitch, check for a watchdog before you check anything else.

Some titles also upload a crash dump to a server on every crash. While you are making them
crash on purpose, keep the network off.
