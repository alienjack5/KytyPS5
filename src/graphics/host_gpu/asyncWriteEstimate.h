#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_ASYNCWRITEESTIMATE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_ASYNCWRITEESTIMATE_H_

#include <cstdint>

// The estimator of asynchronous guest write tracking (KYTY_UFFD_WP_ASYNC_ESTIMATE=1).
//
// With the current tracking mode left as it is, it counts what the asynchronous mode
// (KYTY_UFFD_WP_ASYNC, uffdWriteWatch.h) would collect in a game: every consumer of CPU-dirty
// state notes the range it is about to read (NoteConsume), and the collection gate
// (writeCollectGate.h) says which 2 MiB chunks of it a collection would scan at the current
// synchronization epoch. The KYTY_FAULT_COST_LOG period line gets one more line, "Kyty async
// estimate: ...": the chunks per frame by consumer and their estimated time (the startup
// self-test's measured scan cost, or an assumed one where the kernel offers no asynchronous
// write-protection), next to the measured cost of the current mode's faults and protection calls. A
// summary of the whole run is printed at exit. The estimate is an upper bound of the scan time: a
// chunk counts once per epoch, as the mode would scan it.
namespace Libs::Graphics::AsyncWriteEstimate {

// The consumers of CPU-dirty state (design D4 of the change): a buffer synchronized for reading
// or for writing (which then marks it GPU-dirty), an image refreshed, the BDA pass over the mapped
// ranges, a range marked GPU-dirty, and a download (readback) of a range.
enum class Site : uint8_t { Buffer = 0, WrittenBuffer, Image, Bda, GpuDirty, Download, Count };

[[nodiscard]] bool Enabled() noexcept; // KYTY_UFFD_WP_ASYNC_ESTIMATE=1, read once

// A consumer is about to read the CPU-dirty state of [vaddr, vaddr + size). Any thread; nothing
// when the estimator is off.
void NoteConsume(Site site, uint64_t vaddr, uint64_t size) noexcept;

struct Counters {
	uint64_t calls[static_cast<int>(Site::Count)] {};  // consumer calls
	uint64_t chunks[static_cast<int>(Site::Count)] {}; // 2 MiB chunks a collection would scan
	uint64_t runs[static_cast<int>(Site::Count)] {};   // scans (runs of chunks) it would make
	[[nodiscard]] uint64_t TotalChunks() const noexcept;
	[[nodiscard]] uint64_t TotalCalls() const noexcept;
};
[[nodiscard]] Counters Take() noexcept;  // since the previous Take
[[nodiscard]] Counters Total() noexcept; // since the start

// The time one chunk's scan is estimated at: the startup self-test's measurement (2 MiB with 64
// written pages), or AssumedScanUsPerChunk when the kernel offers no asynchronous mode.
constexpr double     AssumedScanUsPerChunk = 10.0;
[[nodiscard]] double ScanUsPerChunk() noexcept;
[[nodiscard]] bool   ScanCostMeasured() noexcept;

// The period line (FaultCost::AdvanceFrame): the counters since the previous period, per frame,
// against the measured per-frame cost of the current mode.
void ReportPeriod(double seconds, uint64_t frames, double fault_ms_per_frame,
                  double protect_ms_per_frame, double unprotect_ms_per_frame);

} // namespace Libs::Graphics::AsyncWriteEstimate

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_ASYNCWRITEESTIMATE_H_
