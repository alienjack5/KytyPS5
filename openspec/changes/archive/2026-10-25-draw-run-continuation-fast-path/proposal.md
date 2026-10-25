# Proposal

## Why

In ASTRO's PLAYROOM (PPSA01325) gameplay runs at 13–35 fps on an i7-11800H + RTX 3070 Laptop while
the GPU is idle 40–50% of the time: the frame is bounded by the GPU thread that commits each
prepared draw (the "resolver" under `KYTY_CP_SEQ=1`), which is ~90% of one core while the sequencer,
the recorder and the six draw-prep workers spin waiting for it. `KYTY_CP_COMMIT_STATS=steps` over 38
ten-second intervals (2026-10-18, `_console-20261018-144552.txt`) shows the resolver's time per frame
(draws × cost per draw) tracking the frame time 1:1 (13 fps ↔ 60 ms, 27 fps ↔ 28 ms, 42 fps ↔ 18 ms).
The game issues 6 800–9 150 draws per frame; **90.6% of them continue a draw run** (`KYTY_DRAW_RUN=1`
is on; 67% of draws sit in runs of 17–32), yet a continuation still costs **5.4 µs of commit + 0.7 µs
of gap**, because the delta path re-resolves and re-binds every per-draw datum: V# buffers 1.1 µs
(`RebindBuffers`, ~3.3 binding-memo hits and 0.6 misses per draw), vertex/index buffers 0.9 µs
(`PrepareIndexBuffer`/`AcquireVertexBuffersInto`: one `ObtainBuffer` per range), descriptor pushes
0.8 µs (2.8 writes per draw), certificate validation 0.75 µs (~5.9 ranges), fixed setup/emit/post
1.1 µs. Cross-frame replay cannot help: the structure repeats 95–99% between frames but the exact
binding inputs repeat 0.0% (per-draw data is ring-allocated). Making a continuation cheap is the one
lever that scales with the draw count.

## What Changes

- **Run-scoped buffer binding reuse** (`KYTY_DRAW_RUN_BINDINGS`, default off; `verify`/`exit` as
  for the other `KYTY_DRAW_RUN_*` switches): a continuation whose V#, vertex-range or index-buffer
  guest range is exactly a range the same run already resolved and synchronized in this command
  buffer and scheduler tick, with no command-processor activity since (`DrawRun::ActivityEpoch`),
  reuses that resolution (cache buffer and offset) without a buffer-cache lookup, memory tracker
  signature or synchronization, and leaves identical vertex and index bindings bound. Anything else
  takes the current path. (The draft also reused a recorded buffer for a *different* range inside
  it; the in-game verify run showed the lookup may legitimately choose another covering buffer, so
  that part was dropped — design.md D1.)
- ~~Run-level certificate validation (`KYTY_DRAW_RUN_CERT`)~~ — dropped after the D1 measurement:
  the work it could skip is ≤ 0.2 µs of a 4.65 µs commit (design.md D2).
- **Leaner continuation bookkeeping**: the delta path stops redoing per-draw work whose inputs a
  continuation keeps by definition (state resets of kept fields, descriptor-set resets, binding
  marks and statistics hooks that only inform run starts). No switch: it is part of the existing
  continuation, covered by `KYTY_DRAW_RUN=verify`.
- **Diagnostics**: the `StartPhases`/`Steps` lines report the reuse outcomes per continuation
  (bindings reused, reuses that fell back) and the `DrawRun` line the totals with verify checks and
  mismatches, so the gain is measured in-game, not inferred.
- **Measurement gate before any default change**: the switch ships off; the preset turns it on for
  measurement; the acceptance numbers are in `design.md`.

Not in this change (separate proposals): the cost of a rendering-instance boundary (216 per frame at
~30 µs in this title), V# buffers through buffer device addresses instead of descriptor writes, the
spin-versus-park policy of the waiting threads, and every GPU-side item (alias-sync copies, pass
splits, game compute).

## Capabilities

### New Capabilities

- `command-processor/draw-run-continuation`: how a draw that continues a draw run is committed:
  what it may reuse from the run (structure today; buffer resolutions and certificate verdicts with
  this change), the conditions under which reuse is valid, the guarantee that the result equals the
  normal path, the verify modes that prove it, and the diagnostics that report what a continuation
  costs and reuses.

### Modified Capabilities

None: the project has no specs for the command processor yet.

## Impact

- Code: `src/graphics/host_gpu/renderer/renderDraw.cpp` (continuation path: `DrawRunCandidate`,
  `ExecutePreparedDraw`, `RebindBuffers`, `AcquireVertexBuffersInto`, `PrepareIndexBuffer`,
  the run record `m_run`), `src/graphics/host_gpu/renderer/drawPrep/drawRun.{h,cpp}` (record,
  switches, verify counters), `src/graphics/host_gpu/renderer/drawPrep/drawPrep.cpp`
  (`Validate`/`CertificateHolds`: run-level certified set), `src/graphics/host_gpu/renderer/cache/bufferCache.{h,cpp}`
  (a reuse entry point beside `ObtainBuffer`/`ObtainReadBinding`),
  `src/graphics/host_gpu/renderer/drawPrep/commitStats.{h,cpp}` (new items).
- Tests: `tests/DrawPrepTests.cpp`, `tests/BindingPathTests.cpp` and the `draw_run_*` /
  `draw_prep_engine_*` ctest variants (`CMakeLists.txt` ~1750–1800) gain variants with the new
  switches on and in `exit` verify mode; GPU tests run `-j1` on this machine.
- Configuration and documentation: two `KYTY_DRAW_RUN_*` switches in `u59-preset.json` for
  measurement; `docs/CHANGE-CATALOG.md` entry; `PROGRESS.md` numbers.
- Platform: none; the change is host-GPU-agnostic CPU work on the GPU thread.
