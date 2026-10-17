# fault-bench: guest write-tracking microbenchmark (Linux)

The GPU caches write-protect the guest pages they mirror and learn about the CPU's writes from the
protection. This benchmark measures what that costs per frame, per tracking mode, without the
emulator: a memfd mapped `MAP_SHARED` (as guest memory is), writer threads writing random pages,
and a protector thread playing the command processor, which collects what was written and
protects it again while the writers run.

| mode | how a write is noticed | how pages are protected again |
|---|---|---|
| `mprotect` | SIGSEGV; the handler releases a `window_pages` window (the fault-ahead window) | `mprotect` per window |
| `uffd` | SIGBUS, userfaultfd write-protection (`KYTY_UFFD_WP`) | `UFFDIO_WRITEPROTECT` per window |
| `async` | no signal: the kernel resolves the fault (`UFFD_FEATURE_WP_ASYNC`, Linux 6.7+) | one `PAGEMAP_SCAN` over the area reports and re-protects the written pages |

Per frame it prints the writers' wall time, the faults and their handler time (or the scans and
their time), the protection calls, the pages left CPU-dirty (what the emulator would upload) and the
kernel's minor page faults (the write-notify faults of shared memory). The first tenth of the frames
is warm-up.

## Build and run

```sh
cmake --build _Build/linux --target fault_bench
_Build/linux/fault_bench <mprotect|uffd|async> <threads> <window_pages> [frames=200] [pages_per_frame=4800] [area_mib=64]
```

`window_pages` is ignored by `async`. A user-mode-only userfaultfd descriptor is enough
(`vm.unprivileged_userfaultfd=0`); `uffd` and `async` fail with `userfaultfd failed` otherwise.

The reference matrix (the emulator's Linux default is `mprotect` with 1 MiB windows, 256 pages):

```sh
for t in 1 4 8 15; do
  _Build/linux/fault_bench mprotect $t 256
  _Build/linux/fault_bench uffd $t 8
  _Build/linux/fault_bench async $t 8
done
for a in 64 256 1024 4096; do _Build/linux/fault_bench async 8 8 100 4800 $a; done
```

Use the performance power profile: in the power-saver profile the same runs are 3-5x slower.

## Reference numbers

i7-11800H (8 cores, 16 threads), Linux 7.2.4, Fedora 44, performance profile, 64 MiB area,
4,800 pages written per frame, 200 frames. Per frame; "protect" and "scans" are the protector's
time, "dirty" the pages left CPU-dirty.

| threads | `mprotect`, 1 MiB windows (the Linux default) | `async` |
|---|---|---|
| 1 | writers 1.63 ms, protect 1.54 ms, 26,711 dirty | writers 2.52 ms, scans 0.54 ms (10 scans, 57 µs each), 4,714 dirty |
| 4 | writers 1.12 ms, protect 1.07 ms, 22,602 dirty | writers 0.91 ms, scans 0.38 ms (4 scans, 95 µs each), 4,539 dirty |
| 8 | writers 1.24 ms, protect 1.26 ms, 21,707 dirty | writers 0.76 ms, scans 0.38 ms (3 scans, 121 µs each), 4,456 dirty |
| 15 | writers 2.05 ms, protect 1.69 ms, 29,707 dirty | writers 0.70 ms, scans 0.36 ms (3 scans, 131 µs each), 4,319 dirty |

- `mprotect` convoys on the mmap lock: the handler costs 4 µs with one writer and 60-110 µs with 15.
  With 32 KiB windows the writers take 10-25 ms per frame, with 256 KiB 1.6-4.1 ms, which is why
  the Linux default is 1 MiB windows. Each fault then marks 256 pages dirty, most never written.
- `uffd` (synchronous) only wins with 32 KiB windows (writers 16.7 / 7.1 / 5.2 / 4.2 ms at
  1 / 4 / 8 / 15 threads); shared memory adds one write-notify minor fault per page written
  (~4,500 per frame), which sinks it at the larger windows. Releasing the window with
  `MADV_POPULATE_WRITE` instead made it far worse (23-283 ms per frame) and was dropped.
- `async` takes the same ~4,500 minor faults, but no signals and no per-window calls, and its
  dirty pages are exact. One scan costs about 0.5 µs per MiB of area: 127 µs at 64 MiB, 282 µs at
  256 MiB, 819 µs at 1 GiB, 1,945 µs at 4 GiB (8 threads). The emulator therefore scans only the
  ranges GPU work is about to use, not all of guest memory.
