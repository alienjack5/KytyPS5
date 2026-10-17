#include "graphics/host_gpu/asyncWriteEstimate.h"

#include "common/platform/uffdWriteWatch.h"
#include "graphics/host_gpu/faultCost.h"
#include "graphics/host_gpu/syncEpoch.h"
#include "graphics/host_gpu/writeCollectGate.h"

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace Libs::Graphics::AsyncWriteEstimate {
namespace {

std::mutex       g_mutex; // the gate and the counters
WriteCollectGate g_gate;
Counters         g_period; // since the previous Take
Counters         g_total;

const char* CostSource() {
	return ScanCostMeasured() ? "measured by the startup self-test"
	                          : "assumed, no asynchronous mode here";
}

void PrintSummary() {
	const auto total         = Total();
	const auto frames        = FaultCost::Frame();
	const auto per_us        = ScanUsPerChunk();
	const auto ms            = static_cast<double>(total.TotalChunks()) * per_us / 1e3;
	char       per_frame[64] = "";
	if (frames != 0) {
		std::snprintf(per_frame, sizeof(per_frame), ", ~%.2f ms per frame",
		              ms / static_cast<double>(frames));
	}
	std::printf("Kyty async estimate: whole run: %" PRIu64 " chunks of 2 MiB to collect in %" PRIu64
	            " consumer calls over %" PRIu32 " frames (buffer %" PRIu64 ", written %" PRIu64
	            ", image %" PRIu64 ", bda %" PRIu64 ", gpu-dirty %" PRIu64 ", download %" PRIu64
	            "): ~%.1f ms at %.1f us per chunk (%s)%s\n",
	            total.TotalChunks(), total.TotalCalls(), frames, total.chunks[0], total.chunks[1],
	            total.chunks[2], total.chunks[3], total.chunks[4], total.chunks[5], ms, per_us,
	            CostSource(), per_frame);
	std::fflush(stdout);
}

bool ReadEnabled() {
	const auto* value   = std::getenv("KYTY_UFFD_WP_ASYNC_ESTIMATE");
	const bool  enabled = value != nullptr && std::strcmp(value, "1") == 0;
	if (enabled) {
		std::atexit(PrintSummary);
	}
	return enabled;
}

} // namespace

uint64_t Counters::TotalChunks() const noexcept {
	uint64_t sum = 0;
	for (const auto n: chunks) {
		sum += n;
	}
	return sum;
}

uint64_t Counters::TotalCalls() const noexcept {
	uint64_t sum = 0;
	for (const auto n: calls) {
		sum += n;
	}
	return sum;
}

bool Enabled() noexcept {
	static const bool enabled = ReadEnabled();
	return enabled;
}

void NoteConsume(Site site, uint64_t vaddr, uint64_t size) noexcept {
	if (!Enabled() || size == 0) {
		return;
	}
	const auto       index = static_cast<int>(site);
	const auto       epoch = SyncEpoch::Current();
	std::scoped_lock lock(g_mutex);
	uint32_t         runs   = 0;
	const auto       chunks = g_gate.Claim(vaddr, size, epoch, [&](uint64_t, uint64_t) { runs++; });
	for (auto* counters: {&g_period, &g_total}) {
		counters->calls[index]++;
		counters->chunks[index] += chunks;
		counters->runs[index] += runs;
	}
}

Counters Take() noexcept {
	std::scoped_lock lock(g_mutex);
	const auto       taken = g_period;
	g_period               = Counters {};
	return taken;
}

Counters Total() noexcept {
	std::scoped_lock lock(g_mutex);
	return g_total;
}

bool ScanCostMeasured() noexcept {
	const auto& support = Common::UffdWriteWatch::Get();
	return support.async && support.selftest.ok && support.selftest.scan_us_per_2mib > 0;
}

double ScanUsPerChunk() noexcept {
	return ScanCostMeasured() ? Common::UffdWriteWatch::Get().selftest.scan_us_per_2mib
	                          : AssumedScanUsPerChunk;
}

void ReportPeriod(double seconds, uint64_t frames, double fault_ms_per_frame,
                  double protect_ms_per_frame, double unprotect_ms_per_frame) {
	const auto   period = Take();
	const double f      = static_cast<double>(frames != 0 ? frames : 1);
	const auto   per_us = ScanUsPerChunk();
	const auto   per    = [&](Site site) {
		return static_cast<double>(period.chunks[static_cast<int>(site)]) / f;
	};
	std::printf(
	    "Kyty async estimate: last %.0f s, per frame: %.1f chunks of 2 MiB to collect in %.1f "
	    "consumer calls "
	    "(buffer %.1f, written %.1f, image %.1f, bda %.1f, gpu-dirty %.1f, download %.1f), ~%.2f "
	    "ms at %.1f us "
	    "per chunk (%s); measured now %.2f ms (faults %.2f, protect %.2f, unprotect %.2f)\n",
	    seconds, static_cast<double>(period.TotalChunks()) / f,
	    static_cast<double>(period.TotalCalls()) / f, per(Site::Buffer), per(Site::WrittenBuffer),
	    per(Site::Image), per(Site::Bda), per(Site::GpuDirty), per(Site::Download),
	    static_cast<double>(period.TotalChunks()) / f * per_us / 1e3, per_us, CostSource(),
	    fault_ms_per_frame + protect_ms_per_frame + unprotect_ms_per_frame, fault_ms_per_frame,
	    protect_ms_per_frame, unprotect_ms_per_frame);
	std::fflush(stdout);
}

} // namespace Libs::Graphics::AsyncWriteEstimate
