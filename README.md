# eden-ios

An iOS and iPadOS port of [Eden](https://git.eden-emu.dev/eden-emu/eden), a Nintendo Switch emulator.

This is a **port**, not a fork of somebody else's port. Eden's core is C++ and has never run on
iOS; the work here is making it do that.

## Status — 2026-09-11

**No game has ever run. No frame has ever reached a screen.** What is true so far is that parts
of the emulator now compile for `iphoneos` arm64. This file states what is actually verified, not
what is intended; if a claim here is wrong, the code changed, so fix this file.

Everything is built on GitHub Actions runners. The machine driving this port has no Xcode and
under a gigabyte of free disk, so there is no local build and there never will be.

### Milestones

| Milestone | Gate | State |
|---|---|---|
| M0 | Eden's source mirrored into this repo, CI in place | **met** |
| M1 | Eden's core libraries cross-compile for `iphoneos` arm64 | **partly met** — 4 of 10 targets |
| M2 | A libretro core links for iOS | not started |
| M3 | An installable artifact launches on a real device | not started |
| M4 | A homebrew `.nro` boots and something it draws reaches the screen | not started |
| M5 | A retail title boots | not started |
| M6 | Input, audio, and a stable frame pace | not started |

### M1 detail — what compiles for iphoneos arm64

CMake configure is **clean**. These targets build:

| Target | State |
|---|---|
| `common` | builds |
| `dynarmic` | builds — this is the ARM64 JIT |
| `network` | builds |
| `shader_recompiler` | builds |
| `core` | blocked |
| `hid_core` | blocked |
| `audio_core` | blocked |
| `video_core` | blocked |
| `input_common` | blocked |
| `frontend_common` | blocked |

All six blocked targets fail for a single shared reason — Boost.Process not compiling — not for
six different reasons. See the commit history.

## What has actually been fixed, and why it mattered

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
because it already does that whenever `fastmem_pointer` is `nullopt`. **This is a real performance
cost, not a free win.** Correctness first.

### JIT — the thing that decides whether this is usable at all

Eden's CPU layer is dynarmic (`src/dynarmic`), which recompiles guest ARM64 into host ARM64 at
runtime. **There is no interpreter fallback**: no JIT means no emulation, not slow emulation.

The good news is that Apple's W^X rules are already handled. dynarmic's arm64 backend calls
`mem.protect()` / `mem.unprotect()` under `__APPLE__`, and oaknut (the assembler) does the
`MAP_JIT` work, because Eden already supports macOS on Apple silicon, which has the same rules.

What iOS adds is that the *process* must be permitted to have JIT memory at all:

- **Newer iOS** — StikDebug attaches a debugger, which sets `CS_DEBUGGED`, which makes `MAP_JIT`
  pages usable.
- **Older iOS** — TrollStore, with the entitlements genuinely embedded in the signature.

Entitlements only exist inside a code signature, so an unsigned build carries none. That is why
the eventual artifact has to come in more than one variant.

### Graphics — better than expected

Eden renders through Vulkan, and iOS has no Vulkan, only Metal via MoltenVK. But
`vulkan_common/vulkan_surface.cpp` already has a `VkMetalSurfaceCreateInfoEXT` path that takes a
`CAMetalLayer*`, under `#elif defined(__APPLE__)` — which includes iOS. A frontend sets
`WindowSystemType::Cocoa` and hands it a layer from a `UIView`.

What is *not* established is which Vulkan features Eden requires that MoltenVK lacks. Geometry
shaders and transform feedback are the usual casualties on Apple GPUs.

### Memory limits

The Switch gives games about 3.2 GB. iOS kills apps that ask for too much, so builds need
`com.apple.developer.kernel.increased-memory-limit`, and
`com.apple.developer.kernel.extended-virtual-addressing` on some devices.

## Devices

iPhone and iPad are both first-class: A-series and M-series get the same effort. The deployment
target is iOS 15, kept as low as the code allows so older devices are not excluded, with newer OS
features gated behind availability checks rather than by raising the floor.

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
