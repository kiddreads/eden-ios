<!--
SPDX-FileCopyrightText: Copyright 2026 eden-ios contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->

# iOS port notes

Things about this port that are not obvious from the code, each verified against the source
rather than assumed. If something here is wrong, the code changed — fix this file.

Written for whoever builds the iOS app shell around the core.

## The app must do four things the core cannot do for itself

### 1. Give the core a real `CAMetalLayer`

Not optional, and not "it falls back to software". `RendererVulkan`'s constructor initialises
`surface(CreateSurface(instance, render_window.GetWindowInfo()))` in its **member initialiser
list** (`src/video_core/renderer_vulkan/renderer_vulkan.cpp`), and `CreateSurface` ends with:

```cpp
if (!unsafe_surface) {
    LOG_ERROR(Render_Vulkan, "Presentation not supported on this platform");
    throw vk::Exception(VK_ERROR_INITIALIZATION_FAILED);
}
```

`WindowSystemType::Headless` matches no branch, so it throws and GPU init aborts. There is no
headless path on Eden.

The good news is that the branch we need already exists. `vulkan_surface.cpp` builds a
`VkMetalSurfaceCreateInfoEXT` with `.pLayer = static_cast<const CAMetalLayer*>(render_surface)`
for `WindowSystemType::Cocoa`, under `#elif defined(__APPLE__)` — which is true on iOS. So:

- report `WindowSystemType::Cocoa` (yes, on iOS — see the caveat below),
- pass an actual `CAMetalLayer*`, not a `UIView*`. The simplest correct source is a `UIView`
  whose `+layerClass` returns `CAMetalLayer`, then `view.layer`,
- pass sizes in **physical pixels**, already multiplied by `contentsScale`.

*Caveat:* reusing `Cocoa` for iOS is correct today only because Eden's Apple branches are
`#elif defined(__APPLE__)` and macOS and iOS take the same one. If upstream ever splits them,
this breaks quietly.

### 2. Tell the core where it may write

`Common::FS::SetAppDirectory()` used to be a no-op on Apple platforms. `Reinitialize()`'s
desktop branch opened with `eden_path = GetCurrentDir() / PORTABLE_DIR;`, assigning straight
over its own argument, so the path was discarded and everything resolved through XDG into
`$HOME/.local/share/eden` inside the container. Writable, so nothing looked broken — but
dot-hidden, therefore invisible to Files.app, which is exactly where a user has to put
`prod.keys` and firmware.

An iOS branch now honours the argument, the same way Android does. **Call it before loading
anything**, with a directory inside the app container that is visible to Files.app.

### 3. Keep `retro_run` off the main thread

It blocks waiting on the frame signal. iOS kills an app whose main thread stops responding.

### 4. Hold security-scoped bookmarks

A ROM outside the container needs its security-scoped resource held for the whole session, not
just at open time.

## JIT: the thing that decides whether any of this is usable

**There is no interpreter.** `HAS_NCE` is gated on `ARCHITECTURE_arm64 AND (ANDROID OR LINUX)`,
and `KProcess::InitializeInterfaces` constructs `ArmDynarmic64`/`ArmDynarmic32` unconditionally
with no error path. No JIT does not mean slow emulation; it means no emulation.

Two mechanisms are legitimate on iOS, and which one is available depends on how the app was
installed, so `oaknut::CodeBlock` tries both:

| Mechanism | Needs | Typical install |
|---|---|---|
| `MAP_JIT` + `pthread_jit_write_protect_np` | JIT entitlement genuinely in the signature | TrollStore |
| plain anonymous memory toggled RX↔RW with `mprotect` | `dynamic-codesigning`, or being debugged (`CS_DEBUGGED`) | StikDebug |

`MAP_JIT` is tried first because its toggle is a per-thread register write rather than a syscall
over the whole mapping. Whichever succeeded is remembered, because `protect()`/`unprotect()` must
match the mapping, not the platform.

Two hazards worth knowing:

- **`pthread_jit_write_protect_np` is per-thread.** With `use_multi_core` on, a cross-thread
  unprotect is a silent no-op and the subsequent write faults. The `mprotect` mode is per-mapping
  and does not have this problem. This is unresolved and needs a device to settle.
- **Entitlements only exist inside a code signature.** An unsigned build carries none, which is
  why the artifact has to ship in more than one variant.

JIT memory is no longer allocated during static initialisation — `SpinLockImpl` was a
namespace-scope global whose constructor mmapped executable memory at dyld load, before `main()`,
so a device without JIT permission killed the app before any UI could explain why.

## Memory

- Host pages are **16 KiB** on Apple silicon; the Switch guest uses **4 KiB**. The fastmem arena
  maps guest pages directly into the host address space and cannot work, so iOS joins the
  existing "platform doesn't support fastmem" category and dynarmic uses the page-table MMU.
  This is a real performance cost.
- The Switch gives games ~3.2 GB. Builds need
  `com.apple.developer.kernel.increased-memory-limit`, and
  `com.apple.developer.kernel.extended-virtual-addressing` on some devices.
- Unmeasured: the code cache is 128 MiB per core. Whether iOS jetsam counts `MAP_JIT`
  reservations as dirty decides whether this fits on a 6 GB device at all. Needs measuring, not
  reasoning about.

## Things that are already handled, so don't rewrite them

- **MoltenVK's missing features.** `vulkan_device.cpp` already special-cases `IsMoltenVK()` for
  `geometryShader`, `logicOp`, `shaderCullDistance` and `wideLines` and logs "using fallback"
  instead of rejecting the device.
- **Apple W^X in dynarmic.** `backend/arm64/address_space.h` already calls `mem.protect()` /
  `mem.unprotect()` under `__APPLE__`, because Eden supports macOS on Apple silicon.
- **SDL3.** Both `audio_core` and `input_common` link it unconditionally, and it cross-configures
  for iOS. SDL3 supports iOS audio and GameController-framework input, so MFi controllers and
  audio output may need much less bespoke code than expected.

## Unverified

Stated so nobody treats them as established:

- Which mandatory Vulkan features MoltenVK actually lacks on which Apple GPU family. Nothing in
  this port has read MoltenVK's source or run it.
- Whether `mmap(MAP_JIT)` succeeds on a `CS_DEBUGGED` process that lacks the JIT entitlement.
  This decides whether the `mprotect` fallback is the primary path on StikDebug installs or dead
  code.
- Whether a second `Core::System::Load` after `ShutdownMainProcess` works. Reasoned from the
  source, never executed.
