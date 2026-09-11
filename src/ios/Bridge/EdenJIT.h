// SPDX-FileCopyrightText: Copyright 2026 eden-ios contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Is this process allowed to make memory executable, and if so by what mechanism.
//
// Adapted from cemu-ios-muffin/src/ios/Bridge/CemuBridge.mm's ios_jit_is_permitted()
// (lines ~760-1035), which is the hard-won working version. Eden-specific, standalone,
// and plain C rather than Objective-C++ - it needs no C++ and the app target is
// deliberately C-only (see src/ios/project.yml).
//
// ===========================================================================
// WHY THIS MATTERS MORE HERE THAN IN MOST EMULATORS
// ===========================================================================
// docs/IOS_PORT_NOTES.md: "There is no interpreter. HAS_NCE is gated on
// ARCHITECTURE_arm64 AND (ANDROID OR LINUX), and KProcess::InitializeInterfaces
// constructs ArmDynarmic64/ArmDynarmic32 unconditionally with no error path. No JIT
// does not mean slow emulation; it means no emulation."
//
// So there is nothing to fall back to. The only useful thing to do with a "no" is to
// say so clearly, before the user watches the app die at first code emission with no
// explanation.
//
// ===========================================================================
// THIS PROBE NEVER EXECUTES ANYTHING
// ===========================================================================
// cemu's first version proved the question properly, by jumping into the test page. On
// iOS a code-signing violation arrives as an uncatchable signal, so "no" became
// indistinguishable from the app simply dying - its device log ends on "Entering stage
// 2 - calling into the page" on every single launch. That comment is carried over
// verbatim in spirit: syscalls only, no jump, ever.
//
// The consequence is stated honestly in the UI: a syscall succeeding is not proof that
// executing the page will succeed. cemu shipped a build where CS_DEBUGGED was set,
// mprotect(R+X) returned 0, this check passed, and the first jump into generated code
// took signal 10.
//
// ===========================================================================
// THE MECHANISM EDEN ACTUALLY USES
// ===========================================================================
// docs/IOS_PORT_NOTES.md is unusually specific, and it contradicts the macOS folklore:
//
//   "On iOS there is exactly one mechanism, not two. oaknut::CodeBlock maps plain
//    anonymous memory and toggles it between RX and RW with mprotect. That requires the
//    process to be allowed to do it: dynamic-codesigning (TrollStore), or being
//    debugged, which sets CS_DEBUGGED (StikDebug).
//
//    The macOS mechanism - MAP_JIT paired with pthread_jit_write_protect_np - is not
//    available on iOS at all. pthread_jit_write_protect_np is not merely restricted
//    there; it is absent from the iOS SDK, declared __attribute__((unavailable)), so
//    even naming it is a compile error."
//
// So this probe tests the MPROTECT path as the one that matters. It also tries
// mmap(MAP_JIT) separately, because the answer is genuinely useful: IOS_PORT_NOTES.md
// lists "whether mmap(MAP_JIT) succeeds on a CS_DEBUGGED process that lacks the JIT
// entitlement" as unverified, and this is the instrument that answers it. But a
// MAP_JIT success is reported as a DIAGNOSTIC, not as a verdict, because Eden's oaknut
// cannot use MAP_JIT on iOS - it has no way to unprotect the page afterwards.

#ifndef EDEN_JIT_H
#define EDEN_JIT_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    /// dynamic-codesigning is in the signature. TrollStore-class install.
    EdenJITViaEntitlement = 0,
    /// CS_DEBUGGED is set. StikDebug / SideStore-class install, this launch only.
    EdenJITViaDebugger = 1,
    /// No mechanism found, or the answer could not be established. There is no
    /// emulation in this state - see the header comment.
    EdenJITUnavailable = 2,
} EdenJITVerdict;

typedef struct {
    EdenJITVerdict verdict;

    /// Raw csops(CS_OPS_STATUS) flags, or 0 if the call failed.
    unsigned int cs_flags;
    bool cs_readable;
    bool cs_debugged;          ///< CS_DEBUGGED
    bool cs_valid;             ///< CS_VALID

    /// Entitlements read back from the embedded signature blob.
    bool entitlements_readable;
    bool has_dynamic_codesigning;
    bool has_allow_jit;
    bool has_get_task_allow;

    /// mmap(MAP_JIT) result. Informational on iOS - see the header.
    bool map_jit_succeeded;
    int map_jit_errno;

    /// The path oaknut actually takes: plain anonymous RX, then mprotect RW/RX.
    bool mprotect_rx_succeeded;
    int mprotect_errno;
} EdenJITReport;

/**
 * Run the probe and cache the result.
 *
 * MUST be called from real app startup (the App's init, or a first-view task) and
 * NEVER from a static initialiser or a +load. docs/IOS_PORT_NOTES.md records why:
 * "JIT memory is no longer allocated during static initialisation - SpinLockImpl was a
 * namespace-scope global whose constructor mmapped executable memory at dyld load,
 * before main(), so a device without JIT permission killed the app before any UI could
 * explain why." Probing from a static initialiser would reintroduce exactly that.
 *
 * Safe to call again - "Check again" is a real button, because CS_DEBUGGED can appear
 * mid-process when a debugger attaches to an already-running app.
 */
EdenJITReport eden_jit_probe(void);

/// The cached report, probing once if nothing has yet.
EdenJITReport eden_jit_report(void);

/// Convenience: verdict != EdenJITUnavailable.
bool eden_jit_is_permitted(void);

/**
 * Freeze the verdict. Called once emulation starts.
 *
 * Apple documents an arm64 process as having a single JIT region, and I could not
 * verify that a probe mapping released with munmap frees it cleanly rather than
 * consuming or contending with oaknut's later allocation. So the probe munmaps at
 * once, is never run on a timer, and stops running entirely after this call.
 */
void eden_jit_lock_verdict(void);

/// Multi-line raw diagnostics (flag values in hex, per-step errno). For the
/// "Copy diagnostics" button. Writes at most `len` bytes including the terminator.
void eden_jit_copy_diagnostics(char *buf, size_t len);

// The literal entitlement keys the probe looks for. If src/ios/*.entitlements ever
// uses different names, change these too or the probe reports "no entitlement" for a
// build that has one.
#define EDEN_JIT_ENTITLEMENT_DYNAMIC_CODESIGNING "dynamic-codesigning"
#define EDEN_JIT_ENTITLEMENT_ALLOW_JIT           "com.apple.security.cs.allow-jit"
#define EDEN_JIT_ENTITLEMENT_GET_TASK_ALLOW      "get-task-allow"

#ifdef __cplusplus
}
#endif

#endif // EDEN_JIT_H
