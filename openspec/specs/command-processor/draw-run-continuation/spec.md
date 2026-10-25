# command-processor/draw-run-continuation Specification

## Purpose
A draw run is a sequence of consecutive guest draws that share their rendering structure and differ
only in per-draw data; this capability covers how a draw that continues a run is committed: what it
may reuse from the run, when that reuse is valid, the guarantee that its result equals the normal
commit, the verify modes that prove it, and the diagnostics that report what a continuation costs.

## Requirements

### Requirement: A continuation renders exactly as the normal commit
A draw committed as a run continuation SHALL produce the same recorded GPU commands and the same
bound resources (buffers and offsets, descriptors, vertex and index bindings, push constants) as
the same draw committed through the normal path from the same guest state.

#### Scenario: Reused buffer resolution
- **WHEN** a continuation reuses a buffer resolution of its run for a V#, vertex range or index buffer
- **THEN** the cache buffer and offset it binds are the ones the normal path would have obtained for that guest range

#### Scenario: Verify mode agreement
- **WHEN** the emulator runs with `KYTY_DRAW_RUN_BINDINGS=verify`
- **THEN** every continuation also takes the normal path for the reused item and counts any difference, and the counts stay at zero over a run of the test suite and of the measured title

#### Scenario: Exit mode stops at the first difference
- **WHEN** the emulator runs with `KYTY_DRAW_RUN_BINDINGS=exit` and a reused resolution differs from the normal path's
- **THEN** the difference is logged and the emulator stops, whatever `KYTY_DRAW_RUN`'s own mode is

### Requirement: Buffer resolutions are reused only inside a valid run
With `KYTY_DRAW_RUN_BINDINGS` set to `1` or a verify value, the emulator SHALL reuse a run's buffer
resolution for a continuation only when the draw continues the run that produced it, the command
buffer, scheduler tick and rendering instance are the run's, and no command-processor activity other
than committed draws happened since the resolution.

#### Scenario: Activity between the draws
- **WHEN** any command-processor operation other than a committed draw ran since the resolution (a CPU write by the command processor, a barrier, a flush, a wait, a dispatch, a flip)
- **THEN** no resolution of the run is reused and the continuation resolves every range as without this capability

#### Scenario: Switch off
- **WHEN** `KYTY_DRAW_RUN_BINDINGS` is unset, empty, `0` or `off`
- **THEN** continuations resolve buffers exactly as before this capability

### Requirement: Reuse covers only the recorded range
A reused resolution SHALL serve a continuation's binding only when the binding's guest range is
exactly the range the resolution was made for and the resolution's buffer is still valid (a cache
buffer: the buffer structure unchanged since; a stream copy: the same scheduler tick). Any other
range, and any range whose recorded buffer is no longer valid, SHALL be resolved as it is without
this capability, and that resolution replaces the recorded one.

#### Scenario: Same range as the previous draw
- **WHEN** a continuation's V#, vertex range or index buffer is exactly the guest range an earlier draw of the same run resolved in this command buffer and tick
- **THEN** the continuation binds the same cache buffer at the same offset without a buffer-cache lookup, memory-tracker query or synchronization

#### Scenario: Range inside a recorded cache buffer but not the recorded range
- **WHEN** a continuation's guest range lies in the cache buffer an earlier draw of the run resolved but differs from the recorded range
- **THEN** the range is resolved through the normal lookup (a lookup may legitimately pick another buffer covering it), and the run keeps that resolution for the next continuation

#### Scenario: Buffer structure changed
- **WHEN** a cache buffer was registered or unregistered since a resolution was recorded
- **THEN** the continuation resolves that range through the normal lookup and counts a fallback

### Requirement: A continuation's certificate is checked as any draw's
A continuation's certificate (coherence-log interval and clean verdicts of its read ranges) SHALL be
checked exactly as a draw that starts a run; no verdict is carried over between draws.

#### Scenario: A coherence-log entry lands mid-run
- **WHEN** the command processor appends a coherence-log entry (a CPU write, an unmap, a GPU-written range) between two draws of a run
- **THEN** the next continuation's certificate check sees it exactly as before this capability

### Requirement: Continuation bookkeeping is limited to what a continuation can change
A continuation SHALL NOT redo per-draw bookkeeping whose inputs are, by the definition of a
continuation, those of the previous draw (render-target entries, texture and sampler resolutions,
pipeline selection, attachment acquisition, dynamic state, the descriptor-set shape), and the
`KYTY_DRAW_RUN=verify` comparison SHALL continue to cover every item a continuation keeps.

#### Scenario: Verify after the lean path
- **WHEN** the test suite's `draw_run_*_verify` variants and a gameplay session of the measured title run with `KYTY_DRAW_RUN=exit`
- **THEN** they finish without a reported difference

### Requirement: Reuse is reported
With `KYTY_CP_COMMIT_STATS` enabled, the periodic `StartPhases` and `Steps` lines SHALL report, for
continuations, how many buffer resolutions were reused and how many reuses fell back, so that the
effect of the switch is measurable in-game; the periodic `DrawRun` line SHALL carry the same totals
with the verify checks and mismatches.

#### Scenario: Measurement run
- **WHEN** the title runs with `KYTY_CP_COMMIT_STATS=steps` and the switch on
- **THEN** every ten-second line carries the reuse counts per continued draw next to the phase times, and the counts are zero with the switch off

### Requirement: A failure to reuse is never a rendering failure
If a reuse precondition cannot be evaluated (a run record missing a field, a resolution evicted from
the buffer cache, a verify copy unavailable), the emulator SHALL fall back to the normal path for that
item, count the fallback, and keep rendering.

#### Scenario: Evicted cache buffer
- **WHEN** the cache buffer a run resolved was replaced or freed before a continuation reuses it
- **THEN** the continuation resolves the range through the normal lookup and the run record is refreshed from that result
