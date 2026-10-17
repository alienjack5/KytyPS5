# Tasks

## 1. Synthetic benchmark in the repository (gate, part 1)

- [x] 1.1 Add `tools/fault-bench/fault_bench.cpp` with the modes `mprotect`, `uffd` and `async` and the threads, window, frames, pages and area parameters, plus a Linux-only `EXCLUDE_FROM_ALL` CMake target `fault_bench`; verify that `cmake --build _Build/linux --target fault_bench` succeeds and that each mode prints one result line
- [x] 1.2 Write `tools/fault-bench/README.md` with the commands and the reference numbers from this change's proposal; verify that the documented commands run as written

## 2. Asynchronous probe and self-test (D1)

- [x] 2.1 Probe a dedicated asynchronous descriptor in `uffdWriteWatch` (`UFFD_FEATURE_WP_ASYNC | UFFD_FEATURE_WP_HUGETLBFS_SHMEM`, user-mode-only first, `/proc/self/pagemap`) and report it in `Support`; verify with a new test that forces the probe and checks the reported features on the local kernel
- [x] 2.2 Add the startup self-test on a one-page memfd (protect, write, scan, expect one page protected again, `PAGE_IS_HUGE` absent, a `read()` into the page) and the scan microbenchmark (µs per 2 MiB) to the `Kyty platform` lines; verify that the lines appear with `KYTY_UFFD_WP_ASYNC=1` and that the test asserts each self-test result
- [x] 2.3 Select the mode with the fallback chain (asynchronous, then `KYTY_UFFD_WP`, then `mprotect`) and log the reason once; verify with a test that hides a required feature and checks the logged fallback and that tracking still reports writes

## 3. Cost estimator (gate, part 2, D9)

- [ ] 3.1 Add the chunk and epoch bookkeeping (2 MiB chunks, epoch of the last collection) as standalone logic; verify with unit tests that a chunk is counted once per epoch and again after `SyncEpoch::Advance`
- [ ] 3.2 Count, under `KYTY_UFFD_WP_ASYNC_ESTIMATE=1`, the chunks each D4 site would collect (buffer synchronization with its relaxed and memo checks, `RefreshImage`, `SynchronizeBdaBuffers`, GPU-dirty transitions, downloads), and add the estimated collections and time to the `KYTY_FAULT_COST_LOG` line next to the measured tracking cost; verify that `shader_recompiler_compute_tests` passes with the estimator on and that its log shows the fields
- [ ] 3.3 Document the estimator and how to read its line in `docs/LINUX-U59.md`; verify that the documented commands run as written
- [ ] 3.4 Run a game the user owns with the estimator, record the estimated and measured costs in `design.md`, and decide whether to continue; verify that the numbers and the decision are recorded

## 4. Registration and host protection (D2, D3)

- [ ] 4.1 Register guest regions for asynchronous tracking lazily and arm each region once, and reset the registration state when a backing is replaced (both userfaultfd modes); verify with a new `VirtualMemoryAllocationTests` case that replaces a backing and finds the region registered again instead of `Failed`
- [ ] 4.2 Give asynchronous memory the `PageManager` semantics of D3 (watching arms, releasing makes no host call, NoAccess keeps `mprotect`, applied state records arming); verify with new `PageManagerTests` cases over a real asynchronous memfd mapping: an armed page is written without a signal and reported by a scan, and a page that goes from NoAccess to Read and is then written is collected
- [ ] 4.3 Fall back per region when `PM_SCAN_CHECK_WPASYNC` aborts a scan, logged once; verify with a test that scans an unregistered region and sees the fallback and one log line

## 5. Collection and replay (D4-D7)

- [ ] 5.1 Implement `CollectWrites(range)`: chunk and epoch gate, `PAGEMAP_SCAN` with `PM_SCAN_WP_MATCHING | PM_SCAN_CHECK_WPASYNC` and `PAGE_IS_WRITTEN`, filtering against write watchers, and batched replay into `MemoryTracker::InvalidateRegion` (page-exact) and `TextureCache::InvalidateMemory` on the command processor; verify with new `MemoryTrackerTests` cases over real memfd mappings: several threads write, one collection marks exactly the written pages CPU-dirty, and the write stress test (today Windows-only) passes on Linux with no lost writes
- [ ] 5.2 Call `CollectWrites` at every D4 site, and adapt the compute tests' write cases, which call `HandleFault(Write)` directly, to write without a fault in asynchronous mode; verify that those cases pass with `KYTY_UFFD_WP_ASYNC=1`
- [ ] 5.3 Add `KYTY_UFFD_WP_ASYNC_VERIFY=1`, which stops with the range and site when a consumer reads a chunk not collected in the current epoch; verify that the full suite passes with asynchronous mode and verification on
- [ ] 5.4 Give ranges under a `KYTY_IMAGE_WRITEBACK_GUARD` write-back a faulting protection on asynchronous memory (D6); verify with a test in which a CPU write during the write-back waits for it
- [ ] 5.5 Bypass fault-ahead windows, `KYTY_FAULT_AHEAD_ADAPT`, the Linux `SlowLevel` floor and hot-page streaks for asynchronous memory (D7); verify with a tracker test in which one byte written in a 1 MiB range makes exactly one page CPU-dirty

## 6. Diagnostics and documentation (D8)

- [ ] 6.1 Report collections, reported pages, replayed pages and scan time in the `KYTY_FAULT_COST_LOG` line, and collected writes as their own `FaultMap` source; verify with a run at `KYTY_FAULT_COST_LOG=1` that shows the fields
- [ ] 6.2 Document `KYTY_UFFD_WP_ASYNC`, its requirements (Linux 6.7+, user-mode-only descriptor), the fallback chain and the `ESTIMATE` and `VERIFY` switches in `docs/LINUX-U59.md`, including how to set it in `u59-preset.json`; verify that the documented steps work as written

## 7. Integration

- [ ] 7.1 Run the full suite locally with `KYTY_UFFD_WP_ASYNC=1` (CPU-side tests `-j6`, GPU tests `-j1`); verify that all tests pass
- [ ] 7.2 Add to the fork CI a ctest pass of the CPU-side tests with `KYTY_UFFD_WP_ASYNC=1` on lavapipe, which also covers the fallback path when the runner's kernel lacks a feature; verify with a green CI run whose log names the mode used
- [ ] 7.3 Compare frame times and `KYTY_FAULT_COST_LOG` lines in a game the user owns with the mode off and on, and record them in `design.md`; verify that the comparison is recorded

## Workflow follow-up

- Archive the change after review once the tasks are done (`/opsx:archive`).
- Any change to the default is a separate change, decided on the numbers from tasks 3.4 and 7.3.
