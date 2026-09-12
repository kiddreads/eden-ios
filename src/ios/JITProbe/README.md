<!--
SPDX-FileCopyrightText: Copyright 2026 eden-ios contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->

# JIT probe

A tiny app that answers the one question the whole eden-ios port rests on:

> **Can this process write machine instructions into memory and then run them?**

Nothing else. It does not link Eden, does not open the GPU, does not need `prod.keys`,
and cannot load a game.

## Why this exists and why it is first

`docs/IOS_PORT_NOTES.md`:

> There is no interpreter. `HAS_NCE` is gated on `ARCHITECTURE_arm64 AND (ANDROID OR
> LINUX)`, and `KProcess::InitializeInterfaces` constructs `ArmDynarmic64`/`ArmDynarmic32`
> unconditionally with no error path. No JIT does not mean slow emulation; it means no
> emulation.

and, under **Unverified**:

> Whether `mprotect(PROT_READ | PROT_EXEC)` on anonymous memory actually succeeds under
> StikDebug's `CS_DEBUGGED` on current iOS, and separately under TrollStore's
> `dynamic-codesigning`. Both are believed to work and neither has been run here. If
> both fail on a device, there is no fallback left.

Everything in `README.md`'s milestone table — the Metal layer, the data root, the
libretro graft, M3 through M6 — is conditional on that unverified line. If the answer is
no, the correct next action is to stop, not to keep building. Finding that out after
weeks of work instead of today is the single most expensive mistake available to this
project, and this app is what makes it a one-day question instead.

It deliberately depends on **nothing**: no CMake tree, no `libeden_libretro.a`, no
MoltenVK. Two Swift files and one C file. It can be built and handed to a tester before
the core links.

## For the tester

You do not need to know what any of this means.

1. Install the IPA the way you normally sideload apps.
2. Open **JIT Probe**.
3. Wait a few seconds.
4. You will see one big word: **PASS** or **FAIL**.
5. Tap **Copy the full report** and paste it back to whoever asked you to run this.

**If the app closes by itself, that is a result, not a bug.** It means iOS refused to run
the instructions, which is exactly one of the things being measured. Open it again — it
remembers what happened and carries on from the next step, and it will never retry the
step that closed it. You may have to reopen it up to four times before it settles on an
answer.

If you were given more than one version (sideload / trollstore / debug / none), **run each
one and send all of the reports**. The difference between them is itself the finding.

If a JIT enabler (StikDebug, SideStore, LiveContainer, or similar) is involved, please say
in your message whether you launched through it or not. The same device gives different
answers either way and the report cannot tell which you did.

The report is also written to the app's `Documents` folder as `jit-probe-report.txt`,
reachable from the **Files** app under *On My iPhone → JIT Probe*. That is the fallback if
the app will not stay open long enough to read.

## For the project: how to read the result

The app tries four different ways of getting runnable memory, in a fixed order. Each row
of the report is one of them.

| # | Shape | Who uses it |
|---|-------|-------------|
| 0 | anonymous `RW`, then `mprotect` to `RX` | **Eden.** `IOS_PORT_NOTES.md`: "oaknut::CodeBlock maps plain anonymous memory and toggles it between RX and RW with mprotect" |
| 1 | anonymous `RX` at map time, `RW` to write, back to `RX` | the exact shape `src/ios/Bridge/EdenJIT.m` probes |
| 2 | `MAP_JIT`, `RWX` at map time | the macOS shape; `IOS_PORT_NOTES.md` says it cannot work on iOS |
| 3 | `MAP_JIT` read-write, then `mprotect` to `RX` | the shape cemu-ios-muffin found working on an A12Z |

**Strategy 0 is the answer. The rest are diagnostics.**

### Outcomes, and what each one means for eden-ios

| Report line | What happened | What it means |
|---|---|---|
| `PASS - ran and returned 42 and 90` | the page executed correctly | For strategy 0: **the port is viable on this device, installed this way.** Proceed. |
| `mmap refused` | the kernel would not hand over the memory | Nothing was executed, no risk was taken. With `errno 1`/`13` this is a permission refusal; try a variant with more entitlements, or a JIT enabler. |
| `could not be made executable` | `mprotect` refused | This process cannot get executable memory at all. No Eden, under any launcher, until the install method changes. |
| `faulted when written to` | the page could not even be written | Expected for strategies 2 and 3 on Apple arm64 — a `MAP_JIT` region is executable-not-writable and only `pthread_jit_write_protect_np` flips it, which an iOS build cannot call. Seeing this is `IOS_PORT_NOTES.md` being **confirmed**. |
| `EXECUTED AND FAULTED` | the jump was taken and hit `SIGBUS`/`SIGILL` | **The important failure.** The syscalls all succeeded and the kernel still refused at instruction fetch. |
| `EXECUTED AND KILLED THE PROCESS` | the app died; recorded by the crash journal | Same meaning as above, one severity worse — the refusal is not catchable here. |
| `ran but returned the wrong value` | something executed, but not what was written | Has never been observed. Would most likely mean the instruction-cache flush is not working, and would need investigating before any other conclusion is drawn from this app. |

### The three results that would change what this project does

1. **Strategy 0 PASSES.** The assumption holds. `IOS_PORT_NOTES.md`'s "Unverified"
   entry can be rewritten as verified, with the device and install method named.

2. **Strategy 0's syscalls succeed but it FAULTS or is KILLED on the jump.** This is the
   case cemu-ios-muffin hit and it has a direct consequence here:
   `src/ios/Bridge/EdenJIT.m` would report this device as JIT-capable, because it
   checks `mprotect` and stops — its own comment says so ("AND WE STOP HERE"). Eden would
   then launch, pass its own check, and die at first code emission. That probe's verdict
   logic would need to become pessimistic, and the app would need to say "unknown", not
   "available".

3. **Strategy 3 PASSES while strategy 0 fails.** Then `IOS_PORT_NOTES.md`'s "on iOS there
   is exactly one mechanism, not two" is wrong for this device, and oaknut's iOS path is
   worth revisiting — with the caveat that Eden still has no way to make a `MAP_JIT` page
   writable again afterwards, so it would be a research result rather than a fix.

### Reading the rest of the report

- **`CS_DEBUGGED`** — set means a debugger has waived signature enforcement for this
  launch (the StikDebug/SideStore case). It is the single most useful line for
  interpreting everything else, because a pass with it set says nothing about a launch
  without it.
- **`P_TRACED`** — reported separately and on purpose. A debugger can be attached without
  the codesigning flag being set, and the flag can outlive the debugger. Two lines means
  the two can be told apart in a bug report instead of guessed at.
- **Entitlements** — read from the *kernel*, via `csops(CS_OPS_ENTITLEMENTS_BLOB)` and
  `SecTaskCopyValueForEntitlement`, **not** from the `.entitlements` files in this
  directory. Those are an input to signing, not evidence that signing kept them. A
  free-developer re-sign is entitled to drop keys, and whether it does is one of the
  things this app exists to establish. `src/ios/Eden.entitlements` records the same open
  question about `increased-memory-limit`.
- **`pthread_jit_write_protect_np exported`** — this symbol is **never called**. It is
  absent from the iOS SDK (`__API_UNAVAILABLE(ios)`) and clang merges availability across
  redeclarations, so naming it is a compile error no local prototype can rescue. Whether
  libsystem still *exports* it is a different question, answered here with `dlsym` on a
  string literal.
- **host page size** — expect `16384`. That is the 16 KiB vs 4 KiB mismatch that turns
  fastmem off, per `IOS_PORT_NOTES.md`.

## What this app does NOT prove

Stated plainly, because a probe that overclaims is worse than none:

- **It does not prove Eden works.** It proves four instructions ran. dynarmic emits
  megabytes of code, patches it in place, and re-protects it thousands of times a second.
  A page that runs once is necessary, not sufficient.
- **It does not prove the next launch will pass.** `CS_DEBUGGED` is per-launch. A pass
  with a debugger attached says nothing about a launch without one.
- **It does not measure speed, memory, or whether a 128 MiB code cache survives jetsam.**
  `IOS_PORT_NOTES.md` lists that last one as unmeasured and it stays unmeasured.
- **A `KILLED` result has one false positive.** The crash journal records "about to jump"
  and is turned into "killed" if the next launch finds it unfinished. Anything else that
  ends the process in that window reads the same way — a force-quit at exactly the wrong
  instant, or a jetsam kill. The window is microseconds wide, so it is unlikely rather
  than impossible. **Run the whole thing again** exists so a suspected false positive can
  be retested instead of argued about.

## The instructions it writes

Two functions, each two instructions, each returning a *different* constant:

```
offset 0:  mov w0, #42   0x52800540      offset 8:  mov w0, #90   0x52800B40
           ret           0xD65F03C0                 ret           0xD65F03C0
```

`MOVZ Wd, #imm16, LSL #0` is encoded `sf(1) opc(2) 100101(6) hw(2) imm16(16) Rd(5)`, so
with `sf=0` (32-bit, so `W0`), `opc=10` (MOVZ), `hw=00`, `imm16=42`, `Rd=0`:

```
0 10 100101 00 0000000000101010 00000  =  0101 0010 1000 0000 0000 0101 0100 0000  =  0x52800540
```

`RET` defaults to `X30`: `1101011 0 010 11111 000000 11110 00000` = `0xD65F03C0`.

Two constants rather than one is the point: a lucky `42` from a stale register or a bare
`RET` will not also be a `90` from eight bytes further on. The derivation is in
`Probe/jit_probe.c`, and it was checked against the assembler rather than trusted:

```
$ printf '.text\n_f:\n mov w0,#42\n ret\n mov w0,#90\n ret\n' | clang -x assembler -c -arch arm64 -o t.o -
$ otool -t t.o
0000000000000000  52800540 d65f03c0 52800b40 d65f03c0
```

(The task this was written from quotes `0x528005400` for the first of these — nine hex
digits, 36 bits. The correct 32-bit encoding is `0x52800540`.)

The instruction cache is flushed with `sys_icache_invalidate` after the write and before
the jump. Apple arm64 cores are not I/D coherent; skipping it does not fail cleanly, it
executes whatever stale bytes the icache held, which is indistinguishable from a JIT
permission failure and would make this app lie.

## How a fatal "no" still gets reported

`src/ios/Bridge/EdenJIT.m` stops one step short of the jump on purpose, and its reasoning
is correct *for the shipping app*: on iOS a refusal can be an uncatchable `SIGKILL`, so a
"no" is indistinguishable from the app dying. cemu-ios-muffin removed its jump for exactly
this reason — its device log ends on "Entering stage 2 — calling into the page" on every
single launch, having learned nothing.

That caution is wrong for a *diagnostic*, because the step it skips is the entire question.
So this app takes the jump, with two independent nets:

1. **A crash journal.** Before each jump, one line is written and `fsync`ed — and the
   directory is `fsync`ed too, because a file that only reached the page cache is not on
   disk when the process stops existing. An uncatchable kill leaves `arming N` behind, and
   the next launch reads it, reports strategy N as killed, and moves on. A death becomes
   data.
2. **A signal handler.** `SIGBUS`/`SIGILL`/`SIGSEGV`/`SIGTRAP` with `sigaltstack` and
   `sigsetjmp`/`siglongjmp`, so a *catchable* refusal — cemu's "signal 10" — is a clean
   FAIL rather than a crash.

Both the jump **and the write** are covered. That is not belt-and-braces: a `MAP_JIT`
region on an APRR core is handed over executable-not-writable, so storing into it faults,
and a journal that recorded only the jump would blame the wrong step and report the exact
opposite of what happened. This was not theoretical — it fired on the first run.

## Building it

No CMake, no core, no `link-flags.txt`. On a runner with Xcode and `xcodegen`:

```sh
cd src/ios/JITProbe
xcodegen generate
xcodebuild -project JITProbe.xcodeproj -scheme JITProbe \
  -sdk iphoneos -configuration Release \
  CODE_SIGNING_ALLOWED=NO build
```

Then package once per entitlements variant, exactly as `.github/workflows/build-ios-app.yml`
does for Eden — one build, re-signed several times:

```sh
APP=<DerivedData>/Build/Products/Release-iphoneos/JITProbe.app
for V in sideload:JITProbe.entitlements \
         trollstore:JITProbe-TrollStore.entitlements \
         debug:JITProbe-Debug.entitlements \
         none:JITProbe-None.entitlements; do
  NAME=${V%%:*}; ENTS=${V##*:}
  rm -rf stage-$NAME && mkdir -p stage-$NAME/Payload
  cp -R "$APP" stage-$NAME/Payload/
  codesign --force --sign - --entitlements "$ENTS" \
    --generate-entitlement-der stage-$NAME/Payload/JITProbe.app
  ( cd stage-$NAME && zip -qry ../JITProbe-$NAME.ipa Payload )
done
```

`--generate-entitlement-der` matters for the same reason it does in Eden's workflow: a
signature carrying only the XML blob reads as declaring no entitlements to some checks.

(The four variants above are illustrative, not what CI actually ships. The `debug` one
re-signs a *Release* binary with `JITProbe-Debug.entitlements`, which declares the same
three keys as `JITProbe.entitlements` — so it comes out byte-for-byte identical to
`sideload` under another name. `.github/workflows/build-jit-probe.yml` packages `unsigned`
in that slot instead — a plain, unsigned copy of the `.app` for SideStore/AltStore to
re-sign themselves — and adds it to `sideload`/`trollstore`/`none` rather than replacing
one of them, so CI ships four IPAs that are four distinct pieces of information.)

**This has never been through `xcodebuild`.** The machine driving this port has no Xcode
(`README.md`: "no local build and there never will be"). What *has* been done is stronger
than nothing and is stated exactly: `Probe/jit_probe.c` compiles clean under
`clang -std=c11 -Wall -Wextra -Wshadow -Wmissing-declarations -Wunused -arch arm64` against
the macOS SDK, and was **compiled, run, and its output read** as a native arm64 binary —
including the crash-journal replay path, driven by planting a journal file by hand. The
Swift and the `project.yml` have not been compiled by anything.

`.github/workflows/build-jit-probe.yml` now does exactly the build/package/verify sequence
above on `macos-15` via `xcodegen` + `xcodebuild`, and it has been reviewed accordingly —
but it has never actually been run by GitHub Actions, because the machine driving this
port cannot start a workflow run either. What was checked instead, all read/replay, no
build: every path the workflow touches exists on disk; `src/ios/JITProbe/project.yml`
parses as YAML with the keys XcodeGen expects; the workflow YAML itself parses; every
step's script is `bash -n` clean; the two independence gates (quoted-include scan,
`project.yml` coupling scan) were re-run by hand against the real files here and pass;
and the packaging + entitlement-verification steps (`Package four IPAs`, `Verify every
IPA`, the `check-entitlements.py` helper written by `Write the verification helpers`) were
exercised end-to-end against a synthetic stand-in `.app` — real `Info.plist`, real
`*.entitlements` files from this directory, `codesign --generate-entitlement-der`, the DER
blob check — and produced four correctly-signed, correctly-verified IPAs. None of that
touches `xcodebuild` itself: whether the Swift, the bridging header, and `jit_probe.c`
actually compile and link as an iOS app is still open until a runner does it.

## Files

| File | |
|---|---|
| `Probe/jit_probe.h` | the API, and the reasoning, at length |
| `Probe/jit_probe.c` | all of the probing; plain C, no Objective-C, no C++ |
| `App/JITProbeApp.swift` | sets the state directory before anything can die |
| `App/ProbeView.swift` | the one screen |
| `JITProbe-Bridging-Header.h` | the C surface Swift sees — scalars and C strings only |
| `project.yml` | XcodeGen spec; depends on nothing |
| `Info.plist` | `UIFileSharingEnabled` is load-bearing here |
| `JITProbe*.entitlements` | four variants; `-None` is the control |

`jp_report`'s string fields live behind accessor functions rather than in the struct.
Swift's C importer turns a `char[3072]` member into a 3072-element tuple, which is a
well-known way to make `swiftc` take minutes or fall over. That is why the Swift-facing
API is flat.
