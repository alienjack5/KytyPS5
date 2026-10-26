# Design

## Context

See proposal.md for the motivation and the measured split of a continuation. The pieces this
design builds on, as they are today (fork `alien` at c44d9dfe, Jetsku int21 merged):

- **Commit pipeline under `KYTY_CP_SEQ=1`**: the sequencer parses PM4 and publishes draws; six
  workers prepare them (`DrawPrep::Prepare`: decoded V# ranges in the binding plan, certificates,
  texture resolutions); the GPU thread (resolver) commits each one in order
  (`RenderExecutor::DrawIndex` → `ExecutePreparedDraw`, `renderDraw.cpp`), encoding ops into the
  command stream the recorder turns into Vulkan. Every resolution against the buffer cache runs on
  the resolver, which is 99% of one core while the GPU is 32–36% busy.
- **Draw runs (`drawRun.h`)**: an eligible draw leaves a record (`m_run`): structure key, activity
  epoch, command identity, tick, rendering serial, programs, targets, image marks, and since
  `KYTY_DRAW_RUN_BINDINGS` the resolution of every V#, vertex range and index buffer. A
  continuation keeps targets, texture/sampler resolutions, pipeline, acquisition, transitions and
  dynamic state, re-resolves per-draw data (reusing a resolution only for the exact recorded
  range) and goes through the whole generic commit flow (`ExecutePreparedDraw` has 22 `m_run_active`
  branches inside a ~1 000-line function).
- **How a shader reads a bound buffer**: slot *i* of `program.info.buffers` is element *i* of the
  stage's `Buffers` descriptor binding (`DescriptorBindingKind::Buffers`, a storage-buffer array);
  the host writes `vk::DescriptorBufferInfo {buffer, aligned offset, range}` per slot per draw and
  packs the sub-alignment adjustment into the shader data (`BindingLayout::memory_offset_dword`,
  one byte per slot), uploaded per draw (`UploadShaderData`, a stream-ring allocation deduplicated
  against the site's last upload) and bound as the `ShaderData` descriptor. Loads bounds-check in
  the shader (`EmitMemoryElementInBounds`: an out-of-range element reads zero; `robust_buffer_loads`)
  and the descriptor range is whole dwords (`StorageDescriptor`). Int21 binds constant-only slots
  as uniform buffers (`KYTY_UNIFORM_BUFFERS`, `DescriptorBindingKind::UniformBuffers`). Push
  constants are one 128-byte range (`PushData::DwordCount` 32), used by mesh draws and user clip
  planes; graphics vertex/pixel programs push nothing per draw unless those apply.
- **Buffer device addresses**: every cache buffer has `BufferDeviceAddress()`; the BDA page table
  (`BufferCache::m_bda_pagetable_buffer`, one 64-bit device address per 16 KiB guest page,
  `CACHING_PAGEBITS` 14, written by `ChangeRegister` on register/unregister; the host mirror is
  `m_page_table[page] = id`) lets a shader turn a guest address into a `PhysicalStorageBuffer`
  pointer (`get_bda_pointer`: shift, table load, missing-page fault bit, add) — the path
  `KYTY_SRT_VARIANT_READS` and `KYTY_BDA_WRITES` use for V#s the shader computes
  (`ResourceKind::IndirectBuffer`, `LoadBda`/`LoadBdaDword` in `spirvEmitterMemory.cpp`). A
  program that uses it has `info.uses_dma`, and `PrepareGraphicsBindings` calls
  `RenderContext::PrepareBda` before its draws.
- **Current-data for GPU reads**: `ObtainBuffer` synchronizes one binding (CPU-dirty bytes
  uploaded or copied to the stream ring, GPU-written buffers given their barrier) and the binding
  memo (`ObtainReadBindingMemo`) short-cuts repeats. Separately, `BufferCache::SynchronizeBdaBuffers`
  makes *all* registered buffers in the GPU-mapped ranges current: once per synchronization epoch
  (`SyncEpoch`, advanced before every ordering packet, slice start, service command and mapping
  change; `KYTY_BDA_SYNC_EPOCH` skips a pass inside an unchanged epoch and structure), through the
  dirty-log pass (ranges the tracker dirtied since the last pass), the hot-page pass and a rare
  full scan. In the measured run it ran 2.1 dirty-log passes and 3.9 hot passes per frame (1.47 MB)
  for the few programs with computed V#s; the console line `Kyty BDA passes` reports it.
- **Diagnostics**: `KYTY_CP_COMMIT_STATS=steps` (`commitStats.h`: 13 phases, 59 steps, sub-phases
  and items per draw class), `KYTY_DRAW_RUN=verify|exit` and `KYTY_DRAW_RUN_BINDINGS=verify|exit`
  (the normal path recomputes a kept item and compares), the engine suite's pixel comparison in the
  `draw_run_*` ctest variants, `KYTY_BUFFER_LOOKUP_STATS`.
- **Constraints**: the resolver is the only thread that mutates buffer-cache binding state; a
  continuation never ends the rendering instance; `drawRun.h`'s rule for reuse: everything skipped
  must be a function of inputs the record pins, and its side effects must have no observer between
  the two draws; a changed shader codegen option changes the shader/pipeline cache key (one-time
  rebuild on first boot, as int21's key change did).

## Goals / Non-Goals

**Goals:**

- A continuation at ≤ 2.5 µs (commit + gap) on the measured machine, from 4.62, at ≥ 4 000
  continuations per frame, with the GPU's work unchanged in content.
- Per-draw data for read-only buffers described once per run (the descriptor set) and addressed per
  draw (one table pointer); no per-binding memo, signature or synchronization for addressable
  ranges; no descriptor write per continuation when the stage's buffers are all addressable.
- A commit path for continuations that touches only what a continuation can change, with every
  skipped item covered by a verify compare or the pixel suite.
- The epoch synchronization pass measured *before* the shader work decides between the two
  variants of D1; every fallback explicit and counted; the gain visible in the ten-second lines.

**Non-Goals:**

- In-shader vertex fetch (vertex input stays fixed-function; the binds remain), compute dispatches,
  written or formatted buffers by address, cross-frame reuse, the instance-boundary cost, GPU-side
  work, the spin policy.
- Changing run eligibility or the structure key.
- Removing the certificate check from continuations (D2 of the archived change: ≤ 0.3 µs and it is
  the correctness proof of the plan's decoded ranges).

## Decisions

### D0. Measurement gate: the epoch synchronization pass under every draw

Before any shader change, a diagnostic switch `KYTY_BDA_SYNC_ALL_DRAWS=1` makes
`PrepareGraphicsBindings` call `PrepareBda` for every draw (as D1 will for draws with addressable
slots), with nothing else changed. ASTRO's PLAYROOM, same area, 3–5 minutes, `KYTY_CP_COMMIT_STATS=steps`;
read the `Kyty BDA passes` line (passes per frame by kind, MB uploaded) and the `bda` step of the
`cont` and `instance` classes. The pass replaces per-binding synchronization worth about 0.4 µs per
continuation today (`readhit` 0.14 + `readmiss` 0.27 sub-phases: 0.46 memo hits and 0.56 stream
copies per draw) plus the memo part of vertex/index acquisition: **≈ 3 ms per frame at 7 000
draws**. Decision rule: if the passes cost ≤ 1 ms per frame in that run (and the dirty-log pass
stays the common kind), D1 goes *variant A* (current data by the epoch pass, lean lookup without
synchronization). If they cost more, D1 goes *variant B*: the lean lookup keeps the binding memo
and `ObtainBufferNow` for misses (today's synchronization), and only the descriptor side changes;
the acceptance target moves to ≤ 3.0 µs. The switch stays as a diagnostic either way.

**Measured 2026-10-25 (`_console-20261025-195210.txt`, 3.4 min, same area): variant A is rejected.**
With the pass under every draw: 5 200–8 100 calls per frame, 98.5% skipped by the epoch gate, but
still **80 passes per frame** (55–66 hot-page passes, 22–24 dirty-log passes uploading 29–33 MB per
frame, against 2–4 passes and 8–12 MB without the switch): **3.5–3.6 ms per frame inside the
passes**, the epoch advancing about 80 times per frame (command-buffer and wait boundaries). The
passes land on the instance starts (`instance` class `bda` step 0.8 → 11.1 µs, 18.9 → 31.8 µs per
boundary) and the resolver went from 41.3 to 46.2 ms per frame (continuation 4.62 → 5.19 µs from
the extra upload traffic), fps p50 in the heavy windows 18.1 → 16. The pass would cost more than
the ≈ 3 ms of per-binding synchronization it replaces, and it uploads four times the bytes (whole
dirtied ranges, not the ranges the draws read). **D1 and D2 go variant B**: the binding memo and
`ObtainBufferNow` keep synchronizing each range; what changes is how the result is delivered (the
table, no descriptors) and how a continuation re-finds a buffer it already resolved (the owner
check). `PrepareBda` stays where it is (programs with computed V#s). Acceptance: ≤ 3.0 µs.

*Why first*: it is a 20-line change and one gameplay run, and it decides the shape of everything
below. *Expected*: the epoch advances at ordering points (per command buffer, wait, flush), of
which this title has a few hundred per frame; most passes find an unchanged epoch or an empty
dirty log (`BdaSyncEpochSkips`), and the dirty-log pass uploads the same ring bytes the stream
copies move today, in fewer and larger copies.

### D1. Read-only buffers addressed through a per-draw binding table

**Which slots**: a slot of `program.info.buffers` is *addressable* when the resource is read-only
(`!written`), unformatted (`!formatted`: raw dword loads, `S_BUFFER_LOAD`, structured loads with
stride and swizzle handled in the shader as today), not a mip-statistics or indirect-parameter
buffer, and — unless `KYTY_BUFFERS_BY_ADDRESS_UNIFORM=1` — not a uniform-view slot of int21
(`UniformBufferResources`: those keep the uniform descriptor and its constant-cache path; the
option lets the GPU-time trade be measured). Everything else keeps its descriptor. The
classification is a function of the compiled program, so a program's descriptor set shape is fixed
and a run (same program) never changes it.

**The table — refinement found at task 1.2 (2026-10-25)**: the stage's *shader data* block already
travels per draw by push constants when it fits the 128-byte range (`BindingLayout::UsesPushData`,
cursor shared by the stages) and by a stream-ring upload with the `ShaderData` descriptor
otherwise (`EmitShaderDataDwordLoad` reads either). The binding table is therefore not a new
object: it is the shader data block extended with the slot entries (four dwords per addressable
slot after the mip-statistics fields), delivered in the first of three ways that fits: (1) in push
constants as today; (2) by a two-dword device-address pointer in push constants to the uploaded
block (new: `BindingLayout::table_pointer_dword`, `EmitShaderDataDwordLoad` reads through a
`PhysicalStorageBuffer` pointer), which keeps zero descriptor writes for the stage; (3) by the
`ShaderData` descriptor as today (one write per stage per draw). Addressable slots have no
`Buffers` or `UniformBuffers` element and their memory-offset byte stays zero. The paragraph
below describes the same content as first drafted; the delivery order above supersedes its
"one upload per draw with a pointer" wording.

**The table (first draft)**: per stage, one stream-ring upload per draw holding, in this order, the slot entries
(`uint64 device_address`, `uint32 size_bytes`, `uint32 flags`; a null slot has address 0 and size
0) for every addressable slot, then the shader data the stage uploads today (user dwords, memory
offsets of the descriptor-bound slots, mip-statistics fields), then the flattened SRT when the
program has one. Its device address (`BufferDeviceAddress()` of the stream buffer plus offset) is
written to push constants at a layout slot after the existing mesh/clip-plane dwords
(`PushData::TablePointerDword`, two dwords per stage that has a table, assigned by
`BindingLayout`). The `ShaderData` and `FlattenedSrt` descriptors are not emitted for a stage with a
table: the shader reads those sections from the table through the same pointer. Dedup: the upload
keeps `UploadShaderData`'s last-upload check (identical table bytes within a tick reuse the
allocation); with addresses that advance every draw the check rarely hits, so it is kept only
because it is already there. Size: 16 bytes per slot + the shader data; Astro's stages use 3–4
slots, so about 100–200 bytes per stage per draw, the same order as today's shader-data upload.

**The shader**: a new codegen option `buffers_by_address` (`KYTY_BUFFERS_BY_ADDRESS`, default off,
part of the shader and pipeline cache keys). With it, `BindingLayout` gives addressable slots no
descriptor element and records the table layout; the emitter loads the table pointer from push
constants once per invocation (`OpConvertUToPtr` to a `PhysicalStorageBuffer` struct), and for an
addressable slot loads its entry once (address and size), then serves every access of that slot
with `address + byte_offset` through a `PhysicalStorageBuffer` dword pointer, bounds-checked
against `size_bytes` with the same predicate as the descriptor path (`EmitMemoryElementInBounds`
on the element index; out of range reads zero), including the whole-dword rule and the
`readonly`/`NonWritable` semantics where the storage class allows (`Restrict`/`Aliased` decorations
per the SPIR-V rules for physical pointers). The existing `LoadBdaDword` is refactored so the
"load a dword from a device address" part is shared and `get_bda_pointer` is only used by the
computed-V# path. Programs without addressable slots compile exactly as before. Alignment: the
entry's address is the exact guest base mapped to device memory, so no sub-alignment adjustment
exists for addressable slots (the `memory_offset` byte of those slots is unused).

**The host**: `RebindBuffers` resolves an addressable slot with the lean lookup (D2) and writes
its table entry instead of calling `NativeStorageBuffer`; `CommitBindings` builds the descriptor
write list from the descriptor-bound slots only, pushes the table pointer (push constants; the
shadow compare in `CommandBuffer::PushConstants` skips an equal push) and, for a continuation whose
descriptor set has no per-draw member, pushes no descriptors at all (`KYTY_DRAW_RUN_PUSH` already
drops image/sampler writes for continuations; the set becomes empty of per-draw writes). The run
record keeps, per addressable slot, the owner buffer id and structure epoch it resolved (D2's
fast path for the next draw).

*Alternatives considered*: (a) guest addresses in the table and `get_bda_pointer` per access on the
GPU — no host lookup at all, but a page-table load per dword access and the missing-page fault
round trip on first use; rejected for GPU cost (the title has 20–29 ms of GPU work in heavy
scenes) and because the host lookup is ~30 ns per slot. (b) Per-slot device addresses in push
constants — 16 bytes per slot do not fit the 128-byte range with mesh/clip data for all stages;
rejected. (c) One descriptor set per run with `VK_EXT_descriptor_buffer` — not available on every
target driver and still one write per slot per draw; rejected.

### D2. Lean resolution: owner lookup, structure epoch, current data by the epoch pass

For an addressable V# range, a vertex range or an index buffer, the resolver:

1. Takes the run record's entry for the slot when the record is this run's and the entry's owner
   id and structure epoch still hold (`BufferStructureEpoch()` unchanged since) and the owner
   covers the new range (`[CpuAddress, CpuAddress + Size)` ⊇ range): the device address is
   `owner.BufferDeviceAddress() + (range.base − owner.CpuAddress())`; no cache call. This is the
   D1 reuse of the archived change generalised from "exactly the recorded range" to "inside the
   recorded owner", which is now sound because the *owner* is a function of the page (host page
   table), not of `FindBuffer`'s choice among overlapping buffers (the archived change's dropped
   hint path). A range whose first page's owner differs from the recorded one, or that the owner
   does not cover whole, goes to 2.
2. Otherwise asks the cache for the owner of the range's first page (`m_page_table` lookup) and
   checks that it covers the range; if so, uses it as in 1 and refreshes the record entry.
3. Otherwise (no owner, or the range spans two buffers) calls `ObtainBuffer` as today, which may
   create or join buffers (the structure epoch then moves and the next epoch pass covers the new
   buffer's pages, which start CPU-dirty).

Current data — variant A, **rejected by D0** (kept here as the measured alternative):
`PrepareGraphicsBindings` would call `PrepareBda` before a draw with addressable slots, so every registered buffer's CPU-dirty bytes within the mapped ranges
are uploaded once per synchronization epoch, before the draws of that epoch are recorded — the same
guarantee `KYTY_SRT_VARIANT_READS` relies on for computed V#s, now for bound ones. The lean lookup
does no per-binding upload. A GPU-written owner (`IsGpuModified`, pending write ownership) keeps the
read-after-write handling of `ObtainBuffer`: the lean lookup checks the flag and, when set, takes
path 3 for that range (counted). Vertex and index ranges follow the same three steps; their bind
needs the owner's handle and offset, not a device address, and the bind is encoded only when the
owner or offset changed since the previous draw of the run (the archived change found identical
binds cost 0.01 µs to re-encode; the saving here is the resolution, not the bind).

**Variant B (chosen)**: per-binding synchronization stays, so the owner lookup of steps 1–2 cannot
serve a range *inside* the recorded owner that differs from the recorded range: its bytes are new
CPU writes (a ring advancing) that only the memo path uploads. A continuation therefore reuses a
resolution exactly as `KYTY_DRAW_RUN_BINDINGS` does today (the exact recorded range, D1 of the
archived change) and every other range goes through `ObtainReadBinding`'s memo and
`ObtainBufferNow`; the owner lookup is not built in this variant. What changes is the delivery:
the resolution becomes an address entry instead of a descriptor (D1), for both the reused and the
freshly resolved slot. Expected from the step
table: descriptors and push 0.52 → ≈ 0.15, `rbbindings` 0.66 → ≈ 0.6 (no `StorageDescriptor`
build), vertex/index unchanged; with D3 the control, post and gap groups 2.2 → ≈ 1.1: a
continuation of about 3.0–3.2 µs.

*Why the owner is sound where `FindBuffer` was not*: the dropped hint path reused *a* buffer that
covered the range while another registered buffer could cover it too and hold the GPU-written
bytes; the page-table owner is unique per page and is what the BDA page table on the GPU points
to, so an address read through it reads what the computed-V# path already reads. A range that
straddles two owners is exactly the case `ObtainBuffer`'s join exists for, and it takes that path.

### D3. The continuation fast lane

With `KYTY_DRAW_RUN_FAST=1`, `DrawIndex`/`DrawAuto` route a draw whose `DrawRunCandidate` accepted
it as a continuation to `ExecuteRunContinuation`, a new function beside `ExecutePreparedDraw`
that does, in order: (1) the late check as today (command identity, `DrawRunImagesUnchanged`,
activity epoch; a failure falls back to `ExecutePreparedDraw` — counted, `latefallback`); (2) the
certificate (`DrawPrep::Validate`) and the plan activation of the prepared slot (the two
`programs`-phase steps that are correctness, 0.45 µs; `RefreshShaders` itself is skipped: the
programs are the record's); (3) per-draw data: lean resolution of every addressable slot and of
the vertex/index ranges (D2), the table upload and the push-constant write, descriptor writes only
for descriptor-bound slots whose resolution changed (none when all slots are addressable);
(4) vertex/index binds when changed; (5) the draw command with the occlusion and shader-write hooks
the generic path emits after it; (6) the run record update (in place) and the statistics. Not
executed, because the record pins their inputs: register binding into the executor, state reset,
target ops and resolution, `RunDrawChecks`, `DrawHasValidVertexShader`/`DrawHasActivePixelShader`,
primitive-restart and index-size resolution (recorded; a change breaks the run at
`DrawRunCandidate`, which already keys on them), `PrepareBindings` of textures and samplers,
`RebindImages`, pipeline selection, acquisition, `DrawIsBarrierSafe`, dynamic state, `BeginRendering`,
pipeline bind, debug info, repeat trace. Mutex: the lane takes the render mutex once around (3)–(6)
as the generic path does.

Verify: `KYTY_DRAW_RUN=verify` already recomputes pipeline, acquisition, transitions, dynamic state
and descriptors for a continuation and compares; the lane adds compares for the items it newly
skips and that have an observable result: the plan activation outcome, the primitive-restart /
index-size decision, the barrier-safety classification, and the descriptor write list (shape and
contents) of descriptor-bound slots. In verify mode the lane runs, then the generic path's
decisions are recomputed on a scratch state and compared; `exit` stops at the first difference.
Pixel equality: the engine suite in the `draw_run_fast*` variants.

*Why a separate function and not more branches*: the measured control cost is fifty steps of
0.02–0.27 µs each inside one function whose every branch pays for the generic case (the archived
change's D3 trimmed three of them and gained nothing); a lane that starts from "nothing to do" and
adds the six things above has a floor near the sum of those six (≈ 0.45 + 0.3 + 0.2 + 0.15 + 0.15
+ 0.1 ≈ 1.4 µs plus the gap). The generic path stays the reference and the fallback.

### D4. Switches, modes and the record

`KYTY_BUFFERS_BY_ADDRESS` (0 / 1; `verify` and `exit` apply to the host side: the lean resolution
also runs `ObtainBuffer` and compares owner and offset, as `KYTY_DRAW_RUN_BINDINGS=verify` does
today — the shader cannot run both paths), `KYTY_BUFFERS_BY_ADDRESS_UNIFORM` (0 / 1: uniform-view
slots addressable too), `KYTY_DRAW_RUN_FAST` (0 / 1; verify through `KYTY_DRAW_RUN=verify|exit`),
`KYTY_BDA_SYNC_ALL_DRAWS` (diagnostic, D0). The first is a codegen option (read at shader compile,
in the cache key); the others are `Live::Switch`es with the family's parsing. The run record
stores the modes it was made under; a record made under other modes is not continued. All default
off; the measuring preset turns on what D6 accepts.

### D5. Diagnostics

`CommitStats` items for the `cont` and `start` classes: `addr-slots` (addressable slots per draw),
`addr-reused` (step 1 of D2), `addr-owner` (step 2), `addr-fallback` (step 3, by reason: no owner,
straddle, GPU-written), `table-bytes`, `desc-writes-saved`; the `fast` sub-phase for the lane's
sections; the `DrawRun` line adds the fast lane's draws, late fallbacks and verify checks and
mismatches for the new compares. `Kyty BDA passes` already reports the pass counts and bytes; D0
adds the ms per frame of the passes to it. `KYTY_BUFFER_LOOKUP_STATS` gains a part for the lean
lookup.

### D6. Measurement gate and acceptance

Same procedure as before (`PROGRESS.md`, "Como medir"): ASTRO's PLAYROOM, the same gameplay area,
`kyty_run_check.sh` with the measuring preset (`KYTY_LABEL_MODE=record`, int21 switches on), 3–5
minutes, before and after, intervals with ≥ 4 000 continuations per frame; the 2026-10-25 run
`_console-20261025-190222.txt` is the baseline (4.14 + 0.48 µs, 41.3 ms/frame, fps 18.1, GPU 36%).
Accept for the preset when:

- the `cont` class commit + gap is ≤ 2.5 µs (variant A) or ≤ 3.0 µs (variant B), with ≥ 90% of the
  continuations taking the fast lane and ≥ 95% of addressable slots resolved without `ObtainBuffer`;
- the resolver's ms/frame (Σ class × (commit + gap)) is ≤ 60% of the baseline at equal draw counts
  (variant A) or ≤ 75% (variant B);
- fps p50 in those intervals ≥ 25 (A) or ≥ 21 (B), and GPU busy rises by no more than the fps ratio
  implies plus 10 points (the by-address loads' own GPU cost);
- `KYTY_BUFFERS_BY_ADDRESS=exit`, `KYTY_DRAW_RUN=exit` and `KYTY_DRAW_RUN_BINDINGS=exit` each
  survive five minutes of the same gameplay — knowing that `KYTY_DRAW_RUN=exit` today stops on the
  pre-existing acquisition-verify race (archived tasks 5.2); that race is either fixed first (it is a
  follow-up of the archived change) or excluded from this gate by counting its part separately;
- God of War Sons of Sparta stays at 60 fps with every switch on, 0 verify mismatches;
- the full ctest suite passes with the new variants, including the by-address shader selector;
- the fps and resolver claims are made on runs **without** `KYTY_CP_COMMIT_STATS` (hang trace:
  fps, CP busy; `DrawRun 10s` draws per window), because task 0.3 measured the `steps`
  instrumentation at about a quarter of the frame time per draw (26.4 vs 18.2 fps at equal draw
  counts, 2026-10-25); the `steps` runs give the per-step attribution only, compared stats-on against
  stats-on. The fast lane marks fewer steps, so a stats-on comparison would overstate its gain.

## Risks / Trade-offs

- [Epoch pass too frequent or too costly under every draw] → D0 measures before anything is built;
  variant B is the fallback with a reduced target. The pass already has epoch and structure skips
  and the dirty-log incremental path.
- [GPU time rises: `PhysicalStorageBuffer` loads lose the constant cache of uniform views, or
  scalarization on AMD] → uniform-view slots stay descriptors by default
  (`KYTY_BUFFERS_BY_ADDRESS_UNIFORM=0`); the acceptance bounds GPU busy; the engine suite compares
  pixels, not timing, so the in-game `gpu_busy_us` is the gauge.
- [Bounds behaviour differs from the descriptor path (robustBufferAccess2 no longer clamps)] → the
  in-shader predicate is the same one used today and already decides the value (zero out of range);
  the compute test selector exercises edges: last dword, straddling dwords, unaligned bytes, size 0,
  size not a dword multiple, null slot.
- [A CPU write lands after the epoch pass and before the draw] → the same race exists today for
  computed V#s and, on the console, for the real GPU; `SyncEpoch` advances at every ordering point,
  which is where the guest may rely on visibility. `verify` compares against `ObtainBuffer`'s
  answer, which has the same window.
- [Owner lookup picks a buffer that will be joined or retired in this epoch] → the structure epoch
  check and the record's owner id; `KYTY_REBIND_AFTER_JOIN` (int21) already re-resolves draws whose
  buffers lost one to a join, and the fast lane calls the same hook.
- [The fast lane skips a decision that can change inside a run] → the run's eligibility keys
  (`DrawRunCandidate`) are the list of what may not change; the lane's verify compares cover the
  rest; anything a compare cannot cover stays in the lane (the certificate is the example).
- [Shader cache invalidation] → one rebuild on the first boot with the option, as with every key
  change; the first minutes of a run are excluded from measurement as today.
- [Push-constant range exhausted on programs with mesh dwords and clip planes and three stages with
  tables] → the stage falls back to the `ShaderData` descriptor for its block (one write per stage
  per draw, as today) while its slots stay addressable; the suite's layout test covers the three
  placements, and the `desc-writes-saved` item shows how often the fallback happens in-game.
- [Gain smaller than modelled] → the steps line attributes each µs; D1/D2 and D3 are independent
  switches, each kept or dropped on its own number.

## Migration Plan

1. D0: the diagnostic switch, the `ms per frame` field of the BDA-passes line, one gameplay run;
   record the decision (variant A or B) in this design and in `PROGRESS.md`.
2. D1 shader side behind `KYTY_BUFFERS_BY_ADDRESS` (off): classification, table layout, emitter,
   cache key, compute-test selector; the engine suite passes with the option on and off.
3. D1/D2 host side: the table build, lean resolution, `PrepareBda` gate, descriptor-list changes,
   verify mode, items; engine suite variants; an in-game `verify` run.
4. D3 behind `KYTY_DRAW_RUN_FAST` (off), with the new verify compares and variants; in-game
   `KYTY_DRAW_RUN=verify` run.
5. D6 measurement; the accepted switches into the measuring preset; numbers into `PROGRESS.md`,
   `docs/CHANGES-U59.md`, `docs/CHANGE-CATALOG.md`; GoW regression; full ctest; commit per stage.
6. Rollback: unset the switches (the shader option restores the descriptor compile; the cache keeps
   both variants keyed).

## Open Questions

- Whether the per-draw table should be one upload for all stages of a draw (one allocation, one
  pointer, two dwords of push constants) or one per stage (simpler layout, matches today's per-stage
  shader data); decided by the push-range budget per program found in step 2.
- Whether vertex ranges that straddle two owners are common enough in this title to deserve a
  join-ahead at run start; the `addr-fallback straddle` item of the first in-game run says.
- Whether the `programs`-phase certificate can be evaluated by the worker for a continuation (the
  plan's ranges are known there) and only re-checked against the coherence generation on the
  resolver — a later proposal if `validate` remains the largest step of the lane.
