# Architecture

The console has one memory that the CPU and the GPU share; a PC has two and a bus between them.
The README makes the case for not mirroring one into the other. This file is the how: the
invariants, who owns what, which thread calls what, and which lock may be held where.

## The model

Every 64 KiB page of title memory has exactly one home at a time. Moving between homes is an
explicit transfer, done when someone needs the bytes where they are not.

| Home | The page is | Its bytes are |
|---|---|---|
| Zero | allocated, never written | nowhere: every reader sees zeros, nothing is committed |
| File | loaded, nobody has touched it since | the recorded load requests: staged once for the first reader, else replayed from the file |
| Texture | loaded, and a host texture was made from those bytes | as File (records rebuild it); the staged bytes went into the texture |
| Card | asked by shaders as a buffer and possibly written by them | its tile in the card view |
| Ram | touched by the CPU | its section memory, committed; CPU writes are tracked |

The invariants. Break one and you get bugs that look like title bugs; LESSONS.md has the
stories.

1. One home per page. A page's RAM is never read while its home is elsewhere, and a tile is
   never read as the page's bytes unless the home is Card (a tile of a Ram page is a copy that
   is only written).
2. Whenever the home is not Ram, every CPU view of the page is closed: `PAGE_NOACCESS`, or not
   committed. Any CPU access faults into `Pages::handleFault`, which transfers the page to Ram
   before the access is retried.
3. The library never reads or writes a title address. It reaches page bytes through its own
   view of each section. So a library thread never faults on title memory, and no library lock
   is ever held by a thread inside the fault path's caller.
4. Host kernel calls that touch title memory are bracketed (`KernelWrite`): the kernel fails
   where a thread would fault.
5. Nothing reaches the card except in `CardView::beforeSubmit`, on the submitting thread,
   ahead of the title's lists on the same queue; except read-backs, which any thread puts on
   the queue under the queue order lock (7) and then waits for with no lock held.
6. A page number names one page for as long as anything can reach that page (numbers are reused
   only after the page is freed and its last view is gone).

## Components

| Component | Header | Owns |
|---|---|---|
| `Context` | `context.hpp` | the components, the emulator's callbacks, queued change notifications |
| `PhysicalMemory` | `physicalMemory.hpp` | sections, page numbers, CPU views over placeholders, the library's own section views, commit and protection |
| `GpuPageTable` | `gpuPageTable.hpp` | GPU slot -> page (unified ranges follow CPU views; GPU-only ranges take explicit mappings) |
| `Pages` | `pages.hpp` | per-page home and flags, kinds of address ranges, the fault entry, write tracking, kernel brackets, parking |
| `Loads` | `loads.hpp` | files, per-page load records, staging, decoded-request cache, decode workers, zlib |
| `CardView` | `cardView.hpp` | spans, reserved buffers (segments), tile heaps, slot -> tile, batched mapping changes, fills, read-backs |
| `Census` | `census.hpp` | allocations by category, system numbers, twice-held bytes |
| `patch` | `patch.hpp` | sites files, code writes, stubs near the image, verified call redirection |
| C ABI | `deunify.h` | the same over an opaque context |

```
            emulator (xmem, queue, DirectStorage, renderer, exception handler)
                |            |             |            |               |
         PhysicalMemory  GpuPageTable    Loads       CardView        Pages::handleFault
                |   \        |   \         |   \        |   \             |
                |    `-----> |    `------> |    `-----> |    `--------->  |
                v            v             v            v                 v
             sections     slots        records,      tiles, heaps      homes, flags,
             views        -> pages     staging       segments          write list
```

Calls between components (all through private, friend-only methods declared in the headers):

- PhysicalMemory -> Pages: `pagesAllocated`, `pageFreed`, `pageForgotten`, `viewMapped`,
  `viewUnmapping`. PhysicalMemory -> GpuPageTable: `cpuViewMapped`, `cpuViewUnmapped`.
- GpuPageTable -> CardView: `slotChanged`. GpuPageTable -> PhysicalMemory: `map`/`unmap` at GPU
  addresses (no card view only), `holdPage`/`releasePage` for explicit slots (card view only).
  GpuPageTable -> Pages: `readPage`.
- Pages -> PhysicalMemory: `bytesOf`, `commit`, `reset`, `protectViews`, `protectView`,
  `remapGeneration`. Pages -> Loads: `takeStaged`, `takeStagedPage`, `replay`, `prepare`,
  `dropStaged`, `forget`, `rebuildable`. Pages -> CardView: `readBack`, `cardHomeLeft`,
  `pageForgotten`. Pages -> GpuPageTable: `slotsOf` (to queue `CpuWritten` in `takeWritten`).
- Loads -> Pages: `placeLoad`. Loads -> PhysicalMemory: `bytesOf` (Ram placement). Loads ->
  CardView: `loaded`.
- CardView -> Pages: `bytesForTile`, `becomeCardHome`, `cardHomeLost`, `tileChanged`,
  `takeWritten`. CardView -> Loads: `prepare`. CardView -> Context: `deliverChanges`.
- Everything -> Context: `queueChange`, `log`.

## Data flow

**Allocation and mapping.** The title allocates physical pages: one `SEC_RESERVE` section per
call, page numbers packed low. It maps them at addresses it reserved as placeholders; each view
replaces a placeholder. A new view commits nothing. Pages closes the view if the page's home is
not Ram (by leaving it uncommitted or `PAGE_NOACCESS`) and arms it read-only if the page is
armed.

**GPU view.** In unified ranges the GPU sees what the CPU sees. In GPU-only ranges the title
changes the GPU page table itself (page-mapping copies): with a card view this only moves slots
between tiles; without one the mappings become RAM views at the GPU addresses.

**Loads.** A request is read and decoded once, on the caller's thread. For each destination page
Pages decides the placement: a page no CPU-reachable view shows becomes (or stays) File home and
its bytes are staged; a page with a tile gets the bytes queued for the tile; anything else is
written into RAM. Every request is recorded per page so the page can be rebuilt later.

**First readers.**
- CPU touch -> fault -> `handleFault`: the page's bytes come from its home (staging, replay or
  read-back), are written through the library's view, the page is committed and its views
  opened. The access is retried.
- Shader view -> `bufferFor`: tiles for the asked slots; at `beforeSubmit` each new tile is
  filled from the page's home (`bytesForTile`).
- Texture upload -> `GpuPageTable::read(..., TextureUpload)`: bytes from the home; a File page
  becomes Texture.

**CPU writes.** Ram pages are armed read-only when `takeWritten` lists them; the next write
faults, opens every view of the page and lists it. `beforeSubmit` uploads listed pages that have
tiles, then re-arms them. `takeWritten` queues the `CpuWritten` changes for the GPU slots of
the pages it lists (CardView does not queue them). Arming order, under the Pages lock: the
armed bit is set before any view goes read-only, and cleared only after every view is writable
again, so a write fault that finds a read-only view always finds the bit set.

**Faults during a view change.** A view change (map, mapScatter, unmap, including the cut and
remap of the outer parts of a longer view) bumps `PhysicalMemory::remapGeneration` at its start
and at its end, so the generation is odd while one is under way, and a title thread may fault
on an address whose view is gone for a moment. The fault entry reads the generation, tries to
handle the fault (`pageAt`, then the page's home), and reads the generation again. Only a fault
that was not handled with the generation even and unchanged around the attempt is genuine
(returned to the emulator). Otherwise it waits until the generation is even, with no lock held
(the mapping path calls into Pages), for at most 10 s, and tries again. For now the wait spins
on `yield`; see the rough edges in the README.

**Shader writes.** `shaderWrites` marks the range; at `beforeSubmit` its pages become Card home:
views closed, bytes sent to the tile (from RAM, staging or replay), RAM reset. After the lists
ran, a CPU touch reads the page back from its tile (waits for the queue). Pages makes a page Card
home only in `becomeCardHome`, and CardView enters the page in its own record of Card pages
before it calls it, so a read-back racing the change finds the record. `cardHomeLost` (an
orphan's bytes coming back) does nothing for a page that is no longer Card home.

**Submission.** `beforeSubmit` on the submitting thread, in this order: retired heaps the queue
finished with released; orphaned Card pages (their last slot went) read back to RAM; tile
mapping changes (one `UpdateTileMappings` per heap and buffer); home changes for shader writes
and Card spans (the pages' bytes are taken from their old homes and held for their tiles);
fills of new tiles, loaded bytes, CPU-written pages and the held bytes, in one list; change
notifications delivered.

## Threads and locks

| Lock | Protects | Held across |
|---|---|---|
| 1 PhysicalMemory mapping | serialization of map/unmap calls | host `MapViewOfFile3`, `UnmapViewOfFile2`, `VirtualFree` |
| 2 PhysicalMemory table | page number -> (section, index), free numbers | short |
| 3 PhysicalMemory views | address <-> page | short, never across a host call |
| 4 GpuPageTable | slots, ranges | short (alias views without a card view: calls PhysicalMemory, taking 1-3) |
| 5 Pages | homes, flags, kinds, write list, kernel brackets | state and protection changes only; never a read-back, a replay or a decode (those run unlocked, and their bytes land only if the page's version did not change meanwhile, else the transfer is retried) |
| 6 Loads records -> staging -> decoded | per-page records, staged bytes, decoded requests | short; decoding runs unlocked (a once-cell per request) |
| 7 CardView queue order -> state | ordering of queue work, slots, tiles, heaps, pending work | queue order: across `UpdateTileMappings`, fills and the read-back copy's submission, never while waiting for the queue; state: short |
| 8 Census | categories and counters | short |

A thread takes locks only in increasing number, with one exception: 2 and 3 are leaves. They are
taken briefly under any other lock (Pages reads `committed`, `viewsOf` and protects views under
5) and nothing is taken while holding them. A call that needs an earlier lock drops the later
ones first. PhysicalMemory calls Pages holding at most lock 1 (the mapping path: `viewMapped`),
never 2 or 3. CardView calls Pages with none of its locks held, tile news included (a sender
checks after telling and tells again what changed meanwhile). No component holds a lock across
a card read-back, a replay or a decode.

Callbacks: `decode` runs on worker threads and on faulting title threads (inside the vectored
handler) with no lock held. `pagesChanged` runs only in `deliverChanges` (from `beforeSubmit` or
the emulator), on that thread, with no lock held. `log` may run with locks held and must not call
back into deunify.

Who calls what:

| Call | Thread |
|---|---|
| `PhysicalMemory::*` | the title's thread making the xmem call |
| `GpuPageTable::copyPageMappings` | the emulator's queue thread, at the point of the submission order where the title put it |
| `Loads::load` | DirectStorage worker threads, many at once |
| `Pages::handleFault` | any title thread, inside the vectored exception handler |
| `KernelWrite` | the thread making the kernel call |
| `CardView::bufferFor`, `shaderWrites` | the emulator's recording threads |
| `CardView::beforeSubmit` | the one submitting thread |
| `Census::snapshot` | any, typically once a second |

## Page-home state machine

States are homes; flags qualify Ram (`tile`, `armed`, `written`) and Card (`tile` always set).

```
                 load (eligible)                  bufferFor / span Card
        +------------------------------> File ----------------------------+
        |                               | ^ |                             |
        |                  TextureUpload| | |CPU touch / kernel           v
      Zero ----------------------+      v | |   (staging or replay)     Card
        |  CPU touch / kernel    |  Texture |                           ^ | ^
        |  / load into RAM       |    |  |  |                           | | |
        |                        |    |  +--+-> Ram <-------------------+-+ |
        |                        |    | CPU touch  |  CPU touch (read-back) |
        +------------------------+----+----------->|                       |
                                      |            +--- shaderWrites ------+
                                      +--- bufferFor (replay into tile) -> Card
```

Every transition, with what triggers it and what it does:

| # | From | To | Trigger | Action |
|---|---|---|---|---|
| 1 | (none) | Zero | `PhysicalMemory::allocate` | number given; nothing committed |
| 2 | Zero | Ram | CPU touch (fault on an uncommitted view), `KernelWrite`, `placeLoad` -> Ram | commit (zero-filled), open views, list as written |
| 3 | Zero | File | load, page eligible (`placeLoad` -> File) | record; stage bytes; views stay uncommitted |
| 4 | Zero | Card | tile asked in a `SpanHome::Card` span, or `shaderWrites` | tile cleared to zeros at `beforeSubmit` |
| 5 | File | Ram | CPU touch, `KernelWrite`, new view where CPU-reachable *and touched*, partial load not eligible | bytes from staging (first reader) else replay; commit; write; open; list |
| 6 | File | Card | `bufferFor` gives a tile (Card span) or `shaderWrites` | tile filled from staging or replay; staging dropped |
| 7 | File | Texture | `GpuPageTable::read` / `readTitle` with `TextureUpload` covering staged bytes | staged bytes given to the texture and dropped; records kept |
| 8 | Texture | Ram | CPU touch, `KernelWrite` | replay; commit; open; list; `CpuWritten` change queued for the texture cache once written |
| 9 | Texture | Card | as 6 | tile filled by replay |
| 10 | Card | Ram | CPU touch, `KernelWrite` | read back (waits for the queue; bytes still on their way are used directly); commit; write; open; the tile stays as a copy (`cardHomeLeft`) |
| 11 | Ram | Card | `shaderWrites` over a slot of the page, or a tile given in a Card span | at `beforeSubmit`: views closed, bytes read from RAM and sent to the tile, RAM reset |
| 12 | Card | Ram | the page's last slot is unmapped (orphan) | at `beforeSubmit`: read back, then as 10, tile released |
| 13 | Ram | File | `Pages::park` and its records rebuild it byte for byte | views closed, RAM reset |
| 14 | Ram/File/Texture | File | a whole-page load on an eligible page | records restart; old bytes dropped; `Loaded` change |
| 15 | Ram | Ram | a load that is not eligible | written through the library's view; listed as written |
| 16 | Card | Card | a load | bytes queued for the tile (`CardView::loaded`); `Loaded` change |
| 17 | any | (gone) | page freed and last view gone (`pageForgotten`) | tile released, records and staging dropped, number free |

Within Ram:

| From | To | Trigger |
|---|---|---|
| open, written | armed | `takeWritten` (not under a write bracket) |
| armed | open, written | a write faults (every view opened) |
| open | open, written | `KernelWrite` (write) ends |
| any | + tile | `bufferFor` asks a slot of the page (Shared span); filled from RAM |

"Eligible" (`Pages::placeLoad` -> File): every view of the page is at an address of Kind
`GpuOnly` or `Shared`, no view is under a kernel bracket, the page's records rebuild it
(`Loads::rebuildable`, asked after `Loads::load` recorded the request), and either the load
writes the whole page or the page's earlier bytes are themselves rebuildable (home Zero, File or
Texture). A page whose records overflowed (`Config::maxRecordsPerPage`) or that `Loads::write`
wrote (records unknown) goes to RAM: nothing could rebuild it from File home.

A page with no views at all is eligible. The earlier implementation refused it. We don't,
because invariant 2 is kept when a view is mapped (`viewMapped` leaves a new view of a page
outside RAM uncommitted or makes it `PAGE_NOACCESS`, and a touch faults the page in from its
records). Refusing would commit RAM for every page loaded before it is mapped, which is the
usual order for DirectStorage loads into physical pages that the title maps afterwards, often
only in GPU ranges.

## What is counted twice

The census reports what is still held twice: Ram pages with a tile (`ramTiled`), and tiles
under host textures (needs `Callbacks::texturesOver`). Both should be small. They measure how
far an integration is from one home per page, and they are the first numbers we'd look at.
