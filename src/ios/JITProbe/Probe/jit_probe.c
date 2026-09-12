// SPDX-FileCopyrightText: Copyright 2026 eden-ios contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// See jit_probe.h for why this file exists and why it is allowed to do the one thing
// src/ios/Bridge/EdenJIT.m refuses to do.

#include "jit_probe.h"

#include <CoreFoundation/CoreFoundation.h>

#include <arpa/inet.h>              // ntohl - the entitlement blob header is big-endian
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <libkern/OSCacheControl.h> // sys_icache_invalidate
#include <setjmp.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#if __has_include(<sys/proc.h>)
#  include <sys/proc.h>          // struct kinfo_proc, P_TRACED
#  define JP_HAVE_PROC_H 1
#else
#  define JP_HAVE_PROC_H 0
#endif
#include <sys/syscall.h>            // SYS_csops
#include <sys/sysctl.h>
#include <sys/types.h>
#include <unistd.h>

#if __has_include(<mach/mach.h>) && __has_include(<mach/vm_region.h>)
#  include <mach/mach.h>
#  include <mach/vm_region.h>
#  define JP_HAVE_VM_REGION 1
#else
#  define JP_HAVE_VM_REGION 0
#endif

// ===========================================================================
// THE CODE WE WRITE, AND HOW IT IS ENCODED
// ===========================================================================
// Two functions, each two instructions, each returning a different constant. One
// would be enough to test "did it run"; two is what distinguishes "our code ran" from
// "something ran, or w0 happened to already hold 42". If a page of stale memory or a
// bare RET returned a lucky 42, it will not also return 90 from eight bytes further
// on.
//
// MOVZ Wd, #imm16, LSL #0   (this is what "mov w0, #N" assembles to for small N)
//
//   31 | 30 29 | 28 ... 23 | 22 21 | 20 ......... 5 | 4 ... 0
//   sf |  opc  |  1 0 0 1 0 1 |  hw  |     imm16      |   Rd
//
//   sf    = 0        32-bit destination, so W0 rather than X0
//   opc   = 10       MOVZ (00 = MOVN, 10 = MOVZ, 11 = MOVK)
//   hw    = 00       shift the immediate left by 0
//   imm16 = the constant
//   Rd    = 00000    W0, which is also the AAPCS64 return register
//
//   mov w0, #42:  0 10 100101 00 0000000000101010 00000
//                 = 0101 0010 1000 0000 0000 0101 0100 0000 = 0x52800540
//   mov w0, #90:  0 10 100101 00 0000000001011010 00000
//                 = 0101 0010 1000 0000 0000 1011 0100 0000 = 0x52800B40
//
// (The task brief quotes 0x528005400 for the first of these. That is nine hex digits,
// i.e. 36 bits; the correct 32-bit encoding is 0x52800540. Derivation above.)
//
//   RET Xn, with Xn = X30, the link register:
//   1101011 0 010 11111 000000 11110 00000
//                 = 1101 0110 0101 1111 0000 0011 1100 0000 = 0xD65F03C0
//
// Stored through a uint32_t*, so each word lands little-endian, which is the byte
// order an arm64 instruction fetch expects. No byte swapping is correct here.
//
// The functions take no arguments and clobber only w0, so they need no prologue,
// no frame, and no stack. Nothing else in the process can be disturbed by them.
// ===========================================================================

static const uint32_t JP_MOV_W0_42 = 0x52800540u;
static const uint32_t JP_MOV_W0_90 = 0x52800B40u;
static const uint32_t JP_RET       = 0xD65F03C0u;

/// Byte offset of the second function inside the page.
#define JP_SECOND_FN_OFFSET 8
#define JP_EXPECT_FIRST     42
#define JP_EXPECT_SECOND    90

// ===========================================================================
// csops
//
// Declared in the kernel's <sys/codesign.h>, which is not in the iOS SDK. The symbol
// IS exported by libsystem, so dlsym finds it; syscall(SYS_csops) is the fallback for
// an OS that stops exporting it. src/ios/Bridge/EdenJIT.m uses the syscall form only -
// this reports which one answered, so a future "csops returned nothing" is
// attributable.
// ===========================================================================

#define JP_CS_OPS_STATUS            0
#define JP_CS_OPS_ENTITLEMENTS_BLOB 7

#define JP_CS_VALID              0x00000001u
#define JP_CS_ADHOC              0x00000002u
#define JP_CS_GET_TASK_ALLOW     0x00000004u
#define JP_CS_INSTALLER          0x00000008u
#define JP_CS_HARD               0x00000100u
#define JP_CS_KILL               0x00000200u
#define JP_CS_CHECK_EXPIRATION   0x00000400u
#define JP_CS_RESTRICT           0x00000800u
#define JP_CS_ENFORCEMENT        0x00001000u
#define JP_CS_REQUIRE_LV         0x00002000u
#define JP_CS_ENTITLEMENTS_VALID 0x00004000u
#define JP_CS_RUNTIME            0x00010000u
#define JP_CS_LINKER_SIGNED      0x00020000u
#define JP_CS_KILLED             0x01000000u
#define JP_CS_DYLD_PLATFORM      0x02000000u
#define JP_CS_PLATFORM_BINARY    0x04000000u
#define JP_CS_PLATFORM_PATH      0x08000000u
#define JP_CS_DEBUGGED           0x10000000u   // <- the one that matters
#define JP_CS_SIGNED             0x20000000u
#define JP_CS_DEV_CODE           0x40000000u

/// <sys/proc.h>'s P_TRACED, restated so this file needs no kernel header.
#define JP_P_TRACED              0x00000800

typedef int (*jp_csops_fn)(pid_t, unsigned int, void *, size_t);

static bool g_csops_via_dlsym = false;

static int jp_csops(pid_t pid, unsigned int ops, void *addr, size_t size) {
    static jp_csops_fn fn = NULL;
    static bool resolved = false;
    if (!resolved) {
        fn = (jp_csops_fn)dlsym(RTLD_DEFAULT, "csops");
        g_csops_via_dlsym = (fn != NULL);
        resolved = true;
    }
    if (fn != NULL) {
        return fn(pid, ops, addr, size);
    }
    // syscall() is __API_DEPRECATED on iOS. It is the fallback only - reached when
    // libsystem stops exporting csops by name - and a deprecation warning on a line
    // that may never execute is not worth a noisy build.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    return (int)syscall(SYS_csops, pid, ops, addr, size);
#pragma clang diagnostic pop
}

// ===========================================================================
// state
// ===========================================================================

static jp_report g_report;
static bool      g_began = false;
static char      g_state_dir[1024];
static char      g_os_hint[64];
static char      g_text[16384];

// Out of jp_report on purpose - see the note on the struct in jit_probe.h. Swift
// imports a fixed-size char array member as a tuple with one element per byte.
static char g_cs_flag_names[640];
static char g_ent_xml[3072];
static char g_hw_machine[64];
static char g_hw_model[64];
static char g_os_version[64];
static char g_os_build[64];

#define JP_JOURNAL_NAME "jit-probe-journal.txt"
#define JP_RESULTS_NAME "jit-probe-results.txt"

// ===========================================================================
// strategy table
//
// Order is deliberate. Strategy 0 is the shape Eden's patched oaknut actually uses,
// so it is attempted first: if a jump is going to kill this process, the answer that
// matters most should already be on disk when it does.
// ===========================================================================

static const char *const JP_NAMES[JP_STRATEGY_COUNT] = {
    "anonymous RW, then mprotect to RX",
    "anonymous RX at map time, RW to write, back to RX",
    "MAP_JIT, RWX at map time",
    "MAP_JIT read-write, then mprotect to RX",
    "anonymous RWX at map time (no MAP_JIT), then mprotect down to RX",
};

static const char *const JP_DETAILS[JP_STRATEGY_COUNT] = {
    "Eden's ORIGINAL design for this (superseded 2026-09-12, kept here for "
    "comparison): oaknut::CodeBlock mapped memory without EXECUTE from the start "
    "and asked mprotect() to add it later. A real device showed that fails - see "
    "strategy 4, which replaced it.",

    "The same memory, mapped executable up front instead of promoted. This is the "
    "exact shape src/ios/Bridge/EdenJIT.m probes (mmap PROT_READ|PROT_EXEC, mprotect "
    "to RW, write, mprotect back). Included so that probe's syscall-only \"pass\" can "
    "be compared against whether the page actually runs.",

    "The macOS shape. docs/IOS_PORT_NOTES.md says MAP_JIT cannot be used on iOS "
    "because its partner pthread_jit_write_protect_np is absent from the SDK; "
    "cemu-ios-muffin observed this exact mmap returning EINVAL on an A12Z with "
    "CS_DEBUGGED set. Tried because being able to write \"refused, errno 22, on this "
    "device too\" is worth more than repeating the claim.",

    "MAP_JIT without asking for PROT_EXEC at map time. cemu-ios-muffin found THIS "
    "succeeded on the device where the one above failed. It never executed the page, "
    "so what a jump into it does is unknown to both projects. This probe finds out.",

    "Added after a real device showed strategies 0 and 1 both SIGBUS: mprotect() "
    "does not fail when asked for a bit that was not in the mapping's ORIGINAL "
    "mmap() protection - it silently intersects with that ceiling and returns "
    "success anyway, so a page that was never mapped executable cannot become "
    "executable later no matter what mprotect() claims. This asks for "
    "READ|WRITE|EXECUTE in the one mmap() call, then mprotects DOWN to RX before "
    "running - removing a bit that was already granted, never adding one that "
    "was not. This is the exact shape MeloNX's DualMappedJitAllocator uses after "
    "hitting and fixing the identical bug, confirmed executing real JIT-compiled "
    "code on this device class. THIS IS WHAT EDEN'S OAKNUT PATCH ACTUALLY DOES NOW "
    "(.patch/oaknut/0001-ios-jit-modes.patch, 2026-09-12) - if this row fails, the "
    "patch is wrong and needs another look; if strategies 0/1 fail and this one "
    "passes, the fix is confirmed.",
};

// ===========================================================================
// the crash journal
//
// A one-line file, rewritten and fsync()ed around the only dangerous instruction in
// the process. If the app vanishes, this file is the whole finding.
// ===========================================================================

static void jp_path(char *out, size_t len, const char *name) {
    out[0] = '\0';
    if (g_state_dir[0] == '\0') {
        return;
    }
    snprintf(out, len, "%s/%s", g_state_dir, name);
}

/// Writes `text` as the entire contents of `path`, then fsync()s the file AND its
/// directory. Both matter: a file that only reached the page cache is not on disk
/// when the process stops existing, which is precisely the case being recorded.
static bool jp_write_atomic(const char *path, const char *text) {
    if (path[0] == '\0') {
        return false;
    }
    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return false;
    }
    const size_t len = strlen(text);
    const ssize_t written = write(fd, text, len);
    (void)fsync(fd);
    (void)close(fd);
    if (written != (ssize_t)len) {
        return false;
    }
    const int dirfd = open(g_state_dir, O_RDONLY);
    if (dirfd >= 0) {
        (void)fsync(dirfd);
        (void)close(dirfd);
    }
    return true;
}

static size_t jp_read_file(const char *path, char *buf, size_t len) {
    buf[0] = '\0';
    if (path[0] == '\0') {
        return 0;
    }
    const int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return 0;
    }
    const ssize_t n = read(fd, buf, len - 1);
    (void)close(fd);
    if (n <= 0) {
        buf[0] = '\0';
        return 0;
    }
    buf[n] = '\0';
    return (size_t)n;
}

static void jp_remove(const char *name) {
    char path[1152];
    jp_path(path, sizeof(path), name);
    if (path[0] != '\0') {
        (void)unlink(path);
    }
}

/**
 * Two phases are dangerous, not one, and they have to be told apart.
 *
 * The jump is the obvious one. The WRITE is the other: on an APRR core a MAP_JIT
 * region is handed over executable-and-not-writable, with a per-thread switch deciding
 * which it is, and this probe deliberately never touches that switch (its only control
 * is pthread_jit_write_protect_np, which docs/IOS_PORT_NOTES.md establishes cannot be
 * called from an iOS target at all). So storing into a MAP_JIT page here may itself
 * fault - and a journal that said only "arming 3" would blame that on the jump and
 * report the exact opposite of what happened.
 */
static void jp_journal_arm(int index, const char *phase) {
    char path[1152];
    char line[96];
    jp_path(path, sizeof(path), JP_JOURNAL_NAME);
    snprintf(line, sizeof(line), "arming %d %s\n", index, phase);
    (void)jp_write_atomic(path, line);
}

static void jp_journal_survived(int index, const char *phase) {
    char path[1152];
    char line[96];
    jp_path(path, sizeof(path), JP_JOURNAL_NAME);
    snprintf(line, sizeof(line), "survived %d %s\n", index, phase);
    (void)jp_write_atomic(path, line);
}

// ===========================================================================
// the results file
//
// One line per strategy that has an outcome, so a strategy is never retried after it
// killed the process. Deliberately a flat text file: a tester can read it in Files.app
// and paste it into a message without the app running at all.
// ===========================================================================

static void jp_results_save(void) {
    char path[1152];
    char body[2048];
    size_t used = 0;

    jp_path(path, sizeof(path), JP_RESULTS_NAME);
    body[0] = '\0';
    for (int i = 0; i < JP_STRATEGY_COUNT; ++i) {
        const jp_strategy_result *s = &g_report.strategies[i];
        if (s->outcome == JP_OUTCOME_UNTRIED) {
            continue;
        }
        const int n = snprintf(body + used, sizeof(body) - used,
                               "%d|%d|%d|%d|%d|%d|%s\n",
                               i, (int)s->outcome, s->failed_errno, s->caught_signal,
                               s->returned_first, s->returned_second,
                               s->failed_call ? s->failed_call : "-");
        if (n <= 0 || (size_t)n >= sizeof(body) - used) {
            break;
        }
        used += (size_t)n;
    }
    (void)jp_write_atomic(path, body);
}

/// Static storage for failed_call strings restored from disk; the struct field is a
/// const char* and must not point at a stack buffer.
static char g_failed_call_store[JP_STRATEGY_COUNT][64];

static void jp_results_load(void) {
    char path[1152];
    char body[2048];

    jp_path(path, sizeof(path), JP_RESULTS_NAME);
    if (jp_read_file(path, body, sizeof(body)) == 0) {
        return;
    }

    char *save = NULL;
    for (char *line = strtok_r(body, "\n", &save); line != NULL;
         line = strtok_r(NULL, "\n", &save)) {
        int idx = -1, outcome = 0, err = 0, sig = 0, r1 = -1, r2 = -1;
        char call[64] = {0};
        if (sscanf(line, "%d|%d|%d|%d|%d|%d|%63s",
                   &idx, &outcome, &err, &sig, &r1, &r2, call) < 6) {
            continue;
        }
        if (idx < 0 || idx >= JP_STRATEGY_COUNT) {
            continue;
        }
        if (outcome < 0 || outcome > (int)JP_OUTCOME_PASS) {
            continue;
        }
        jp_strategy_result *s = &g_report.strategies[idx];
        s->outcome = (jp_outcome)outcome;
        s->failed_errno = err;
        s->caught_signal = sig;
        s->returned_first = r1;
        s->returned_second = r2;
        if (call[0] != '\0' && strcmp(call, "-") != 0) {
            strlcpy(g_failed_call_store[idx], call, sizeof(g_failed_call_store[idx]));
            s->failed_call = g_failed_call_store[idx];
        }
    }
}

// ===========================================================================
// signal handling around the jump
//
// Catches the refusals that ARE catchable. cemu-ios-muffin's shipped build took
// "signal 10" (SIGBUS) on its first entry into generated code with CS_DEBUGGED set
// and mprotect(R+X) having returned 0, so this is not a hypothetical path.
//
// The ones that are not catchable - a code-signing SIGKILL - are what the journal
// above is for. Between the two, every refusal produces a report.
// ===========================================================================

// Defined below, after the signal machinery they are wrapped in.
static void jp_write_code(void *page);
static void jp_flush_icache(void *page, size_t len);

static sigjmp_buf                g_jump;
static volatile sig_atomic_t     g_jump_armed = 0;
static volatile sig_atomic_t     g_caught_signal = 0;
static volatile int              g_ret_first = -1;
static volatile int              g_ret_second = -1;

static const int JP_TRAPPED[] = { SIGBUS, SIGSEGV, SIGILL, SIGTRAP };
#define JP_TRAPPED_COUNT ((int)(sizeof(JP_TRAPPED) / sizeof(JP_TRAPPED[0])))

static struct sigaction g_saved[JP_TRAPPED_COUNT];
static char            *g_altstack = NULL;

static void jp_handler(int sig, siginfo_t *info, void *uap) {
    (void)info;
    (void)uap;
    if (!g_jump_armed) {
        // Not ours. Put the default back and return, so the faulting instruction
        // re-executes and produces a genuine, symbolicatable crash rather than a
        // crash this probe invented.
        struct sigaction dfl;
        memset(&dfl, 0, sizeof(dfl));
        dfl.sa_handler = SIG_DFL;
        (void)sigemptyset(&dfl.sa_mask);
        (void)sigaction(sig, &dfl, NULL);
        return;
    }
    g_jump_armed = 0;
    g_caught_signal = sig;
    siglongjmp(g_jump, 1);
}

static const size_t JP_ALTSTACK_SIZE = (size_t)SIGSTKSZ * 2;

static void jp_install_handlers(void) {
    // A code-signing fault can arrive with the stack in a state the normal handler
    // cannot use. SIGSTKSZ is the platform's own answer to how much is enough.
    //
    // ALLOCATED ONCE, INSTALLED EVERY TIME. sigaltstack() is PER THREAD, not per
    // process. Allocating and installing together inside a `if (g_altstack == NULL)`
    // was wrong: Swift concurrency is free to resume an async function on a different
    // thread than it suspended on, and on that thread the handlers would have been
    // registered SA_ONSTACK with no alternate stack in place. Calling sigaltstack
    // again on a thread that already has this one is a no-op, so the repeat costs
    // nothing. (The probe is also driven from @MainActor so this should not arise;
    // "should not" is not a reason to leave a latent fault in the one code path whose
    // entire job is to survive faults.)
    if (g_altstack == NULL) {
        g_altstack = (char *)malloc(JP_ALTSTACK_SIZE);
    }
    if (g_altstack != NULL) {
        stack_t ss;
        memset(&ss, 0, sizeof(ss));
        ss.ss_sp = g_altstack;
        ss.ss_size = JP_ALTSTACK_SIZE;
        ss.ss_flags = 0;
        (void)sigaltstack(&ss, NULL);
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = jp_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    (void)sigemptyset(&sa.sa_mask);

    for (int i = 0; i < JP_TRAPPED_COUNT; ++i) {
        (void)sigaction(JP_TRAPPED[i], &sa, &g_saved[i]);
    }
}

static void jp_restore_handlers(void) {
    for (int i = 0; i < JP_TRAPPED_COUNT; ++i) {
        (void)sigaction(JP_TRAPPED[i], &g_saved[i], NULL);
    }
}

/**
 * Write the two functions with the fault handlers installed.
 *
 * Needed because strategies 2 and 3 write into a MAP_JIT region, and a MAP_JIT region
 * on an APRR core may be executable-not-writable for this thread. Without this the
 * store in jp_write_code() would be an uncaught SIGBUS that looks, from the outside,
 * exactly like the jump having failed.
 *
 * The instruction-cache flush is inside the protected region too: it is a cache
 * maintenance operation on the same addresses and belongs to the same fault domain.
 */
static bool jp_protected_write(void *page, size_t flush_len, int *out_signal) {
    static void *volatile target;
    static volatile size_t target_len;
    target = page;
    target_len = flush_len;

    g_caught_signal = 0;
    jp_install_handlers();

    if (sigsetjmp(g_jump, 1) == 0) {
        g_jump_armed = 1;
        jp_write_code(target);
        jp_flush_icache(target, target_len);
        g_jump_armed = 0;
    }

    g_jump_armed = 0;
    jp_restore_handlers();

    *out_signal = (int)g_caught_signal;
    return g_caught_signal == 0;
}

/**
 * THE JUMP. Everything else in this file exists to make this line survivable or, when
 * it is not, attributable.
 *
 * The two function pointers go through a union rather than a cast: converting void*
 * to a function pointer directly is not something ISO C defines, and clang warns
 * about it under -Wpedantic. POSIX guarantees the union punning works, which is why
 * dlsym is usable at all.
 *
 * NOTE FOR A FUTURE arm64e BUILD: src/ios/project.yml sets ARCHS to arm64, so raw
 * function pointers are plain addresses. On arm64e they are signed with pointer
 * authentication and calling a pointer this probe manufactured would fault in a way
 * that has nothing to do with JIT permission. If ARCHS ever gains arm64e, this call
 * needs ptrauth_sign_unauthenticated and this comment needs deleting.
 */
static bool jp_enter_page(void *page, int *out_first, int *out_second, int *out_signal) {
    static void *volatile entry;
    entry = page;

    g_caught_signal = 0;
    g_ret_first = -1;
    g_ret_second = -1;

    jp_install_handlers();

    if (sigsetjmp(g_jump, 1) == 0) {
        g_jump_armed = 1;

        union { void *ptr; int (*fn)(void); } first, second;
        first.ptr = entry;
        second.ptr = (void *)((char *)entry + JP_SECOND_FN_OFFSET);

        g_ret_first = first.fn();
        g_ret_second = second.fn();

        g_jump_armed = 0;
    }
    // else: arrived here from jp_handler via siglongjmp. g_caught_signal says which.

    g_jump_armed = 0;
    jp_restore_handlers();

    *out_first = g_ret_first;
    *out_second = g_ret_second;
    *out_signal = (int)g_caught_signal;
    return g_caught_signal == 0;
}

// ===========================================================================
// running one strategy
// ===========================================================================

static void jp_write_code(void *page) {
    uint32_t *code = (uint32_t *)page;
    code[0] = JP_MOV_W0_42;
    code[1] = JP_RET;
    code[2] = JP_MOV_W0_90;
    code[3] = JP_RET;
}

/**
 * Apple arm64 cores are not instruction/data coherent: a store through the data cache
 * is not visible to an instruction fetch until the icache line is invalidated. Skipping
 * this does not fail cleanly - it executes whatever stale bytes the icache held, which
 * is indistinguishable from a JIT permission failure and would make this probe lie.
 *
 * cemu-ios-muffin's BackendAArch64 notes record exactly that confusion: a truncated
 * size meant part of a function was never invalidated, the entry took SIGBUS, and the
 * memory mapping was blamed for it for weeks.
 *
 * Called while the page is still writable. Cache maintenance by virtual address does
 * not require the page to be executable, and doing it before the mprotect means the
 * window between "correct bytes visible" and "page runnable" is closed in that order.
 */
static void jp_flush_icache(void *page, size_t len) {
    sys_icache_invalidate(page, len);
}

static void jp_query_vm(jp_strategy_result *s, const void *addr, size_t len) {
    s->vm_prot_readable = false;
    s->vm_cur_prot = 0;
    s->vm_max_prot = 0;
#if JP_HAVE_VM_REGION
    vm_address_t region = (vm_address_t)addr;
    vm_size_t region_size = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;

    memset(&info, 0, sizeof(info));
    const kern_return_t kr = vm_region_64(mach_task_self(), &region, &region_size,
                                          VM_REGION_BASIC_INFO_64,
                                          (vm_region_info_t)&info, &count, &object);
    if (object != MACH_PORT_NULL) {
        (void)mach_port_deallocate(mach_task_self(), object);
    }
    if (kr != KERN_SUCCESS) {
        return;
    }
    // vm_region_64 returns the first region at OR AFTER the address it is given, so
    // KERN_SUCCESS alone does not mean it described the mapping that was asked about.
    if (region > (vm_address_t)addr ||
        (vm_address_t)addr + len > region + region_size) {
        return;
    }
    s->vm_prot_readable = true;
    s->vm_cur_prot = (unsigned int)info.protection;
    s->vm_max_prot = (unsigned int)info.max_protection;
#else
    (void)addr;
    (void)len;
#endif
}

static void jp_fail_map(jp_strategy_result *s, const char *call, int err) {
    s->outcome = JP_OUTCOME_MAP_FAILED;
    s->failed_call = call;
    s->failed_errno = err;
}

static void jp_fail_prepare(jp_strategy_result *s, const char *call, int err) {
    s->outcome = JP_OUTCOME_PREPARE_FAILED;
    s->failed_call = call;
    s->failed_errno = err;
}

/**
 * Map, write, flush, protect, JUMP.
 *
 * Returns with s->outcome set. May not return at all, which is the case the journal
 * covers.
 */
static void jp_run_strategy(int index) {
    jp_strategy_result *s = &g_report.strategies[index];
    const size_t page_len = (size_t)g_report.page_size;
    void *page = MAP_FAILED;

    // --- map ------------------------------------------------------------
    errno = 0;
    switch (index) {
    case 0:
        page = mmap(NULL, page_len, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANON, -1, 0);
        break;
    case 1:
        page = mmap(NULL, page_len, PROT_READ | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANON, -1, 0);
        break;
    case 2:
        page = mmap(NULL, page_len, PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
        break;
    case 3:
        page = mmap(NULL, page_len, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
        break;
    case 4:
        // No MAP_JIT: max_protection on Darwin is fixed at THIS call, so every bit
        // the page will ever need has to be requested here, not added later.
        page = mmap(NULL, page_len, PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANON, -1, 0);
        break;
    default:
        return;
    }

    // MAP_FAILED is (void*)-1, never NULL. docs/IOS_PORT_NOTES.md records that
    // upstream oaknut tested for nullptr here, so a failed mapping was kept as a valid
    // pointer and faulted later somewhere unrelated - "and on iOS a failed mapping is
    // the normal outcome when JIT is not permitted, so the common case produced an
    // unattributable crash". Test the right sentinel.
    if (page == MAP_FAILED) {
        jp_fail_map(s, "mmap", errno);
        return;
    }

    // --- make it writable, if it is not already --------------------------
    if (index == 1) {
        errno = 0;
        if (mprotect(page, page_len, PROT_READ | PROT_WRITE) != 0) {
            jp_fail_prepare(s, "mprotect(PROT_READ|PROT_WRITE)", errno);
            (void)munmap(page, page_len);
            return;
        }
    }

    // --- write the two functions, and flush the icache --------------------
    jp_journal_arm(index, "write");
    int write_signal = 0;
    if (!jp_protected_write(page, 4 * sizeof(uint32_t), &write_signal)) {
        s->outcome = JP_OUTCOME_WRITE_FAULTED;
        s->failed_call = "storing instructions into the page";
        s->failed_errno = 0;
        s->caught_signal = write_signal;
        jp_journal_survived(index, "write");
        (void)munmap(page, page_len);
        return;
    }
    jp_journal_survived(index, "write");

    // --- make it executable ----------------------------------------------
    // Strategy 2 asked for PROT_EXEC at map time and must NOT be mprotected: on an
    // APRR core a MAP_JIT region is governed by a per-thread write switch rather than
    // by mprotect, and cemu-ios-muffin's BackendAArch64 notes record that mprotecting
    // such a region is how you turn a working mapping into a faulting one.
    if (index != 2) {
        errno = 0;
        if (mprotect(page, page_len, PROT_READ | PROT_EXEC) != 0) {
            jp_fail_prepare(s, "mprotect(PROT_READ|PROT_EXEC)", errno);
            (void)munmap(page, page_len);
            return;
        }
    }

    jp_query_vm(s, page, page_len);

    // --- the dangerous part ----------------------------------------------
    jp_journal_arm(index, "jump");

    int first = -1;
    int second = -1;
    int sig = 0;
    const bool survived = jp_enter_page(page, &first, &second, &sig);

    jp_journal_survived(index, "jump");

    s->returned_first = first;
    s->returned_second = second;
    s->caught_signal = sig;

    if (!survived) {
        s->outcome = JP_OUTCOME_SIGNALLED;
    } else if (first == JP_EXPECT_FIRST && second == JP_EXPECT_SECOND) {
        s->outcome = JP_OUTCOME_PASS;
    } else {
        s->outcome = JP_OUTCOME_WRONG_VALUE;
    }

    (void)munmap(page, page_len);
}

// ===========================================================================
// environment
// ===========================================================================

static void jp_append_flag(char *buf, size_t len, const char *name) {
    if (buf[0] != '\0') {
        strlcat(buf, ", ", len);
    }
    strlcat(buf, name, len);
}

static void jp_decode_cs_flags(uint32_t f, char *buf, size_t len) {
    buf[0] = '\0';
    if (f & JP_CS_VALID)              jp_append_flag(buf, len, "CS_VALID");
    if (f & JP_CS_ADHOC)              jp_append_flag(buf, len, "CS_ADHOC");
    if (f & JP_CS_GET_TASK_ALLOW)     jp_append_flag(buf, len, "CS_GET_TASK_ALLOW");
    if (f & JP_CS_INSTALLER)          jp_append_flag(buf, len, "CS_INSTALLER");
    if (f & JP_CS_HARD)               jp_append_flag(buf, len, "CS_HARD");
    if (f & JP_CS_KILL)               jp_append_flag(buf, len, "CS_KILL");
    if (f & JP_CS_CHECK_EXPIRATION)   jp_append_flag(buf, len, "CS_CHECK_EXPIRATION");
    if (f & JP_CS_RESTRICT)           jp_append_flag(buf, len, "CS_RESTRICT");
    if (f & JP_CS_ENFORCEMENT)        jp_append_flag(buf, len, "CS_ENFORCEMENT");
    if (f & JP_CS_REQUIRE_LV)         jp_append_flag(buf, len, "CS_REQUIRE_LV");
    if (f & JP_CS_ENTITLEMENTS_VALID) jp_append_flag(buf, len, "CS_ENTITLEMENTS_VALIDATED");
    if (f & JP_CS_RUNTIME)            jp_append_flag(buf, len, "CS_RUNTIME");
    if (f & JP_CS_LINKER_SIGNED)      jp_append_flag(buf, len, "CS_LINKER_SIGNED");
    if (f & JP_CS_KILLED)             jp_append_flag(buf, len, "CS_KILLED");
    if (f & JP_CS_DYLD_PLATFORM)      jp_append_flag(buf, len, "CS_DYLD_PLATFORM");
    if (f & JP_CS_PLATFORM_BINARY)    jp_append_flag(buf, len, "CS_PLATFORM_BINARY");
    if (f & JP_CS_PLATFORM_PATH)      jp_append_flag(buf, len, "CS_PLATFORM_PATH");
    if (f & JP_CS_DEBUGGED)           jp_append_flag(buf, len, "CS_DEBUGGED");
    if (f & JP_CS_SIGNED)             jp_append_flag(buf, len, "CS_SIGNED");
    if (f & JP_CS_DEV_CODE)           jp_append_flag(buf, len, "CS_DEV_CODE");
    if (buf[0] == '\0') {
        strlcat(buf, "(none set)", len);
    }
}

static void jp_probe_csops(void) {
    uint32_t flags = 0;
    errno = 0;
    if (jp_csops(getpid(), JP_CS_OPS_STATUS, &flags, sizeof(flags)) == 0) {
        g_report.cs_readable = true;
        g_report.cs_flags = flags;
        g_report.cs_debugged        = (flags & JP_CS_DEBUGGED) != 0;
        g_report.cs_valid           = (flags & JP_CS_VALID) != 0;
        g_report.cs_signed          = (flags & JP_CS_SIGNED) != 0;
        g_report.cs_adhoc           = (flags & JP_CS_ADHOC) != 0;
        g_report.cs_get_task_allow  = (flags & JP_CS_GET_TASK_ALLOW) != 0;
        g_report.cs_platform_binary = (flags & JP_CS_PLATFORM_BINARY) != 0;
        g_report.cs_enforcement     = (flags & JP_CS_ENFORCEMENT) != 0;
        jp_decode_cs_flags(flags, g_cs_flag_names, sizeof(g_cs_flag_names));
    } else {
        g_report.cs_errno = errno;
        strlcpy(g_cs_flag_names, "(csops failed)", sizeof(g_cs_flag_names));
    }
    g_report.cs_used_dlsym = g_csops_via_dlsym;
}

/**
 * The entitlements ACTUALLY in the signature, read from the kernel rather than from
 * the .entitlements file on disk - those are an input to signing, not evidence that
 * signing kept them. A free-developer re-sign is entitled to drop keys, and whether it
 * does is one of the things this probe exists to establish.
 *
 * The documented dance: call with a small buffer, get -1/ERANGE with the real length
 * in a big-endian CS_GenericBlob header, allocate that, call again.
 */
static void jp_probe_entitlements(void) {
    struct { uint32_t magic; uint32_t length; } header;
    const pid_t pid = getpid();

    memset(&header, 0, sizeof(header));
    errno = 0;
    if (jp_csops(pid, JP_CS_OPS_ENTITLEMENTS_BLOB, &header, sizeof(header)) == 0) {
        // A signature with no entitlements at all is a legitimate answer, and is
        // exactly what the unsigned/no-entitlements variant should report.
        g_report.ent_readable = true;
        g_report.ent_length = 0;
        strlcpy(g_ent_xml, "(signature carries no entitlements)", sizeof(g_ent_xml));
        return;
    }
    if (errno != ERANGE) {
        g_report.ent_errno = errno;
        strlcpy(g_ent_xml, "(could not read the entitlement blob)", sizeof(g_ent_xml));
        return;
    }

    const uint32_t length = ntohl(header.length);
    if (length <= sizeof(header) || length > (1u << 20)) {
        g_report.ent_errno = EINVAL;
        strlcpy(g_ent_xml, "(entitlement blob length out of range)", sizeof(g_ent_xml));
        return;
    }

    char *blob = (char *)calloc(1, length + 1);
    if (blob == NULL) {
        g_report.ent_errno = ENOMEM;
        return;
    }

    errno = 0;
    if (jp_csops(pid, JP_CS_OPS_ENTITLEMENTS_BLOB, blob, length) != 0) {
        g_report.ent_errno = errno;
        free(blob);
        strlcpy(g_ent_xml, "(second csops call failed)", sizeof(g_ent_xml));
        return;
    }

    g_report.ent_readable = true;
    g_report.ent_length = length;

    const char *payload = blob + sizeof(header);
    const size_t payload_len = (size_t)length - sizeof(header);
    if (payload_len + 1 > sizeof(g_ent_xml)) {
        g_report.ent_truncated = true;
    }
    // strlcpy would stop at the first NUL; the payload is XML and should have none,
    // but a DER-only signature would put binary here, so bound it explicitly.
    size_t copy = payload_len;
    if (copy > sizeof(g_ent_xml) - 1) {
        copy = sizeof(g_ent_xml) - 1;
    }
    memcpy(g_ent_xml, payload, copy);
    g_ent_xml[copy] = '\0';

    free(blob);
}

// SecTask lives in Security.framework. Its header is not in the public iOS SDK, so the
// two functions are resolved by name. This is a cross-check on the blob above, not a
// replacement: the blob shows EVERY entitlement, SecTask answers only for keys named
// here, but SecTask is the API the system itself consults.
typedef struct __SecTask *jp_SecTaskRef;
typedef jp_SecTaskRef (*jp_sectask_create_fn)(CFAllocatorRef);
typedef CFTypeRef (*jp_sectask_copy_fn)(jp_SecTaskRef, CFStringRef, CFErrorRef *);

static int jp_sectask_query(jp_sectask_copy_fn copy_value, jp_SecTaskRef task,
                            const char *key) {
    CFStringRef cf_key = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
    if (cf_key == NULL) {
        return -1;
    }
    CFErrorRef error = NULL;
    CFTypeRef value = copy_value(task, cf_key, &error);
    CFRelease(cf_key);
    if (error != NULL) {
        CFRelease(error);
    }
    if (value == NULL) {
        return 0;   // absent
    }
    int result = 2;  // present, but not a true boolean
    if (CFGetTypeID(value) == CFBooleanGetTypeID()) {
        result = CFBooleanGetValue((CFBooleanRef)value) ? 1 : 2;
    }
    CFRelease(value);
    return result;
}

static void jp_probe_sectask(void) {
    g_report.sec_dynamic_codesigning = -1;
    g_report.sec_allow_jit = -1;
    g_report.sec_get_task_allow = -1;
    g_report.sec_increased_memory_limit = -1;

    jp_sectask_create_fn create =
        (jp_sectask_create_fn)dlsym(RTLD_DEFAULT, "SecTaskCreateFromSelf");
    jp_sectask_copy_fn copy_value =
        (jp_sectask_copy_fn)dlsym(RTLD_DEFAULT, "SecTaskCopyValueForEntitlement");
    if (create == NULL || copy_value == NULL) {
        return;
    }
    jp_SecTaskRef task = create(NULL);
    if (task == NULL) {
        return;
    }
    g_report.sectask_available = true;

    g_report.sec_dynamic_codesigning =
        jp_sectask_query(copy_value, task, "dynamic-codesigning");
    g_report.sec_allow_jit =
        jp_sectask_query(copy_value, task, "com.apple.security.cs.allow-jit");
    g_report.sec_get_task_allow =
        jp_sectask_query(copy_value, task, "get-task-allow");
    g_report.sec_increased_memory_limit =
        jp_sectask_query(copy_value, task,
                         "com.apple.developer.kernel.increased-memory-limit");

    CFRelease(task);
}

static void jp_sysctl_string(const char *name, char *out, size_t len) {
    size_t size = len;
    out[0] = '\0';
    if (sysctlbyname(name, out, &size, NULL, 0) != 0) {
        out[0] = '\0';
    } else {
        out[len - 1] = '\0';
    }
}

static void jp_probe_device(void) {
    jp_sysctl_string("hw.machine", g_hw_machine, sizeof(g_hw_machine));
    jp_sysctl_string("hw.model", g_hw_model, sizeof(g_hw_model));
    jp_sysctl_string("kern.osproductversion", g_os_version, sizeof(g_os_version));
    jp_sysctl_string("kern.osversion", g_os_build, sizeof(g_os_build));

    if (g_os_version[0] == '\0' && g_os_hint[0] != '\0') {
        strlcpy(g_os_version, g_os_hint, sizeof(g_os_version));
    }

    uint64_t mem = 0;
    size_t mem_size = sizeof(mem);
    if (sysctlbyname("hw.memsize", &mem, &mem_size, NULL, 0) == 0) {
        g_report.physical_memory = mem;
    }

    g_report.page_size = sysconf(_SC_PAGESIZE);
    if (g_report.page_size <= 0) {
        g_report.page_size = 16384;
    }

    // P_TRACED, which is a different question from CS_DEBUGGED. A debugger can be
    // attached without the codesigning flag having been set yet, and the flag can
    // persist after a debugger detaches. Reporting both means the two can be told
    // apart in a bug report instead of guessed at.
#if JP_HAVE_PROC_H
    struct kinfo_proc info;
    size_t info_size = sizeof(info);
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid() };
    memset(&info, 0, sizeof(info));
    if (sysctl(mib, 4, &info, &info_size, NULL, 0) == 0) {
        g_report.traced_readable = true;
        g_report.traced = (info.kp_proc.p_flag & JP_P_TRACED) != 0;
    }
#endif

    // NEVER CALLED. docs/IOS_PORT_NOTES.md: pthread_jit_write_protect_np "is absent
    // from the iOS SDK, declared __attribute__((unavailable)), so even naming it is a
    // compile error". A dlsym on a string literal is not naming it as an identifier,
    // and whether libsystem_pthread exports it anyway is a genuinely open question -
    // cemu-ios-muffin resolves it the same way and found it present on iOS 26.
    g_report.write_protect_symbol_present =
        dlsym(RTLD_DEFAULT, "pthread_jit_write_protect_np") != NULL;
}

// ===========================================================================
// public API
// ===========================================================================

void jp_set_state_directory(const char *dir) {
    if (dir == NULL) {
        g_state_dir[0] = '\0';
        return;
    }
    strlcpy(g_state_dir, dir, sizeof(g_state_dir));
}

const char *jp_hw_machine(void)      { return g_hw_machine; }
const char *jp_hw_model(void)        { return g_hw_model; }
const char *jp_os_version(void)      { return g_os_version; }
const char *jp_os_build(void)        { return g_os_build; }
const char *jp_cs_flag_names(void)   { return g_cs_flag_names; }
const char *jp_entitlements_xml(void){ return g_ent_xml; }
const char *jp_state_directory(void) { return g_state_dir; }

jp_strategy_result jp_strategy(int index) {
    if (index < 0 || index >= JP_STRATEGY_COUNT) {
        jp_strategy_result empty;
        memset(&empty, 0, sizeof(empty));
        return empty;
    }
    (void)jp_begin();
    return g_report.strategies[index];
}

void jp_set_os_version_hint(const char *version) {
    if (version == NULL) {
        return;
    }
    strlcpy(g_os_hint, version, sizeof(g_os_hint));
}

static void jp_recompute(void) {
    g_report.any_pass = false;
    g_report.any_killed = false;
    g_report.next_strategy = JP_STRATEGY_COUNT;

    for (int i = 0; i < JP_STRATEGY_COUNT; ++i) {
        jp_strategy_result *s = &g_report.strategies[i];
        s->index = i;
        s->name = JP_NAMES[i];
        s->detail = JP_DETAILS[i];
        if (s->outcome == JP_OUTCOME_PASS) {
            g_report.any_pass = true;
        }
        if (s->outcome == JP_OUTCOME_KILLED) {
            g_report.any_killed = true;
        }
        if (s->outcome == JP_OUTCOME_UNTRIED && i < g_report.next_strategy) {
            g_report.next_strategy = i;
        }
    }
    g_report.all_done = (g_report.next_strategy >= JP_STRATEGY_COUNT);
}

/**
 * Turn an unfinished journal entry into a result.
 *
 * "arming N" still on disk with no result for N means the process stopped existing
 * between the fsync and the return. That is the uncatchable refusal, and it is the
 * single most important thing this probe can report.
 *
 * HONEST CAVEAT, stated here and in the README because it is a real false positive:
 * any other cause of death in that window reads the same way - the tester force-quits
 * at exactly the wrong instant, or iOS jetsams the app under memory pressure. The
 * window is microseconds wide, so this is unlikely rather than impossible. "Start
 * over" exists so a suspected false positive can be retested rather than argued about.
 */
static void jp_replay_journal(void) {
    char path[1152];
    char body[128];

    jp_path(path, sizeof(path), JP_JOURNAL_NAME);
    if (jp_read_file(path, body, sizeof(body)) == 0) {
        return;
    }

    int index = -1;
    char phase[16] = {0};
    if (sscanf(body, "arming %d %15s", &index, phase) >= 1 &&
        index >= 0 && index < JP_STRATEGY_COUNT) {
        if (g_report.strategies[index].outcome == JP_OUTCOME_UNTRIED) {
            g_report.strategies[index].outcome = JP_OUTCOME_KILLED;
            // A journal written by an older build has no phase word; "jump" is the
            // right default because it is the phase that existed first.
            g_report.strategies[index].failed_call =
                (strcmp(phase, "write") == 0) ? "storing instructions into the page"
                                              : "jump into the page";
            jp_results_save();
        }
    }
    jp_remove(JP_JOURNAL_NAME);
}

const jp_report *jp_begin(void) {
    if (g_began) {
        return &g_report;
    }

    jp_probe_device();
    jp_probe_csops();
    jp_probe_entitlements();
    jp_probe_sectask();

    // Does the state directory actually accept a write? A journal that silently fails
    // is worse than no journal: it would make a killed strategy look untried forever
    // and the probe would loop on it.
    if (g_state_dir[0] != '\0') {
        char probe_path[1152];
        jp_path(probe_path, sizeof(probe_path), "jit-probe-writable.tmp");
        g_report.state_dir_usable = jp_write_atomic(probe_path, "ok\n");
        (void)unlink(probe_path);
    }

    jp_results_load();
    jp_replay_journal();
    jp_recompute();

    g_began = true;
    return &g_report;
}

const jp_report *jp_run_next_strategy(void) {
    (void)jp_begin();
    if (g_report.all_done) {
        return &g_report;
    }
    const int index = g_report.next_strategy;
    jp_run_strategy(index);
    jp_results_save();
    jp_remove(JP_JOURNAL_NAME);
    jp_recompute();
    return &g_report;
}

const jp_report *jp_current_report(void) {
    return jp_begin();
}

void jp_reset(void) {
    for (int i = 0; i < JP_STRATEGY_COUNT; ++i) {
        jp_strategy_result *s = &g_report.strategies[i];
        s->outcome = JP_OUTCOME_UNTRIED;
        s->failed_call = NULL;
        s->failed_errno = 0;
        s->caught_signal = 0;
        s->returned_first = -1;
        s->returned_second = -1;
        s->vm_prot_readable = false;
        s->vm_cur_prot = 0;
        s->vm_max_prot = 0;
        g_failed_call_store[i][0] = '\0';
    }
    jp_remove(JP_JOURNAL_NAME);
    jp_remove(JP_RESULTS_NAME);
    jp_recompute();
}

bool jp_jit_works(void) {
    return jp_begin()->any_pass;
}

const char *jp_headline(void) {
    const jp_report *r = jp_begin();
    if (r->any_pass) {
        return "PASS";
    }
    if (!r->all_done) {
        return "TESTING";
    }
    return "FAIL";
}

// ===========================================================================
// flat accessors - see the note in jit_probe.h
// ===========================================================================

bool jp_all_done(void)        { return jp_begin()->all_done; }
bool jp_any_pass(void)        { return jp_begin()->any_pass; }
bool jp_any_killed(void)      { return jp_begin()->any_killed; }
int  jp_next_strategy(void)   { return jp_begin()->next_strategy; }
bool jp_journal_writable(void){ return jp_begin()->state_dir_usable; }
bool jp_cs_debugged(void)     { return jp_begin()->cs_debugged; }
int  jp_strategy_total(void)  { return JP_STRATEGY_COUNT; }

static const char *jp_outcome_text(jp_outcome o);

const char *jp_strategy_label(int index) {
    if (index < 0 || index >= JP_STRATEGY_COUNT) {
        return "";
    }
    (void)jp_begin();
    return JP_NAMES[index];
}

const char *jp_strategy_outcome_text(int index) {
    if (index < 0 || index >= JP_STRATEGY_COUNT) {
        return "";
    }
    return jp_outcome_text(jp_begin()->strategies[index].outcome);
}

int jp_strategy_outcome_code(int index) {
    if (index < 0 || index >= JP_STRATEGY_COUNT) {
        return (int)JP_OUTCOME_UNTRIED;
    }
    return (int)jp_begin()->strategies[index].outcome;
}

bool jp_strategy_is_untried(int index) {
    return jp_strategy_outcome_code(index) == (int)JP_OUTCOME_UNTRIED;
}

bool jp_strategy_is_pass(int index) {
    return jp_strategy_outcome_code(index) == (int)JP_OUTCOME_PASS;
}

bool jp_strategy_is_fatal(int index) {
    const int code = jp_strategy_outcome_code(index);
    return code == (int)JP_OUTCOME_SIGNALLED || code == (int)JP_OUTCOME_KILLED;
}

bool jp_strategy_is_decisive(int index) {
    // Strategy 4 is the shape .patch/oaknut/0001-ios-jit-modes.patch actually gives
    // CodeBlock on iOS as of 2026-09-12 - RWX in one mmap() call, mprotected down.
    // Strategy 0 held this title until a real device showed it, and strategy 1
    // (oaknut's actual prior shape), both fail. Every other row is diagnostic;
    // this one is the project's current answer.
    return index == 4;
}

// ===========================================================================
// the copyable report
// ===========================================================================

static const char *jp_outcome_text(jp_outcome o) {
    switch (o) {
    case JP_OUTCOME_UNTRIED:        return "not tried yet";
    case JP_OUTCOME_MAP_FAILED:     return "mmap refused - nothing was executed";
    case JP_OUTCOME_PREPARE_FAILED: return "could not be made executable - nothing was executed";
    case JP_OUTCOME_WRITE_FAULTED:  return "faulted when written to - nothing was executed";
    case JP_OUTCOME_SIGNALLED:      return "EXECUTED AND FAULTED";
    case JP_OUTCOME_KILLED:         return "EXECUTED AND KILLED THE PROCESS";
    case JP_OUTCOME_WRONG_VALUE:    return "ran but returned the wrong value";
    case JP_OUTCOME_PASS:           return "PASS - ran and returned 42 and 90";
    }
    return "unknown";
}

static const char *jp_tri(int v) {
    switch (v) {
    case 1:  return "yes";
    case 0:  return "absent";
    case 2:  return "present but not true";
    default: return "could not ask";
    }
}

static const char *jp_signal_name(int sig) {
    switch (sig) {
    case SIGBUS:  return "SIGBUS";
    case SIGSEGV: return "SIGSEGV";
    case SIGILL:  return "SIGILL";
    case SIGTRAP: return "SIGTRAP";
    case 0:       return "none";
    default:      return "other";
    }
}

void jp_format_report(char *buf, size_t len) {
    const jp_report *r = jp_begin();
    size_t used = 0;
    g_text[0] = '\0';

#define JP_ADD(...)                                                              \
    do {                                                                         \
        const int n_ = snprintf(g_text + used, sizeof(g_text) - used, __VA_ARGS__); \
        if (n_ > 0 && (size_t)n_ < sizeof(g_text) - used) {                      \
            used += (size_t)n_;                                                  \
        }                                                                        \
    } while (0)

    JP_ADD("eden-ios JIT probe\n");
    JP_ADD("==================\n\n");
    JP_ADD("VERDICT: %s\n", r->any_pass ? "JIT WORKS ON THIS DEVICE, AS INSTALLED"
                          : (r->all_done ? "JIT DOES NOT WORK - Eden cannot emulate here"
                                         : "incomplete - run the remaining strategies"));
    if (r->any_pass) {
        JP_ADD("A page of memory this app wrote was executed and returned the two\n"
               "constants that were written into it. This is the assumption the whole\n"
               "port rests on, and on this device, installed this way, it holds.\n");
    } else if (r->all_done) {
        JP_ADD("Every way of getting executable memory was refused or fatal. There is\n"
               "no interpreter in Eden, so this is not slow emulation - it is none.\n");
    }
    JP_ADD("\n");

    JP_ADD("DEVICE\n");
    JP_ADD("  model             : %s%s%s%s\n",
           g_hw_machine[0] ? g_hw_machine : "(unknown)",
           g_hw_model[0] ? " / " : "", g_hw_model, "");
    JP_ADD("  iOS               : %s (build %s)\n",
           g_os_version[0] ? g_os_version : "(unknown)",
           g_os_build[0] ? g_os_build : "?");
    JP_ADD("  physical memory   : %llu bytes (%.2f GB)\n",
           (unsigned long long)r->physical_memory,
           (double)r->physical_memory / (1024.0 * 1024.0 * 1024.0));
    JP_ADD("  host page size    : %ld bytes%s\n", r->page_size,
           r->page_size == 16384
               ? "  [16 KiB vs the Switch's 4 KiB - fastmem is off, per IOS_PORT_NOTES.md]"
               : "");
    JP_ADD("\n");

    JP_ADD("CODE SIGNING STATUS  csops(CS_OPS_STATUS)\n");
    if (r->cs_readable) {
        JP_ADD("  raw flags         : 0x%08x\n", r->cs_flags);
        JP_ADD("  decoded           : %s\n", g_cs_flag_names);
        JP_ADD("  CS_DEBUGGED       : %s%s\n", r->cs_debugged ? "YES" : "no",
               r->cs_debugged ? "   <- a debugger has waived signature enforcement"
                              : "   <- no StikDebug/SideStore-style JIT grant");
        JP_ADD("  CS_VALID          : %s\n", r->cs_valid ? "yes" : "no");
        JP_ADD("  CS_SIGNED         : %s\n", r->cs_signed ? "yes" : "no");
        JP_ADD("  CS_ADHOC          : %s\n", r->cs_adhoc ? "yes" : "no");
        JP_ADD("  CS_GET_TASK_ALLOW : %s\n", r->cs_get_task_allow ? "yes" : "no");
        JP_ADD("  CS_ENFORCEMENT    : %s\n", r->cs_enforcement ? "yes" : "no");
        JP_ADD("  CS_PLATFORM_BINARY: %s\n", r->cs_platform_binary ? "yes" : "no");
    } else {
        JP_ADD("  UNREADABLE        : errno %d (%s)\n", r->cs_errno, strerror(r->cs_errno));
    }
    JP_ADD("  reached via       : %s\n", r->cs_used_dlsym ? "dlsym(\"csops\")"
                                                          : "syscall(SYS_csops)");
    JP_ADD("  P_TRACED          : %s\n",
           r->traced_readable ? (r->traced ? "yes - a debugger is attached right now"
                                           : "no")
                              : "could not ask");
    JP_ADD("\n");

    JP_ADD("ENTITLEMENTS ACTUALLY IN THE SIGNATURE\n");
    JP_ADD("  (read from the kernel, not from the .entitlements file - a re-sign can\n");
    JP_ADD("   drop keys and this is the only way to find out that it did)\n");
    if (r->sectask_available) {
        JP_ADD("  SecTaskCopyValueForEntitlement:\n");
        JP_ADD("    dynamic-codesigning                 : %s\n",
               jp_tri(r->sec_dynamic_codesigning));
        JP_ADD("    com.apple.security.cs.allow-jit     : %s\n", jp_tri(r->sec_allow_jit));
        JP_ADD("    get-task-allow                      : %s\n", jp_tri(r->sec_get_task_allow));
        JP_ADD("    kernel.increased-memory-limit       : %s\n",
               jp_tri(r->sec_increased_memory_limit));
    } else {
        JP_ADD("  SecTask API not resolvable on this OS.\n");
    }
    if (r->ent_readable) {
        JP_ADD("  raw blob (%u bytes%s):\n", r->ent_length,
               r->ent_truncated ? ", TRUNCATED below" : "");
        JP_ADD("%s\n", g_ent_xml);
    } else {
        JP_ADD("  blob unreadable   : errno %d (%s)\n", r->ent_errno,
               strerror(r->ent_errno));
    }
    JP_ADD("\n");

    JP_ADD("EXECUTION STRATEGIES\n");
    JP_ADD("  Each one maps memory, writes 'mov w0,#42; ret' at offset 0 and\n");
    JP_ADD("  'mov w0,#90; ret' at offset 8, flushes the instruction cache with\n");
    JP_ADD("  sys_icache_invalidate, makes the page executable, and CALLS BOTH.\n\n");
    for (int i = 0; i < JP_STRATEGY_COUNT; ++i) {
        const jp_strategy_result *s = &r->strategies[i];
        JP_ADD("  [%d] %s\n", i, s->name ? s->name : "?");
        JP_ADD("      result        : %s\n", jp_outcome_text(s->outcome));
        if (s->failed_call != NULL && s->failed_errno != 0) {
            JP_ADD("      refused at    : %s, errno %d (%s)\n", s->failed_call,
                   s->failed_errno, strerror(s->failed_errno));
        } else if (s->failed_call != NULL) {
            JP_ADD("      stopped at    : %s\n", s->failed_call);
        }
        // Printed for every outcome that carries one, not only SIGNALLED: a write
        // fault has a signal too, and leaving it out was how the first run of this
        // probe reported "errno 0 (Undefined error: 0)" and explained nothing.
        if (s->caught_signal != 0) {
            JP_ADD("      signal        : %d (%s)\n", s->caught_signal,
                   jp_signal_name(s->caught_signal));
        }
        if (s->outcome == JP_OUTCOME_WRITE_FAULTED && (i == 2 || i == 3)) {
            JP_ADD("      note          : this is what docs/IOS_PORT_NOTES.md predicts for\n"
                   "                      MAP_JIT on Apple arm64 - the region is executable-\n"
                   "                      not-writable and only pthread_jit_write_protect_np\n"
                   "                      flips it, which an iOS build cannot call.\n");
        }
        if (s->outcome == JP_OUTCOME_PASS || s->outcome == JP_OUTCOME_WRONG_VALUE) {
            JP_ADD("      returned      : %d (want 42) and %d (want 90)\n",
                   s->returned_first, s->returned_second);
        }
        if (s->vm_prot_readable) {
            JP_ADD("      vm_region_64  : cur_prot 0x%x, max_prot 0x%x\n",
                   s->vm_cur_prot, s->vm_max_prot);
        }
    }
    JP_ADD("\n");

    JP_ADD("OTHER FACTS\n");
    JP_ADD("  pthread_jit_write_protect_np exported: %s\n",
           r->write_protect_symbol_present ? "yes (found by dlsym; NEVER CALLED here)"
                                           : "no");
    JP_ADD("    docs/IOS_PORT_NOTES.md says this symbol is absent from the iOS SDK, so\n");
    JP_ADD("    it cannot be called from Eden's C++ at all. Whether libsystem still\n");
    JP_ADD("    EXPORTS it is a separate question, and this line is the answer.\n");
    JP_ADD("  crash journal directory: %s\n",
           g_state_dir[0] ? g_state_dir : "(none - a fatal result would be lost)");
    JP_ADD("  journal writable       : %s\n", r->state_dir_usable ? "yes" : "NO");
    JP_ADD("\n");
    JP_ADD("Instruction encodings used (see jit_probe.c for the bit-field derivation):\n");
    JP_ADD("  mov w0, #42  MOVZ Wd,#imm16  0x%08x\n", JP_MOV_W0_42);
    JP_ADD("  mov w0, #90  MOVZ Wd,#imm16  0x%08x\n", JP_MOV_W0_90);
    JP_ADD("  ret          RET X30         0x%08x\n", JP_RET);

#undef JP_ADD

    if (buf != NULL && len > 0) {
        strlcpy(buf, g_text, len);
    }
}
