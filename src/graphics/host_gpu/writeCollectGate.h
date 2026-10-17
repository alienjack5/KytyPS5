#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_WRITECOLLECTGATE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_WRITECOLLECTGATE_H_

#include <cstdint>
#include <unordered_map>

// The collection gate of asynchronous guest write tracking (KYTY_UFFD_WP_ASYNC) and of its
// estimator (KYTY_UFFD_WP_ASYNC_ESTIMATE).
//
// In that mode a CPU write to tracked guest memory takes no fault: before GPU work consumes the
// CPU-dirty state of a range, the written pages of the range are collected from the kernel
// (PAGEMAP_SCAN). A collection has to happen once per synchronization epoch (syncEpoch.h: the
// points that order CPU writes before GPU work), not once per consumer: guest memory is divided
// into chunks, each remembering the epoch of its last collection, and Claim() hands out the
// chunks of a range whose last collection is older than the current epoch, as chunk-aligned runs,
// marking them collected. The same chunk asked for again in the same epoch is not handed out; it
// is once the epoch advanced. A write that lands between a chunk's collection and the GPU work of
// the same epoch races that work on the console too.
//
// One instance per tracker; the caller serializes its use (the command processor's thread).
namespace Libs::Graphics {

class WriteCollectGate {
public:
	static constexpr uint64_t DefaultChunkBytes = 2u << 20u; // 2 MiB

	explicit WriteCollectGate(uint64_t chunk_bytes = DefaultChunkBytes)
	    : m_chunk_bytes(chunk_bytes) {}

	[[nodiscard]] uint64_t ChunkBytes() const noexcept { return m_chunk_bytes; }
	// Chunks ever claimed (the table's size).
	[[nodiscard]] size_t KnownChunks() const noexcept { return m_epochs.size(); }

	// The chunks of [vaddr, vaddr + size) not yet collected at `epoch`, as chunk-aligned runs
	// `func(start, end)` in address order, marked collected at `epoch`. Returns the chunk count.
	// A chunk collected at a later epoch (the epoch went backwards: never in practice) is not
	// handed out either.
	template <typename Func>
	uint32_t Claim(uint64_t vaddr, uint64_t size, uint64_t epoch, Func&& func) {
		if (size == 0) {
			return 0;
		}
		const uint64_t first     = vaddr / m_chunk_bytes;
		const uint64_t last      = (vaddr + size - 1) / m_chunk_bytes;
		uint32_t       count     = 0;
		uint64_t       run_start = 0;
		bool           in_run    = false;
		for (uint64_t chunk = first; chunk <= last; chunk++) {
			auto& recorded = m_epochs[chunk];
			if (recorded >= epoch) {
				if (in_run) {
					func(run_start, chunk * m_chunk_bytes);
					in_run = false;
				}
				continue;
			}
			recorded = epoch;
			count++;
			if (!in_run) {
				run_start = chunk * m_chunk_bytes;
				in_run    = true;
			}
		}
		if (in_run) {
			func(run_start, (last + 1) * m_chunk_bytes);
		}
		return count;
	}

	// Whether every chunk of the range was collected at `epoch` (no claim).
	[[nodiscard]] bool Collected(uint64_t vaddr, uint64_t size, uint64_t epoch) const {
		if (size == 0) {
			return true;
		}
		const uint64_t first = vaddr / m_chunk_bytes;
		const uint64_t last  = (vaddr + size - 1) / m_chunk_bytes;
		for (uint64_t chunk = first; chunk <= last; chunk++) {
			const auto found = m_epochs.find(chunk);
			if (found == m_epochs.end() || found->second < epoch) {
				return false;
			}
		}
		return true;
	}

	// The chunks of the range forget their collection (a mapping changed under them): the next
	// claim hands them out again, whatever the epoch.
	void Forget(uint64_t vaddr, uint64_t size) {
		if (size == 0) {
			return;
		}
		const uint64_t first = vaddr / m_chunk_bytes;
		const uint64_t last  = (vaddr + size - 1) / m_chunk_bytes;
		for (uint64_t chunk = first; chunk <= last; chunk++) {
			m_epochs.erase(chunk);
		}
	}

private:
	uint64_t                               m_chunk_bytes;
	std::unordered_map<uint64_t, uint64_t> m_epochs; // chunk index -> epoch of its last collection
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_WRITECOLLECTGATE_H_
