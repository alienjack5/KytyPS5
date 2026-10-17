// Guest write-tracking microbenchmark (Linux): what it costs to learn about CPU writes to guest
// memory, per tracking mode. Built on request only (CMake target fault_bench).
//
// A memfd mapped MAP_SHARED, as guest memory is, of `area_mib` MiB, all write-protected at the
// start. Each frame, `threads` writer threads write one byte to `pages_per_frame` random pages in
// total. A protector thread plays the command processor: it collects what was written and protects
// it again while the writers run, and once more at the end of the frame (the uploads).
//
//   mprotect  a write to a protected page raises SIGSEGV; the handler makes a `window_pages`-page
//             aligned window writable (the emulator's fault-ahead window, KYTY_FAULT_AHEAD_KB) and
//             marks it; the protector re-protects marked windows with mprotect.
//   uffd      the same with userfaultfd write-protection (KYTY_UFFD_WP): SIGBUS and
//             UFFDIO_WRITEPROTECT instead of SIGSEGV and mprotect.
//   async     userfaultfd asynchronous write-protection (UFFD_FEATURE_WP_ASYNC, Linux 6.7+): a
//             write to a protected page is resolved by the kernel without a signal, and the
//             protector collects and re-protects the written pages with one PAGEMAP_SCAN over the
//             area (PM_SCAN_WP_MATCHING). `window_pages` is ignored.
//
// Per frame it reports the writers' wall time, the faults and their handler time (or the scans and
// their time), the protection calls, the pages left CPU-dirty (what the emulator would upload) and
// the minor page faults (the kernel's own write-notify faults on shared memory). The first tenth of
// the frames is warm-up.
//
//   fault_bench <mprotect|uffd|async> <threads> <window_pages> [frames=200] [pages_per_frame=4800]
//               [area_mib=64]

#include <fcntl.h>
#include <linux/fs.h>
#include <linux/userfaultfd.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <barrier>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

namespace {

constexpr uint64_t PageBytes = 4096;

enum class Mode { Mprotect, Uffd, Async };

Mode                   g_mode       = Mode::Mprotect;
uint64_t               g_area_pages = 16384; // 64 MiB
int                    g_uffd       = -1;
int                    g_pagemap    = -1; // /proc/self/pagemap, async only
uint8_t*               g_area       = nullptr;
uint64_t               g_window     = 1; // pages
uint64_t               g_windows    = 0;
std::atomic<uint64_t>* g_marked     = nullptr; // one bit per window

std::atomic<uint64_t> g_faults {0};
std::atomic<uint64_t> g_fault_ns {0};
std::atomic<uint64_t> g_protect_calls {0};
std::atomic<uint64_t> g_protect_ns {0};
std::atomic<uint64_t> g_scans {0};
std::atomic<uint64_t> g_scan_ns {0};
std::atomic<uint64_t> g_scan_pages {0};

uint64_t NowNs() {
	timespec ts {};
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

[[noreturn]] void Die(const char* what) {
	std::fprintf(stderr, "fault_bench: %s failed: %s\n", what, std::strerror(errno));
	std::exit(2);
}

bool UffdProtect(uint64_t first_page, uint64_t count, bool protect) {
	uffdio_writeprotect wp {};
	wp.range.start = reinterpret_cast<uint64_t>(g_area) + first_page * PageBytes;
	wp.range.len   = count * PageBytes;
	wp.mode        = protect ? UFFDIO_WRITEPROTECT_MODE_WP : 0;
	for (;;) {
		if (ioctl(g_uffd, UFFDIO_WRITEPROTECT, &wp) == 0) return true;
		if (errno != EAGAIN) return false;
	}
}

void SetWritable(uint64_t first_page, uint64_t count, bool writable) {
	if (g_mode != Mode::Mprotect) {
		if (!UffdProtect(first_page, count, !writable)) Die("UFFDIO_WRITEPROTECT");
	} else if (mprotect(g_area + first_page * PageBytes, count * PageBytes,
	                    writable ? PROT_READ | PROT_WRITE : PROT_READ) != 0) {
		Die("mprotect");
	}
}

// mprotect and uffd: the tracking fault. Releases the write's window and marks it for the protector.
void OnFault(int /*signal*/, siginfo_t* info, void* /*context*/) {
	const auto begin   = NowNs();
	const auto address = reinterpret_cast<uint8_t*>(info->si_addr);
	if (address < g_area || address >= g_area + g_area_pages * PageBytes) {
		std::abort(); // not ours
	}
	const uint64_t window = static_cast<uint64_t>(address - g_area) / PageBytes / g_window;
	SetWritable(window * g_window, g_window, true);
	g_marked[window / 64].fetch_or(1ull << (window % 64), std::memory_order_release);
	g_faults.fetch_add(1, std::memory_order_relaxed);
	g_fault_ns.fetch_add(NowNs() - begin, std::memory_order_relaxed);
}

// The windows marked since the last call (the writes an upload will pick up).
std::vector<uint64_t> TakeMarked() {
	std::vector<uint64_t> windows;
	for (uint64_t word = 0; word < (g_windows + 63) / 64; word++) {
		uint64_t bits = g_marked[word].exchange(0, std::memory_order_acquire);
		while (bits != 0) {
			windows.push_back(word * 64 + static_cast<uint64_t>(__builtin_ctzll(bits)));
			bits &= bits - 1;
		}
	}
	return windows;
}

// Protects windows again, one call per window, as an upload re-protects what it copied.
void Reprotect(const std::vector<uint64_t>& windows) {
	for (const auto window: windows) {
		const auto begin = NowNs();
		SetWritable(window * g_window, g_window, false);
		g_protect_ns.fetch_add(NowNs() - begin, std::memory_order_relaxed);
		g_protect_calls.fetch_add(1, std::memory_order_relaxed);
	}
}

// async: one PAGEMAP_SCAN over the area reports the pages written since they were protected and
// protects them again in the same walk. Returns the pages reported.
uint64_t ScanWritten() {
	static thread_local std::vector<page_region> regions(4096);
	const auto begin = NowNs();
	uint64_t   start = reinterpret_cast<uint64_t>(g_area);
	const auto end   = start + g_area_pages * PageBytes;
	uint64_t   pages = 0;
	while (start < end) {
		pm_scan_arg arg {};
		arg.size          = sizeof(arg);
		arg.flags         = PM_SCAN_WP_MATCHING | PM_SCAN_CHECK_WPASYNC;
		arg.start         = start;
		arg.end           = end;
		arg.vec           = reinterpret_cast<uint64_t>(regions.data());
		arg.vec_len       = regions.size();
		arg.category_mask = PAGE_IS_WRITTEN;
		arg.return_mask   = PAGE_IS_WRITTEN;
		const long count  = ioctl(g_pagemap, PAGEMAP_SCAN, &arg);
		if (count < 0) Die("PAGEMAP_SCAN");
		for (long i = 0; i < count; i++) pages += (regions[i].end - regions[i].start) / PageBytes;
		if (arg.walk_end >= end) break;
		start = arg.walk_end;
	}
	g_scans.fetch_add(1, std::memory_order_relaxed);
	g_scan_ns.fetch_add(NowNs() - begin, std::memory_order_relaxed);
	g_scan_pages.fetch_add(pages, std::memory_order_relaxed);
	return pages;
}

long MinorFaults() {
	rusage usage {};
	getrusage(RUSAGE_SELF, &usage);
	return usage.ru_minflt;
}

[[noreturn]] void Usage() {
	std::fprintf(stderr,
	             "usage: fault_bench <mprotect|uffd|async> <threads> <window_pages> [frames=200] "
	             "[pages_per_frame=4800] [area_mib=64]\n");
	std::exit(1);
}

} // namespace

int main(int argc, char** argv) {
	if (argc < 4) Usage();
	if (std::strcmp(argv[1], "mprotect") == 0) {
		g_mode = Mode::Mprotect;
	} else if (std::strcmp(argv[1], "uffd") == 0) {
		g_mode = Mode::Uffd;
	} else if (std::strcmp(argv[1], "async") == 0) {
		g_mode = Mode::Async;
	} else {
		Usage();
	}
	const auto     threads = static_cast<uint32_t>(std::atoi(argv[2]));
	g_window               = static_cast<uint64_t>(std::atoi(argv[3]));
	const uint32_t frames  = argc > 4 ? static_cast<uint32_t>(std::atoi(argv[4])) : 200;
	const uint32_t pages   = argc > 5 ? static_cast<uint32_t>(std::atoi(argv[5])) : 4800;
	if (argc > 6) g_area_pages = static_cast<uint64_t>(std::atoll(argv[6])) * 1024 * 1024 / PageBytes;
	if (threads == 0 || g_window == 0 || frames < 10 || pages == 0 || g_area_pages < g_window) Usage();
	g_windows = g_area_pages / g_window;
	g_marked  = new std::atomic<uint64_t>[(g_windows + 63) / 64]();

	const int memfd = static_cast<int>(syscall(SYS_memfd_create, "guest", 0));
	if (memfd < 0 || ftruncate(memfd, static_cast<off_t>(g_area_pages * PageBytes)) != 0) Die("memfd");
	g_area = static_cast<uint8_t*>(
	    mmap(nullptr, g_area_pages * PageBytes, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, 0));
	if (g_area == MAP_FAILED) Die("mmap");
	std::memset(g_area, 1, g_area_pages * PageBytes); // populated, as guest memory in use is

	if (g_mode != Mode::Mprotect) {
		g_uffd = static_cast<int>(syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY));
		if (g_uffd < 0) Die("userfaultfd");
		uffdio_api api {};
		api.api      = UFFD_API;
		api.features = g_mode == Mode::Async ? (UFFD_FEATURE_WP_ASYNC | UFFD_FEATURE_WP_HUGETLBFS_SHMEM)
		                                     : (UFFD_FEATURE_SIGBUS | UFFD_FEATURE_WP_HUGETLBFS_SHMEM);
		if (ioctl(g_uffd, UFFDIO_API, &api) != 0) Die("UFFDIO_API");
		uffdio_register reg {};
		reg.range.start = reinterpret_cast<uint64_t>(g_area);
		reg.range.len   = g_area_pages * PageBytes;
		reg.mode        = UFFDIO_REGISTER_MODE_WP;
		if (ioctl(g_uffd, UFFDIO_REGISTER, &reg) != 0) Die("UFFDIO_REGISTER");
		if (g_mode == Mode::Async && (g_pagemap = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC)) < 0) {
			Die("open /proc/self/pagemap");
		}
	}
	if (g_mode != Mode::Async) {
		struct sigaction action {};
		action.sa_sigaction = OnFault;
		action.sa_flags     = SA_SIGINFO | SA_NODEFER;
		sigemptyset(&action.sa_mask);
		if (sigaction(g_mode == Mode::Uffd ? SIGBUS : SIGSEGV, &action, nullptr) != 0) Die("sigaction");
	}
	SetWritable(0, g_area_pages, false);

	std::barrier             start(static_cast<std::ptrdiff_t>(threads) + 1);
	std::barrier             end(static_cast<std::ptrdiff_t>(threads) + 1);
	std::atomic<bool>        writing {false};
	std::vector<std::thread> writers;
	for (uint32_t t = 0; t < threads; t++) {
		writers.emplace_back([&, t] {
			std::mt19937_64 random(1234 + t);
			for (uint32_t frame = 0; frame < frames; frame++) {
				start.arrive_and_wait();
				for (uint32_t i = t; i < pages; i += threads) {
					volatile uint8_t* page = g_area + (random() % g_area_pages) * PageBytes;
					*page                  = static_cast<uint8_t>(i);
				}
				end.arrive_and_wait();
			}
		});
	}
	// The protector runs while the writers write (the command processor's uploads re-protect
	// pages): every ~200 us it protects again the windows it took on its previous pass, so an
	// upload trails the writes it copies instead of racing the faulting write's retry. In async
	// mode each pass is one scan.
	std::atomic<bool> stop {false};
	std::atomic<bool> idle {true};
	std::thread       protector([&] {
		std::vector<uint64_t> pending;
		while (!stop.load(std::memory_order_acquire)) {
			if (!writing.load(std::memory_order_acquire)) {
				if (!pending.empty()) {
					Reprotect(pending);
					pending.clear();
				}
				idle.store(true, std::memory_order_release);
				std::this_thread::yield();
				continue;
			}
			idle.store(false, std::memory_order_release);
			std::this_thread::sleep_for(std::chrono::microseconds(200));
			if (g_mode == Mode::Async) {
				ScanWritten();
				continue;
			}
			Reprotect(pending);
			pending = TakeMarked();
		}
	});

	const uint32_t warmup  = frames / 10;
	uint64_t       wall_ns = 0, faults = 0, fault_ns = 0, calls = 0, protect_ns = 0, scans = 0,
	         scan_ns = 0, scan_pages = 0;
	long minor = 0;
	for (uint32_t frame = 0; frame < frames; frame++) {
		const auto f0 = g_faults.load(), fn0 = g_fault_ns.load(), c0 = g_protect_calls.load(),
		           pn0 = g_protect_ns.load(), s0 = g_scans.load(), sn0 = g_scan_ns.load(),
		           sp0 = g_scan_pages.load();
		const auto m0 = MinorFaults();
		writing.store(true, std::memory_order_release);
		const auto begin = NowNs();
		start.arrive_and_wait();
		end.arrive_and_wait();
		const auto elapsed = NowNs() - begin;
		writing.store(false, std::memory_order_release);
		// The rest of the frame's uploads: the protector's pending windows, then what is still marked.
		while (!idle.load(std::memory_order_acquire)) {
			std::this_thread::yield();
		}
		if (g_mode == Mode::Async) {
			ScanWritten();
		} else {
			Reprotect(TakeMarked());
		}
		if (frame >= warmup) {
			wall_ns += elapsed;
			faults += g_faults.load() - f0;
			fault_ns += g_fault_ns.load() - fn0;
			calls += g_protect_calls.load() - c0;
			protect_ns += g_protect_ns.load() - pn0;
			minor += MinorFaults() - m0;
			scans += g_scans.load() - s0;
			scan_ns += g_scan_ns.load() - sn0;
			scan_pages += g_scan_pages.load() - sp0;
		}
	}
	stop.store(true, std::memory_order_release);
	protector.join();
	for (auto& writer: writers) writer.join();

	const double n = frames - warmup;
	if (g_mode == Mode::Async) {
		std::printf("%-8s threads=%2u area=%5lluMiB: writers %7.3f ms/frame | scans %5.0f/frame, "
		            "%8.2f us/scan (%6.2f ms/frame) | dirty pages %6.0f/frame | minor faults %6.0f/frame\n",
		            argv[1], threads, static_cast<unsigned long long>(g_area_pages * PageBytes >> 20),
		            wall_ns / n / 1e6, scans / n, scans ? scan_ns / 1e3 / scans : 0.0, scan_ns / n / 1e6,
		            scan_pages / n, minor / n);
	} else {
		std::printf("%-8s threads=%2u window=%4lluKiB: writers %7.3f ms/frame | faults %6.0f/frame, "
		            "handler %6.2f us | protect %6.0f calls/frame, %6.2f us/call (%6.2f ms/frame) | "
		            "dirty pages %6.0f/frame | minor faults %6.0f/frame\n",
		            argv[1], threads, static_cast<unsigned long long>(g_window * PageBytes / 1024),
		            wall_ns / n / 1e6, faults / n, faults ? fault_ns / 1e3 / faults : 0.0, calls / n,
		            calls ? protect_ns / 1e3 / calls : 0.0, protect_ns / n / 1e6,
		            calls / n * static_cast<double>(g_window), minor / n);
	}
	return 0;
}
