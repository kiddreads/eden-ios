# eden-ios

An iOS and iPadOS port of [Eden](https://git.eden-emu.dev/eden-emu/eden), a Nintendo Switch emulator.

This is a **port**, not a fork of somebody else's port. Eden's core is C++ and has never run on
iOS; the work here is making it do that.

## Status — 2026-09-11

**No game has ever run. No frame has ever reached a screen. Nothing in this repository has ever
executed on an iOS device.** What is true so far is that parts of the emulator compile for
`iphoneos` arm64, and that a large amount of code has been written around them which no machine
has run.

Everything is built on GitHub Actions runners. The machine driving this port has no Xcode and
roughly 300 MB of free disk, so there is no local build and there never will be. That is also why
the distinction below matters more here than in a normal project: nobody on this side can just
run the thing and look.

### How to read every claim in this file

Three labels, used consistently. If a claim carries no label, treat it as the third one.

| Label | Means |
|---|---|
| **CI-verified** | A GitHub Actions job did it and the log says so. Only ever about compiling, archive contents, and bundle structure — never about behaviour. |
| **Written, not executed** | The code exists and was checked against the real headers on disk. No machine has run it. Reviewing is not running. |
| **Reasoned** | A conclusion drawn from reading source. Could be wrong in a way no amount of re-reading would reveal. Each one below says what would falsify it. |

Nothing here is **behaviour-verified**, because that requires a device and no device has been
used yet.

### Where each piece actually stands

| Area | State | What exists |
|---|---|---|
| Core cross-compile | **CI-verified** | 10 core targets build for `iphoneos` arm64 (`build-ios-core.yml`) |
| JIT backend present in the archive | **CI-verified** | `libdynarmic.a` symbol check, run in both workflows |
| libretro core links | **CI-verified** | `eden_libretro` builds for `iphoneos` arm64 |
| App shell, bridge, Metal layer view | **Written, not executed** | `src/ios/App`, `src/ios/Bridge` — 24 files |
| App target link + IPA packaging | **Written, CI incomplete** | `build-ios-app.yml`; see "the app job has not gone green yet" below |
| Audio sink | **Written, not executed** | `src/libretro_core/retro_audio.cpp` — no sample has ever been played |
| Input: pads, touch, on-screen controls | **Written, not executed** | `retro_input.cpp`, `src/ios/App/*Input*.swift`, `OnScreenControlLayer.swift` |
| Content: keys, firmware, game paths | **Written, not executed** | `retro_content.cpp`, key/firmware status the UI can report |
| JIT probe app | **Written, never built** | `src/ios/JITProbe` — the app that is supposed to settle the JIT question |
| MoltenVK feature audit | **Reasoned** | `docs/MOLTENVK.md` — MoltenVK's source was read, never run |
| Device install (M3) | **Not started** | no IPA has been put on hardware |
| Homebrew boot (M4), retail boot (M5), frame pace (M6) | **Not started** | — |

### The app job has not gone green yet

`build-ios-app.yml` is committed and configured, but its one recorded run failed: it got through
configure, the core build, the `libdynarmic.a` check, the MoltenVK fetch and XcodeGen, and then
failed compiling Swift on `'sharedView' has been renamed to 'shared'` — the ObjC importer strips
the type-name suffix from a class property. Two call sites, fixed in `954b3e7`.

So as of this writing:

- **no run has ever reached the linker.** The link of ~60 static archives into an app binary is
  written and unproven, not proven.
- **no `.ipa` has ever been produced**, by CI or anywhere else, so `ci/verify-ipa.sh` — the gate
  that checks bundle structure, entitlement DER blobs, and that the four `retro_*` symbols really
  survived into the binary — has never gated a real artifact.

The packaging step produces **four** IPAs, not three: `unsigned`, plus ad-hoc re-signs for
`sideload`, `trollstore` and `debug` entitlements, all four verified before upload.

### Milestones

| Milestone | Gate | State | Evidence |
|---|---|---|---|
| M0 | Eden's source mirrored into this repo, CI in place | **met** | repo + workflows |
| M1 | Eden's core libraries cross-compile for `iphoneos` arm64 | **met** | CI: 10 targets, Arm64 symbols in `libdynarmic.a` |
| M2 | A libretro core links for iOS | **met** | CI: `eden_libretro` builds for iphoneos arm64 |
| M3 | An installable artifact launches on a real device | **half met** | app links, 17/17 verify-ipa.sh checks pass, IPAs published (`app-v1`) — not yet confirmed launching on a device |
| M4 | A homebrew `.nro` boots and something it draws reaches the screen | not started | — |
| M5 | A retail title boots | not started | — |
| M6 | Input, audio, and a stable frame pace | not started | — |

M3 is the one that is easy to overclaim. There is a lot of app code now. None of it has run.

### M1 detail — what compiles for iphoneos arm64

CMake configure is clean and **all ten core targets build**:

`dynarmic` · `common` · `core` · `hid_core` · `audio_core` · `shader_recompiler` ·
`video_core` · `network` · `input_common` · `frontend_common`

plus `eden_libretro` (M2), grafted from suyu. The whole 85 KB graft produced exactly one compile
error on its first attempt — a vexing parse, `void(Qualified::Name())` reading as a declaration —
because every Eden API it calls was checked against the real headers rather than assumed to match
suyu's.

"It compiled" is not the claim, though, so CI also inspects the archive:

```
archive: build/src/dynarmic/src/dynarmic/libdynarmic.a (4.7M)
objects: 68
symbols matching Arm64: 2003
symbols matching A64:   779
```

That check exists because it was needed. An earlier run reported `dynarmic | BUILT` for an
archive containing **no backend at all**: an iOS toolchain must set `CMAKE_OSX_ARCHITECTURES`,
which made `DetectArchitecture.cmake` declare a single-architecture build "multiarch", which
routed every arm64 source through a wrapper emitting `#if defined(ARCHITECTURE_ARM64)` while
the build defined `ARCHITECTURE_arm64`. Every backend file compiled to an empty translation
unit and linked cleanly. A green build of an empty archive is worse than a red one.

**What this does not mean.** The core compiles and the recompiler is in the archive. Nothing has
run, no app binary has been linked, and no device has seen any of it.

## What has been written since M2, and what it has not done

Six lanes landed in `954b3e7`. Every one of them is **written, not executed** — that sentence is
the status, not a caveat on it.

- **App shell and bridge.** SwiftUI shell, an Objective-C bridge owning every libretro call (plain
  ObjC rather than ObjC++, because `eden_libretro.h` is `extern "C"` and `libretro.h` is plain C),
  a `UIView` whose `+layerClass` is `CAMetalLayer`, three entitlement sets, and the IPA workflow.
  Before the first CI attempt, what could be checked without Xcode was: both YAML files parse, the
  shell scripts pass `bash -n`, the plists pass `plutil -lint`, every C symbol Swift calls is
  declared in the bridging header, and every declared bridge symbol has a definition.
- **Audio.** A real libretro sink rather than silence: registering a `Sink` is the only way a
  frontend gets PCM out of Eden, so the core sums every running stream through
  `SinkStream::ProcessAudioOutAndRender`. Format needed no conversion and both halves were checked
  against the source — 48 kHz `static_assert`ed against the sink's rate, stereo downmix already
  done by `AppendBuffer`, interleaved s16 exactly as `retro_audio_sample_batch_t` wants, volume
  already applied upstream. Whether the override hook is reachable from a stock build was the open
  end at commit time and is being closed in a parallel lane. Either way: **no sample has ever been
  played.**
- **Input.** The stranded-buttons defect fixed by tracking the binding and releasing the whole old
  port, plus touch and on-screen controls.
- **Content.** The paths-latch disagreement fixed, and key/firmware status the UI can tell the
  truth with. Every path is cited against the code that opens it. No key, firmware or game is
  shipped, downloaded or generated by any of it.
- **JIT probe.** A standalone diagnostic that mmaps, writes arm64, mprotects to exec, *calls* it,
  and reports which step failed with `errno`, across four mapping strategies. It deliberately
  depends on nothing — two Swift files and one C file, no CMake, no MoltenVK. It has never been
  through `xcodebuild`; a CI job to package it is in progress in a parallel lane. **Until it runs
  on real hardware it has answered nothing.**
- **MoltenVK.** The fetch script no longer guesses at an asset name, and `docs/MOLTENVK.md` records
  what Eden requires against what MoltenVK provides — by reading MoltenVK's source, not by running
  it.

## What was fixed, and why it mattered

Three of these were bugs in upstream Eden that only an iOS build exposes:

- **`DetectArchitecture.cmake`** used `foreach(ARCH IN ${CMAKE_OSX_ARCHITECTURES})`. `IN` must be
  followed by `LISTS` or `ITEMS`, so this was a hard CMake error for anyone who set that variable.
  Desktop macOS builds usually leave it unset and skip the branch; an iOS toolchain must set it.
- **`device_power_state.cpp`** guarded IOKit power-sources with `#if TARGET_OS_MAC`. That macro is
  `1` on *every* Apple platform including iOS — it means "Mac family", not "macOS". `TARGET_OS_OSX`
  is the desktop-only one.
- **`host_memory.cpp`** included `<sys/random.h>` on Apple. It is not in the iOS SDK, and nothing
  in the file used it.

Plus genuinely iOS-specific work:

- **The platform library block** required `Carbon` and `Cocoa`, neither of which exists in the
  iPhoneOS SDK. iOS now gets its own list, with UIKit.
- **Boost.Process** calls `wordexp()`, which the iOS SDK declares but marks unavailable. Boost
  already has a no-`wordexp` fallback for OpenBSD and Android; a patch in `.patch/boost/` extends
  that to iOS rather than pinning a fork.
- **oaknut tested `mmap` failure against `nullptr`.** `mmap` reports failure as `MAP_FAILED`
  (`(void*)-1`), so a failed mapping was kept as a valid pointer and faulted later somewhere
  unrelated. On iOS a failed mapping is the *normal* outcome when JIT is not permitted, so the
  common case produced an unattributable crash. Fixed for every POSIX platform, not just iOS.
- **JIT memory is no longer allocated during static initialisation.** `SpinLockImpl` was a
  namespace-scope global whose constructor mmapped executable memory at dyld load, before
  `main()` — so a device without JIT permission killed the app before any UI could explain why.
- **`SetAppDirectory()` now honours its argument on iOS**, instead of assigning over it and
  resolving everything into a dot-hidden XDG path invisible to Files.app — which is exactly where
  a user has to put `prod.keys` and firmware.
- **Fastmem is off on iOS** (below).

## The hard constraints

These are the real obstacles. They are stated here so nobody rediscovers them.

### Host page size — solved, at a cost

The Switch's guest page size is 4 KiB. Apple silicon's host page size is **16 KiB**. yuzu-lineage
emulators lean on "fastmem", mapping guest memory directly into the host address space so a guest
load becomes a host load — and that needs host pages no larger than guest pages.
`HostMemory::Impl::Init()` asserts exactly this (`page size ... is incompatible with 4K paging`).

It cannot work here, so iOS joins the existing "platform doesn't support fastmem" category that
OpenOrbis and managarm already use. dynarmic then falls back to the page-table MMU automatically,
because it already does that whenever `fastmem_pointer` is `nullopt`. **This is a real, permanent
performance cost, not a free win.** Correctness first.

### JIT — the thing that decides whether any of this matters

Eden's CPU layer is dynarmic (`src/dynarmic`), which recompiles guest ARM64 into host ARM64 at
runtime. **There is no interpreter fallback**: `HAS_NCE` is gated on `ARCHITECTURE_arm64 AND
(ANDROID OR LINUX)`, and `KProcess::InitializeInterfaces` constructs `ArmDynarmic64`/`ArmDynarmic32`
unconditionally with no error path. No JIT does not mean slow emulation; it means **no emulation**.

On iOS there is exactly **one** mechanism, not two. `oaknut::CodeBlock` maps plain anonymous
memory and toggles it between RX and RW with `mprotect`. The macOS mechanism — `MAP_JIT` paired
with `pthread_jit_write_protect_np` — is not merely restricted on iOS, it is **absent from the iOS
SDK**, declared `__attribute__((unavailable))`, so even naming it is a compile error. An attempt to
support both was made and reverted for exactly that reason.

What remains is whether the *process* is permitted to have executable memory at all:

- **Newer iOS** — StikDebug attaches a debugger, which sets `CS_DEBUGGED`.
- **Older iOS** — TrollStore, with `dynamic-codesigning` genuinely embedded in the signature.

Entitlements only exist inside a code signature, so an unsigned build carries none. That is why
the artifact ships in more than one variant.

**This is the load-bearing assumption of the entire project, and it is Reasoned, not verified.**
Nobody here has watched `mprotect(PROT_READ | PROT_EXEC)` succeed on an iPhone.

> **What would falsify it.** Run `src/ios/JITProbe` on real hardware, one report per install
> method. Strategy 0 (anonymous RW → `mprotect` to RX → call) is the one Eden depends on; the
> other three are diagnostics. `could not be made executable` or `EXECUTED AND FAULTED` on
> strategy 0 under *both* StikDebug and TrollStore means there is no fallback left and the correct
> next action is to stop, not to keep building. Everything below M2 in the milestone table is
> conditional on that one line.

### Graphics — worse than this file used to claim

Eden renders through Vulkan; iOS has no Vulkan, only Metal via MoltenVK. The surface path is fine:
`vulkan_common/vulkan_surface.cpp` already builds a `VkMetalSurfaceCreateInfoEXT` from a
`CAMetalLayer*` under `#elif defined(__APPLE__)`, which is true on iOS. The frontend reports
`WindowSystemType::Cocoa` and hands over a real layer in physical pixels. There is **no headless
path** — `RendererVulkan` initialises `surface(CreateSurface(...))` in its member-initialiser list
and `CreateSurface` throws on an unmatched window type, so a headless window aborts GPU init.

What this file — and `docs/IOS_PORT_NOTES.md`, under "things that are already handled" — called
handled is not. `docs/MOLTENVK.md` Part 0 supersedes both, with `path:line` citations; the port
notes have not been updated yet and their owner should do it:

- **The `IsMoltenVK()` feature whitelist in `vulkan_device.cpp` is dead code.** `IsMoltenVK()`
  reads `properties.driver.driverID`, which is only filled by the `GetProperties2` call at
  `vulkan_device.cpp:1234` — *after* the mandatory-feature loop at `:1147-1172` that consults it.
  At that point the comparison is `0 == 14`, false on every device and every platform. Reasoned by
  reading the source; see `docs/MOLTENVK.md` Part 0.
- It is not currently a boot failure, because `suitable` is advisory: `vulkan_device.cpp:473-474`
  logs "Unsuitable driver - continuing anyways" and nothing reads the result. So the expected first
  device log is four `Missing required feature` lines plus that warning — **that output is normal,
  not the failure.** The corollary is worse: because it tolerates everything, a genuinely fatal gap
  will not announce itself either.
- Of Eden's 29 mandatory features, five are not unconditionally true on MoltenVK, and four of them
  are the four already named. The uncovered one is **`multiViewport`**, true only from
  `MTLGPUFamilyApple5` — the A12, i.e. iPhone XS/XR and the 2018 iPad Pro. That GPU-family-to-chip
  mapping is inferred from Apple's published table, not read out of a file.

> **What would falsify this.** A first device run whose log does *not* contain those four
> `Missing required feature` lines would mean the ordering bug is not what it looks like. A device
> older than A12 that renders anyway would falsify the `multiViewport` floor; one that fails at
> pipeline creation would confirm it. Both need a device; neither can be settled by more reading.

The `vulkan_device.cpp` ordering bug is recorded, not fixed — it belongs to whoever owns that file.

### Memory

The Switch gives games about 3.2 GB. iOS kills apps that ask for too much, so builds need
`com.apple.developer.kernel.increased-memory-limit`, and
`com.apple.developer.kernel.extended-virtual-addressing` on some devices.

Unmeasured: dynarmic's code cache is 128 MiB per core. Whether iOS jetsam counts a large RX
mapping as dirty against the app's footprint decides whether this fits on a 6 GB device at all.

> **What would falsify this.** Nothing in this repository. It needs a memory-graph capture from a
> device under load. Reasoning about jetsam is not evidence about jetsam.

### Other things reasoned but never run

Collected so they are not mistaken for established facts:

- Whether a second `Core::System::Load` after `ShutdownMainProcess` works. Read from the source,
  never executed. Falsified by loading a second game in one session.
- Whether `retro_run` off the main thread behaves as expected under iOS's watchdog.
- Whether MoltenVK's tessellation-as-compute and absent transform feedback matter for real titles.

## Devices

iPhone and iPad are both first-class: A-series and M-series get the same effort. The deployment
target is iOS 15, kept as low as the code allows so older devices are not excluded, with newer OS
features gated behind availability checks rather than by raising the floor.

Note the tension, rather than hiding it: a deployment target of 15.0 lets pre-A12 hardware
*install* the app, while the `multiViewport` finding above suggests it will not render on it.
Whether that becomes a hard floor is a decision nobody should make before a device has been tried.

## What you must provide yourself

This repository contains no Nintendo code, keys, firmware, or games, and never will. To run
anything beyond homebrew you need your own `prod.keys`, your own firmware dump, and your own game
dumps, taken from hardware you own.

## Relationship to upstream Eden

Nothing here goes upstream. Eden's `CLAUDE.md` prohibits AI-assisted contributions to their
codebase and community, so this fork does not file issues or pull requests there. Eden is
GPL-3.0 and forking is within that licence; the maintainers' preference about contributions is
respected by simply not making any.

## Licence

Eden is GPL-3.0-or-later; this port inherits it. See `LICENSE.txt`.

---

*If a claim in this file is wrong, the code changed — fix this file. A claim that moved from
"written" to "verified" without a log to point at is the failure mode this document exists to
prevent.*
