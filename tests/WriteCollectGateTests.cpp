// WriteCollectGate (graphics/host_gpu/writeCollectGate.h): a chunk is handed out once per epoch,
// again once the epoch advanced, as chunk-aligned runs; Forget hands it out again at once.

#include "graphics/host_gpu/syncEpoch.h"
#include "graphics/host_gpu/writeCollectGate.h"

#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>

namespace {

using Libs::Graphics::WriteCollectGate;
using Run = std::pair<uint64_t, uint64_t>;

void Check(bool ok, const char* message) {
	if (!ok) {
		std::fprintf(stderr, "WriteCollectGateTests: failed: %s\n", message);
		std::abort();
	}
}

constexpr uint64_t MiB = 1u << 20u;

// Claims and returns the runs handed out.
std::vector<Run> Claim(WriteCollectGate& gate, uint64_t vaddr, uint64_t size, uint64_t epoch, uint32_t* count = nullptr) {
	std::vector<Run> runs;
	const auto       n = gate.Claim(vaddr, size, epoch, [&](uint64_t start, uint64_t end) { runs.emplace_back(start, end); });
	if (count != nullptr) {
		*count = n;
	}
	return runs;
}

void OncePerEpoch() {
	WriteCollectGate gate;
	uint32_t         count = 0;
	auto             runs  = Claim(gate, 0x1000, 0x100, 5, &count);
	Check(count == 1 && runs == std::vector<Run> {{0, 2 * MiB}}, "a first claim hands out the range's chunk, aligned");
	runs = Claim(gate, 0x1000, 0x100, 5, &count);
	Check(count == 0 && runs.empty(), "the same chunk in the same epoch is not handed out again");
	runs = Claim(gate, 0x3000, 0x100, 5, &count);
	Check(count == 0 && runs.empty(), "another range of the same chunk neither");
	runs = Claim(gate, 0x1000, 0x100, 6, &count);
	Check(count == 1 && runs == std::vector<Run> {{0, 2 * MiB}}, "the next epoch hands it out again");
	runs = Claim(gate, 0x1000, 0x100, 5, &count);
	Check(count == 0 && runs.empty(), "an older epoch does not");
	Check(gate.Collected(0x1000, 0x100, 6) && !gate.Collected(0x1000, 0x100, 7), "Collected follows the last claim");
	Check(gate.KnownChunks() == 1, "one chunk known");
	std::printf("[host]    %-48s ok\n", "OncePerEpoch");
}

void AlignedRuns() {
	WriteCollectGate gate;
	uint32_t         count = 0;
	// Half a MiB into chunk 0 up to half a MiB into chunk 2: three chunks, one run.
	auto runs = Claim(gate, MiB / 2, 4 * MiB, 1, &count);
	Check(count == 3 && runs == std::vector<Run> {{0, 6 * MiB}}, "a range over three chunks is one aligned run");
	// Chunk 1 collected again at epoch 2; chunks 0 and 2 are still at 1: two runs around it.
	runs = Claim(gate, 2 * MiB, 2 * MiB, 2, &count);
	Check(count == 1 && runs == std::vector<Run> {{2 * MiB, 4 * MiB}}, "the middle chunk alone");
	runs = Claim(gate, 0, 6 * MiB, 2, &count);
	Check(count == 2 && runs == std::vector<Run> {{0, 2 * MiB}, {4 * MiB, 6 * MiB}},
	      "the chunks around it, as two runs");
	Check(Claim(gate, 0, 6 * MiB, 2, &count).empty() && count == 0, "all collected at 2");
	Check(Claim(gate, 0, 0, 3, &count).empty() && count == 0, "an empty range claims nothing");
	// A range ending exactly on a chunk boundary does not touch the next chunk.
	runs = Claim(gate, 6 * MiB, 2 * MiB, 3, &count);
	Check(count == 1 && runs == std::vector<Run> {{6 * MiB, 8 * MiB}}, "a chunk-exact range is one chunk");
	Check(gate.KnownChunks() == 4, "four chunks known");
	std::printf("[host]    %-48s ok\n", "AlignedRuns");
}

void ForgetAndChunkSize() {
	WriteCollectGate gate(4096);
	uint32_t         count = 0;
	auto             runs  = Claim(gate, 0x10000, 0x3000, 9, &count);
	Check(count == 3 && runs == std::vector<Run> {{0x10000, 0x13000}}, "4 KiB chunks: three pages, one run");
	gate.Forget(0x11000, 0x1000);
	runs = Claim(gate, 0x10000, 0x3000, 9, &count);
	Check(count == 1 && runs == std::vector<Run> {{0x11000, 0x12000}}, "a forgotten chunk is handed out again in the same epoch");
	Check(gate.Collected(0x10000, 0x3000, 9), "collected again");
	gate.Forget(0x10000, 0x3000);
	Check(!gate.Collected(0x10000, 0x1000, 1) && gate.KnownChunks() == 0, "Forget removes the chunks");
	std::printf("[host]    %-48s ok\n", "ForgetAndChunkSize");
}

void WithSyncEpoch() {
	namespace SyncEpoch = Libs::Graphics::SyncEpoch;
	WriteCollectGate gate;
	uint32_t         count = 0;
	(void)Claim(gate, 0x200000, 0x1000, SyncEpoch::Current(), &count);
	Check(count == 1, "claimed at the current epoch");
	(void)Claim(gate, 0x200000, 0x1000, SyncEpoch::Current(), &count);
	Check(count == 0, "not again in the same epoch");
	SyncEpoch::Advance();
	(void)Claim(gate, 0x200000, 0x1000, SyncEpoch::Current(), &count);
	Check(count == 1, "again after SyncEpoch::Advance");
	(void)Claim(gate, 0x200000, 0x1000, SyncEpoch::Current(), &count);
	Check(count == 0, "and once only");
	std::printf("[host]    %-48s ok\n", "WithSyncEpoch");
}

} // namespace

int main() {
	OncePerEpoch();
	AlignedRuns();
	ForgetAndChunkSize();
	WithSyncEpoch();
	std::printf("WriteCollectGateTests: all cases passed\n");
	return 0;
}
