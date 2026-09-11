# eden-ios

An iOS and iPadOS port of [Eden](https://git.eden-emu.dev/eden-emu/eden), a Nintendo Switch emulator.

This is a **port**, not a fork of somebody else's port. Eden's core is C++ and has never
run on iOS; the work here is making it do that.

## Status

**Nothing works yet.** This repository was created on 2026-09-11 and currently contains the
CI needed to mirror Eden and start cross-compiling it. No frame has ever reached a screen.
This file will say what is actually true as the port progresses — if a claim here is wrong,
the code changed, so fix this file.

| Milestone | Gate | State |
|---|---|---|
| M0 | Eden's source mirrored into this repo and buildable in CI for its native target | in progress |
| M1 | Eden's core libraries cross-compile for `iphoneos` arm64 | not started |
| M2 | An `.ipa` links, installs, and launches to a UI on a real device | not started |
| M3 | A homebrew `.nro` boots and something it draws reaches the screen | not started |
| M4 | A retail title boots | not started |
| M5 | Input, audio, and a stable frame pace | not started |

## Why this is hard

These are the real obstacles, stated up front so nobody rediscovers them:

- **JIT.** Eden's CPU layer is dynarmic, vendored at `src/dynarmic`, which recompiles guest
  ARM64 into host ARM64 at runtime. iOS forbids that for ordinary apps. On newer iOS the
  route is StikDebug (a debugger attaches, `CS_DEBUGGED` gets set, `MAP_JIT` pages become
  writable-then-executable); on older iOS it is TrollStore with the entitlements genuinely
  embedded in the signature. Both are supported here — that is why the build produces more
  than one IPA. Apple silicon also requires `pthread_jit_write_protect_np` around every code
  write, which desktop dynarmic does not need.
- **Host page size.** The Switch's guest page size is 4 KiB. Apple silicon's host page size
  is 16 KiB. yuzu-lineage emulators lean on "fastmem" — mapping guest memory directly into
  the host address space so a guest load is a host load — and that trick needs host pages no
  larger than guest pages. It cannot work as written here. The port starts on the slower
  soft-MMU path and treats fastmem as a later, separate problem.
- **Graphics.** Eden renders through Vulkan. iOS has no Vulkan, only Metal, so everything
  goes through MoltenVK, which does not implement all of Vulkan. The features Switch
  emulators lean on hardest — geometry shaders, transform feedback, custom border colours —
  are exactly the awkward ones on Apple GPUs.
- **Memory.** The Switch gives games ~3.2 GB. iOS kills apps that ask for too much, so the
  build needs `increased-memory-limit`, and `extended-virtual-addressing` on some devices.
- **Build machine.** The maintainer's Mac has no Xcode and under a gigabyte of free disk.
  Every build here happens on a GitHub Actions runner. That is a permanent constraint, not
  a temporary one, and the CI is designed around it.

## Devices

iPhone and iPad are both first-class targets: A-series and M-series get the same effort.
The deployment target is kept as low as the code allows so older devices are not excluded,
with newer OS features gated behind availability checks rather than raising the floor.

## What you must provide yourself

This repository contains no Nintendo code, keys, firmware, or games, and never will.
To run anything beyond homebrew you need your own `prod.keys`, your own firmware dump, and
your own game dumps, taken from hardware you own.

## Licence

Eden is GPL-3.0; this port inherits it. See `LICENSE.txt` once upstream is mirrored.
