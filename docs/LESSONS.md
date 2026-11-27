# Lessons

All of these happened to us while we built the first version of this design inside an emulator
running real Xbox Series titles. Each one says what we saw, what it turned out to be, what we
changed, and how you would spot the same thing in your own integration. Most were found by
reading code or thread stacks. A few were found by staring at a broken picture for far too
long. Those were mostly somebody else's.

## When it hangs or crashes

The freeze came a minute into a scene. The title stopped: no crash, hardly any CPU load. A dump
of every thread's stack showed one thread inside our page opener and everybody else, the fault
handler and the DirectStorage workers included, queued on the lock it held. The caller had
passed an end address where the opener wanted a length. `end - start` wrapped, and the opener
set off to open every page up to the top of the address space, one page at a time, holding the
lock. We fixed the call. We also made the opener walk an ordered map of the pages that are
actually closed instead of stepping through addresses, so a silly length now costs one lookup.
A file read with a saturated length had done the same thing a week earlier, and we hadn't
learned. To spot it, take the stacks of a hung process before you kill it. Any loop over a
caller's range that holds a lock deserves a test that passes `UINT64_MAX` and expects it to
return at once.

Then a crash during a remap. A video decoder's worker thread died a few minutes in and the
title's crash reporter came up. Its write had faulted while the emulator was remapping the view
it wrote to (the rest of a view split by an unmap). The fault handler waited for the remap,
then checked whether the page was writable now. It wasn't, correctly: write tracking had armed
the new view read-only because the page's other views were armed. Nobody answered the fault,
so the title got it. The fix was to ask the write tracker after waiting out the remap. In
deunify terms, `Pages::handleFault` has to see every access violation, including the ones your
own handler thinks it already explained. If you have a "wait for the remap and retry" path,
call deunify after it, not instead of it.

The arming gap is the same family, smaller. The fault handler read the armed bits without
taking the lock. A view was made read-only before its bit was set, and the bit was cleared
before the view was writable again. A write in either moment found a read-only page that
nobody claimed. The bit has to cover the whole time the view is read-only: set it before
`VirtualProtect(PAGE_READONLY)`, clear it after `VirtualProtect(PAGE_READWRITE)` has returned,
and let a fault that lands during a change wait for the lock and look again. Closing had the
same hole. A write to the very first page we ever parked, while it was being parked, found it
closed and unclaimed, because the fault path only looked for parked pages once one had been
counted. These show up as rare access violations at addresses you know are yours, with a stack
in ordinary title code. They look like title bugs. They aren't.

The first card-view run lost the device almost at once with `DXGI_ERROR_INVALID_CALL`. With the
debug layer on, the culprit was a raw view made "to the end of what we know", and what we knew
was the whole GPU view: billions of elements, starting well inside a huge reserved buffer.
Buffer views are limited to 2^27 elements, and a view must begin in the first 4 GiB of its
buffer. The next run found the second rule for us, with a small view that began past the
4 GiB mark of the same buffer. That is why the card view is built from 4 GiB segments that
start every 2 GiB: any view under 2 GiB begins in the first half of some segment. Run once with
the debug layer before you run without it. It's slow. Guessing is slower.

## When bytes go missing

One run logged pages whose bytes "cannot be rebuilt". A File page is only as good as its load
records, and we kept a short list per page. Some pages took more partial loads than that (many
small requests landing in the same 64 KiB), the list overflowed, the page was marked unknown,
and when somebody finally read it there was nothing to rebuild it from. Now a partial load into
a page whose list is full brings the page to RAM and writes it there, and the cap is much
higher (`Config::maxRecordsPerPage`). Watch `CardView::Totals::lostPages` and `Status::NoSource`
from reads and kernel brackets. Anything above zero is a bug, not noise.

Partial loads into a page the CPU had written were found by reading, never seen in a log, which
is how we like them. A load covering half a page, into a page the title had written and that
had no load record, was allowed to keep the page out of RAM. The page was parked, its RAM
reset, and its rebuild was zeros followed by the load. The title's half was gone. The rule is
now the one in ARCHITECTURE.md: a page may become File home only if the load covers it whole or
its earlier bytes are themselves rebuildable. The test is three lines: write half a page from
the CPU, load the other half, read both back.

A load into a Card page went nowhere, because three pieces of code each assumed another would
deal with it. A page at home on the card (shaders had written it) counted as parked, staging
skipped card-home pages, and the card view skipped pages it thought of as written. When the
title reused that physical page for a new asset, the card kept showing the old one. Loaded
bytes now go to the page's tile (`CardView::loaded`, transition 16 in ARCHITECTURE.md). It looks
like stale assets after streaming has churned for a while. In a test, load into a page after
`shaderWrites` made it Card home and read it back.

Missed shader writes turned up in the same run as the lost bytes, by going through every path a
shader write could take. Shaders were writing through views they picked from a descriptor heap
at run time, and those writes were never declared to the card view. Their pages stayed Ram
home, so the next upload of CPU writes went up over what the card had just computed. Every
writable view a shader can reach has to go through `CardView::shaderWrites`, the ones it
selects at run time included. If you can't tell which views a shader may pick, declare every
writable view in the heap range it can index. Over-declaring costs some read-backs.
Under-declaring costs correctness, quietly.

A lost CPU write: moving a page to Card home read its bytes, then closed its views. A title
write between the two went into RAM after the copy was taken and was reset away with the rest.
Close first, then take the bytes. A write in between faults, waits for the move, and follows
the page to its new home. The taken bytes stay with the card view until the list that uploads
them is on the queue, so a read-back in that window gets them instead of an unfilled tile. You
won't find this by looking at the screen. Write a test where one thread hammers a page while
another moves it home, and check the last value.

The orphan was a Card page whose last GPU slot was unmapped. It lost its tile, and its bytes
with it. The title had every right to unmap the slot and touch the page from the CPU later. Now
a Card page keeps its tile until its bytes are read back to RAM, before the unmapping reaches
the card (transition 12). A cousin of that one: we once closed pages by an address we resolved
again later. By then the slot showed a different page, and we closed an innocent one. Key
everything by page number.

Two owners of page protection is one too many. The emulator's write-watch guards made a closed
page read-only for a moment and back, or read-write, as they would for any page they tracked. A
read in that moment saw zeros. Pages has to be the only thing that changes protection on views
deunify made. If your emulator has guards of its own, keep them off deunify's ranges.

The wrong file got replayed once. Load records named a file by the address of the emulator's
file object, and a new file object at a reused address replayed the old file. deunify
duplicates the handle (`Loads::openFile`) and holds it for as long as any record names it. In
the same spirit, a bad compressed stream used to report success at load time and show up much
later as lost bytes. Loads now decode up front, so a codec error reaches the title at the
request that caused it.

## When it is slow

The read-back that held a lock is the one we'd tell first. As soon as UAV buffers got a Card
home, a lock held while waiting for the whole card cost most of a frame. The CPU touched a
handful of pages of those buffers every frame. Each touch read its page back, which means
waiting for everything already queued on the card, and it waited holding the page lock. The
submitting thread takes that lock before every submission. So once per touch, the submitter
waited for the card to drain. The fix: read back without the lock, and write the page only if
it wasn't closed again in the meantime (a per-page count of closings tells you). If the
submitting thread's stack shows it in a lock whose owner is a fault handler, and that fault
handler is waiting on a fence, this is your story. If you adapt deunify and find yourself
holding the page lock across a read-back, you are writing it again.

Decoding twice. The submitting thread's prep work, everything it does before the title's lists
can go to the queue, grew until it was the longest thing in the frame and the card sat idle
waiting for it. DirectStorage workers decoded each load and threw the bytes away for pages that
stayed out of RAM. When the card view filled tiles later, it rebuilt each page alone on the
submitting thread: read the request, decode it, keep 64 KiB, then read and decode the same
request again for its next page. A small per-thread cache of recent requests barely helped.
Now a load is decoded once and its bytes are staged for each page's first reader. Replays go
through one shared store where concurrent askers wait for a single decode, and before a batch
of replays the distinct requests are decoded on a worker pool (`Loads::prepare`). Prep work
became a small slice of the frame. Compare requests decoded with requests loaded in
`Loads::Totals`. If decodes run at a multiple of loads, something decodes per page.

Write watch under lazy commit went from a rounding error to a large share of the frame the day
views stopped committing at map time. The emulator's watch was built for committed memory:
`GetWriteWatch` where it could, `VirtualQuery` and guard pages where it couldn't.
`GetWriteWatch` refuses mapped views, and an uncommitted page in a view looks like a hole, so
every hole was asked about again at every check in case something had committed it. Where
there had been next to no kernel queries, there was now a flood of them every frame. The fix
was to stop asking the kernel. deunify committed those pages and opened them, so it already
knows: each page is open, armed or closed in its own table, and a hole is a known hole. Count
`VirtualQuery` calls per frame. The right number is close to zero.

`VirtualQuery` on big regions bit the copy-back path, which puts GPU results back into title
memory. It went from cheap to one of the slowest things in the frame. Each landed piece asked
`VirtualQuery` about its range first, sometimes twice. On a committed region of several
gigabytes, `VirtualQuery` walks the page tables to the end of the region to report its size,
and that is slow. With hundreds of pieces a frame, the landing threads queued behind it. Our own
records already said what was committed, so now we ask them, and the kernel only about a view
page whose commit bit isn't set. Stack samples ending in `NtQueryVirtualMemory` give it away.
On small regions you'll never notice. On a console title's engine heaps you will.

The texture storm: one frame spent most of its time re-reading textures. Possible shader writes
over the card view, and there were a lot of them, were reported to the emulator's write
tracking as CPU writes would be, and it re-read every texture over those pages. Now only the
texture cache hears about them (`ChangeReason::ShaderWritten` through `pagesChanged`), and only
for pages that changed. Time your texture-cache invalidation per frame. It should be boring.

## When memory doesn't come back

The census once showed the emulator's texture allocator holding a lot of video memory in
blocks, and about half of those blocks were empty. Decoupling had freed that much, and the
allocator kept it in blocks it would never fill again. None of that memory is deunify's, which
is rather the point: the gain was real, and it vanished into somebody else's fragmentation.
Track your own heaps with `Census::track` so you can see the same thing in yours.

Earlier, tile heaps never shrank. One run held far more heap memory than it had tiles, because
free tiles were reused last-freed-first and heaps were never released, so every heap kept a few
tiles alive. Now tiles come from the lowest heap and index first, the higher heaps drain, and a
heap with no tiles left is released once the submissions that used it are done, keeping
`kSpareEmptyHeaps`. If `CardView::Totals` shows heaps far above tiles divided by `kHeapTiles`,
you have the same problem.

We worried that card-view tiles and host textures would hold the same bytes twice. The census
said about a quarter of the tiles in use lay under host textures. Real, worth a look one day,
and much smaller than the fragmentation above. Count before you optimize.

Bindless views over huge ranges were a quieter leak. For views that shaders pick from a
descriptor heap at run time, the renderer looked up the card copy by the whole buffer the view
belonged to. Under the card view that gave a tile to every page of the buffer, including pages
no view could reach, textures among them. Ask by the view's own range: `CardView::bufferFor`
with the view's bytes, not the buffer's. The same mistake made a table view into a buffer
larger than `kLargestAskBytes` fall back to a full copy. Tiles climbing far past what draws can
touch, or `tooLargeAsks` above zero, point here.

Commit also leaked from reads. Several paths that only meant to read ended up committing: the
emulator's safe copy reading an uncommitted page, parking that opened a page's window (and so
committed it) before checking whether it could park, a new view of a never-touched page
bringing it back. Each was small. Together they put pages in RAM for no reason. In deunify only
a write or a title touch commits, and the library reads with `bytesOf(page, false)`, which
returns null rather than commit. If committed pages rise while faults and writes don't, look
for a reader that commits.

## When it isn't the memory layer

The black picture. After a merge, the picture came up mostly black, with well under half the
draws of the run before. The memory layer had changed the most, so naturally we blamed it. It
was the shader translator: a change that came in with the merge refused hundreds of shaders
where a handful had been refused before. Draws and shader refusals per frame would have said so
in the first minute. We took rather longer.

Shader writes read through a texture broke one class of materials in the first card-view run.
This one was ours. Shader writes stayed on the card, and a texture made later from the same
pages read stale RAM. It is the reason Card home exists, and why texture uploads must read
through `GpuPageTable::read`, which reads back from the tile.

Addresses that moved: the emulator's own page tables could still name a page the title had
already unmapped, and serving bytes from it served the wrong page. deunify reuses a page number
only after its last view is gone. If you cache address-to-page anywhere else, check that the
address still shows that page before you serve bytes for it.
