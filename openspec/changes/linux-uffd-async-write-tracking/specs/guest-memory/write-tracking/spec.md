# Spec Delta

## Purpose

Guest write tracking tells the GPU caches which guest memory the CPU changed, so that GPU work never
uses stale copies of it; this capability covers its tracking modes, their selection and fallback,
and the diagnostics that report what tracking costs.

## ADDED Requirements

### Requirement: Asynchronous write tracking is opt-in
The emulator SHALL use asynchronous write tracking only when `KYTY_UFFD_WP_ASYNC=1` is set on Linux.
Without that setting, or with any other value, write tracking SHALL behave exactly as it does
without this capability.

#### Scenario: Setting absent
- **WHEN** the emulator starts on Linux without `KYTY_UFFD_WP_ASYNC`
- **THEN** write tracking uses the mode it used before this capability, and the startup log names that mode

#### Scenario: Setting enabled on a supported kernel
- **WHEN** the emulator starts on a Linux kernel that supports asynchronous write protection with `KYTY_UFFD_WP_ASYNC=1`
- **THEN** guest write tracking runs in asynchronous mode and the startup log says so

#### Scenario: Other platforms
- **WHEN** the emulator runs on Windows or macOS with `KYTY_UFFD_WP_ASYNC=1`
- **THEN** the setting has no effect

### Requirement: Unsupported kernels fall back
When asynchronous mode is requested but the kernel lacks asynchronous userfaultfd write protection or
the page scan interface, or registering a guest mapping fails, the emulator SHALL log the reason once
and track writes to that memory with the mode it would use without the setting, and keep running.

#### Scenario: Kernel without asynchronous write protection
- **WHEN** `KYTY_UFFD_WP_ASYNC=1` is set on a kernel older than Linux 6.7
- **THEN** the startup log names the missing feature and the fallback mode, and the game runs with that mode

#### Scenario: One mapping cannot be registered
- **WHEN** registering one guest mapping for asynchronous tracking fails while others succeed
- **THEN** that mapping is tracked with the fallback mode, the failure is logged once, and the other mappings stay asynchronous

### Requirement: GPU work observes earlier CPU writes
In every tracking mode, a CPU write to tracked guest memory that completed before the emulator
prepares GPU work reading that memory SHALL be visible to that GPU work.

#### Scenario: Buffer written before a draw
- **WHEN** a guest thread writes a buffer and a later draw reads it
- **THEN** the draw reads the written data, not the GPU's earlier copy

#### Scenario: Several writer threads
- **WHEN** several guest threads write different pages of tracked memory before a dispatch that reads all of them
- **THEN** the dispatch reads every written page's new data

#### Scenario: Writes made on the guest's behalf
- **WHEN** the emulator writes guest memory for the guest (a file read into a guest buffer) before GPU work reads it
- **THEN** that GPU work reads the written data

### Requirement: Writes to write-protected pages take no signal
In asynchronous mode, a CPU write to a page that is only write-protected for tracking SHALL complete
without delivering a signal to the emulator; the page SHALL be reported as written by the next
collection of written pages.

#### Scenario: First write to a protected page
- **WHEN** a guest thread writes a write-protected tracked page in asynchronous mode
- **THEN** no fault handler runs for that write, and the next collection reports the page as written

### Requirement: CPU-dirty state is page-exact
In asynchronous mode, write tracking SHALL mark CPU-dirty exactly the pages written since they were
last protected, without extending a write to neighbouring pages.

#### Scenario: One byte written in a large range
- **WHEN** a guest thread writes one byte in a 1 MiB tracked range in asynchronous mode
- **THEN** the next collection marks one page of that range CPU-dirty

### Requirement: Collection protects again without losing writes
A collection of written pages SHALL protect each page it reports in the same operation, and a write
that completes after a page's collection SHALL be reported by a later collection.

#### Scenario: Write after collection
- **WHEN** a page is collected and a guest thread writes it again
- **THEN** a later collection reports the page again

#### Scenario: Write concurrent with collection
- **WHEN** a guest thread writes a page while a collection over that page runs
- **THEN** the write is reported by that collection or by the next one, never by neither

### Requirement: No-access protection keeps its synchronous behavior
Guest pages protected against all CPU access, because the GPU holds their newest data, SHALL keep
the current behavior in every mode: a CPU access first makes the GPU data available in guest memory.

#### Scenario: CPU reads a GPU-modified page in asynchronous mode
- **WHEN** a guest thread reads a page the GPU modified while asynchronous mode is on
- **THEN** the read returns the GPU's data, as it does without asynchronous mode

### Requirement: Tracking costs are reported
The fault-cost log SHALL report, for asynchronous mode, the collections made, the pages they reported
and the time they took per period, next to the counters it already reports for the other modes.

#### Scenario: Periodic cost line
- **WHEN** asynchronous mode is on and a `KYTY_FAULT_COST_LOG` period ends
- **THEN** the log line includes the period's collections, reported pages and collection time

### Requirement: The asynchronous cost can be estimated with the current mode
With `KYTY_UFFD_WP_ASYNC_ESTIMATE=1`, the emulator SHALL report each fault-cost period an estimate of
the collections asynchronous mode would make and their time, next to the measured cost of the
current tracking mode, without changing how writes are tracked.

#### Scenario: Estimate during a game
- **WHEN** a game runs with `KYTY_UFFD_WP_ASYNC_ESTIMATE=1` and asynchronous mode off
- **THEN** each period line reports the estimated collections and time and the current mode's measured tracking time, and write tracking behaves as without the setting

### Requirement: Missed collections are detectable
With `KYTY_UFFD_WP_ASYNC_VERIFY=1`, the emulator SHALL stop with a diagnostic that names the range
when it reads the CPU-dirty state of asynchronously tracked memory that was not collected at the
current synchronization point.

#### Scenario: A consumer skips collection
- **WHEN** verification is on and GPU work preparation reads CPU-dirty state of an asynchronously tracked range not collected since the last synchronization point
- **THEN** the emulator stops and the diagnostic names the range and the reading site
