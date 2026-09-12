// SPDX-FileCopyrightText: Copyright 2026 eden-ios contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// ===========================================================================
// THE ONE QUESTION THIS PORT HAS NEVER ASKED
// ===========================================================================
// docs/IOS_PORT_NOTES.md: "There is no interpreter. HAS_NCE is gated on
// ARCHITECTURE_arm64 AND (ANDROID OR LINUX), and KProcess::InitializeInterfaces
// constructs ArmDynarmic64/ArmDynarmic32 unconditionally with no error path. No JIT
// does not mean slow emulation; it means no emulation."
//
// So the whole port rests on one untested assumption: that this process may make
// memory executable AND THEN RUN IT. Everything else - the Metal layer, the data
// root, the libretro graft - is wasted if the answer is no.
//
// src/ios/Bridge/EdenJIT.m already asks part of it, and stops one step short on
// purpose: "AND WE STOP HERE. The page is now RX and holds a byte we wrote. Jumping
// to it would be the only real proof - and is precisely what must not happen."
// That caution is correct for the shipping app, which must not die at launch.
// It is wrong for a diagnostic, because the step it skips is the entire question.
//
// cemu-ios-muffin learned why, expensively. Its CemuBridge.mm records a shipped build
// where CS_DEBUGGED was set, mprotect(R+X) returned 0, the check passed - and the
// first jump into generated code took signal 10. A mapping syscall succeeding is NOT
// proof that executing the page will succeed. Only executing the page is.
//
// ===========================================================================
// HOW A FATAL "NO" IS STILL REPORTED
// ===========================================================================
// cemu's first attempt at this did jump, and its device log ends on "Entering stage 2
// - calling into the page" on every single launch. A refusal on iOS can arrive as an
// uncatchable SIGKILL, so "no" was indistinguishable from the app simply dying, and
// the probe learned nothing. It was removed for that reason, not because the question
// stopped mattering.
//
// This probe answers it anyway, with two independent safety nets:
//
//   1. A CRASH JOURNAL. Before every jump, a one-line file is written and fsync()ed:
//      "arming <n>". If the process survives, it becomes "survived <n>". An
//      uncatchable kill leaves "arming <n>" on disk, and THE NEXT LAUNCH READS IT and
//      reports strategy n as killed. A death is therefore data, not a dead end.
//
//   2. A SIGNAL HANDLER. SIGBUS/SIGILL/SIGSEGV/SIGTRAP around the jump, with
//      sigaltstack + sigsetjmp/siglongjmp, so a *catchable* refusal (which is what
//      cemu's "signal 10" was) is reported as a clean FAIL instead of a crash.
//
// Consequence for the tester: if the app closes itself, that IS a result. Reopen it
// and it continues from the next strategy. It is never necessary to guess.
//
// ===========================================================================
// WHY FOUR STRATEGIES AND NOT ONE
// ===========================================================================
// Which *mapping shape* a device grants is the single fact that cost cemu the most
// time, and the two source trees here disagree about it:
//
//   docs/IOS_PORT_NOTES.md: "On iOS there is exactly one mechanism, not two.
//   oaknut::CodeBlock maps plain anonymous memory and toggles it between RX and RW
//   with mprotect. [...] The macOS mechanism - MAP_JIT paired with
//   pthread_jit_write_protect_np - is not available on iOS at all."
//
//   cemu-ios-muffin/src/ios/Bridge/CemuBridge.mm (the strategy-B block): on an A12Z,
//   iOS 26.6.1, CS_DEBUGGED set, mmap(MAP_JIT|PROT_EXEC) returned EINVAL but
//   mmap(MAP_JIT, read-write) SUCCEEDED and behaved like a real JIT region.
//
// Both cannot be the whole truth on every device, and neither has been run here. So
// this probe tries each shape separately and reports each result separately. Strategy
// 0 is the one Eden actually uses, and is tried first so that the answer that matters
// most is the one obtained before any riskier jump can kill the process.
//
// pthread_jit_write_protect_np is NEVER CALLED and never named as an identifier. The
// iOS SDK declares it __API_UNAVAILABLE(ios), and clang merges availability across
// redeclarations, so even a local prototype will not build. Its *presence* is reported
// via dlsym() with a string literal, which is a fact worth having and is not a call.
// ===========================================================================

#ifndef EDEN_JIT_PROBE_H
#define EDEN_JIT_PROBE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// How one execution strategy ended.
typedef enum {
    /// Not attempted yet.
    JP_OUTCOME_UNTRIED = 0,
    /// mmap() refused the mapping. Nothing was executed; no risk was taken.
    JP_OUTCOME_MAP_FAILED,
    /// The mapping existed but could not be made executable (mprotect refused).
    /// Nothing was executed.
    JP_OUTCOME_PREPARE_FAILED,
    /// The mapping existed but FAULTED WHEN WRITTEN TO. Nothing was executed.
    ///
    /// This is the expected fate of a MAP_JIT region on Apple arm64: such a region is
    /// handed over executable-not-writable, and the only thing that flips it to
    /// writable is pthread_jit_write_protect_np, which docs/IOS_PORT_NOTES.md
    /// establishes cannot be called from an iOS target at all. Reaching this outcome
    /// on a MAP_JIT strategy is that claim being demonstrated rather than repeated.
    JP_OUTCOME_WRITE_FAULTED,
    /// The jump was taken and the process caught a fatal signal. Answer: no.
    JP_OUTCOME_SIGNALLED,
    /// The jump was taken and the process DIED. Learned from the crash journal on a
    /// later launch. Answer: no, and fatally so.
    JP_OUTCOME_KILLED,
    /// The jump returned, but with the wrong value. Something executed; it was not
    /// what we wrote. Treated as a failure, loudly - this would be the worst of all
    /// possible worlds and has never been observed.
    JP_OUTCOME_WRONG_VALUE,
    /// The jump returned 42 and 90 from two separately encoded functions.
    JP_OUTCOME_PASS
} jp_outcome;

// A 5th strategy was added 2026-09-12 after a real device (iPad8,11, iOS 26.6.1)
// showed strategies 0 and 1 both fail with SIGBUS: mprotect() silently intersects
// a requested protection with the ceiling max_protection set at the ORIGINAL
// mmap() call rather than failing, so asking for PROT_EXEC later than the first
// mmap never actually grants it. Strategy 4 requests every bit at once.
#define JP_STRATEGY_COUNT 5

typedef struct {
    int         index;
    const char *name;      ///< short label for the UI
    const char *detail;    ///< what this shape is and who uses it
    jp_outcome  outcome;
    /// The libc call that refused, e.g. "mmap" or "mprotect(PROT_READ|PROT_EXEC)".
    /// NULL when nothing refused.
    const char *failed_call;
    int         failed_errno;
    int         caught_signal;   ///< 0, or SIGBUS/SIGILL/SIGSEGV/SIGTRAP
    int         returned_first;  ///< expected 42
    int         returned_second; ///< expected 90
    /// vm_region_64's view of the mapping just before the jump. 0 if unavailable.
    unsigned int vm_cur_prot;
    unsigned int vm_max_prot;
    bool         vm_prot_readable;
} jp_strategy_result;

typedef struct {
    // --- csops(CS_OPS_STATUS) ------------------------------------------
    bool     cs_readable;
    int      cs_errno;
    uint32_t cs_flags;
    bool     cs_debugged;          ///< CS_DEBUGGED 0x10000000 - the StikDebug case
    bool     cs_valid;             ///< CS_VALID
    bool     cs_signed;            ///< CS_SIGNED
    bool     cs_adhoc;             ///< CS_ADHOC
    bool     cs_get_task_allow;    ///< CS_GET_TASK_ALLOW
    bool     cs_platform_binary;   ///< CS_PLATFORM_BINARY
    bool     cs_enforcement;       ///< CS_ENFORCEMENT
    bool     cs_used_dlsym;        ///< csops reached via dlsym, else syscall(SYS_csops)

    // --- kernel's own "am I traced" bit, independent of CS_DEBUGGED ------
    bool     traced_readable;
    bool     traced;               ///< P_TRACED

    // --- csops(CS_OPS_ENTITLEMENTS_BLOB): what is REALLY in the signature
    bool     ent_readable;
    int      ent_errno;
    uint32_t ent_length;
    bool     ent_truncated;

    // --- SecTaskCopyValueForEntitlement, resolved by dlsym --------------
    // -1 = could not ask, 0 = absent, 1 = present and true, 2 = present but not true
    bool     sectask_available;
    int      sec_dynamic_codesigning;
    int      sec_allow_jit;
    int      sec_get_task_allow;
    int      sec_increased_memory_limit;

    // --- the macOS write-protect switch: present or not. NEVER called. --
    bool     write_protect_symbol_present;

    // --- device -------------------------------------------------------
    // The strings live behind the jp_*_string() accessors below, NOT in this struct.
    // Swift's C importer turns a `char[3072]` member into a 3072-ELEMENT TUPLE, which
    // is a well-known way to make swiftc take minutes or fall over outright. Keeping
    // jp_report to scalars and small structs is what makes it importable at all.
    uint64_t physical_memory;      ///< sysctl hw.memsize, bytes
    long     page_size;            ///< sysconf(_SC_PAGESIZE). 16384 on Apple silicon.

    // --- execution ----------------------------------------------------
    jp_strategy_result strategies[JP_STRATEGY_COUNT];
    int      next_strategy;        ///< first index still JP_OUTCOME_UNTRIED, or COUNT
    bool     all_done;
    bool     any_pass;             ///< at least one strategy executed correctly
    bool     any_killed;           ///< at least one strategy killed the process
    bool     state_dir_usable;     ///< false => the crash journal cannot be written
} jp_report;

// The strings, kept out of jp_report for the reason given above. Each returns a
// pointer to storage owned by the probe that stays valid for the life of the process
// and is only rewritten by jp_begin().
const char *jp_hw_machine(void);
const char *jp_hw_model(void);
const char *jp_os_version(void);
const char *jp_os_build(void);
/// csops flags decoded to "CS_VALID, CS_DEBUGGED, ..." - never NULL.
const char *jp_cs_flag_names(void);
/// The raw entitlement blob payload, XML in practice. Possibly truncated; see
/// jp_report.ent_truncated. Never NULL.
const char *jp_entitlements_xml(void);
/// Where the crash journal is written, or "" if none was set.
const char *jp_state_directory(void);

/// One strategy's result by index. Returns a zeroed result for an out-of-range index.
/// Exists so Swift never has to index the fixed-size array inside jp_report, which it
/// imports as a tuple.
jp_strategy_result jp_strategy(int index);

/**
 * Where the crash journal and the results file live. Call once, before anything
 * else, with a directory that survives a process death - the app's Documents
 * directory. Without it the probe still runs, but a process that is KILLED during a
 * jump leaves no evidence and the next launch will simply try the same thing again.
 */
void jp_set_state_directory(const char *dir);

/**
 * iOS version as Swift sees it (UIDevice.current.systemVersion). sysctl
 * kern.osproductversion is read first and is normally right; this is the fallback so
 * that the field is never blank in a report a tester pastes somewhere.
 */
void jp_set_os_version_hint(const char *version);

/**
 * Read the environment (csops, entitlements, sysctls) and replay the journal and
 * results from any previous launch. Executes nothing. Safe and idempotent.
 *
 * MUST NOT be called from a static initialiser or +load. docs/IOS_PORT_NOTES.md:
 * "JIT memory is no longer allocated during static initialisation - SpinLockImpl was
 * a namespace-scope global whose constructor mmapped executable memory at dyld load,
 * before main(), so a device without JIT permission killed the app before any UI
 * could explain why."
 */
const jp_report *jp_begin(void);

/**
 * Run AT MOST ONE execution strategy - the next untried one - and return the updated
 * report. THIS IS THE CALL THAT MAY KILL THE PROCESS. Paint the UI first: if the app
 * disappears here, the journal is what tells the next launch what happened.
 *
 * Returns immediately if every strategy has an outcome.
 */
const jp_report *jp_run_next_strategy(void);

/// The current report without running anything.
const jp_report *jp_current_report(void);

/// Forget every result and the journal, so the whole sequence can be run again.
void jp_reset(void);

/// The full human-readable report, for the copy button. Writes at most `len` bytes
/// including the terminator.
void jp_format_report(char *buf, size_t len);

/// One line for the big label at the top of the screen. "PASS", "FAIL" or "TESTING".
const char *jp_headline(void);

// ---------------------------------------------------------------------------
// FLAT ACCESSORS - the entire surface Swift uses.
//
// Deliberately scalars and C strings only. Swift's C importer handles a plain
// `typedef enum` inconsistently across language modes (sometimes a Swift enum,
// sometimes a RawRepresentable struct, sometimes a typealias plus globals), and
// jp_strategy_result is a struct of pointers. Neither has to be imported at all if
// the questions the UI asks are answered in C, and this app is only ever built by CI
// on a machine nobody here can debug on - so the import surface is kept to the things
// that cannot be misinterpreted.
// ---------------------------------------------------------------------------

bool jp_all_done(void);
bool jp_any_pass(void);
/// True if any strategy killed the process outright on a previous launch.
bool jp_any_killed(void);
/// Index of the strategy that jp_run_next_strategy() would attempt.
int  jp_next_strategy(void);
/// False means a fatal result would leave no evidence. Shown in the UI for that reason.
bool jp_journal_writable(void);
bool jp_cs_debugged(void);
/// Number of strategies, so the UI does not hardcode it.
int  jp_strategy_total(void);

/// Human label for strategy `index`, e.g. "anonymous RW, then mprotect to RX".
const char *jp_strategy_label(int index);
/// What happened to it, in a sentence.
const char *jp_strategy_outcome_text(int index);
/// jp_outcome as a plain int, for a caller that wants to branch without the enum.
int  jp_strategy_outcome_code(int index);
bool jp_strategy_is_untried(int index);
bool jp_strategy_is_pass(int index);
/// SIGNALLED or KILLED - the page ran and the process paid for it.
bool jp_strategy_is_fatal(int index);
/// True only for strategy 0, the shape Eden itself uses. The UI says so.
bool jp_strategy_is_decisive(int index);

/// True when at least one strategy both mapped and executed correctly.
bool jp_jit_works(void);

#ifdef __cplusplus
}
#endif

#endif // EDEN_JIT_PROBE_H
