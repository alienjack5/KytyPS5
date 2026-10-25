# Tasks

## 0. Measurement gate (design D0)

- [ ] 0.1 Add the diagnostic `Live::Switch` `KYTY_BDA_SYNC_ALL_DRAWS` (drawRun.cpp family parsing) and make `RenderExecutor::PrepareGraphicsBindings` call `m_context.PrepareBda()` when it is on, as it does for `uses_dma`; add the milliseconds per frame of the passes to the `Kyty BDA passes` line (`BufferCache::ReportBdaSubmissionGate` / the passes line in bufferCache.cpp); verify in the engine suite that the switch changes no pixels and that the line carries the new field
- [ ] 0.2 Run ASTRO's PLAYROOM, same area, 3–5 min, measuring preset + `KYTY_BDA_SYNC_ALL_DRAWS=1`; record passes per frame by kind, MB and ms, and the `bda` step of the `cont`/`instance` classes against `_console-20261025-190222.txt`; decide variant A (≤ 1 ms/frame) or B and write the decision into design.md D0 and `PROGRESS.md`
- [ ] 0.3 Run the baseline once more without `KYTY_CP_COMMIT_STATS` (hang trace only: fps, CP busy) so the instrumentation's own cost is known before the acceptance (D6)

## 1. Shader side: buffers by address (D1, `shader-recompiler/buffers-by-address`)

- [ ] 1.1 Add `CodegenOptions::buffers_by_address` and `buffers_by_address_uniform` (`KYTY_BUFFERS_BY_ADDRESS`, `KYTY_BUFFERS_BY_ADDRESS_UNIFORM`, default off), include both in the shader and pipeline cache keys (`pipeline/shaders.cpp`, `pipelineCache.cpp`, the program-cache salt); verify with `spirv_cache_salt_tests`/`shader_program_cache` cases that flipping either option changes the key
- [ ] 1.2 Classify addressable slots in the resource passes (`ResourceTracking.cpp`/`ResourceMaterialization.cpp`: read-only, unformatted, not mip-statistics or indirect-parameter, uniform-view only with the uniform option) and expose the list on `CompiledShaderInfo`; verify with compute-test cases that a written, a formatted and a uniform-view slot stay descriptor-bound and a raw read-only slot is addressable
- [ ] 1.3 Extend `BindingLayout` (ShaderIR.h, BindingLayout.cpp): the binding table layout per stage (entries, shader data, flattened SRT), the push-constant dwords of the table pointer after mesh/clip dwords (`PushData::TablePointerDword`, `StartFor`), no `Buffers` element for addressable slots and no `ShaderData`/`FlattenedSrt` binding for a stage with a table; a program whose pointer does not fit compiles without addressable slots and sets a flag; verify with layout unit cases (fit, no fit, mixed stages)
- [ ] 1.4 Emit the address path in `spirvEmitterMemory.cpp`: table pointer from push constants once per invocation, slot entry loaded once, dword loads through `PhysicalStorageBuffer` at `address + offset` with the descriptor path's in-bounds predicate (zero out of range), shared dword-load helper with `LoadBdaDword`, shader data and flattened SRT read from the table; verify that programs without addressable slots emit identical SPIR-V with the option on (snapshot cases)
- [ ] 1.5 Add the compute-test selector `--buffers-by-address-only`: each case runs the descriptor and the by-address program on the same inputs and compares outputs, covering the last dword, a dword straddle, unaligned 8/16-bit loads, size 0, a size that is not a dword multiple, a null slot, structured loads with stride and swizzle, `S_BUFFER_LOAD`; register the ctest entries; verify the selector passes on the GPU (`-j1`)

## 2. Host side: table, lean resolution, descriptors (D1/D2)

- [ ] 2.1 `PreparedBindings`: the table buffer (`std::vector<uint32_t>`), per-slot owner records; `RebindBuffers`: for an addressable slot run the lean resolution (record owner → page-table owner → `ObtainBuffer`), write the entry, count `addr-reused`/`addr-owner`/`addr-fallback <reason>`; descriptor-bound slots unchanged; verify with `binding_path_tests` cases for the three steps and the GPU-written owner fallback
- [ ] 2.2 `BufferCache`: the owner lookup entry point (`FindOwnerCovering(address, size)` → buffer, offset, structure epoch; no memo, signature or synchronization) and `BufferLookupStats` part; verify with `memory_tracker`/`binding_path` cases that a straddled range and an unregistered range return no owner
- [ ] 2.3 `PrepareGraphicsBindings`: call `PrepareBda` before a draw with addressable slots (variant A) — or, variant B, keep `ObtainReadBinding` for a slot not served from the record; verify in the engine suite with `KYTY_BUFFERS_BY_ADDRESS=1` that pixels match the descriptor build
- [ ] 2.4 `CommitBindings`: build the descriptor write list from descriptor-bound slots only, upload the table (dedup against the site's last upload as `UploadShaderData`), write the table pointer to push constants, skip the descriptor push when a continuation has no per-draw write; `desc-writes-saved` and `table-bytes` items; verify with the engine suite and `draw_run_*` variants under the option
- [ ] 2.5 Verify mode for the host side (`KYTY_BUFFERS_BY_ADDRESS=verify|exit`): the lean resolution also runs `ObtainBuffer` and compares owner and offset through `DrawRun::ReportMismatch` (part "address"); verify with an injected mismatch in a unit case that the count and the exit happen
- [ ] 2.6 Vertex and index ranges through the lean resolution (`AcquireVertexBuffersInto`, `PrepareIndexBuffer`), binds encoded only when owner or offset changed; verify with an indexed-draw engine case that repeats a vertex buffer across a run (the archived change's pending unit test) and with the verify mode

## 3. The continuation fast lane (D3)

- [ ] 3.1 Add `Live::Switch` `KYTY_DRAW_RUN_FAST` and the record's mode field; route accepted continuations from `DrawIndex`/`DrawAuto` to `RenderExecutor::ExecuteRunContinuation`: late check, `DrawPrep::Validate` + plan activation, per-draw data (2.1/2.6), binds when changed, draw command with post-draw hooks, run record; fallback to `ExecutePreparedDraw` on a failed late check (`latefallback`); verify with `draw_prep_tests` that a run of N draws commits N−1 through the lane and that a mid-run image change falls back
- [ ] 3.2 Extend `KYTY_DRAW_RUN=verify` for lane draws: recompute on scratch state and compare plan activation outcome, primitive-restart/index-size decision, barrier classification and the descriptor write list; verify with an injected difference per item in a unit case
- [ ] 3.3 ctest variants `draw_run_fast`, `draw_run_fast_verify`, `draw_run_fast_address`, `draw_run_fast_address_verify` (engine suite) and `draw_prep_engine_parallel_fast`; verify they pass on the GPU `-j1`
- [ ] 3.4 In-game: ASTRO's PLAYROOM 5 min with `KYTY_DRAW_RUN=verify` and the lane on, then `KYTY_DRAW_RUN=exit` once the acquisition-verify race (archived tasks follow-up) is fixed or its part is separated in the counts; record the mismatch counts

## 4. Diagnostics (D5)

- [ ] 4.1 `CommitStats` items `addr-slots`, `addr-reused`, `addr-owner`, `addr-fallback-noowner`, `addr-fallback-straddle`, `addr-fallback-gpuwritten`, `table-bytes`, `desc-writes-saved`; `fast` sub-phase; the `DrawRun` line's lane draws, late fallbacks, verify checks and mismatches; verify the lines print `0.00` with the switches off

## 5. Measurement and acceptance (D6)

- [ ] 5.1 ASTRO's PLAYROOM, same area, 3–5 min, measuring preset + the switches; compare with `_console-20261025-190222.txt`: `cont` commit + gap, resolver ms/frame, fps p50, GPU busy, lane share, addressable share; the acceptance numbers of D6 by variant
- [ ] 5.2 The same run without `KYTY_CP_COMMIT_STATS` (fps and CP busy from the hang trace)
- [ ] 5.3 `KYTY_BUFFERS_BY_ADDRESS=exit`, `KYTY_DRAW_RUN_BINDINGS=exit` and (see 3.4) `KYTY_DRAW_RUN=exit` runs of 5 min each
- [ ] 5.4 God of War Sons of Sparta regression with every switch on (`kyty_run_check.sh ~/Games/ps5/PPSA28997`, verify modes): 60 fps, 0 mismatches
- [ ] 5.5 Full ctest after `ninja kyty_tests` (CPU parallel, GPU `-j1`), every new variant included
- [ ] 5.6 Decision: which switches enter the measuring preset; the numbers and the decision in `PROGRESS.md`, `docs/CHANGES-U59.md` ("What was actually measured" row) and the `drawRun.h` comments

## Workflow follow-up

- Commit per stage (0, 1, 2, 3, 5) with the measured numbers; `docs/CHANGE-CATALOG.md` entry per commit; archive the change.
- Report to Jetsku, with this change's measurements: the `KYTY_LABEL_MODE=split` cost on Linux (A/B of 2026-10-25) and the acquisition-verify race of `KYTY_DRAW_RUN=exit`.
- Next proposals the design excludes: the rendering-instance boundary (322 × 18.9 µs per frame), in-shader vertex fetch (would remove the remaining vertex/index resolution and binds), certificate evaluation on the workers.
