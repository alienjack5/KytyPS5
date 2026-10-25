# Proposal

## Why

ASTRO's PLAYROOM (PPSA01325) still runs at 13–45 fps in gameplay on the i7-11800H + RTX 3070
Laptop with the GPU 32–36% busy: the frame is the GPU thread that commits every prepared draw (the
resolver under `KYTY_CP_SEQ=1`, 99% of one core). After `KYTY_DRAW_RUN_BINDINGS` (archived change
`draw-run-continuation-fast-path`) and the merge of Jetsku's int21 with `KYTY_LABEL_MODE=record`
(2026-10-25, `_console-20261025-190222.txt`), the ten-second intervals with ≥ 4 000 continuations
per frame show 7 095 draws per frame, 92% of them continuations, at **4.14 µs of commit + 0.48 µs
of gap each** (4.62 µs), 322 rendering-instance starts at 18.9 µs, 41.3 ms of resolver per frame,
fps p50 18.1. The `Steps` line splits the 4.62 µs of a continuation into:

| group | µs | what it is |
|---|---|---|
| V# buffers | 1.04 | `RebindBuffers`: one `NativeStorageBuffer` → `ObtainBuffer` per slot (memo hit 0.46/draw, stream copy of CPU-dirty data 0.56/draw), shader-data upload |
| vertex / index | 0.66 | `AcquireVertexBuffers`, `PrepareIndexBuffer` (one `ObtainBuffer` per range), the two binds |
| descriptors + push | 0.52 | `CommitBindings`: 3.9–4.6 descriptor writes per draw built and pushed, push constants |
| control and bookkeeping | 1.38 | register binding, state reset, program refresh and certificate (`validate` 0.27, `apply` 0.17), run candidate, target ops, draw checks, pipeline selection, acquisition and barrier classification, late check |
| emit | 0.24 | dynamic state, begin, pipeline bind, the draw command (0.12) |
| post | 0.34 | run record, reset bindings, commit tail |
| gap | 0.48 | op exit, retire, head, loop |

Two facts bound what the previous change could reach. First, a continuation still *moves and
describes* its per-draw data the generic way: every V# is resolved against the buffer cache and
written as a descriptor, although a run keeps the same program, targets, pipeline and images, so
the only thing that changes per draw is *where* the data is. Second, 2.2 µs (control, post, gap) is
the generic commit flow itself: fifty small steps that each cost 0.02–0.27 µs, which the previous
change's trimming (design D3) could not remove one by one. Int21's new paths (`KYTY_CLEAN_UNTOUCHED`,
`KYTY_STREAM_COPY_DEFER`) saved 0.5 µs together and are already in the number above.

The lever that scales with the draw count is to make a continuation what it is logically: *the
same draw with new addresses*. The emulator already has the machinery: buffer device addresses
(`BufferCache::BufferDeviceAddress`, the BDA page table, `PhysicalStorageBuffer` loads in the SPIR-V
emitter for computed V#s under `KYTY_SRT_VARIANT_READS`), an epoch-gated synchronization pass that
makes CPU-dirty guest memory current for GPU reads (`PrepareBda` / `SynchronizeBdaBuffers`, with the
dirty-log and hot-page incremental passes), and the run record with its exact invalidation
conditions. This change puts them together for the common case.

## What Changes

- **Read-only V# buffers addressed per draw, not described** (`KYTY_BUFFERS_BY_ADDRESS`, default
  off; a shader codegen option, part of the pipeline/shader cache key): a program's read-only,
  unformatted buffer slots read guest memory through a per-draw *binding table* (device address and
  size per slot, plus the shader data the draw already uploads) whose device address is the only
  per-draw push constant. No descriptor write per such slot, no `ShaderData`/`FlattenedSrt`
  descriptor when every slot of the stage is addressable; the stage's descriptor set then holds only
  images, samplers and the remaining descriptor-bound buffers, which a run keeps constant. Formatted
  (typed) buffers, written buffers and, behind its own option, int21's uniform-view buffers stay
  descriptors. The shader reads with the same bounds check as today (an out-of-range element reads
  zero) through a pointer loaded once per slot per invocation; no page-table lookup per access.
- **Lean resolution of addressable ranges**: a V#, vertex or index range is resolved to its cache
  buffer by the host page-table owner lookup plus a structure-epoch check (`FindBuffer` without the
  binding memo, the memory-tracker signature or per-binding synchronization), and to a device
  address or bind offset from the owner's base. A range without a covering owner takes today's
  `ObtainBuffer` (which may create or join buffers). A range not served from the run record keeps
  today's per-binding synchronization (binding memo, stream copy of CPU-dirty bytes): the
  alternative — current data through the epoch synchronization pass (`PrepareBda`) under every
  draw — was measured first (design D0, 2026-10-25) and rejected: 80 passes and 3.5 ms per frame,
  four times the upload bytes. GPU-written buffers keep the read-after-write barrier decision of
  `ObtainBuffer`.
- **A continuation fast lane** (`KYTY_DRAW_RUN_FAST`, default off): a draw that continues a run is
  committed by a dedicated path that does only what can differ per draw: late check (command
  identity, image state, activity epoch), certificate, resolution of addressable ranges, the table
  upload and push, vertex/index binds when their ranges changed, the draw command, the run record.
  Everything the record pins (register binding and state reset, program refresh beyond the
  certificate, target ops, draw checks, pipeline selection, acquisition and barrier classification,
  dynamic state, rendering begin, descriptor pushes) is not re-derived. `KYTY_DRAW_RUN=verify`
  gains compares for the items the lane skips that it did not compare before.
- **Measurement gate first** (design D0, done 2026-10-25): the diagnostic switch
  `KYTY_BDA_SYNC_ALL_DRAWS` made every draw trigger the epoch synchronization pass, and the `Kyty
  BDA passes` line (now with calls, gate skips and ms per frame) showed 80 passes and 3.5 ms per
  frame: more than the per-binding synchronization it would replace. The change therefore keeps
  per-binding synchronization (variant B) and takes the descriptor, identity and fast-lane gains;
  the acceptance target is 3.0 µs per continuation.
- **Diagnostics**: `StartPhases`/`Steps` report addressable slots per draw, lean lookups that fell
  back, table bytes, pushes and descriptor writes saved; the `DrawRun` line reports the fast lane's
  draws and its verify checks and mismatches.

Not in this change: in-shader vertex fetch (the vertex and index binds stay fixed-function, so
their encode cost remains), the rendering-instance boundary cost (322 × 18.9 µs = 6.1 ms per frame,
next proposal), compute dispatches, cross-frame reuse, the spin policy of the waiting threads, every
GPU-side item.

## Capabilities

### New Capabilities

- `shader-recompiler/buffers-by-address`: how a compiled program reads a read-only buffer slot
  through a per-draw device address instead of a descriptor: which slots qualify, the table layout,
  the bounds behaviour, the equality with descriptor reads, the cache-key change and the switch.

### Modified Capabilities

- `command-processor/draw-run-continuation`: a continuation's per-draw data is resolved by address
  and pushed as one table pointer; the current-data guarantee for addressable ranges comes from the
  epoch synchronization pass; the fast lane replaces the "lean bookkeeping" requirement; new
  diagnostics and fallbacks.

## Impact

- Shader recompiler: `src/graphics/shader/recompiler/CodegenOptions.h` (option), `ir/ShaderIR.h`
  (`DescriptorBindingKind`, `BindingLayout`, `PushData`: the table pointer), `ir/passes/BindingLayout.cpp`
  and `ResourceMaterialization.cpp` / `ResourceTracking.cpp` (slot classification: addressable or
  descriptor), `backend/spirv/spirvEmitterMemory.cpp` (`PrepareMemoryResourceAccess`,
  `LoadWordInBounds`: the address path next to the descriptor path, reusing the `LoadBda` helpers
  without `get_bda_pointer`), the shader/pipeline cache key (`pipeline/shaders.cpp`,
  `pipelineCache.cpp`), `pipeline/pipelineLayoutCache.cpp` (push range).
- Host GPU: `src/graphics/host_gpu/renderer/pipeline/descriptors.{h,cpp}` (`FindBuffers`,
  `RebindBuffers` ~2100–2200, `NativeStorageBuffer` ~240–300, `CommitBindings` 2649–3003,
  `PrepareGraphicsBindings` ~2410: the `PrepareBda` gate; `PreparedBindings`: the table),
  `renderDraw.cpp` (`ExecutePreparedDraw` 3969, `DrawIndex` 4912, `AcquireVertexBuffersInto`,
  `PrepareIndexBuffer`, `DrawRunCandidate` 2842, `DrawRunRecordDraw` 3255: the fast lane),
  `cache/bufferCache.{h,cpp}` (the lean owner lookup beside `ObtainBuffer`; `SynchronizeBdaBuffers`
  statistics), `renderContext.cpp` (`PrepareBda`), `drawPrep/drawRun.{h,cpp}` (switches, record
  fields, verify items), `drawPrep/commitStats.{h,cpp}` (items), `context.cpp` (push constants).
- Tests: `tests/ShaderRecompilerComputeTests.cpp` (a selector for by-address programs: same results
  as descriptor reads, out-of-range zero, straddled and unaligned accesses), `tests/DrawPrepTests.cpp`
  and `tests/BindingPathTests.cpp` (fast lane and lean lookup verify), ctest variants in
  `CMakeLists.txt` (`draw_run_fast*`, `buffers_by_address*`, engine suite in the three modes). GPU
  tests run `-j1` on this machine; `ninja kyty_tests` before ctest.
- Configuration and documentation: the switches in the measuring `u59-preset.json`; the shader
  cache rebuilds once when the option changes (cache key); `docs/CHANGES-U59.md`,
  `docs/CHANGE-CATALOG.md`, `PROGRESS.md`.
- Platform: Vulkan `bufferDeviceAddress` and `shaderInt64`, both already required (the capabilities
  line reports `BDA 1`); `PhysicalStorageBuffer` loads are what `KYTY_SRT_VARIANT_READS` emits
  today. Host-agnostic CPU work otherwise.
