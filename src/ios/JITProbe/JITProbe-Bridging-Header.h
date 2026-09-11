// SPDX-FileCopyrightText: Copyright 2026 eden-ios contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Everything Swift may call, which is the whole of the probe and nothing else.
//
// Note what is NOT here: none of Eden, none of libretro, no CMake-built archive. This
// app links no part of the emulator. That is deliberate and is what makes it useful -
// it answers the question that decides the port without needing the port to build, and
// it can therefore be handed to a tester today rather than after M3.

#ifndef JIT_PROBE_BRIDGING_HEADER_H
#define JIT_PROBE_BRIDGING_HEADER_H

#import "jit_probe.h"

#endif // JIT_PROBE_BRIDGING_HEADER_H
