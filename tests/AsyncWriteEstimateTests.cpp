// The asynchronous write-tracking estimator (graphics/host_gpu/asyncWriteEstimate.h): consumers'
// ranges become chunks to collect once per epoch, counted by site, and the period line reports
// them per frame next to the measured cost of the current mode.

#include "graphics/host_gpu/asyncWriteEstimate.h"
#include "graphics/host_gpu/syncEpoch.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace {

namespace Estimate  = Libs::Graphics::AsyncWriteEstimate;
namespace SyncEpoch = Libs::Graphics::SyncEpoch;
using Estimate::Site;

void Check(bool ok, const char* message) {
	if (!ok) {
		std::fprintf(stderr, "AsyncWriteEstimateTests: failed: %s\n", message);
		std::abort();
	}
}

constexpr uint64_t MiB = 1u << 20u;

uint64_t Of(const Estimate::Counters& c, Site site, const uint64_t (Estimate::Counters::*field)[static_cast<int>(Site::Count)]) {
	return (c.*field)[static_cast<int>(site)];
}

// What a function printed on stdout (Linux: stdout goes to a memfd meanwhile).
template <typename Func>
std::string Captured(Func&& func) {
#if defined(__linux__)
	std::fflush(stdout);
	const int saved = dup(STDOUT_FILENO);
	const int sink  = static_cast<int>(syscall(__NR_memfd_create, "estimate-output", 0));
	Check(saved >= 0 && sink >= 0 && dup2(sink, STDOUT_FILENO) == STDOUT_FILENO, "capture stdout");
	func();
	std::fflush(stdout);
	Check(dup2(saved, STDOUT_FILENO) == STDOUT_FILENO, "restore stdout");
	close(saved);
	std::string output;
	char        buffer[4096];
	lseek(sink, 0, SEEK_SET);
	for (ssize_t got = 0; (got = read(sink, buffer, sizeof(buffer))) > 0;) {
		output.append(buffer, static_cast<size_t>(got));
	}
	close(sink);
	return output;
#else
	func();
	return "";
#endif
}

bool Contains(const std::string& text, const char* part) {
	return text.find(part) != std::string::npos;
}

} // namespace

int main() {
	// Read once, before the first Enabled(): the estimator is on for the whole process.
	setenv("KYTY_UFFD_WP_ASYNC_ESTIMATE", "1", 1);
	Check(Estimate::Enabled(), "KYTY_UFFD_WP_ASYNC_ESTIMATE=1 enables the estimator");
	(void)Estimate::Take();

	// Chunks to collect: once per epoch whatever the site, counted where they were first asked for.
	Estimate::NoteConsume(Site::Buffer, 0x1000, 0x100);
	Estimate::NoteConsume(Site::Buffer, 0x1000, 0x100);
	Estimate::NoteConsume(Site::Image, 2 * MiB - 0x10, 4 * MiB); // chunks 0 (collected), 1 and 2
	Estimate::NoteConsume(Site::Download, 256 * MiB, 0x1000);
	Estimate::NoteConsume(Site::GpuDirty, 0, 0); // an empty range is nothing
	auto period = Estimate::Take();
	Check(Of(period, Site::Buffer, &Estimate::Counters::calls) == 2 && Of(period, Site::Buffer, &Estimate::Counters::chunks) == 1,
	      "a buffer's chunk is collected once in an epoch, over two calls");
	Check(Of(period, Site::Image, &Estimate::Counters::chunks) == 2 && Of(period, Site::Image, &Estimate::Counters::runs) == 1,
	      "an image over three chunks collects the two not yet collected, in one run");
	Check(Of(period, Site::Download, &Estimate::Counters::chunks) == 1, "a download collects its chunk");
	Check(Of(period, Site::GpuDirty, &Estimate::Counters::calls) == 0, "an empty range is not a call");
	Check(period.TotalChunks() == 4 && period.TotalCalls() == 4, "totals");
	Check(Estimate::Take().TotalChunks() == 0, "Take resets the period");
	std::printf("[host]    %-48s ok\n", "ChunksOncePerEpoch");

	// The next epoch collects the same chunks again.
	SyncEpoch::Advance();
	Estimate::NoteConsume(Site::Bda, 0, 6 * MiB);
	period = Estimate::Take();
	Check(Of(period, Site::Bda, &Estimate::Counters::chunks) == 3 && Of(period, Site::Bda, &Estimate::Counters::runs) == 1,
	      "after SyncEpoch::Advance the chunks are collected again");
	Check(Estimate::Total().TotalChunks() == 7, "Total accumulates");
	std::printf("[host]    %-48s ok\n", "AgainNextEpoch");

	// The scan cost: measured by the self-test when the kernel offers the asynchronous mode.
	Check(Estimate::ScanUsPerChunk() > 0, "a scan cost");
	Check(Estimate::ScanCostMeasured() || Estimate::ScanUsPerChunk() == Estimate::AssumedScanUsPerChunk,
	      "assumed when not measured");
	std::printf("[host]    scan cost %.1f us per chunk (%s)\n", Estimate::ScanUsPerChunk(),
	            Estimate::ScanCostMeasured() ? "measured" : "assumed");

	// The period line: per frame, by site, against the measured cost.
	SyncEpoch::Advance();
	Estimate::NoteConsume(Site::WrittenBuffer, 0, 2 * MiB);
	Estimate::NoteConsume(Site::Image, 8 * MiB, 4 * MiB);
	const auto line = Captured([] { Estimate::ReportPeriod(2.0, 2, 0.5, 0.2, 0.1); });
	Check(Contains(line, "Kyty async estimate: last 2 s, per frame: 1.5 chunks of 2 MiB to collect in 1.0 consumer calls "
	                     "(buffer 0.0, written 0.5, image 1.0, bda 0.0, gpu-dirty 0.0, download 0.0), ~"),
	      "the period line reports the chunks per frame by site");
	Check(Contains(line, " us per chunk (") && (Contains(line, "measured by the startup self-test") || Contains(line, "assumed")),
	      "the period line names the scan cost and its source");
	Check(Contains(line, "; measured now 0.80 ms (faults 0.50, protect 0.20, unprotect 0.10)\n"),
	      "the period line reports the measured cost of the current mode");
	Check(Estimate::Take().TotalChunks() == 0, "the period line took the period");
	std::printf("[host]    %-48s ok\n", "PeriodLine");

	std::printf("AsyncWriteEstimateTests: all cases passed\n");
	return 0;
}
