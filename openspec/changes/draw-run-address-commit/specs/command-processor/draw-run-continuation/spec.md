# Spec Delta

## Purpose

A draw run is a sequence of consecutive guest draws that share their rendering structure and differ
only in per-draw data; this capability covers how a draw that continues a run is committed: what it
may reuse from the run, when that reuse is valid, the guarantee that its result equals the normal
commit, the verify modes that prove it, and the diagnostics that report what a continuation costs.
This delta adds per-draw data by address (delivered as address entries of the shader data, with
per-binding synchronization unchanged), the measured diagnostic of the epoch pass, and the
continuation fast lane.

## ADDED Requirements

### Requirement: Addressable slots keep per-binding synchronization
With `KYTY_BUFFERS_BY_ADDRESS` on, an addressable V# slot SHALL be resolved and synchronized as a
descriptor-bound slot is (the run record's exact-range reuse, else the normal lookup with its memo
and stream copy), and only its delivery SHALL differ: the resolution becomes the slot's address
entry (device address and size) in the stage's shader data instead of a descriptor element.

#### Scenario: Ring buffer advancing inside one cache buffer
- **WHEN** a continuation's V# range differs from the recorded one, even inside the same cache buffer
- **THEN** the range is resolved through the normal lookup, which uploads its CPU-dirty bytes, and the entry carries the resulting buffer's device address and offset

#### Scenario: Null slot
- **WHEN** an addressable slot's V# has a null base or a zero size
- **THEN** its entry holds address 0 and size 0 and every read of it returns zero

### Requirement: The epoch synchronization pass is a measured diagnostic only
`KYTY_BDA_SYNC_ALL_DRAWS=1` SHALL make every graphics draw run the buffer synchronization pass as a
draw with computed V#s does, and the periodic `Kyty BDA passes` line SHALL report the calls per
frame, the calls the epoch or submission gate skipped and the milliseconds spent inside the passes;
the switch SHALL change no rendering and default off.

#### Scenario: Diagnostic gate
- **WHEN** `KYTY_BDA_SYNC_ALL_DRAWS=1` is set
- **THEN** every draw triggers the pass, the line carries the three fields, and the engine suite renders the same pixels as without the switch

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
`KYTY_BUFFERS_BY_ADDRESS` on, the same rule applies to an addressable slot, whose reused resolution
becomes its address entry. Any other range, and any range whose recorded buffer is no longer valid, SHALL be
resolved as it is without this capability, and that resolution replaces the recorded one.

#### Scenario: Same range as the previous draw
- **WHEN** a continuation's V#, vertex range or index buffer is exactly the guest range an earlier draw of the same run resolved in this command buffer and tick
- **THEN** the continuation binds the same cache buffer at the same offset without a buffer-cache lookup, memory-tracker query or synchronization

#### Scenario: Reused resolution of an addressable slot
- **WHEN** `KYTY_BUFFERS_BY_ADDRESS` is on and a continuation's addressable V# is exactly the recorded range with a still-valid buffer
- **THEN** the recorded buffer and offset become the slot's address entry without a lookup

#### Scenario: Range inside a recorded cache buffer but not the recorded range
- **WHEN** a continuation's guest range lies in the cache buffer an earlier draw of the run resolved but differs from the recorded range
- **THEN** the range is resolved through the normal lookup (a lookup may legitimately pick another buffer covering it), and the run keeps that resolution for the next continuation

#### Scenario: Buffer structure changed
- **WHEN** a cache buffer was registered or unregistered since a resolution was recorded
- **THEN** the continuation resolves that range through the normal lookup and counts a fallback
