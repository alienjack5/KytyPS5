// The userfaultfd write-tracking probes (common/platform/uffdWriteWatch.h) against the running
// kernel: what Support reports must match what the kernel allows this process, for the synchronous
// (KYTY_UFFD_WP) and the asynchronous (KYTY_UFFD_WP_ASYNC) descriptor. Linux only.

#include "common/platform/uffdWriteWatch.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#if defined(__linux__)
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <linux/userfaultfd.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <vector>

#ifndef UFFD_USER_MODE_ONLY
#define UFFD_USER_MODE_ONLY 1
#endif

namespace {

using Common::UffdWriteWatch::Get;
using Common::UffdWriteWatch::Support;

void Check(bool ok, const char* message) {
	if (!ok) {
		std::fprintf(stderr, "UffdWriteWatchTests: failed: %s\n", message);
		std::abort();
	}
}

// What the kernel offers this process, read independently of the module under test.
struct Kernel {
	bool     descriptor = false; // a userfaultfd descriptor can be opened
	bool     user_mode  = false; // ... user-mode only
	uint64_t features   = 0;
	bool     pagemap    = false; // /proc/self/pagemap can be opened
};

Kernel ProbeKernel() {
	Kernel kernel;
	int    fd        = static_cast<int>(syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY));
	kernel.user_mode = fd >= 0;
	if (fd < 0) {
		fd = static_cast<int>(syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK));
	}
	if (fd >= 0) {
		kernel.descriptor = true;
		uffdio_api api {};
		api.api = UFFD_API;
		if (ioctl(fd, UFFDIO_API, &api) == 0) {
			kernel.features = api.features;
		}
		close(fd);
	}
	const int pagemap = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
	kernel.pagemap    = pagemap >= 0;
	if (pagemap >= 0) {
		close(pagemap);
	}
	return kernel;
}

bool Open(int fd) {
	return fd >= 0 && fcntl(fd, F_GETFD) != -1;
}

constexpr uint64_t FeatureSigbus  = 1ull << 7u;
constexpr uint64_t FeatureWpShmem = 1ull << 12u;
constexpr uint64_t FeatureWpAsync = 1ull << 15u;
constexpr uint64_t PageBytes      = 4096;

// A populated MAP_SHARED memfd page, as guest memory is.
struct Page {
	uint8_t* base = nullptr;
	int      fd   = -1;
	Page() {
		fd = static_cast<int>(syscall(__NR_memfd_create, "uffd-test", 0));
		Check(fd >= 0 && ftruncate(fd, static_cast<off_t>(PageBytes)) == 0, "memfd for a test page");
		auto* mapped = mmap(nullptr, PageBytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		Check(mapped != MAP_FAILED, "map the test page");
		base = static_cast<uint8_t*>(mapped);
		std::memset(base, 1, PageBytes);
	}
	~Page() {
		munmap(base, PageBytes);
		close(fd);
	}
	Page(const Page&)            = delete;
	Page& operator=(const Page&) = delete;
	[[nodiscard]] uint64_t Address() const { return reinterpret_cast<uint64_t>(base); }
};

// The self-test's kernel-mode write, done again here with the public primitives: a read() from a
// pipe into a registered, protected page, then a scan.
struct KernelWrite {
	bool write_ok = false;
	bool reported = false;
};

KernelWrite ReadIntoProtectedPage() {
	KernelWrite result;
	Page        page;
	Check(Common::UffdWriteWatch::RegisterAsync(page.Address(), PageBytes), "RegisterAsync on a memfd page");
	Check(Common::UffdWriteWatch::WriteProtectAsync(page.Address(), PageBytes, true), "WriteProtectAsync(protect)");
	int pipe_fds[2] = {-1, -1};
	Check(pipe2(pipe_fds, O_CLOEXEC) == 0, "pipe");
	const char byte = 'k';
	Check(write(pipe_fds[1], &byte, 1) == 1, "write to the pipe");
	result.write_ok = read(pipe_fds[0], page.base, 1) == 1;
	close(pipe_fds[0]);
	close(pipe_fds[1]);
	std::vector<Common::UffdWriteWatch::WrittenRun> runs;
	int                                             error = 0;
	Check(Common::UffdWriteWatch::ScanWritten(page.Address(), PageBytes, true, runs, &error), "ScanWritten after a read()");
	result.reported = runs.size() == 1 && runs[0].start == page.Address() && runs[0].end == page.Address() + PageBytes;
	return result;
}

// The primitives on a page of their own: a write is reported exactly once per protection, a second
// write after the re-protecting scan is reported again, and a scan over an unregistered page fails
// with EPERM (PM_SCAN_CHECK_WPASYNC), which the address space uses to fall back per region.
void ExpectPrimitives() {
	using namespace Common::UffdWriteWatch;
	std::vector<WrittenRun> runs;
	int                     error = 0;
	{
		Page page;
		Check(!ScanWritten(page.Address(), PageBytes, true, runs, &error) && error == EPERM,
		      "ScanWritten over an unregistered page fails with EPERM");
		Check(RegisterAsync(page.Address(), PageBytes), "RegisterAsync");
		// PAGE_IS_WRITTEN is "no write-protect bit": a registered page nobody protected yet reports as
		// written, and the re-protecting scan is what protects it (the tracker protects before it
		// relies on a report, and filters reports against the pages it watches).
		Check(ScanWritten(page.Address(), PageBytes, true, runs, &error) && runs.size() == 1,
		      "a registered page nobody protected reports as written");
		runs.clear();
		Check(ScanWritten(page.Address(), PageBytes, true, runs, &error) && runs.empty(),
		      "... and that scan protected it");
		Check(WriteProtectAsync(page.Address(), PageBytes, true), "WriteProtectAsync(protect)");
		volatile uint8_t* data = page.base;
		data[0]                = 2;
		data[PageBytes - 1]    = 3;
		runs.clear();
		Check(ScanWritten(page.Address(), PageBytes, true, runs, &error) && runs.size() == 1 &&
		          runs[0].start == page.Address() && runs[0].end == page.Address() + PageBytes &&
		          (runs[0].categories & PageIsWritten) != 0,
		      "two writes to one protected page are reported as that one page");
		runs.clear();
		Check(ScanWritten(page.Address(), PageBytes, true, runs, &error) && runs.empty(),
		      "the re-protecting scan leaves nothing to report");
		data[7] = 4;
		runs.clear();
		Check(ScanWritten(page.Address(), PageBytes, false, runs, &error) && runs.size() == 1,
		      "a write after the re-protection is reported");
		runs.clear();
		Check(ScanWritten(page.Address(), PageBytes, false, runs, &error) && runs.size() == 1,
		      "a scan without re-protection leaves the page reported");
		Check(WriteProtectAsync(page.Address(), PageBytes, false), "WriteProtectAsync(release)");
	}
	const auto stats = GetStats();
	Check(stats.async_registers >= 2 && stats.async_protects >= 3 && stats.scans >= 6 && stats.scan_pages >= 3 &&
	          stats.scan_failures >= 1,
	      "the asynchronous counters count registrations, protections, scans, reported pages and failures");
}

// What Get() printed: stdout goes to a memfd while the probe runs again.
std::string ProbeOutput() {
	std::fflush(stdout);
	const int saved = dup(STDOUT_FILENO);
	const int sink  = static_cast<int>(syscall(__NR_memfd_create, "probe-output", 0));
	Check(saved >= 0 && sink >= 0 && dup2(sink, STDOUT_FILENO) == STDOUT_FILENO, "capture stdout");
	Common::UffdWriteWatch::Testing::Reprobe();
	(void)Get();
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
}

bool Contains(const std::string& text, const char* part) {
	return text.find(part) != std::string::npos;
}

// The fallback modes still report a write: the synchronous mode with its SIGBUS and
// UFFDIO_WRITEPROTECT, mprotect with its SIGSEGV. One handler, which releases the page and counts.
volatile sig_atomic_t g_handler_runs = 0;
uint8_t*              g_handler_page = nullptr;
bool                  g_handler_uffd = false;

void OnWriteFault(int /*signal*/, siginfo_t* info, void* /*context*/) {
	if (info->si_addr < g_handler_page || info->si_addr >= g_handler_page + PageBytes) {
		std::abort(); // not ours
	}
	const auto address = reinterpret_cast<uint64_t>(g_handler_page);
	const bool released = g_handler_uffd ? Common::UffdWriteWatch::WriteProtect(address, PageBytes, false)
	                                     : mprotect(g_handler_page, PageBytes, PROT_READ | PROT_WRITE) == 0;
	if (!released) {
		std::abort();
	}
	g_handler_runs = g_handler_runs + 1;
}

void ExpectModeReportsWrite(Common::UffdWriteWatch::Mode mode) {
	using namespace Common::UffdWriteWatch;
	Check(mode != Mode::Async, "the asynchronous mode reports through scans, checked elsewhere");
	Page page;
	g_handler_page = page.base;
	g_handler_uffd = mode == Mode::Sync;
	g_handler_runs = 0;
	struct sigaction action {}, previous {};
	action.sa_sigaction = OnWriteFault;
	action.sa_flags     = SA_SIGINFO | SA_NODEFER;
	sigemptyset(&action.sa_mask);
	const int signal_number = g_handler_uffd ? SIGBUS : SIGSEGV;
	Check(sigaction(signal_number, &action, &previous) == 0, "install the write-fault handler");
	if (g_handler_uffd) {
		Check(Register(page.Address(), PageBytes) && WriteProtect(page.Address(), PageBytes, true),
		      "synchronous mode: register and protect the page");
	} else {
		Check(mprotect(page.base, PageBytes, PROT_READ) == 0, "mprotect: protect the page");
	}
	volatile uint8_t* data = page.base;
	data[0]                = 5; // faults once; the handler releases the page
	data[1]                = 6; // no second fault
	Check(g_handler_runs == 1 && page.base[0] == 5 && page.base[1] == 6, "the fallback mode reported the write once");
	Check(sigaction(signal_number, &previous, nullptr) == 0, "restore the handler");
	g_handler_page = nullptr;
}

// The mode chain: asynchronous, then synchronous, then mprotect, with the reason a requested mode
// was not taken in the startup line. Features are hidden from the probe to play a kernel without
// them.
void ExpectModeChain(const Kernel& kernel) {
	using namespace Common::UffdWriteWatch;
	using Common::UffdWriteWatch::Testing::HideFeatures;
	const bool async_offered = kernel.descriptor && (kernel.features & FeatureWpAsync) != 0 &&
	                           (kernel.features & FeatureWpShmem) != 0 && kernel.pagemap;
	const bool sync_offered = kernel.descriptor && (kernel.features & FeatureSigbus) != 0 &&
	                          (kernel.features & FeatureWpShmem) != 0;

	unsetenv("KYTY_UFFD_WP");
	unsetenv("KYTY_UFFD_WP_ASYNC");
	HideFeatures(0);
	auto output = ProbeOutput();
	Check(TrackingMode() == Mode::Mprotect && std::strcmp(TrackingModeName(), "mprotect") == 0 &&
	          TrackingModeReason()[0] == '\0',
	      "no setting: mprotect, no reason");
	Check(Contains(output, "Kyty platform: guest write tracking: mprotect\n"), "no setting: the line names mprotect");
	ExpectModeReportsWrite(Mode::Mprotect);

	setenv("KYTY_UFFD_WP_ASYNC", "1", 1);
	output = ProbeOutput();
	if (async_offered) {
		Check(TrackingMode() == Mode::Async && TrackingModeReason()[0] == '\0', "asynchronous mode taken when offered");
		Check(Contains(output, "guest write tracking: asynchronous userfaultfd write-protection (KYTY_UFFD_WP_ASYNC=1)\n"),
		      "the line names the asynchronous mode");
	}

	// A kernel without WP_ASYNC: KYTY_UFFD_WP_ASYNC=1 is not taken, with the missing feature named.
	HideFeatures(FeatureWpAsync);
	output = ProbeOutput();
	Check(TrackingMode() == Mode::Mprotect, "without WP_ASYNC and without KYTY_UFFD_WP: mprotect");
	Check(Contains(TrackingModeReason(), "KYTY_UFFD_WP_ASYNC=1 not taken: ") &&
	          Contains(TrackingModeReason(), kernel.descriptor ? "UFFD_FEATURE_WP_ASYNC" : "could not be opened"),
	      "the reason names the missing feature");
	Check(Contains(output, "guest write tracking: mprotect; KYTY_UFFD_WP_ASYNC=1 not taken: "),
	      "the line names the fallback and the reason");
	ExpectModeReportsWrite(Mode::Mprotect);

	// ... and falls back to the synchronous mode when that is requested too.
	setenv("KYTY_UFFD_WP", "1", 1);
	output = ProbeOutput();
	if (sync_offered) {
		Check(TrackingMode() == Mode::Sync, "without WP_ASYNC, with KYTY_UFFD_WP=1: synchronous mode");
		Check(Contains(output, "guest write tracking: userfaultfd write-protection (KYTY_UFFD_WP=1); "
		                       "KYTY_UFFD_WP_ASYNC=1 not taken: "),
		      "the line names the synchronous fallback and the reason");
		ExpectModeReportsWrite(Mode::Sync);
	} else {
		Check(TrackingMode() == Mode::Mprotect, "neither offered: mprotect");
	}

	// A kernel without SIGBUS: the synchronous mode is not taken either; mprotect, both reasons.
	HideFeatures(FeatureWpAsync | FeatureSigbus);
	output = ProbeOutput();
	Check(TrackingMode() == Mode::Mprotect, "without WP_ASYNC and SIGBUS: mprotect");
	Check(Contains(TrackingModeReason(), "KYTY_UFFD_WP_ASYNC=1 not taken: ") &&
	          Contains(TrackingModeReason(), "KYTY_UFFD_WP=1 not taken: "),
	      "both requested modes explained");
	Check(Contains(output, "guest write tracking: mprotect; KYTY_UFFD_WP_ASYNC=1 not taken: ") &&
	          Contains(output, "; KYTY_UFFD_WP=1 not taken: "),
	      "the line names mprotect and both reasons");
	ExpectModeReportsWrite(Mode::Mprotect);

	HideFeatures(0);
	unsetenv("KYTY_UFFD_WP");
	unsetenv("KYTY_UFFD_WP_ASYNC");
	Common::UffdWriteWatch::Testing::Reprobe();
}

void ExpectSelfTest(const Support& s) {
	const auto& t = s.selftest;
	Check(t.ran, "the self-test ran with the descriptor open");
	Check(t.failed_step == nullptr && t.error == 0, "no self-test step failed");
	Check(t.reported, "self-test: the write was reported as one page");
	Check(t.reprotected, "self-test: the scan protected the page again");
	Check(t.rearmed, "self-test: a second write was reported again");
	Check(t.huge_absent, "self-test: the page is not a huge page");
	Check(t.ok, "self-test ok");
	Check(t.scan_us_per_2mib > 0, "self-test: the scan microbenchmark ran");
	const auto mine = ReadIntoProtectedPage();
	Check(t.kernel_write == mine.write_ok && t.kernel_reported == mine.reported,
	      "self-test: the read() into a protected page matches an independent read()");
	Check(t.kernel_write || t.kernel_error != 0, "self-test: a failed read() reports its errno");
	std::printf("[host]    self-test: ok, read() into a protected page %s%s, scan of 2 MiB %.1f us\n",
	            t.kernel_write ? "succeeded" : "failed", t.kernel_reported ? " and was reported" : "", t.scan_us_per_2mib);
}

void ExpectNotRequested(const Support& s, const char* what) {
	Check(s.probed, what);
	Check(!s.requested && !s.async_requested, what);
	Check(!s.shared && !s.async, what);
	Check(s.fd == -1 && s.async_fd == -1 && s.pagemap_fd == -1, what);
	Check(!Common::UffdWriteWatch::Enabled() && !Common::UffdWriteWatch::AsyncEnabled(), what);
}

// The asynchronous probe against the kernel: open when the kernel offers WP_ASYNC and shmem
// write-protection to a descriptor of this process, unavailable with a reason otherwise.
void ExpectAsyncMatchesKernel(const Support& s, const Kernel& kernel) {
	Check(s.async_requested, "asynchronous tracking requested");
	Check(s.features == kernel.features, "the reported features are the kernel's");
	const bool offered = kernel.descriptor && (kernel.features & FeatureWpAsync) != 0 &&
	                     (kernel.features & FeatureWpShmem) != 0 && kernel.pagemap;
	if (offered) {
		Check(s.async, "asynchronous descriptor open when the kernel offers WP_ASYNC and shmem write-protection");
		Check(Open(s.async_fd) && Open(s.pagemap_fd), "the asynchronous descriptor and the pagemap are open");
		Check(s.async_user_mode == kernel.user_mode, "user-mode-only descriptor as the kernel allows");
		Check(s.async_error == 0 && s.async_reason == nullptr, "no error reported for an open descriptor");
		ExpectSelfTest(s);
		Check(Common::UffdWriteWatch::AsyncEnabled(), "AsyncEnabled with the descriptor open and the self-test passed");
		ExpectPrimitives();
		// The descriptor took the asynchronous features: a second UFFDIO_API is refused (EINVAL, the
		// kernel's cmpxchg on the context's features; EPERM on older kernels).
		uffdio_api again {};
		again.api = UFFD_API;
		Check(ioctl(s.async_fd, UFFDIO_API, &again) != 0 && (errno == EINVAL || errno == EPERM),
		      "the asynchronous descriptor has its API fixed");
	} else {
		Check(!s.async, "asynchronous descriptor unavailable when the kernel does not offer it");
		Check(s.async_fd == -1 && s.pagemap_fd == -1, "no descriptors left open");
		Check(s.async_reason != nullptr && s.async_reason[0] != '\0', "a reason is reported");
		Check(!s.selftest.ran, "no self-test without a descriptor");
		Check(!Common::UffdWriteWatch::AsyncEnabled(), "AsyncEnabled false when unavailable");
		std::vector<Common::UffdWriteWatch::WrittenRun> runs;
		int                                             error = 0;
		Check(!Common::UffdWriteWatch::ScanWritten(0x1000, PageBytes, false, runs, &error) && error == EBADF,
		      "ScanWritten without a pagemap fails with EBADF");
	}
	std::printf("[host]    async probe: %s (kernel features 0x%llx, %s)\n", s.async ? "available" : "unavailable",
	            static_cast<unsigned long long>(s.features), s.async ? "" : s.async_reason);
}

} // namespace

int main() {
	const auto kernel = ProbeKernel();
	std::printf("[host]    kernel: descriptor %s, user-mode-only %s, features 0x%llx, pagemap %s\n",
	            kernel.descriptor ? "yes" : "no", kernel.user_mode ? "yes" : "no",
	            static_cast<unsigned long long>(kernel.features), kernel.pagemap ? "yes" : "no");

	// Neither setting: nothing probed, nothing open, both modes off.
	unsetenv("KYTY_UFFD_WP");
	unsetenv("KYTY_UFFD_WP_ASYNC");
	Common::UffdWriteWatch::Testing::Reprobe();
	ExpectNotRequested(Get(), "no setting: not requested, nothing open");
	std::printf("[host]    %-48s ok\n", "NotRequested");

	// Only exactly "1" requests the asynchronous mode.
	for (const char* value: {"0", "", "true", "yes", "2"}) {
		setenv("KYTY_UFFD_WP_ASYNC", value, 1);
		Common::UffdWriteWatch::Testing::Reprobe();
		ExpectNotRequested(Get(), "KYTY_UFFD_WP_ASYNC other than 1 leaves tracking as it is");
	}
	std::printf("[host]    %-48s ok\n", "AsyncRequestedOnlyByOne");

	// KYTY_UFFD_WP_ASYNC=1 alone: the asynchronous probe, the synchronous one untouched.
	setenv("KYTY_UFFD_WP_ASYNC", "1", 1);
	Common::UffdWriteWatch::Testing::Reprobe();
	{
		const auto& s = Get();
		ExpectAsyncMatchesKernel(s, kernel);
		Check(!s.requested && !s.shared && s.fd == -1 && !Common::UffdWriteWatch::Enabled(),
		      "KYTY_UFFD_WP_ASYNC does not request the synchronous mode");
	}
	std::printf("[host]    %-48s ok\n", "AsyncProbeMatchesKernel");

	// Both: two descriptors, each with its own features.
	setenv("KYTY_UFFD_WP", "1", 1);
	Common::UffdWriteWatch::Testing::Reprobe();
	{
		const auto& s = Get();
		ExpectAsyncMatchesKernel(s, kernel);
		const bool sync_offered = kernel.descriptor && (kernel.features & FeatureSigbus) != 0 &&
		                          (kernel.features & FeatureWpShmem) != 0;
		Check(s.requested, "synchronous tracking requested");
		Check(s.shared == sync_offered && Open(s.fd) == sync_offered, "the synchronous probe matches the kernel");
		Check(!s.async || s.fd != s.async_fd, "the two modes have descriptors of their own");
	}
	std::printf("[host]    %-48s ok\n", "BothProbes");

	// The mode chain and its fallbacks (before the forced case: forcing cannot be undone).
	ExpectModeChain(kernel);
	std::printf("[host]    %-48s ok\n", "ModeChainAndFallbacks");

	// Forced on without the setting (what other tests use).
	unsetenv("KYTY_UFFD_WP");
	unsetenv("KYTY_UFFD_WP_ASYNC");
	Common::UffdWriteWatch::Testing::ForceEnableAsync();
	{
		const auto& s = Get();
		ExpectAsyncMatchesKernel(s, kernel);
		Check(!s.requested, "forcing the asynchronous mode does not request the synchronous one");
	}
	std::printf("[host]    %-48s ok\n", "ForceEnableAsync");

	std::printf("UffdWriteWatchTests: all cases passed\n");
	return 0;
}

#else

int main() {
	std::printf("UffdWriteWatchTests: Linux only, nothing to test\n");
	return 0;
}

#endif
