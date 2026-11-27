# deunify

Memory infrastructure for Xbox Series emulators on PCs with a discrete GPU. C++20, Windows x64,
D3D12.

The console's GPU reads everything from one fast memory. On a PC, whatever the GPU reads from
RAM crosses the PCIe bus, which is roughly ten times slower than the card's own memory, and
worse on most laptops. An emulator that mirrors title memory pays that price every frame, for
data nobody needed moved, and then pays again in CPU time to keep the two copies in step. A
faster card doesn't make that cost go away. It is a floor under your frame time.

If you want 60 fps and more, the memory has to be split the way a PC engine splits it: GPU data
on the card, CPU data in RAM, and explicit transfers between them. That's what this library
does.

The usual alternative is the mirror: all title memory in RAM, watched page by page for CPU
writes, changes uploaded to the card, GPU writes copied back. It works, and we have nothing
against things that work. But it holds the title's memory twice, and on a card with less video
memory than the console has, the second copy simply doesn't fit.

## One home per page

Every 64 KiB page of title memory lives in exactly one place at a time:

- Zero: allocated, never written. Nothing is committed and every reader sees zeros.
- File: loaded and untouched since. Its bytes are the load requests that wrote it, staged for
  the first reader and replayed from the file after that.
- Texture: like File, but a host texture was already made from those bytes.
- Card: shaders asked for it as a buffer, or may have written it. Its bytes are a tile on the
  card.
- RAM: the CPU touched it. Committed, with CPU writes tracked.

A page moves only when somebody needs its bytes somewhere else. A CPU touch faults and brings
the page to RAM, a shader view gets it a tile, a texture upload reads it from wherever it is.
Nothing is kept anywhere just in case. Just-in-case copies are how the mirror grows back.

How much this saves depends on how much of its memory the title's CPU really looks at, and that
is usually less than the title claims. If your title's engine happens to tag every chunk of its
streaming pool as CPU-only, GPU-only or both, believe it and pass it on with `Pages::setKind`.
It costs one breakpoint in the allocator to find out, and it's the difference between saving a
lot and saving nothing. [docs/CONSOLE.md](docs/CONSOLE.md) has more leads of that kind. They are
leads, not facts about every title.

## What's in the box

`include/deunify/*.hpp` is the C++ API, one header per component. `include/deunify.h` is a C
ABI over the same, and `bindings/rust/deunify-sys` declares that ABI for Rust. In `src/`:

- `PhysicalMemory`: sections, page numbers, views over placeholders.
- `Pages`: homes, faults, lazy commit, write tracking, kernel brackets.
- `Loads`: load records, staging, decode-once, replay, zlib.
- `GpuPageTable`: the title's GPU view, including the queue's page-mapping copies.
- `CardView`: title memory on the card as tiles of reserved buffers.
- `Census`: video memory by category, against what the system reports.
- `patch`: helpers for patching title code at load.

`tests/` has a GoogleTest suite per component; the card view tests run on WARP when it offers
tiled resources tier 2.

It does not do rendering, texture caching, shader translation or file systems, it ships no
console codec except zlib (yours go through a callback), and it leaves the title's committed
engine heaps alone.

Further reading, in the order we'd read it:
[ARCHITECTURE](docs/ARCHITECTURE.md) for the invariants, locks and state machine,
[INTEGRATING](docs/INTEGRATING.md) for wiring it into an emulator,
[CONSOLE](docs/CONSOLE.md) for what titles do with memory,
[TITLE-PATCHES](docs/TITLE-PATCHES.md) for when the title needs help, and
[LESSONS](docs/LESSONS.md) for every bug that bit us.

## Building

Visual Studio 2022 with the C++ workload, LLVM `clang-cl`, CMake 3.24 or later, and Ninja.

```
build.cmd
build\deunify_tests.exe
```

Everything lands in `build/`: `deunify_static.lib` (C++ and C API) and `deunify.dll` with its
import library (C ABI only). zlib and GoogleTest are fetched at configure time.

## Where it stands

The design was proven in an earlier implementation inside an emulator running real titles.
This is a C++ port of that code with the emulator taken out. It has its own tests and has not
run a title yet. Treat it as a reference implementation: read it, adapt it to your engine, fix
what you find, and keep the invariants in ARCHITECTURE.md, because each of them is there for a
reason we found the hard way.

Known rough edges, found while putting the components together and still open:

- A view mapped over a page that is already committed is writable for a moment, between the
  host call that maps it and `viewMapped` arming or closing it. A write in that window is not
  seen.
- A page freed while `CardView` is moving it to Card home can leave a stale entry in the card
  view's record of Card pages.
- A fault that waits for a view change spins on `yield` instead of sleeping. Harmless for short
  remaps, wasteful for long ones.

Two things the title may do while you test, neither of them our fault. Some titles take it
personally when a frame takes half a minute, and a watchdog ends the process. Some upload a
crash dump every time they crash, so keep the network off while you are making them crash.

## License

License: to be decided.
