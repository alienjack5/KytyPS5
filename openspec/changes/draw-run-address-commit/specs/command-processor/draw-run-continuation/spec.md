# Spec Delta

## Purpose

A draw run is a sequence of consecutive guest draws that share their rendering structure and differ
only in per-draw data; this capability covers how a draw that continues a run is committed: what it
may reuse from the run, when that reuse is valid, the guarantee that its result equals the normal
commit, the verify modes that prove it, and the diagnostics that report what a continuation costs.
This delta adds per-draw data by address, the current-data guarantee behind it, and the
continuation fast lane.

## ADDED Requirements

### Requirement: Addressable ranges are resolved by owner, not by lookup
With `KYTY_BUFFERS_BY_ADDRESS` on, the emulator SHALL resolve an addressable V#, vertex or index
range to the cache buffer that owns its first page when that buffer covers the whole range and the
buffer structure is unchanged since the owner was recorded, deriving the device address or bind
offset from the owner's base; any other range SHALL take the normal lookup and be counted by reason.

#### Scenario: Ring buffer advancing inside one owner
- **WHEN** a continuation's V# range differs from the recorded one but lies inside the recorded owner buffer, whose structure epoch is unchanged
- **THEN** the range resolves to that owner at the new offset without a cache lookup, memory-tracker query or synchronization

#### Scenario: Range across two owners
- **WHEN** a range's pages belong to two cache buffers
- **THEN** the range is resolved through the normal lookup and a straddle fallback is counted

#### Scenario: GPU-written owner
- **WHEN** the owner buffer has GPU writes not yet made visible to reads
- **THEN** the range takes the normal lookup, which emits the read-after-write barrier as today

### Requirement: Current data for addressable ranges comes from the epoch synchronization pass
Before recording a draw with addressable slots, the emulator SHALL have run the buffer
synchronization pass for the current synchronization epoch (the pass computed V# reads rely on),
so that every CPU-dirty byte of a registered buffer in the GPU-mapped ranges was uploaded before
the draws of that epoch; no per-binding upload SHALL be needed for an addressable range.

#### Scenario: Epoch unchanged
- **WHEN** two draws with addressable slots are committed inside one synchronization epoch with no buffer registered or unregistered between them
- **THEN** the pass runs at most once for both and the second draw reads the same uploaded bytes

#### Scenario: Diagnostic gate
- **WHEN** `KYTY_BDA_SYNC_ALL_DRAWS=1` is set on a build without the by-address option on
- **THEN** every draw triggers the pass as a draw with addressable slots would, and the periodic `Kyty BDA passes` line reports the passes per frame by kind, the bytes uploaded and the milliseconds spent

### Requirement: A continuation's per-draw data is one table and one pointer
With `KYTY_BUFFERS_BY_ADDRESS` on, a continuation SHALL deliver its per-draw buffer data as the
stage's binding table (one upload) and its device address in push constants, and SHALL write
descriptors only for descriptor-bound slots whose resolution changed; a stage whose buffer slots
are all addressable SHALL push no descriptors on a continuation.

#### Scenario: All slots addressable
- **WHEN** every buffer slot of a continuation's stages is addressable and its images and samplers are the run's
- **THEN** the continuation records one table upload per stage, one push-constant write and the draw, and zero descriptor pushes

### Requirement: The fast lane commits only what a continuation can change
With `KYTY_DRAW_RUN_FAST=1`, a draw accepted as a run continuation SHALL be committed by a path
that performs only the late check, the certificate check and plan activation, the per-draw data,
the vertex and index binds when their ranges changed, the draw command with its post-draw hooks and
the run record, and SHALL NOT re-derive any item the run record pins.

#### Scenario: Items the lane does not re-derive
- **WHEN** a draw is committed by the fast lane
- **THEN** register binding, state reset, target resolution, draw checks, primitive-restart and index-size decisions, texture and sampler preparation, pipeline selection, acquisition, barrier classification, dynamic state, rendering begin and pipeline bind are the record's and are not recomputed

#### Scenario: Late check fails
- **WHEN** the command identity, the image state or the activity epoch differs from the record at the lane's late check
- **THEN** the draw is committed by the normal path, the run ends, and a late fallback is counted

#### Scenario: Verify compares the skipped decisions
- **WHEN** the emulator runs with `KYTY_DRAW_RUN=verify` and the fast lane on
- **THEN** for every lane draw the normal path's pipeline, acquisition, transitions, dynamic state, descriptor write list, plan activation outcome, primitive-restart and index-size decision and barrier classification are recomputed and compared, differences are counted and logged, and `exit` stops at the first

### Requirement: Address and lane diagnostics are reported
With `KYTY_CP_COMMIT_STATS` enabled, the periodic lines SHALL report per continued draw the
addressable slots, the resolutions taken from the record, from the owner lookup and by fallback
(by reason), the table bytes and the descriptor writes saved, and the `DrawRun` line SHALL report
the fast lane's draws, late fallbacks, verify checks and mismatches.

#### Scenario: Measurement run
- **WHEN** the title runs with `KYTY_CP_COMMIT_STATS=steps` and the switches on
- **THEN** every ten-second line carries those counts next to the phase times, and they are zero with the switches off

## MODIFIED Requirements

### Requirement: Continuation bookkeeping is limited to what a continuation can change
A continuation SHALL NOT redo per-draw bookkeeping whose inputs are, by the definition of a
continuation, those of the previous draw (render-target entries, texture and sampler resolutions,
pipeline selection, attachment acquisition, dynamic state, the descriptor-set shape); with
`KYTY_DRAW_RUN_FAST=1` this is achieved by the fast lane, which starts from the draw command and
adds only the per-draw items, and the `KYTY_DRAW_RUN=verify` comparison SHALL continue to cover
every item a continuation keeps, including the items the lane newly skips.

#### Scenario: Verify after the lean path
- **WHEN** the test suite's `draw_run_*_verify` and `draw_run_fast*_verify` variants and a gameplay session of the measured title run with `KYTY_DRAW_RUN=exit`
- **THEN** they finish without a reported difference attributable to a continuation's kept or skipped item

### Requirement: Reuse covers only the recorded range
A reused resolution SHALL serve a continuation's binding when the binding's guest range is exactly
the range the resolution was made for and the resolution's buffer is still valid (a cache buffer:
the buffer structure unchanged since; a stream copy: the same scheduler tick); with
`KYTY_BUFFERS_BY_ADDRESS` on, an addressable range SHALL additionally be served from the recorded
owner buffer when it lies inside that owner and the structure is unchanged, since the owner is
unique per page. Any other range, and any range whose recorded buffer is no longer valid, SHALL be
resolved as it is without this capability, and that resolution replaces the recorded one.

#### Scenario: Same range as the previous draw
- **WHEN** a continuation's V#, vertex range or index buffer is exactly the guest range an earlier draw of the same run resolved in this command buffer and tick
- **THEN** the continuation binds the same cache buffer at the same offset without a buffer-cache lookup, memory-tracker query or synchronization

#### Scenario: Range inside the recorded owner, by address
- **WHEN** `KYTY_BUFFERS_BY_ADDRESS` is on and a continuation's addressable range lies inside the recorded owner buffer but differs from the recorded range
- **THEN** the range resolves to the owner at the new offset without a lookup, and the record keeps the owner

#### Scenario: Range inside a recorded cache buffer but not the recorded range
- **WHEN** `KYTY_BUFFERS_BY_ADDRESS` is off and a continuation's guest range lies in the cache buffer an earlier draw of the run resolved but differs from the recorded range
- **THEN** the range is resolved through the normal lookup (a lookup may legitimately pick another buffer covering it), and the run keeps that resolution for the next continuation

#### Scenario: Buffer structure changed
- **WHEN** a cache buffer was registered or unregistered since a resolution was recorded
- **THEN** the continuation resolves that range through the normal lookup and counts a fallback
