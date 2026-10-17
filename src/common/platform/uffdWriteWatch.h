#ifndef EMULATOR_SRC_COMMON_PLATFORM_UFFDWRITEWATCH_H_
#define EMULATOR_SRC_COMMON_PLATFORM_UFFDWRITEWATCH_H_

#include <cstdint>
#include <vector>

// Guest write tracking with userfaultfd write-protection (Linux), KYTY_UFFD_WP.
//
// The GPU caches write-protect the guest pages they mirror. On Linux that used mprotect, and every
// mprotect call takes the process's mmap lock for writing and splits or merges VMAs: each page
// fault of every thread waits behind it, and the tracking faults themselves need one more mprotect
// to resolve. At Astro Bot's ~1,300 tracking faults and ~900 protection calls per frame that convoy
// made one fault cost ~80 us on a Linux PC (Windows: ~5 us).
//
// With userfaultfd write-protection (UFFDIO_REGISTER_MODE_WP) the protection is a bit in each page
// table entry: UFFDIO_WRITEPROTECT takes the mmap lock only for reading and changes no VMA, and a
// write to a protected page raises SIGBUS in the writing thread (UFFD_FEATURE_SIGBUS), which the
// tracker resolves exactly like the SIGSEGV of a read-only page (the error code says "write").
// No-access protection (GPU-owned pages) still uses mprotect.
//
// Measured in WSL2 (15 writer threads, 4,800 tracked pages written per frame): a tracking fault
// costs 1.4-3 us instead of 26-97 us with mprotect, but on memfd views every first write after a
// release also takes a minor (write-notify) fault, ~4,700 per frame, so the total (6-7 ms of fault
// time per frame) loses to mprotect with the larger fault-ahead windows (0.9-2 ms).
// KYTY_UFFD_WP=1 turns it on (default off until it has run in the game on Linux; 0 = mprotect).
// Requirements, probed once at the first use and logged ("Kyty platform: userfaultfd ..."):
//  - the userfaultfd syscall. When vm.unprivileged_userfaultfd is 0, an unprivileged process gets
//    a user-mode-only descriptor (UFFD_USER_MODE_ONLY, Linux 5.11+). Kernel-mode writes to a
//    protected page (a read() into it) then fail with EFAULT, exactly as with mprotect;
//  - SIGBUS mode (UFFD_FEATURE_SIGBUS, Linux 4.14+);
//  - write-protection of shared memory (UFFD_FEATURE_WP_HUGETLBFS_SHMEM, Linux 5.19+): guest direct
//    memory is a memfd mapped MAP_SHARED;
//  - for private (anonymous) guest memory also UFFD_FEATURE_WP_UNPOPULATED (Linux 6.4+), without
//    which a page never touched would not fault; such mappings keep mprotect otherwise.
// Every failure falls back to mprotect (logged once).
//
// Asynchronous write-protection (KYTY_UFFD_WP_ASYNC=1, Linux 6.7+): on a descriptor with
// UFFD_FEATURE_WP_ASYNC the kernel resolves a write to a protected page itself, without a signal,
// and clears the page's protection; which pages were written is read back, and their protection
// restored in the same call, with the PAGEMAP_SCAN ioctl on /proc/self/pagemap
// (PM_SCAN_WP_MATCHING, PAGE_IS_WRITTEN). Features are fixed per descriptor, so this mode has a
// descriptor of its own, probed next to the synchronous one (user-mode-only first; WP_ASYNC implies
// WP_UNPOPULATED, so private memory is covered too). The probe only establishes what the kernel
// offers; which mode tracks guest memory is decided by the address space.
namespace Common::UffdWriteWatch {

// The asynchronous mode's startup self-test, on a one-page memfd of its own: a write to a protected
// page must be reported by PAGEMAP_SCAN and the page protected again by that scan; a second write
// must be reported again. A kernel-mode write (read() into the page) is recorded, not required.
struct AsyncSelfTest {
	bool        ran = false; // the descriptor was open and the self-test ran
	bool        ok  = false; // reported, reprotected, rearmed and huge_absent, no step failed
	const char* failed_step = nullptr; // the first step that failed (a static string), if any
	int         error       = 0;       // its errno
	bool        reported = false; // the write was reported: one page, the written one, nothing else
	bool        reprotected  = false; // the scan protected it again: a second scan reports nothing
	bool        rearmed      = false; // a second write is reported again
	bool        huge_absent  = false; // the page is not part of a huge page (PAGE_IS_HUGE)
	bool        kernel_write = false; // a read() into the protected page succeeded ...
	bool        kernel_reported = false; // ... and was reported written
	int         kernel_error    = 0;     // errno of that read(), if it failed
	double scan_us_per_2mib = 0; // one PAGEMAP_SCAN over 2 MiB with 64 written pages (best of 8)
};

struct Support {
	bool     probed        = false;
	bool     requested     = false; // KYTY_UFFD_WP=1
	bool     shared        = false; // shared-memory mappings can be tracked
	bool     private_anon  = false; // private anonymous mappings can be tracked
	bool     user_mode     = false; // the descriptor is user-mode only
	uint64_t features      = 0;     // what the kernel offers (UFFDIO_API)
	int      error         = 0;     // errno of the failed step, if any
	int      fd            = -1;
	// Asynchronous write-protection (KYTY_UFFD_WP_ASYNC=1).
	bool          async_requested = false;   // KYTY_UFFD_WP_ASYNC=1 (exactly)
	bool          async           = false;   // the asynchronous descriptor and the pagemap are open
	bool          async_user_mode = false;   // that descriptor is user-mode only
	int           async_error     = 0;       // errno of the failed step, if any
	const char*   async_reason    = nullptr; // why it is unavailable (a static string), if so
	int           async_fd        = -1;
	int           pagemap_fd      = -1; // /proc/self/pagemap, for PAGEMAP_SCAN
	AsyncSelfTest selftest;             // run when the descriptor is open
};

// Probes once (thread-safe) and logs the result when KYTY_UFFD_WP=1 or KYTY_UFFD_WP_ASYNC=1.
[[nodiscard]] const Support& Get();
// KYTY_UFFD_WP=1 and at least shared-memory tracking works.
[[nodiscard]] bool Enabled();
// KYTY_UFFD_WP_ASYNC=1, the asynchronous descriptor and the pagemap are open, and the self-test
// passed.
[[nodiscard]] bool AsyncEnabled();

// Which mode tracks guest writes on this PC: the first available of asynchronous write-protection
// (KYTY_UFFD_WP_ASYNC=1), synchronous write-protection (KYTY_UFFD_WP=1) and mprotect (Linux; always
// Mprotect elsewhere). Decided with the probe and logged once ("Kyty platform: guest write
// tracking: ..."), with why a requested mode was not taken.
enum class Mode { Mprotect, Sync, Async };
[[nodiscard]] Mode        TrackingMode();
[[nodiscard]] const char* TrackingModeName();   // "mprotect", "userfaultfd write-protection", ...
[[nodiscard]] const char* TrackingModeReason(); // why a requested mode was not taken; "" when none

// The asynchronous mode (AsyncEnabled()).
// UFFDIO_REGISTER with UFFDIO_REGISTER_MODE_WP on the asynchronous descriptor over
// [vaddr, vaddr + size) (whole mappings). Pages are protected afterwards with WriteProtectAsync.
[[nodiscard]] bool RegisterAsync(uint64_t vaddr, uint64_t size);
// UFFDIO_WRITEPROTECT on the asynchronous descriptor: protect (true) or release (false). Retries
// EAGAIN.
[[nodiscard]] bool WriteProtectAsync(uint64_t vaddr, uint64_t size, bool protect);
// Page categories of a WrittenRun (linux/fs.h PAGE_IS_*, spelled out: older headers lack them).
constexpr uint64_t PageIsWritten = 1ull << 1u;
constexpr uint64_t PageIsHuge    = 1ull << 6u;
struct WrittenRun {
	uint64_t start      = 0; // [start, end): pages written since they were last protected
	uint64_t end        = 0;
	uint64_t categories = 0;
};
// One PAGEMAP_SCAN over [vaddr, vaddr + size): every run of pages written since they were last
// protected is appended to `written`, and protected again in the same call when `reprotect`. False
// (errno in `error`) when a page of the range is not registered for asynchronous write-protection
// (PM_SCAN_CHECK_WPASYNC: EPERM) or the call fails; runs appended before the failure stay.
// "Written" is the kernel's "no write-protect bit": a registered page that was never protected
// reports as written too, so the caller protects a page before it relies on reports about it, and
// filters reports against the pages it watches.
[[nodiscard]] bool ScanWritten(uint64_t vaddr, uint64_t size, bool reprotect,
                               std::vector<WrittenRun>& written, int* error = nullptr);

// UFFDIO_REGISTER with UFFDIO_REGISTER_MODE_WP over [vaddr, vaddr + size) (whole mappings).
[[nodiscard]] bool Register(uint64_t vaddr, uint64_t size);
// UFFDIO_WRITEPROTECT: protect (true) or release (false) [vaddr, vaddr + size). Retries EAGAIN.
// `quiet`: a failure is expected to be retried in parts (not logged or counted).
[[nodiscard]] bool WriteProtect(uint64_t vaddr, uint64_t size, bool protect, bool quiet = false);

// Counters for logs and tests.
struct Stats {
	uint64_t registers       = 0;
	uint64_t protects        = 0;
	uint64_t releases        = 0;
	uint64_t failures        = 0;
	// The asynchronous mode.
	uint64_t async_registers = 0;
	uint64_t async_protects  = 0; // UFFDIO_WRITEPROTECT calls, protecting or releasing
	uint64_t scans           = 0; // PAGEMAP_SCAN calls (one ScanWritten may make several)
	uint64_t scan_pages      = 0; // pages they reported written
	uint64_t scan_failures   = 0;
};
[[nodiscard]] Stats GetStats();

namespace Testing {
// Probes again with KYTY_UFFD_WP forced on (tests).
void ForceEnable();
// Probes again with KYTY_UFFD_WP_ASYNC forced on (tests).
void ForceEnableAsync();
// Probes again on the next Get(), reading the environment again (tests). The descriptors of the
// previous probe are closed, so nothing registered with them may be in use.
void Reprobe();
// The next probes see the kernel's features without these bits (tests: a kernel that lacks them).
// 0 restores them. Takes effect with Reprobe().
void HideFeatures(uint64_t features);
} // namespace Testing

} // namespace Common::UffdWriteWatch

#endif // EMULATOR_SRC_COMMON_PLATFORM_UFFDWRITEWATCH_H_
