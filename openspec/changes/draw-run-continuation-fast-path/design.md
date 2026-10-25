# Design

## Context

See proposal.md for the motivation. The pieces this design builds on, as they are today:

- **Commit pipeline under `KYTY_CP_SEQ=1`**: the sequencer parses PM4 and publishes draws to the
  draw-prep window; six workers prepare them (`DrawPrep::Prepare`, binding plans, certificates); the
  GPU thread (resolver) commits each one in order (`RenderExecutor::ExecutePreparedDraw`,
  `renderDraw.cpp`), encoding ops into the command stream the recorder turns into Vulkan. Every
  per-draw resolution against the buffer and texture caches runs on the resolver.
- **Draw runs (`drawRun.h`)**: `DrawRunRecordDraw` keeps a record (`m_run`) after each eligible draw
  (structure key, activity epoch, command identity, tick, rendering serial, push epoch, programs,
  targets, the images it refers to). `DrawRunCandidate` accepts the next draw as a continuation when
  the record's conditions hold; the continuation then keeps targets, texture/sampler resolutions,
  pipeline, acquisition, transitions and dynamic state, and still resolves, uploads and binds all
  per-draw data: `RebindBuffers` (`descriptors.cpp`: one `NativeStorageBuffer` → `ObtainBuffer` →
  `ObtainReadBinding`/`ObtainBufferNow` per V#, then shader-data uploads), `AcquireVertexBuffersInto`
  and `PrepareIndexBuffer` (one `ObtainBuffer` per merged vertex range and per index buffer),
  `CommitBindings` (descriptor writes, push constants, vertex binds). `DrawPrep::Validate` runs
  `CertificateHolds` first: coherence-log check of the preparation's interval over the draw's
  certificate ranges, then `AllCleanAfterLog` over every range.
- **Binding memo (`ObtainReadBindingMemo`)**: a hit still costs a slot hash, a memory-tracker
  `RangeSignature`, epoch/guard/tick comparisons, an LRU touch and counters (~90 ns); a miss runs
  `ObtainBufferNow` with synchronization (~0.5 µs). Astro's continuations average 3.3 hits and 0.6
  misses per draw.
- **Diagnostics**: `KYTY_CP_COMMIT_STATS=steps` (`commitStats.h`) times every phase and step per
  draw class; `KYTY_DRAW_RUN=verify|exit` recomputes the normal path for each continuation and
  counts differences; the `draw_run_*` ctest variants run the engine suite in those modes.
- Constraints: the resolver is the only thread that mutates buffer-cache binding state; a
  continuation never ends the rendering instance; the project's rule for reuse is the one
  `drawRun.h` states: everything skipped must be a function of inputs the record pins, and its side
  effects must have no observer between the two draws.

## Goals / Non-Goals

**Goals:**

- Cut a continuation's commit from 5.4 µs to ≤ 3.0 µs on the measured machine without changing what
  the GPU receives, with the two reuse mechanisms individually switchable and verifiable.
- Keep every fallback explicit and counted; a precondition that cannot be proven takes today's path.
- Make the gain visible in the existing 10-second statistics lines.

**Non-Goals:**

- Changing the run eligibility rules (what starts or continues a run) or the structure key.
- Touching the preparation side (workers), the recorder, the sequencer or the spin policies.
- Replacing descriptor writes by buffer device addresses (a shader ABI change; a later proposal).
- The cost of rendering-instance boundaries (`instance` class, 216 per frame in this title).
- Cross-frame reuse of bindings (measured at 0.0% exact repeats).

## Decisions

### D1. Run-scoped buffer resolutions live in the run record, keyed by binding slot

Each eligible draw leaves, in `m_run`, one entry per V# binding slot (per stage), per merged vertex
range and for the index buffer: guest range `[base, end)`, the resolved `Buffer*` (or stream buffer
+ offset for stream hits), the cache buffer's base guest address, and the guest span the resolution
synchronized (for `ObtainBufferNow` the uploaded/clean span; for a memo hit the memo's range). On a
continuation with `KYTY_DRAW_RUN_BINDINGS=1`, before calling into the buffer cache for a slot, the
executor checks: the record is this run's (`activity`, `command`, `tick` equal), the slot's entry is
valid, and the draw's range for that slot lies inside the entry's synchronized span. If so, the
binding is `{entry.buffer, entry.offset}` and the step records a "reused" item (the entry is
reused only for exactly the recorded range: a partial range would need the memo's signature
check, which is what the reuse avoids). Any other range goes through `ObtainBuffer` as today and
refreshes the entry from the result. Validity of a cache buffer entry is the buffer structure epoch
(`BufferCache::BufferStructureEpoch`, the memo's own guard: it moves on every buffer
register/unregister), not an id generation; a stream-copy entry is valid within the record's tick.

*Tried and dropped — the "lookup skipped" case*: the first implementation passed the recorded
buffer id as the `ObtainBuffer` hint for a range inside the recorded cache buffer but different
from the recorded range (saving `FindBuffer`). The in-game verify run (ASTRO's PLAYROOM, 5 min,
~5 million checks) found 3 cases where `FindBuffer` returned another registered buffer covering the
same range: with overlapping cache buffers the GPU-written bytes live in one of them, so binding
the other is not equivalent. The hint path was removed; only exact-range reuse stays (0 mismatches
in the same run). The earlier draft's `SynchronizeKnown` is therefore not needed either.

*Why slot-keyed and not address-keyed*: continuations keep the program, so slot i of the next draw
is the same logical buffer (a ring that advanced, or the same mesh); a slot compare is one
comparison, no hash. *Why inside the record*: the record already carries the exact invalidation
conditions (`activity`, `command`, `tick`) and is reset by every non-continuation; no new lifetime.
*Alternative considered*: a faster memo (fewer counters) — it would shave tens of nanoseconds per
hit but keep the signature read and the miss path; the run entry removes both for the common case.

Validity argument: within the same tick and command buffer, with the activity epoch unchanged, the
only writers of guest memory are guest CPU threads; `drawRun.h` already accepts that a CPU write
racing draws of one command buffer is not something the guest can rely on, and the previous draw's
synchronization already decided the span's contents for this command buffer. Stream-buffer hits are
reused only when the stream allocation's tick equals the record's tick (the memo's own `guard`).

### D2. A run-level certified set — dropped after measurement

The draft kept, per run, the certificate ranges found clean in the tick and skipped their clean
verdict for continuations while `Coherence::Generation()` was unchanged. The `Steps` breakdown of a
continuation after D1 (`_console-20261018-165350.txt`) puts that work at `valclean` 0.17 µs plus a
log walk (`vallog`) of 0.05 µs that already reads under one entry per draw: ≤ 4% of the 4.65 µs
commit, for a change inside the coherence path. Dropped by decision with the user on 2026-10-18;
the `KYTY_DRAW_RUN_CERT` switch and its counters were removed again. The `programs` phase's
remaining 0.6 µs is `RefreshShaders` and binding-plan activation, not validation.

### D3. The continuation path drops bookkeeping whose inputs it keeps

A pass over `ExecutePreparedDraw` for the `m_run_active` path, guided by the `Steps` line of the
`cont` class after D1 (tasks.md 4.1 has the list). Each removal must be covered by
`KYTY_DRAW_RUN_BINDINGS=verify` / `KYTY_DRAW_RUN=verify` (the normal path recomputes the item and
compares) or by the engine suite's pixel comparison; an item that neither covers is not removed.

*Outcome (2026-10-18, `_console-20261018-171641.txt` and `-173533.txt`)*: three items were tried,
one is kept. The in-place record fill stays (neutral within run-to-run noise: `cont` commit 4.72 µs
vs 4.65 with D1 alone; the noise between two runs of the same area is ±0.3 µs on the step sum). The
vertex/index bind deduplication was reverted: it kept about one bind per continued draw, but
`vertexcommit` did not move (0.146 → 0.145 µs: the encoder's bind is cheap) while recording the
bound state cost 0.04 µs in `runrecord`. The exact-range `FindBuffer` skip was reverted after its
verify stopped the next in-game `exit` run: `FindBuffer` returned another buffer for the same range
under the same structure epoch, so the lookup is not deterministic between overlapping cache buffers
(the same fact behind the dropped hint path); in mode 1 the skip was never consumed, since the
`RebindBuffers` reuse resolves the slot from the record. The remaining steps of a continuation are per-draw data
movement (stream copies of new constants, descriptor writes, push constants), the draw command and
the late image check; trimming them further needs the run-level commit the proposal excludes (one
descriptor state per run, per-draw data by address).

### D4. Switch semantics follow the existing `KYTY_DRAW_RUN_*` family

`KYTY_DRAW_RUN_BINDINGS` and `KYTY_DRAW_RUN_CERT` are `Live::Switch`es with the `0 / 1 / verify /
exit` parsing of `KYTY_DRAW_RUN_IMAGE_EPOCH`. In `verify`, the fast path computes its answer and the
normal path runs as well; the executor compares buffer handle and offset (D1) and the certificate
verdict (D2), counts differences (`DrawRun::CountVerifyCheck`/`CountVerifyDiff`), logs the first
`KYTY_DRAW_RUN_LOG_LIMIT` of them, and uses the normal path's result; `exit` stops at the first. A
record made under one value is not continued under another (the record stores the mode it was made
with, as `KYTY_DRAW_RUN_WRITES` does). Both default off.

### D5. Measurement gate and acceptance

Same procedure as the baseline (`PROGRESS.md`, "Como medir"): ASTRO's PLAYROOM, the same gameplay
area, `kyty_run_profiled.sh`, `KYTY_CP_COMMIT_STATS=steps`, 3–5 minutes, before and after. Accept the
change for the preset when, over the 10-second intervals with ≥ 5 000 draws per frame:

- the `cont` class commit is ≤ 3.0 µs (baseline 5.4) with the gap unchanged (≤ 0.8 µs);
- the resolver's ms/frame (Σ draws × (commit + gap)) is ≤ 60% of baseline at equal draw counts;
- fps p50 in those intervals is higher than baseline (expected 13–20 → ≥ 25);
- `KYTY_DRAW_RUN_BINDINGS=exit`, `KYTY_DRAW_RUN_CERT=exit` and `KYTY_DRAW_RUN=exit` each survive
  five minutes of the same gameplay;
- God of War Sons of Sparta stays at 60 fps with both switches on;
- the full ctest suite passes with the new variants (GPU tests `-j1` on this machine).

## Risks / Trade-offs

- [A reused span hides a CPU write the normal path would have uploaded] → the reuse is limited to
  one command buffer, tick and activity epoch, the scope `drawRun.h` already treats as a single
  ordering point; `verify` mode compares against the normal path in-game and in the suite before the
  switch goes into the preset.
- [Cache buffer freed or replaced between the two draws] → the record stores the buffer's identity
  (slot id + generation) and checks it; a mismatch falls back and counts (`FallbackEvicted`).
- [Certificate skip misses a transition] → only ranges byte-equal to a certified range skip, only
  while `Coherence::Generation()` is unchanged; shader-map and mapping checks always run; `exit` mode
  in the suite and in-game.
- [Lean bookkeeping removes a side effect something observes] → each removal is tied to a verify
  item or a pixel test; anything not covered stays.
- [Gain smaller than modelled] → the steps line gives the per-step delta immediately; D1 and D2
  are independent, so each is kept or dropped on its own number.
- [Record grows] → ≤ 16 V# slots × stages + 8 vertex ranges + 1 index entry of ~40 bytes each; reset
  per run start, no allocation.

## Migration Plan

1. Land D1 behind `KYTY_DRAW_RUN_BINDINGS` (off), with its verify mode and ctest variants.
2. Land D2 behind `KYTY_DRAW_RUN_CERT` (off), likewise.
3. Land D3 under the existing continuation, covered by `KYTY_DRAW_RUN=verify`.
4. Measure (D5); put the accepted switches in `u59-preset.json`; record the numbers in
   `PROGRESS.md` and `docs/CHANGE-CATALOG.md`.
5. Rollback: unset the switch (live: both are `Live::Switch`, read per use); D3 reverts with the
   commits.

## Open Questions

- Whether vertex ranges that straddle two cache buffers (merged ranges across an allocation seam)
  should get an entry at all or always fall back; the first measurement says how common they are.
- Whether the `instance` class's 14 µs gap is mostly `RebindHead`/`g.head` work that D1's entries
  would also remove, or pass-boundary cost; decided by the post-change steps line, outside this
  change's acceptance.
