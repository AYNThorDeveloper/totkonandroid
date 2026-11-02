# NVN on Vulkan (20 October 2026)

The Android game loader now runs the macOS port's complete NVN layer
(`games/totk/third_party/d2r_gfx/gfx/nvn/hle_nvn.c` and helpers) unchanged
in substance. Only the layer underneath differs: `nvn_vk_backend.c`
implements the C API that `nvn_mtl.m` provides on Apple platforms
(`nvn_mtl.h`). The earlier hand-written `android_nvn_dispatch.c` is gone; its
device-property table was identical to the shared one.

## How the shared layer is built for Android

- `build_module_loader_probe.sh` compiles the shared sources with
  `-include android/compat/android_darwin.h` (Darwin clock, thread-name and
  thread-id helpers) and the Mach compat headers in `android/compat/`.
- Registration records that Apple places in `__DATA,<name>` sections use the
  ELF section `<name>` (`NVN_SECTION`, `HLE_SECTION`); `nvn_vk_host.c` reads
  them via `__start_/__stop_` symbols. `nvnBootstrapLoader` comes from the
  shared layer's `HLE_FORCE` entry.
- `port_macho_asm.py` converts the generated Apple assembly (`nvn_thunks.S`,
  `nvn_entry.S`: 523 counting thunks) to ELF syntax at build time.
- `nvn_vk_host.c` provides the host services the shared layer expects (time,
  dirty-page fault hook with a chained SIGSEGV/SIGBUS handler that reads the
  ESR write bit, guarded reads, and inert Darwin-only diagnostics).

Small `#ifdef __APPLE__` / `#ifndef __APPLE__` changes in shared files:
`nvn_impl.h`, `hle.h` (section names), `nvn_dirty.c` (fault context),
`totk_vpool.c` (dma-buf aliasing, physical pool registration), and
`fs_host.c` (portable durable flush), `audio_mixer.c` (AAudio output).

## Memory pools: GPU address = CPU address without host-pointer import

The Adreno 740 driver (676.53) does not offer `VK_EXT_external_memory_host`,
so game memory cannot be imported as Metal does with `newBufferWithBytesNoCopy`.
It does offer `VK_KHR_external_memory_fd`, and the exported handle is a
dma-buf. A Thor test (`vkalias.c`, run 20 October) showed:

- the dma-buf can be mapped `MAP_SHARED | MAP_FIXED` at any page-aligned
  address, including over existing anonymous pages;
- CPU writes through that mapping are visible to GPU copies and GPU writes
  are visible to the CPU after completion (memory types 0, 1 and 4);
- a second mapping of the same buffer aliases the same pages;
- CPU bandwidth equals ordinary memory (about 45 GB/s read in the test).

So each pool gets dedicated exportable memory (host-visible coherent type 4).
Its contents are copied in and the dma-buf is mapped over the game's own pool
pages. CPU and GPU then share one copy, as on the console. On finalize the
pages are replaced by anonymous memory holding the final contents.

NVN physical pools are `GPU_NO_ACCESS`, so `hle_nvn.c` does not register them;
`totk_vpool.c` does on Android, because virtual pools map them again
(`android_vk_pool_alias`). Virtual pools get no memory of their own; GPU
addresses inside them resolve through the vpool mapping table to the
physical pool's buffer.

Overlapping pools stop the run explicitly (not yet seen in TOTK on Android).

## Submissions

One timeline semaphore orders all submissions. A completion thread waits for
each value, then performs the CPU-visible effects recorded during replay
(event writes/adds, counter records) and wakes `mtl_wait`. Command buffers
come from an 8-entry ring of transient pools.

## Replay coverage

Implemented: command-list control (BEGIN/END/JUMP/CALL), barriers, buffer
copies, uniform updates (`vkCmdUpdateBuffer`), events, counters (timestamp
reports write counter 0, as on Metal; samples-passed can only be 0 because
draws are not replayed). State-only records are accepted.

Texture CPU access (`ReadTexels`/`WriteTexels`) converts between linear data
and the pool's block-linear bytes with the shared (de)swizzle code. That is
the NVN storage model; it stays exact until GPU passes write textures.

Not implemented, each stops the run naming the operation: render targets,
clears, draws, dispatches, texture copies, present. These are the next
renderer milestones.

## Device facts

Thor (AYN Thor Max, Adreno 740, driver 676.53, Vulkan 1.3.128): sampler
min/max reduction, ASTC, BC, S8 attachments, geometry and tessellation
shaders, multiple viewports, timeline semaphores, synchronization2, dynamic
rendering and buffer device address are all supported. Also present:
transform feedback, custom border colors, extended dynamic state 1/2,
vertex-input dynamic state, depth-clip control, provoking vertex, line
rasterization, index type uint8, conservative rasterization, robustness2,
push descriptors. `timestampPeriod` is 52.083 ns (19.2 MHz, the Switch tick).
