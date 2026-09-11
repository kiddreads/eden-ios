// SPDX-FileCopyrightText: Copyright 2026 eden-ios contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// See EdenJIT.h. Syscalls only - this file never executes a generated instruction.

#import <Foundation/Foundation.h>

#include <arpa/inet.h>    // ntohl - the blob header is big-endian on the wire
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>       // malloc, free
#include <string.h>       // strstr, strlcpy (Darwin)
#include <sys/mman.h>
#include <sys/syscall.h>  // SYS_csops
#include <sys/types.h>
#include <unistd.h>

#import "EdenJIT.h"

// ---------------------------------------------------------------------------
// csops
//
// Declared in the kernel's <sys/codesign.h>, which is NOT in the iOS SDK, so the
// prototype and the constants are restated here. This is what every tool that reads
// these flags does. The syscall number SYS_csops comes from <sys/syscall.h>, which IS
// public.
// ---------------------------------------------------------------------------

#define EDEN_CS_OPS_STATUS            0
#define EDEN_CS_OPS_ENTITLEMENTS_BLOB 7

#define EDEN_CS_VALID                 0x00000001
#define EDEN_CS_DEBUGGED              0x10000000

static int eden_csops(pid_t pid, unsigned int ops, void *useraddr, size_t usersize) {
    return (int)syscall(SYS_csops, pid, ops, useraddr, usersize);
}

// ---------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------

static EdenJITReport g_report;
static bool g_have_report = false;
static _Atomic bool g_locked = false;
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static char g_diagnostics[2048];

// ---------------------------------------------------------------------------
// entitlements
// ---------------------------------------------------------------------------

/**
 * Read the embedded entitlement blob with csops(CS_OPS_ENTITLEMENTS_BLOB).
 *
 * The documented dance: call with a small buffer, get -1 with errno == ERANGE and the
 * REAL length written into the first 8 bytes as a big-endian CS_GenericBlob header
 * (magic, length), then allocate that much and call again.
 *
 * UNVERIFIED on a device for this port. If it behaves differently the caller reports
 * "unreadable", which the verdict logic survives - it can still infer the entitlement
 * from MAP_JIT succeeding with no debugger attached - but the diagnostics get poorer.
 *
 * Note also: a signature carrying ONLY the DER entitlement form would have no XML blob
 * here at all. The CI signing step passes --generate-entitlement-der, which should
 * embed both. I did not confirm whether a separate DER-specific csops op should be
 * read as well.
 */
static bool EdenReadEntitlements(EdenJITReport *r) {
    struct {
        uint32_t magic;
        uint32_t length;
    } header;

    memset(&header, 0, sizeof(header));
    const pid_t pid = getpid();

    errno = 0;
    if (eden_csops(pid, EDEN_CS_OPS_ENTITLEMENTS_BLOB, &header, sizeof(header)) == 0) {
        // No entitlements at all is a legitimate answer for an unsigned build.
        return true;
    }
    if (errno != ERANGE) {
        return false;
    }

    // Header is big-endian on the wire.
    const uint32_t length = ntohl(header.length);
    if (length <= sizeof(header) || length > (1u << 20)) {
        return false;
    }

    char *blob = malloc(length);
    if (blob == NULL) {
        return false;
    }
    memset(blob, 0, length);

    errno = 0;
    if (eden_csops(pid, EDEN_CS_OPS_ENTITLEMENTS_BLOB, blob, length) != 0) {
        free(blob);
        return false;
    }

    // The payload after the 8-byte header is an XML property list. A substring search
    // for the key name is enough, and is what this needs to be: parsing it properly
    // would mean a plist parser on a buffer that may be DER, may be truncated, and is
    // only ever consulted for three boolean keys. A false positive here can only make
    // the app more optimistic about a build whose entitlements it could read, which is
    // the case where the signature genuinely does name the key.
    const char *payload = blob + sizeof(header);
    const size_t payload_len = length - sizeof(header);

    // Ensure the search is bounded even if the blob is not terminated.
    char *search = malloc(payload_len + 1);
    if (search == NULL) {
        free(blob);
        return false;
    }
    memcpy(search, payload, payload_len);
    search[payload_len] = '\0';

    r->has_dynamic_codesigning =
        (strstr(search, EDEN_JIT_ENTITLEMENT_DYNAMIC_CODESIGNING) != NULL);
    r->has_allow_jit = (strstr(search, EDEN_JIT_ENTITLEMENT_ALLOW_JIT) != NULL);
    r->has_get_task_allow = (strstr(search, EDEN_JIT_ENTITLEMENT_GET_TASK_ALLOW) != NULL);

    free(search);
    free(blob);
    return true;
}

// ---------------------------------------------------------------------------
// mapping probes
// ---------------------------------------------------------------------------

static void EdenProbeMapJIT(EdenJITReport *r) {
    // Informational only on iOS. Eden's oaknut does not use MAP_JIT here: the pairing
    // partner, pthread_jit_write_protect_np, is declared __attribute__((unavailable))
    // in the iOS SDK, so a MAP_JIT page could be created and then never made writable
    // again. docs/IOS_PORT_NOTES.md records that an attempt to support both mechanisms
    // was made and REVERTED for exactly this reason.
    //
    // It is still worth asking, because IOS_PORT_NOTES.md lists the answer as
    // unverified and cemu-ios-muffin observed errno 22 (EINVAL) here while CS_DEBUGGED
    // was set - which suggests "no", but on a differently-signed binary.
    errno = 0;
    void *p = mmap(NULL, (size_t)getpagesize(), PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (p == MAP_FAILED) {
        // MAP_FAILED is (void*)-1, never NULL. docs/IOS_PORT_NOTES.md records that
        // upstream oaknut tested for nullptr here, so a failed mapping was kept as a
        // valid pointer and faulted later somewhere unrelated - and on iOS a failed
        // mapping is the NORMAL outcome, so the common case produced an unattributable
        // crash. Test the right sentinel.
        r->map_jit_succeeded = false;
        r->map_jit_errno = errno;
        return;
    }
    r->map_jit_succeeded = true;
    r->map_jit_errno = 0;
    // Released immediately. Apple documents an arm64 process as having a single JIT
    // region; whether this munmap frees it cleanly for oaknut's later use is the
    // uncertainty recorded in EdenJIT.h, and holding it would only make that worse.
    munmap(p, (size_t)getpagesize());
}

static void EdenProbeMprotect(EdenJITReport *r) {
    // THE PATH THAT MATTERS. This mirrors oaknut::CodeBlock as patched for this port:
    // plain anonymous memory, mapped PROT_READ|PROT_EXEC at map time (not RW then
    // promoted), then toggled RW and back with mprotect.
    const size_t page = (size_t)getpagesize();

    errno = 0;
    void *p = mmap(NULL, page, PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) {
        r->mprotect_rx_succeeded = false;
        r->mprotect_errno = errno;
        return;
    }

    // unprotect(): make it writable so code can be emitted.
    errno = 0;
    if (mprotect(p, page, PROT_READ | PROT_WRITE) != 0) {
        r->mprotect_rx_succeeded = false;
        r->mprotect_errno = errno;
        munmap(p, page);
        return;
    }

    // Write a byte. Writing is not executing, and this is the operation that would
    // fault on a W^X-enforced page.
    *(volatile unsigned char *)p = 0x1F;   // low byte of arm64 NOP; never executed

    // protect(): back to executable. THIS is the call that a process without
    // dynamic-codesigning or CS_DEBUGGED is refused.
    errno = 0;
    if (mprotect(p, page, PROT_READ | PROT_EXEC) != 0) {
        r->mprotect_rx_succeeded = false;
        r->mprotect_errno = errno;
        munmap(p, page);
        return;
    }

    r->mprotect_rx_succeeded = true;
    r->mprotect_errno = 0;

    // AND WE STOP HERE. The page is now RX and holds a byte we wrote. Jumping to it
    // would be the only real proof - and is precisely what must not happen: on iOS the
    // refusal is an uncatchable signal, so a "no" would be indistinguishable from the
    // app dying at launch. cemu's device log ended on "Entering stage 2 - calling into
    // the page" on every launch until that jump was removed.
    munmap(p, page);
}

// ---------------------------------------------------------------------------
// verdict
// ---------------------------------------------------------------------------

static EdenJITVerdict EdenDecide(const EdenJITReport *r) {
    // 1. An explicit dynamic-codesigning entitlement is the strongest answer, and is
    //    the TrollStore case. It survives a relaunch, unlike a debugger attach.
    if (r->entitlements_readable && r->has_dynamic_codesigning && r->mprotect_rx_succeeded) {
        return EdenJITViaEntitlement;
    }

    // 2. CS_DEBUGGED plus a working mprotect is the StikDebug case. Valid for this
    //    launch only.
    if (r->cs_readable && r->cs_debugged && r->mprotect_rx_succeeded) {
        return EdenJITViaDebugger;
    }

    // 3. The entitlement blob was unreadable, but mprotect RX worked and no debugger
    //    is attached. Something granted it, and the only thing that can is the
    //    entitlement - this also covers a DER-only signature whose XML blob we could
    //    not read.
    if (!r->entitlements_readable && r->mprotect_rx_succeeded && !r->cs_debugged) {
        return EdenJITViaEntitlement;
    }

    // 4. mprotect succeeded but NOTHING explains why. Refuse.
    //
    //    This branch is the lesson from a real cemu-ios-muffin shipping build:
    //    CS_DEBUGGED was set, mprotect(R+X) returned 0, the check passed, MAP_JIT then
    //    failed inside the real allocator, the mprotect path was taken, and the first
    //    jump into generated code took signal 10. A mapping syscall succeeding is not
    //    proof that executing the page will succeed, so "it worked and I cannot say
    //    why" is treated as no.
    //
    //    UNKNOWN IS NOT YES. A failed csops is unavailable, never available.
    return EdenJITUnavailable;
}

static void EdenFormatDiagnostics(const EdenJITReport *r) {
    snprintf(g_diagnostics, sizeof(g_diagnostics),
             "eden-ios JIT probe\n"
             "------------------\n"
             "verdict                 : %s\n"
             "\n"
             "csops(CS_OPS_STATUS)\n"
             "  readable              : %s\n"
             "  raw flags             : 0x%08x\n"
             "  CS_VALID              : %s\n"
             "  CS_DEBUGGED           : %s\n"
             "\n"
             "csops(ENTITLEMENTS_BLOB)\n"
             "  readable              : %s\n"
             "  dynamic-codesigning   : %s\n"
             "  cs.allow-jit          : %s\n"
             "  get-task-allow        : %s\n"
             "\n"
             "mmap(MAP_JIT)           : %s (errno %d)\n"
             "  [informational only - oaknut cannot use MAP_JIT on iOS,\n"
             "   pthread_jit_write_protect_np is unavailable in the SDK]\n"
             "\n"
             "anon RX + mprotect RW/RX: %s (errno %d)\n"
             "  [THIS is the mechanism Eden's oaknut actually uses]\n"
             "\n"
             "Nothing was executed. A syscall succeeding is not proof that running\n"
             "generated code will succeed.\n",
             (r->verdict == EdenJITViaEntitlement) ? "JIT via entitlement"
                 : (r->verdict == EdenJITViaDebugger) ? "JIT via debugger (CS_DEBUGGED)"
                                                      : "UNAVAILABLE",
             r->cs_readable ? "yes" : "no",
             r->cs_flags,
             r->cs_valid ? "yes" : "no",
             r->cs_debugged ? "yes" : "no",
             r->entitlements_readable ? "yes" : "no",
             r->has_dynamic_codesigning ? "yes" : "no",
             r->has_allow_jit ? "yes" : "no",
             r->has_get_task_allow ? "yes" : "no",
             r->map_jit_succeeded ? "OK" : "refused", r->map_jit_errno,
             r->mprotect_rx_succeeded ? "OK" : "refused", r->mprotect_errno);
}

// ---------------------------------------------------------------------------
// public
// ---------------------------------------------------------------------------

EdenJITReport eden_jit_probe(void) {
    pthread_mutex_lock(&g_mutex);

    if (atomic_load(&g_locked) && g_have_report) {
        const EdenJITReport cached = g_report;
        pthread_mutex_unlock(&g_mutex);
        return cached;
    }

    EdenJITReport r;
    memset(&r, 0, sizeof(r));

    unsigned int flags = 0;
    errno = 0;
    if (eden_csops(getpid(), EDEN_CS_OPS_STATUS, &flags, sizeof(flags)) == 0) {
        r.cs_readable = true;
        r.cs_flags = flags;
        r.cs_valid = (flags & EDEN_CS_VALID) != 0;
        r.cs_debugged = (flags & EDEN_CS_DEBUGGED) != 0;
    }

    r.entitlements_readable = EdenReadEntitlements(&r);

    EdenProbeMapJIT(&r);
    EdenProbeMprotect(&r);

    r.verdict = EdenDecide(&r);

    g_report = r;
    g_have_report = true;
    EdenFormatDiagnostics(&r);

    pthread_mutex_unlock(&g_mutex);
    return r;
}

EdenJITReport eden_jit_report(void) {
    pthread_mutex_lock(&g_mutex);
    const bool have = g_have_report;
    const EdenJITReport cached = g_report;
    pthread_mutex_unlock(&g_mutex);

    if (have) {
        return cached;
    }
    return eden_jit_probe();
}

bool eden_jit_is_permitted(void) {
    return eden_jit_report().verdict != EdenJITUnavailable;
}

void eden_jit_lock_verdict(void) {
    (void)eden_jit_report();   // make sure there is one to lock
    atomic_store(&g_locked, true);
}

void eden_jit_copy_diagnostics(char *buf, size_t len) {
    if (buf == NULL || len == 0) {
        return;
    }
    (void)eden_jit_report();
    pthread_mutex_lock(&g_mutex);
    strlcpy(buf, g_diagnostics, len);
    pthread_mutex_unlock(&g_mutex);
}
