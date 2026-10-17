# Design

## Context

See proposal.md (Why) for the motivation and the measured costs.

- **Protection today.** `PageManager` keeps per-page write and access watcher counts and derives
  NoAccess, Read or ReadWrite from them (`pageManager.cpp:243-256`). Every change ends in
  `Impl::Protect` → `ProtectGuestHostMemory` → `GuestAddressSpace::ProtectTransient`, under the
  address space's exclusive mutex (`memory.cpp:4592-4595`, `memoryAddressSpace.inc:1254`). That
  call picks the synchronous userfaultfd path (`ProtectTrackedUnlocked`, `KYTY_UFFD_WP`) or
  `mprotect`.
- **A tracked write faults synchronously.** The host exception handler calls
  `TryHandleGuestAccessFault` and then `RenderContext::HandleFault` (`renderContext.cpp:189-254`):
  - the buffer cache marks the page CPU-dirty: `MemoryTracker::InvalidateRegion` → `MarkWriteFault`.
    That covers the fault-ahead window, mirrors, the serial and dirtied-log bumps,
    `ForgetKnownFills` and hot-page streaks;
  - the texture cache invalidates CPU aliases and pending write-backs;
  - `PageManager::Reconcile` runs last.
- **Consumers read CPU-dirty state on the command processor.** The readers are:
  - `SynchronizeBuffer`/`CollectUpload`, which re-protects before copying (`regionManager.h:335-374`);
  - the relaxed fast paths and the `RangeMemo`/`BindingMemo` signatures;
  - `TextureCache::RefreshImage` (`textureCache.cpp:4004-4031`);
  - `SynchronizeBdaBuffers` with its dirty-log pass (`bufferCache.cpp:5173-5350`);
  - GPU-dirty transitions to NoAccess (`regionManager.h:187-197`).

  Guest threads only produce CPU-dirty state.
- **`SyncEpoch` defines when a CPU write must be visible to the GPU** (`syncEpoch.h`). It advances
  before every fence packet, at every command-processor slice, after service commands and on
  mapping changes, which is many times per frame. Consumers already reuse "range made current at
  epoch E" facts while the epoch is unchanged.
- **Fault-ahead.** The window is `KYTY_FAULT_AHEAD_KB` widened by `KYTY_FAULT_AHEAD_ADAPT`
  (256 KiB << `SlowLevel`). `SlowLevel` is forced to 2 on Linux with `mprotect`
  (`faultCost.cpp:951-957`).
- **Synchronous userfaultfd today.**
  - It registers lazily per `MappedRegion` with `UFFDIO_REGISTER_MODE_WP`
    (`memoryAddressSpace.inc:1623-1629`). The probe requires SIGBUS and shmem write protection,
    and `WP_ASYNC` is only logged (`faultCost.cpp:611-651`).
  - NoAccess uses `mprotect`.
  - A backing replacement keeps the old entry's registration state although `MAP_FIXED` creates
    a new, unregistered VMA (`memoryAddressSpace.inc:516-527`, `1268-1286`).
- **Tests.** No test covers userfaultfd. On Linux the tracker tests stub protection with
  `mprotect` and call the fault entry points directly. The compute tests' write cases call
  `HandleFault(Write)` and then write, assuming synchronous reporting
  (`ShaderRecompilerComputeTests.cpp:8679-8684`).

## Goals / Non-Goals

**Goals:**

- Feed every existing consumer the same write events it gets today, only later and in batches,
  so that no consumer has to change what it does with them.
- Keep the scan cost bounded: scan only memory a consumer is about to use, at most once per range
  per epoch.
- Predict the gain for a specific game with the current backend (an estimator) before relying on
  the new mode.

**Non-Goals:**

- Changing any default, or Windows and macOS.
- Removing the `mprotect` or synchronous userfaultfd paths.
- Tracking no-access (GPU-owned) pages asynchronously.
- Reworking the BDA, buffer or texture consumers to read scan output directly.
- Replacing hot pages. That can follow once this mode is measured.

## Decisions

**D1. A dedicated asynchronous descriptor, probed and self-tested at startup.**
- The probe calls `UFFDIO_API` with `UFFD_FEATURE_WP_ASYNC | UFFD_FEATURE_WP_HUGETLBFS_SHMEM`,
  user-mode-only first like the synchronous probe, and opens `/proc/self/pagemap`.
- It then runs a self-test on a one-page memfd: protect it, write it, scan it, and expect one
  reported page that is protected again.
- The startup line logs the result and a scan microbenchmark (µs per 2 MiB).
- Fallback chain: asynchronous, then synchronous userfaultfd if `KYTY_UFFD_WP=1`, then `mprotect`.
- *Alternative:* sharing the synchronous descriptor. Rejected: features are per descriptor, and
  the asynchronous mode replaces the SIGBUS mode.

**D2. Registration follows the current lazy per-region scheme, with the remap gap closed.**
- A newly registered region is armed once as a whole (`UFFDIO_WRITEPROTECT`).
- A backing replacement resets the region's registration state, so the new VMA registers again.
  This fixes the synchronous mode too.
- `PM_SCAN_CHECK_WPASYNC` aborts a scan over unregistered memory. That region then falls back
  (logged once, as the spec requires).

**D3. Asynchronous memory makes no host call for write-watch transitions, except arming.**
- Watching a page arms it (`UFFDIO_WRITEPROTECT`, read lock only, no VMA change).
- Releasing does nothing: a written page is already writable, and an armed page never blocks a
  write.
- The kernel's write-protect bit is the truth about written-ness, and `PageManager`'s applied
  state for these pages only records arming.
- NoAccess keeps `mprotect` and the synchronous fault and readback.
- Over-arming costs at most one spurious upload, while under-arming loses writes, so arming is
  conservative.
- *Alternative:* keep issuing releases. Rejected: each one is an extra ioctl, and it undoes the
  scan's re-arm.

**D4. Collect on consume, gated per 2 MiB chunk and epoch.** `CollectWrites(range)` runs before
the following read CPU-dirty state for a range:
- `SynchronizeBuffer`, ahead of `CollectUpload` and of the relaxed and memo checks;
- `TextureCache::RefreshImage`, ahead of its CPU-dirty test;
- `SynchronizeBdaBuffers`, ahead of its pass;
- GPU-dirty transitions and downloads.

Each chunk carries the epoch of its last scan, and a chunk is scanned at most once per epoch. A
scan is `PAGEMAP_SCAN` over the chunk's registered part, with
`PM_SCAN_WP_MATCHING | PM_SCAN_CHECK_WPASYNC` and category `PAGE_IS_WRITTEN`.

*Rejected alternatives:*
- Scanning all watched memory at every epoch advance: epochs advance at every fence, and a scan
  costs about 0.5 µs per MiB.
- Periodic background scans: they give no ordering at fences. They could be added later as a
  prefetch.
- Exact per-consumer scans without gating: one syscall per buffer per draw.

**D5. Collected writes replay through the existing write-fault entry points, batched, on the
command processor.**
- `PAGE_IS_WRITTEN` also matches pages nobody watches and pages that are already CPU-dirty, so
  the reported pages are first filtered against the write watchers (`CountWatchedPages`, or a
  watched bitmap).
- Each run of watched written pages goes to `MemoryTracker::InvalidateRegion` with a page-exact
  window, and then to `TextureCache::InvalidateMemory`. There is no `Reconcile`, because nothing
  changes host protection.
- So the CPU-dirty bits, mirrors, serials, the dirtied log, `ForgetKnownFills` and the image marks
  behave as they do today.
- *Alternative:* consumers read scan output. Rejected as too invasive (Non-Goals).

**D6. Synchronous needs keep a faulting protection.**
- While a `KYTY_IMAGE_WRITEBACK_GUARD` write-back runs, its range is protected with `mprotect`,
  because a CPU write has to wait for the write-back.
- With `KYTY_READBACK_SIDE_WRITES`, the retried write no longer faults; the next collection
  reports it.

**D7. Page-exact tracking on asynchronous memory.** Fault-ahead windows, `KYTY_FAULT_AHEAD_ADAPT`
and the Linux `SlowLevel` floor do not apply to it, and hot-page streaks do not form because there
are no faults. Other memory keeps all of them.

**D8. Diagnostics.**
- Startup: mode, features, self-test result and scan cost.
- The `KYTY_FAULT_COST_LOG` period line: collections, reported pages, replayed pages and scan time.
- `FaultMap`: collected writes as their own source.

**D9. The measurement gate comes before the mode itself.**
- `tools/fault-bench`: this session's benchmark, as a Linux-only `EXCLUDE_FROM_ALL` target.
- `KYTY_UFFD_WP_ASYNC_ESTIMATE=1` with the current backend: it counts the (chunk, epoch) pairs
  the consumers in D4 would scan, estimates their cost from the startup microbenchmark, and logs
  that next to the measured tracking cost each period.
- A user-owned game run with the estimator decides whether the mode is worth finishing. Any
  default change is a separate change.

## Risks / Trade-offs

- [Games with many fences and large watched sets make scans dominate] → the estimator gate, 2 MiB
  chunk and epoch gating, and scans limited to registered, consumed memory.
- [A consumer reads CPU-dirty state without collecting first, so GPU work uses stale data] →
  D4's audited call-site list, plus `KYTY_UFFD_WP_ASYNC_VERIFY=1`. It fails loudly when a consumer
  reads a chunk that was not collected in the current epoch, and the tests run with it.
- [A write lands between a collection and its use within one epoch] → that write races the GPU on
  hardware too (`syncEpoch.h`). This is the same reasoning the epoch-gated facts already rely on.
- [Kernel-mode writes (a `read()` into guest memory) with a user-mode-only descriptor] → the
  self-test checks them. If they fail, behavior equals today's `mprotect`, which the I/O paths
  already handle.
- [`mprotect` NoAccess transitions and write-protect bits interact on GPU-owned pages] → tests for
  NoAccess → Read → written → collected on real memfd mappings.
- [Transparent huge pages in guest memfds report at 2 MiB] → the self-test reports
  `PAGE_IS_HUGE`, and such memory stays on the fallback mode.
- [An opt-in mode rots] → the fork CI adds a ctest pass of the CPU-side tests with
  `KYTY_UFFD_WP_ASYNC=1`.

## Migration Plan

Opt-in switch; nothing persists. Unsetting `KYTY_UFFD_WP_ASYNC` restores today's behavior. A later,
separate change may consider a default once the gate (D9) passes in games.

## Open Questions

- The chunk size (2 MiB or 256 KiB) is tuned with the estimator. It does not change the approach.
- Whether asynchronous protection also resolves kernel-mode writes with a user-mode-only descriptor
  is answered by the self-test. It changes a log line, not the design.
