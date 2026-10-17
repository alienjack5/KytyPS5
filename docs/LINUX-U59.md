# U59 + Demon's Souls: Linux x86-64 build

> First written for the 2026-09-30 release (`u59-windows-20260930-demons`).

From now on the GitHub workflow `.github/workflows/u59-linux.yml` attaches
`KytyPS5-U59-Linux-x86_64.tar.gz` (built with Clang on Ubuntu 24.04, not tested in a game) to each release;
extract it, keep the folder together and run `./kyty_launcher` from that folder, so it finds `u59-preset.json`
(without the preset, lighting can render black). No game files, compatibility cheat files, firmware, saves
or caches are distributed. This is an unofficial build, not an upstream Linux release. The general Linux
build steps and dependencies are in the main README.

## Build

Follow the Linux dependencies in the main README. On Fedora, install:

```sh
sudo dnf install clang lld cmake ninja-build glslang qt6-qtbase-devel \
  mesa-libGL-devel libX11-devel libXcursor-devel libXext-devel \
  libXfixes-devel libXi-devel libXrandr-devel libXScrnSaver-devel \
  libXtst-devel libxkbcommon-devel alsa-lib-devel pulseaudio-libs-devel \
  systemd-devel dbus-devel wayland-devel wayland-protocols-devel
```

From this branch:

```sh
git submodule update --init --recursive
cmake -S . -B _Build/linux -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DKYTY_EMULATOR_IPO=ON -DKYTY_BUILD_ORIGIN=Fork \
  -DKYTY_BUILD_REPOSITORY=Jetsku/KytyPS5 \
  -DKYTY_RELEASE_TAG=u59-linux-local
cmake --build _Build/linux --target launcher --parallel 8
cmake --install _Build/linux --prefix _Build/linux/install
cp tools/u59-preset.json _Build/linux/install/
./_Build/linux/install/kyty_launcher
```

The launcher reads the adjacent preset automatically. Direct `kyty_emulator`
CLI calls do not read it automatically. Add your legally obtained game folders
in launcher settings and configure display/per-game settings there.

## Guest write tracking (current main)

Kyty notices the game's writes to memory it shares with the GPU by protecting
those pages and catching the write fault. On Linux each fault is a signal plus
`mprotect` calls, and every `mprotect` takes the process-wide memory-map lock,
so with many game threads writing at once the faults queue behind each other.
A Linux user measured about 83 us per fault (Windows: a few us) and about 8 fps
at the Astro Bot Sky Garden. Current main opens a larger window around each
write fault, scaled automatically to the fault cost the emulator measures on
the PC (`KYTY_FAULT_AHEAD_ADAPT`, on by default; Linux with `mprotect` starts at
1 MiB windows). With a slow-fault simulation on Windows: about 9 -> 27 fps; in a
WSL2 benchmark the writer threads' stalls fell from 35-44 ms to about 1 ms per
frame. The log shows the measured costs in a `Kyty platform:` line and
`Kyty fault cost:` lines (at startup and every 60 s).

1. Kernel 6.4 or newer is recommended (`uname -r`).
2. Raise the memory-map limit (protected pages split the guest mappings; at the
   default limit of 65530, `mprotect` fails and Kyty stops):
   `sudo sysctl -w vm.max_map_count=1048576` (until reboot; to keep it, put
   `vm.max_map_count=1048576` in `/etc/sysctl.d/99-kyty.conf`).
3. Optional: `"KYTY_UFFD_WP": "1"` in `u59-preset.json` tracks the writes with
   userfaultfd write-protection instead of `mprotect` (off by default; untested
   in a game on Linux so far). It needs kernel 5.19 or newer; 6.4 adds it for
   all guest memory. In a WSL2 benchmark with 15 writing threads a fault cost
   about 11 us instead of about 230 us; with the larger windows above, plain
   `mprotect` was as fast in that benchmark. The log then says `guest write
   tracking with userfaultfd write-protection (KYTY_UFFD_WP=1): on`. If it says
   `unavailable`, the kernel is too old or userfaultfd is blocked, and
   everything runs as without the flag.
4. Optional, measurement only: `"KYTY_UFFD_WP_ASYNC_ESTIMATE": "1"` in
   `u59-preset.json` (or `KYTY_UFFD_WP_ASYNC_ESTIMATE=1` in the environment of a
   direct `kyty_emulator` run) estimates what asynchronous write tracking would
   cost in the game, while the game keeps running with the current tracking.
   See the next section.

### Measuring what asynchronous tracking would cost

Linux 6.7 and newer can resolve a write to a write-protected page in the kernel
(`UFFD_FEATURE_WP_ASYNC`), without a signal, and report and protect again
exactly the written pages in one call (`PAGEMAP_SCAN`). The planned
asynchronous tracking mode (`KYTY_UFFD_WP_ASYNC`) would collect the written
pages that way before GPU work uses a range, instead of taking a fault per
write and marking a whole 1 MiB window dirty. `tools/fault-bench` measures the
modes without the emulator (`cmake --build _Build/linux --target fault_bench`,
then see `tools/fault-bench/README.md`); on an i7-11800H the asynchronous mode
cost 1.1-1.3 ms per frame instead of 2.2-3.7 ms with 4-15 writing threads.
Whether a game gains depends on how much memory its draws touch between
synchronization points, so the estimator counts that in the game:

```sh
KYTY_UFFD_WP_ASYNC_ESTIMATE=1 KYTY_FAULT_COST_LOG=10 \
  ./_Build/linux/install/kyty_emulator --game <game folder>
```

With the launcher, put `"KYTY_UFFD_WP_ASYNC_ESTIMATE": "1"` and
`"KYTY_FAULT_COST_LOG": "10"` in `u59-preset.json` (the log line comes every
60 s otherwise). At startup the log shows what the kernel offers, the self-test
of the asynchronous mode with the measured cost of one scan, and the tracking
mode, which stays as it was:

```
Kyty platform: asynchronous userfaultfd write-protection (KYTY_UFFD_WP_ASYNC_ESTIMATE=1): available (kernel features 0x1ffff, user-mode-only descriptor, PAGEMAP_SCAN on /proc/self/pagemap)
Kyty platform: asynchronous write-protection self-test: ok (... one scan of 2 MiB with 64 written pages 8.4 us)
Kyty platform: guest write tracking: mprotect
```

On a kernel without the asynchronous mode the first line says `unavailable`
and why, and the estimate uses an assumed scan cost of 10 us. Then, every
`KYTY_FAULT_COST_LOG` seconds, next to the `Kyty fault cost:` line:

```
Kyty async estimate: last 10 s, per frame: 42.0 chunks of 2 MiB to collect in 310.5 consumer calls (buffer 20.1, written 12.3, image 8.9, bda 0.5, gpu-dirty 0.0, download 0.2), ~0.37 ms at 8.9 us per chunk (measured by the startup self-test); measured now 2.41 ms (faults 1.20, protect 0.91, unprotect 0.30)
```

- `chunks of 2 MiB to collect`: the chunks of guest memory the asynchronous
  mode would scan per frame, once per chunk and synchronization point, counted
  for the consumer that first asked for it: a buffer synchronized for reading
  (`buffer`) or for writing (`written`), an image refreshed (`image`), the BDA
  pass (`bda`), a range marked GPU-written (`gpu-dirty`), a readback
  (`download`). `consumer calls` are the synchronizations that asked.
- `~N ms at M us per chunk`: the estimated scan time per frame; `M` from the
  startup self-test, or 10 us assumed. It is an upper bound: every chunk any
  consumer touches in a synchronization interval counts.
- `measured now`: what the current mode spends per frame in write faults and
  protection calls, the time the asynchronous mode would replace. The
  comparison leaves out one more saving: with 1 MiB windows a fault marks 256
  pages dirty and all of them are uploaded, while the asynchronous mode marks
  only the written pages (5-6 times fewer in the benchmark).

A `Kyty async estimate: whole run:` summary comes at exit. The asynchronous
mode is worth finishing for a game where the estimate stays clearly below the
measured time in the places that matter.

## Portability changes

- Enable exceptions for `src/common/profiler.cpp` on Linux only: loading
  diagnostics use `try`/`catch`, while the global compiler flags disable exceptions.
- Specify the `uint64_t` return type of the `WriteFaultWindow` lambda. On Linux,
  `uint64_t` and the result of `strtoull` are distinct types. Its logic is unchanged.

AI assistance was used to diagnose these errors, apply the minimal changes,
build and test the binaries, prepare documentation, and publish this fork.
No upstream pull request or claim of human code review is made.

## Verification and limitations

The initial local build was tested on Fedora 44 x86-64 with Clang 22.1.8 and
Qt 6.11.2. Release mode and IPO are enabled. Fourteen selected CTest tests
passed, including emulator CLI validation, profiler counters, page manager,
draw preparation, game patch filtering and input pulse helpers.
Installed CLI help and launcher startup with the normal desktop backend,
Wayland and offscreen were checked. Forced X11 platform initialization failed
in the build session; the default desktop launch succeeded.

The full regression suite and game/Vulkan rendering were not tested.
Demon's Souls correctness and FPS are not verified or promised.
Qt libraries/plugins are included in the local archive, but system libraries
remain dependencies. Compatibility with other Linux distributions is untested.
Keep the whole extracted runtime directory together.

See the archive's `LINUX-README.txt`, `test-results.txt`, and
`linux-portability.patch` for build-specific details. Preserve `LICENSE` and
third-party attribution when redistributing.
