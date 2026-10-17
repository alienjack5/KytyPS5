#include "common/platform/uffdWriteWatch.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#if defined(__linux__) && !defined(__APPLE__) && __has_include(<linux/userfaultfd.h>)
#define KYTY_UFFD_WRITE_WATCH 1
#include <fcntl.h>
#include <linux/userfaultfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace Common::UffdWriteWatch {
namespace {

std::mutex          g_mutex;
Support             g_support;
std::atomic<bool>   g_ready {false};
bool                  g_force           = false;
bool                  g_force_async     = false;
uint64_t              g_hidden_features = 0; // Testing::HideFeatures
Mode                  g_mode            = Mode::Mprotect;
std::string           g_mode_reason;
std::atomic<uint64_t> g_registers {0};
std::atomic<uint64_t> g_protects {0};
std::atomic<uint64_t> g_releases {0};
std::atomic<uint64_t> g_failures {0};
std::atomic<uint32_t> g_failure_logs {0};
std::atomic<uint64_t> g_async_registers {0};
std::atomic<uint64_t> g_async_protects {0};
std::atomic<uint64_t> g_scans {0};
std::atomic<uint64_t> g_scan_pages {0};
std::atomic<uint64_t> g_scan_failures {0};

bool Requested() {
	if (g_force) {
		return true;
	}
	const auto* value = std::getenv("KYTY_UFFD_WP");
	return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
}

// Exactly "1": any other value leaves write tracking as it is without the setting.
bool AsyncRequested() {
	if (g_force_async) {
		return true;
	}
	const auto* value = std::getenv("KYTY_UFFD_WP_ASYNC");
	return value != nullptr && std::strcmp(value, "1") == 0;
}

// The estimator (graphics/host_gpu/asyncWriteEstimate.h) wants the self-test's scan cost.
bool EstimateRequested() {
	const auto* value = std::getenv("KYTY_UFFD_WP_ASYNC_ESTIMATE");
	return value != nullptr && std::strcmp(value, "1") == 0;
}

void LogFailure(const char* what, uint64_t vaddr, uint64_t size, int error,
                const char* fallback = "mprotect") {
	g_failures.fetch_add(1, std::memory_order_relaxed);
	if (g_failure_logs.fetch_add(1, std::memory_order_relaxed) < 8) {
		std::fprintf(stderr,
		             "UffdWriteWatch: %s failed at 0x%llx+0x%llx: %s (falling back to %s)\n", what,
		             static_cast<unsigned long long>(vaddr), static_cast<unsigned long long>(size),
		             std::strerror(error), fallback);
		std::fflush(stderr);
	}
}

#if defined(KYTY_UFFD_WRITE_WATCH)
#ifndef UFFD_USER_MODE_ONLY
#define UFFD_USER_MODE_ONLY 1
#endif
// Feature bits by number: older headers lack the newer names.
constexpr uint64_t FeatureSigbus         = 1ull << 7u;
constexpr uint64_t FeatureWpShmem        = 1ull << 12u;
constexpr uint64_t FeatureWpUnpopulated  = 1ull << 13u;
constexpr uint64_t FeatureWpAsync        = 1ull << 15u;

// A descriptor, user-mode-only when the kernel allows it (`user_mode`), or -1 with errno set.
int OpenDescriptor(bool& user_mode) {
	int fd =
	    static_cast<int>(syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY));
	user_mode = fd >= 0;
	if (fd < 0) {
		fd = static_cast<int>(syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK));
	}
	return fd;
}

// The first UFFDIO_API call fixes the features of a descriptor: ask what the kernel offers on a
// throwaway descriptor, then enable what is needed on the real one. Both probes share the answer.
bool ReadFeatures(Support& s, int& error) {
	if (s.features != 0) {
		return true;
	}
	bool user_mode = false;
	int  fd        = OpenDescriptor(user_mode);
	if (fd < 0) {
		error = errno;
		return false;
	}
	uffdio_api api {};
	api.api = UFFD_API;
	if (ioctl(fd, UFFDIO_API, &api) != 0) {
		error = errno;
		close(fd);
		return false;
	}
	close(fd);
	s.features = api.features & ~g_hidden_features;
	return true;
}

void Probe(Support& s) {
	if (!ReadFeatures(s, s.error)) {
		return;
	}
	if ((s.features & FeatureSigbus) == 0 || (s.features & FeatureWpShmem) == 0) {
		s.error = ENOTSUP;
		return;
	}
	int fd = OpenDescriptor(s.user_mode);
	if (fd < 0) {
		s.error = errno;
		return;
	}
	uffdio_api enable {};
	enable.api      = UFFD_API;
	enable.features = FeatureSigbus | FeatureWpShmem | (s.features & FeatureWpUnpopulated);
	if (ioctl(fd, UFFDIO_API, &enable) != 0) {
		s.error = errno;
		close(fd);
		return;
	}
	s.fd           = fd;
	s.shared       = true;
	s.private_anon = (s.features & FeatureWpUnpopulated) != 0;
}

// The asynchronous descriptor (KYTY_UFFD_WP_ASYNC=1): WP_ASYNC and write-protection of shared
// memory are required (WP_ASYNC implies WP_UNPOPULATED), and the pagemap for PAGEMAP_SCAN.
void ProbeAsync(Support& s) {
	if (!ReadFeatures(s, s.async_error)) {
		s.async_reason = "the userfaultfd descriptor could not be opened";
		return;
	}
	if ((s.features & FeatureWpAsync) == 0) {
		s.async_error  = ENOTSUP;
		s.async_reason = "the kernel lacks UFFD_FEATURE_WP_ASYNC (Linux 6.7 or newer)";
		return;
	}
	if ((s.features & FeatureWpShmem) == 0) {
		s.async_error  = ENOTSUP;
		s.async_reason = "the kernel lacks write-protection of shared memory";
		return;
	}
	int fd = OpenDescriptor(s.async_user_mode);
	if (fd < 0) {
		s.async_error  = errno;
		s.async_reason = "the asynchronous userfaultfd descriptor could not be opened";
		return;
	}
	uffdio_api enable {};
	enable.api      = UFFD_API;
	enable.features = FeatureWpAsync | FeatureWpShmem | (s.features & FeatureWpUnpopulated);
	if (ioctl(fd, UFFDIO_API, &enable) != 0) {
		s.async_error  = errno;
		s.async_reason = "UFFDIO_API refused the asynchronous features";
		close(fd);
		return;
	}
	const int pagemap = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
	if (pagemap < 0) {
		s.async_error  = errno;
		s.async_reason = "/proc/self/pagemap could not be opened";
		close(fd);
		return;
	}
	s.async_fd   = fd;
	s.pagemap_fd = pagemap;
	s.async      = true;
}

void CloseDescriptors(Support& s) {
	for (int* fd: {&s.fd, &s.async_fd, &s.pagemap_fd}) {
		if (*fd >= 0) {
			close(*fd);
			*fd = -1;
		}
	}
}

constexpr uint64_t PageBytes = 4096;

bool RegisterOn(int fd, uint64_t vaddr, uint64_t size) {
	uffdio_register reg {};
	reg.range.start = vaddr;
	reg.range.len   = size;
	reg.mode        = UFFDIO_REGISTER_MODE_WP;
	return ioctl(fd, UFFDIO_REGISTER, &reg) == 0;
}

bool WriteProtectOn(int fd, uint64_t vaddr, uint64_t size, bool protect) {
	uffdio_writeprotect wp {};
	wp.range.start = vaddr;
	wp.range.len   = size;
	wp.mode        = protect ? UFFDIO_WRITEPROTECT_MODE_WP : 0;
	for (int attempt = 0; attempt < 1000; attempt++) {
		if (ioctl(fd, UFFDIO_WRITEPROTECT, &wp) == 0) {
			return true;
		}
		if (errno != EAGAIN) {
			break;
		}
	}
	return false;
}

// PAGEMAP_SCAN (linux/fs.h, Linux 6.7+), spelled out: older headers lack it. The layout is UAPI.
struct PmScanArg {
	uint64_t size;
	uint64_t flags;
	uint64_t start;
	uint64_t end;
	uint64_t walk_end; // written by the kernel: where the scan stopped
	uint64_t vec;
	uint64_t vec_len;
	uint64_t max_pages;
	uint64_t category_inverted;
	uint64_t category_mask;
	uint64_t category_anyof_mask;
	uint64_t return_mask;
};
struct PageRegion {
	uint64_t start;
	uint64_t end;
	uint64_t categories;
};
constexpr unsigned long PagemapScan    = _IOWR('f', 16, PmScanArg);
constexpr uint64_t      ScanWpMatching = 1ull << 0u; // protect the pages matched again
constexpr uint64_t      ScanCheckWpAsync =
    1ull << 1u; // EPERM on a page without asynchronous write-protection

bool ScanWrittenOn(int pagemap, uint64_t vaddr, uint64_t size, bool reprotect,
                   std::vector<WrittenRun>& written, int* error) {
	PageRegion regions[256];
	uint64_t   start = vaddr;
	const auto end   = vaddr + size;
	while (start < end) {
		PmScanArg arg {};
		arg.size          = sizeof(arg);
		arg.flags         = ScanCheckWpAsync | (reprotect ? ScanWpMatching : 0);
		arg.start         = start;
		arg.end           = end;
		arg.vec           = reinterpret_cast<uint64_t>(regions);
		arg.vec_len       = std::size(regions);
		arg.category_mask = PageIsWritten;
		arg.return_mask   = PageIsWritten | PageIsHuge;
		const long count  = ioctl(pagemap, PagemapScan, &arg);
		g_scans.fetch_add(1, std::memory_order_relaxed);
		if (count < 0) {
			if (error != nullptr) {
				*error = errno;
			}
			g_scan_failures.fetch_add(1, std::memory_order_relaxed);
			return false;
		}
		for (long i = 0; i < count; i++) {
			written.push_back({regions[i].start, regions[i].end, regions[i].categories});
			g_scan_pages.fetch_add((regions[i].end - regions[i].start) / PageBytes,
			                       std::memory_order_relaxed);
		}
		// The kernel walks to `end` unless the output filled up, and then says where it stopped.
		if (arg.walk_end >= end || arg.walk_end <= start) {
			break;
		}
		start = arg.walk_end;
	}
	return true;
}

// A populated MAP_SHARED memfd, as guest memory is.
struct MappedPages {
	uint8_t* base  = nullptr;
	uint64_t bytes = 0;
	int      fd    = -1;

	bool Create(uint64_t pages) {
		bytes = pages * PageBytes;
		fd    = static_cast<int>(syscall(__NR_memfd_create, "kyty-uffd-selftest", 0));
		if (fd < 0 || ftruncate(fd, static_cast<off_t>(bytes)) != 0) {
			return false;
		}
		auto* mapped = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (mapped == MAP_FAILED) {
			return false;
		}
		base = static_cast<uint8_t*>(mapped);
		std::memset(base, 1, bytes);
		return true;
	}
	~MappedPages() {
		if (base != nullptr) {
			munmap(base, bytes);
		}
		if (fd >= 0) {
			close(fd);
		}
	}
	MappedPages()                              = default;
	MappedPages(const MappedPages&)            = delete;
	MappedPages& operator=(const MappedPages&) = delete;
};

uint64_t NowNs() {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
	                                 std::chrono::steady_clock::now().time_since_epoch())
	                                 .count());
}

// The self-test (AsyncSelfTest in the header) and the scan microbenchmark, on the open descriptors.
void RunAsyncSelfTest(Support& s) {
	auto& t         = s.selftest;
	t.ran           = true;
	const auto fail = [&](const char* step) {
		if (t.failed_step == nullptr) {
			t.failed_step = step;
			t.error       = errno;
		}
	};
	MappedPages page;
	if (!page.Create(1)) {
		fail("map a page");
		return;
	}
	const auto              base = reinterpret_cast<uint64_t>(page.base);
	volatile uint8_t*       data = page.base;
	std::vector<WrittenRun> runs;
	if (!RegisterOn(s.async_fd, base, PageBytes)) {
		fail("UFFDIO_REGISTER");
		return;
	}
	if (!WriteProtectOn(s.async_fd, base, PageBytes, true)) {
		fail("UFFDIO_WRITEPROTECT");
		return;
	}
	// A scan that must report the page, and only it, written.
	const auto one_page = [&](const char* step) {
		runs.clear();
		int error = 0;
		if (!ScanWrittenOn(s.pagemap_fd, base, PageBytes, true, runs, &error)) {
			errno = error;
			fail(step);
			return false;
		}
		return runs.size() == 1 && runs[0].start == base && runs[0].end == base + PageBytes &&
		       (runs[0].categories & PageIsWritten) != 0;
	};
	data[0]    = 2; // a write to the protected page: no signal in asynchronous mode
	t.reported = one_page("PAGEMAP_SCAN after a write");
	if (!t.reported) {
		fail("the write was not reported");
		return;
	}
	t.huge_absent = (runs[0].categories & PageIsHuge) == 0;
	runs.clear();
	int error = 0;
	if (!ScanWrittenOn(s.pagemap_fd, base, PageBytes, true, runs, &error)) {
		errno = error;
		fail("PAGEMAP_SCAN after the page was protected again");
		return;
	}
	t.reprotected =
	    runs.empty(); // the first scan protected the page again, and nothing wrote it since
	data[1]   = 3;
	t.rearmed = one_page("PAGEMAP_SCAN after a second write");
	// A kernel-mode write into the protected page (a read() of guest memory): recorded, not
	// required.
	int pipe_fds[2] = {-1, -1};
	if (pipe2(pipe_fds, O_CLOEXEC) == 0) {
		const char byte = 'k';
		if (write(pipe_fds[1], &byte, 1) == 1) {
			t.kernel_write = read(pipe_fds[0], page.base + 2, 1) == 1;
			if (!t.kernel_write) {
				t.kernel_error = errno;
			}
		}
		close(pipe_fds[0]);
		close(pipe_fds[1]);
		if (t.kernel_write) {
			t.kernel_reported = one_page("PAGEMAP_SCAN after a read() into the page");
		}
	}
	t.ok = t.failed_step == nullptr && t.reported && t.reprotected && t.rearmed && t.huge_absent;
	if (!t.ok) {
		return;
	}
	// The scan microbenchmark: 2 MiB registered and protected, 64 pages written per round, one scan
	// that reports and re-protects them; the best of 8 rounds.
	MappedPages        area;
	constexpr uint64_t AreaPages = 512;
	if (!area.Create(AreaPages) ||
	    !RegisterOn(s.async_fd, reinterpret_cast<uint64_t>(area.base), area.bytes) ||
	    !WriteProtectOn(s.async_fd, reinterpret_cast<uint64_t>(area.base), area.bytes, true)) {
		return;
	}
	double best = 0;
	for (int round = 0; round < 8; round++) {
		for (uint64_t p = 0; p < AreaPages; p += AreaPages / 64) {
			area.base[p * PageBytes] = static_cast<uint8_t>(round);
		}
		runs.clear();
		const auto start = NowNs();
		if (!ScanWrittenOn(s.pagemap_fd, reinterpret_cast<uint64_t>(area.base), area.bytes, true,
		                   runs, nullptr)) {
			return;
		}
		const auto us = static_cast<double>(NowNs() - start) / 1e3;
		best          = round == 0 ? us : std::min(best, us);
	}
	t.scan_us_per_2mib = best;
}
#endif

void EnsureProbed() {
	if (g_ready.load(std::memory_order_acquire)) {
		return;
	}
	std::scoped_lock lock(g_mutex);
	if (g_ready.load(std::memory_order_relaxed)) {
		return;
	}
	Support s;
	s.probed             = true;
	s.requested          = Requested();
	s.async_requested    = AsyncRequested();
	s.estimate_requested = EstimateRequested();
#if defined(KYTY_UFFD_WRITE_WATCH)
	CloseDescriptors(g_support); // a previous probe's (Testing::Reprobe)
	if (s.requested) {
		Probe(s);
		std::printf("Kyty platform: guest write tracking with userfaultfd write-protection (KYTY_UFFD_WP=1): %s "
		            "(kernel features 0x%llx, %s descriptor%s%s)\n",
		            s.shared ? "on" : "unavailable, using mprotect", static_cast<unsigned long long>(s.features),
		            s.user_mode ? "user-mode-only" : "full", s.private_anon ? ", private memory too" : ", shared memory only",
		            s.error != 0 ? (std::string(", error: ") + std::strerror(s.error)).c_str() : "");
		std::fflush(stdout);
	}
	if (s.async_requested || s.estimate_requested) {
		const char* setting =
		    s.async_requested ? "KYTY_UFFD_WP_ASYNC=1" : "KYTY_UFFD_WP_ASYNC_ESTIMATE=1";
		ProbeAsync(s);
		if (s.async) {
			std::printf(
			    "Kyty platform: asynchronous userfaultfd write-protection (%s): available "
			    "(kernel features 0x%llx, %s descriptor, PAGEMAP_SCAN on /proc/self/pagemap)\n",
			    setting, static_cast<unsigned long long>(s.features),
			    s.async_user_mode ? "user-mode-only" : "full");
			RunAsyncSelfTest(s);
			const auto& t = s.selftest;
			if (t.ok) {
				std::printf("Kyty platform: asynchronous write-protection self-test: ok (a write "
				            "to a protected page is "
				            "reported by PAGEMAP_SCAN and the page protected again, a second write "
				            "is reported again, "
				            "no huge page; read() into a protected page %s%s%s; one scan of 2 MiB "
				            "with 64 written pages "
				            "%.1f us)\n",
				            t.kernel_write ? (t.kernel_reported ? "reported"
				                                                : "succeeded but was not reported")
				                           : "failed: ",
				            t.kernel_write ? "" : std::strerror(t.kernel_error),
				            t.kernel_write ? "" : " (such writes keep the fallback mode)",
				            t.scan_us_per_2mib);
			} else {
				std::printf(
				    "Kyty platform: asynchronous write-protection self-test: failed%s%s%s%s "
				    "(reported %s, "
				    "protected again %s, rearmed %s, huge page absent %s); the mode stays off\n",
				    t.failed_step != nullptr ? " at " : "",
				    t.failed_step != nullptr ? t.failed_step : "", t.error != 0 ? ": " : "",
				    t.error != 0 ? std::strerror(t.error) : "", t.reported ? "yes" : "no",
				    t.reprotected ? "yes" : "no", t.rearmed ? "yes" : "no",
				    t.huge_absent ? "yes" : "no");
			}
		} else {
			std::printf(
			    "Kyty platform: asynchronous userfaultfd write-protection (%s): unavailable: "
			    "%s (kernel features 0x%llx%s%s)\n",
			    setting, s.async_reason != nullptr ? s.async_reason : "unknown reason",
			    static_cast<unsigned long long>(s.features), s.async_error != 0 ? ", error: " : "",
			    s.async_error != 0 ? std::strerror(s.async_error) : "");
		}
		std::fflush(stdout);
	}
	// The mode: the first available of asynchronous, synchronous and mprotect, and why a requested
	// one was not taken.
	g_mode_reason.clear();
	const auto not_taken = [&](const char* setting, const std::string& why) {
		g_mode_reason += g_mode_reason.empty() ? "" : "; ";
		g_mode_reason += setting;
		g_mode_reason += " not taken: ";
		g_mode_reason += why;
	};
	const bool async_ok = s.async_requested && s.async && s.selftest.ok;
	const bool sync_ok  = s.requested && s.shared && s.fd >= 0;
	if (s.async_requested && !async_ok) {
		std::string why = s.async_reason != nullptr ? s.async_reason : "";
		if (s.async && !s.selftest.ok) {
			why = "the self-test failed";
			if (s.selftest.failed_step != nullptr) {
				why += std::string(" at ") + s.selftest.failed_step;
			}
		}
		if (s.async_error != 0 && (!s.async || s.selftest.error != 0)) {
			why +=
			    std::string(" (") + std::strerror(s.async ? s.selftest.error : s.async_error) + ")";
		}
		not_taken("KYTY_UFFD_WP_ASYNC=1", why);
	}
	if (s.requested && !sync_ok && !async_ok) {
		not_taken("KYTY_UFFD_WP=1", s.error == ENOTSUP
		                                ? "the kernel lacks SIGBUS or write-protect on shmem"
		                                : std::strerror(s.error));
	}
	g_mode = async_ok ? Mode::Async : sync_ok ? Mode::Sync : Mode::Mprotect;
	std::printf("Kyty platform: guest write tracking: %s%s%s\n",
	            g_mode == Mode::Async
	                ? "asynchronous userfaultfd write-protection (KYTY_UFFD_WP_ASYNC=1)"
	            : g_mode == Mode::Sync ? "userfaultfd write-protection (KYTY_UFFD_WP=1)"
	                                   : "mprotect",
	            g_mode_reason.empty() ? "" : "; ", g_mode_reason.c_str());
	std::fflush(stdout);
#else
	g_mode = Mode::Mprotect;
	g_mode_reason.clear();
#endif
	g_support = s;
	g_ready.store(true, std::memory_order_release);
}

} // namespace

const Support& Get() {
	EnsureProbed();
	return g_support;
}

bool Enabled() {
	const auto& s = Get();
	return s.requested && s.shared && s.fd >= 0;
}

bool AsyncEnabled() {
	const auto& s = Get();
	return s.async_requested && s.async && s.async_fd >= 0 && s.pagemap_fd >= 0 && s.selftest.ok;
}

Mode TrackingMode() {
	EnsureProbed();
	return g_mode;
}

const char* TrackingModeName() {
	switch (TrackingMode()) {
		case Mode::Async: return "asynchronous userfaultfd write-protection";
		case Mode::Sync: return "userfaultfd write-protection";
		default: return "mprotect";
	}
}

const char* TrackingModeReason() {
	EnsureProbed();
	return g_mode_reason.c_str();
}

bool RegisterAsync(uint64_t vaddr, uint64_t size) {
#if defined(KYTY_UFFD_WRITE_WATCH)
	const auto& s = Get();
	if (s.async_fd < 0) {
		return false;
	}
	if (!RegisterOn(s.async_fd, vaddr, size)) {
		LogFailure("UFFDIO_REGISTER (asynchronous)", vaddr, size, errno, "synchronous tracking");
		return false;
	}
	g_async_registers.fetch_add(1, std::memory_order_relaxed);
	return true;
#else
	(void)vaddr;
	(void)size;
	return false;
#endif
}

bool WriteProtectAsync(uint64_t vaddr, uint64_t size, bool protect) {
#if defined(KYTY_UFFD_WRITE_WATCH)
	const auto& s = Get();
	if (s.async_fd < 0) {
		return false;
	}
	if (!WriteProtectOn(s.async_fd, vaddr, size, protect)) {
		LogFailure(protect ? "UFFDIO_WRITEPROTECT(protect, asynchronous)"
		                   : "UFFDIO_WRITEPROTECT(release, asynchronous)",
		           vaddr, size, errno, "synchronous tracking");
		return false;
	}
	g_async_protects.fetch_add(1, std::memory_order_relaxed);
	return true;
#else
	(void)vaddr;
	(void)size;
	(void)protect;
	return false;
#endif
}

bool ScanWritten(uint64_t vaddr, uint64_t size, bool reprotect, std::vector<WrittenRun>& written,
                 int* error) {
#if defined(KYTY_UFFD_WRITE_WATCH)
	const auto& s = Get();
	if (s.pagemap_fd < 0) {
		if (error != nullptr) {
			*error = EBADF;
		}
		return false;
	}
	return ScanWrittenOn(s.pagemap_fd, vaddr, size, reprotect, written, error);
#else
	(void)vaddr;
	(void)size;
	(void)reprotect;
	(void)written;
	if (error != nullptr) {
		*error = ENOTSUP;
	}
	return false;
#endif
}

bool Register(uint64_t vaddr, uint64_t size) {
#if defined(KYTY_UFFD_WRITE_WATCH)
	const auto& s = Get();
	if (s.fd < 0) {
		return false;
	}
	uffdio_register reg {};
	reg.range.start = vaddr;
	reg.range.len   = size;
	reg.mode        = UFFDIO_REGISTER_MODE_WP;
	if (ioctl(s.fd, UFFDIO_REGISTER, &reg) != 0) {
		LogFailure("UFFDIO_REGISTER", vaddr, size, errno);
		return false;
	}
	g_registers.fetch_add(1, std::memory_order_relaxed);
	return true;
#else
	(void)vaddr;
	(void)size;
	return false;
#endif
}

bool WriteProtect(uint64_t vaddr, uint64_t size, bool protect, bool quiet) {
#if defined(KYTY_UFFD_WRITE_WATCH)
	const auto& s = Get();
	if (s.fd < 0) {
		return false;
	}
	uffdio_writeprotect wp {};
	wp.range.start = vaddr;
	wp.range.len   = size;
	wp.mode        = protect ? UFFDIO_WRITEPROTECT_MODE_WP : 0;
	for (int attempt = 0; attempt < 1000; attempt++) {
		if (ioctl(s.fd, UFFDIO_WRITEPROTECT, &wp) == 0) {
			(protect ? g_protects : g_releases).fetch_add(1, std::memory_order_relaxed);
			return true;
		}
		if (errno != EAGAIN) {
			break;
		}
	}
	if (!quiet) {
		LogFailure(protect ? "UFFDIO_WRITEPROTECT(protect)" : "UFFDIO_WRITEPROTECT(release)", vaddr, size, errno);
	}
	return false;
#else
	(void)vaddr;
	(void)size;
	(void)protect;
	(void)quiet;
	return false;
#endif
}

Stats GetStats() {
	Stats stats;
	stats.registers       = g_registers.load(std::memory_order_relaxed);
	stats.protects        = g_protects.load(std::memory_order_relaxed);
	stats.releases        = g_releases.load(std::memory_order_relaxed);
	stats.failures        = g_failures.load(std::memory_order_relaxed);
	stats.async_registers = g_async_registers.load(std::memory_order_relaxed);
	stats.async_protects  = g_async_protects.load(std::memory_order_relaxed);
	stats.scans           = g_scans.load(std::memory_order_relaxed);
	stats.scan_pages      = g_scan_pages.load(std::memory_order_relaxed);
	stats.scan_failures   = g_scan_failures.load(std::memory_order_relaxed);
	return stats;
}

namespace Testing {
void ForceEnable() {
	std::scoped_lock lock(g_mutex);
	g_force = true;
	g_ready.store(false, std::memory_order_release);
}

void ForceEnableAsync() {
	std::scoped_lock lock(g_mutex);
	g_force_async = true;
	g_ready.store(false, std::memory_order_release);
}

void Reprobe() {
	std::scoped_lock lock(g_mutex);
	g_ready.store(false, std::memory_order_release);
}

void HideFeatures(uint64_t features) {
	std::scoped_lock lock(g_mutex);
	g_hidden_features = features;
}
} // namespace Testing

} // namespace Common::UffdWriteWatch
