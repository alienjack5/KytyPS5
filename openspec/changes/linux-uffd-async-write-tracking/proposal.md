# Proposal

## Why

On Linux, guest write tracking resolves every CPU write to a write-protected guest page with a signal
and a protection call (`mprotect`, or `UFFDIO_WRITEPROTECT` under `KYTY_UFFD_WP`). `mprotect`
serializes on the process mmap lock, so Linux starts the tracker at its slowest level with 1 MiB
fault-ahead windows: faults stay rare, but each one marks 256 pages CPU-dirty, most of them never
written, and every one of them is uploaded and protected again. Linux 6.7+ can resolve
write-protect faults in the kernel (`UFFD_FEATURE_WP_ASYNC`) and report and re-protect exactly the
written pages in one call (`PAGEMAP_SCAN`). A synthetic benchmark on an i7-11800H with Linux 7.2
(memfd guest memory, 4,800 random pages written per frame) measured 1.1-1.3 ms per frame for that
instead of 2.2-3.7 ms with 4-15 writer threads, and 4,300-4,700 CPU-dirty pages instead of
21,700-29,700.

## What Changes

- A new opt-in Linux write-tracking mode (`KYTY_UFFD_WP_ASYNC=1`, default off): guest mappings are
  write-protected with userfaultfd in asynchronous mode, so a CPU write to a protected page takes no
  signal, and the tracker learns which pages were written from `PAGEMAP_SCAN`, which protects them
  again in the same call.
- Written pages are collected before GPU work consumes CPU-dirty state, scanning the ranges that work
  uses rather than all of guest memory (one scan of 4 GiB costs ~1.9 ms).
- In this mode CPU-dirty state is page-exact: the fault-ahead window no longer applies to write
  tracking.
- No-access protection (pages whose newest data the GPU holds) keeps the current synchronous path,
  as do Windows, macOS and Linux kernels without the feature (probed once, logged, falling back to
  `mprotect` or `KYTY_UFFD_WP`).
- A measurement gate before any default change: the synthetic benchmark is added to the repository,
  and in-game numbers come from the existing fault-cost log (`KYTY_FAULT_COST_LOG`) and fault map
  (`KYTY_FAULT_MAP=1`) with the mode off and on.
- Tests exercise the mode over real memfd guest mappings, and the full suite passes with it on.

## Capabilities

### New Capabilities

- `guest-memory/write-tracking`: how the emulator learns about CPU writes to guest memory that GPU
  caches mirror: the tracking modes, the guarantee that GPU work never uses stale guest data, mode
  selection and fallback, and the diagnostics that report what tracking costs.

### Modified Capabilities

None: the project has no specs yet.

## Impact

- Code: `src/common/platform/uffdWriteWatch.{h,cpp}` (asynchronous registration, `PAGEMAP_SCAN`),
  `src/kernel/memoryAddressSpace.inc` and `src/kernel/memory.cpp` (guest mapping registration),
  `src/graphics/host_gpu/pageManager.{h,cpp}` and `memoryTracker.{h,cpp}` (protection and
  CPU-dirty state), `src/graphics/host_gpu/faultCost.{h,cpp}` (cost model and slow level), and the
  buffer and texture cache points that upload CPU-dirty ranges
  (`src/graphics/host_gpu/renderer/cache/bufferCache.cpp`, `textureCache.cpp`).
- Tests: `tests/PageManagerTests.cpp`, `tests/MemoryTrackerTests.cpp`,
  `tests/VirtualMemoryAllocationTests.cpp`, and a new test over real memfd mappings; the fork's Linux
  CI (lavapipe) runs the CPU-side tests with the mode on.
- Platform: the new mode needs Linux 6.7+ (`UFFD_FEATURE_WP_ASYNC`, `PAGEMAP_SCAN`); a user-mode-only
  userfaultfd descriptor is enough (`vm.unprivileged_userfaultfd=0`). Other kernels and platforms
  behave as today.
- Configuration and documentation: the `KYTY_UFFD_WP_ASYNC` switch, settable in `u59-preset.json`
  like `KYTY_UFFD_WP`, documented in `docs/LINUX-U59.md`.
- Validation limit: the in-game measurement needs a game the user owns. Until then the mode stays
  opt-in and off by default.
