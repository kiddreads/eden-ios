// SPDX-FileCopyrightText: Copyright 2026 eden-ios contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Everything Swift may call. Set as SWIFT_OBJC_BRIDGING_HEADER in src/ios/project.yml.
//
// Resolution of the two include spellings: project.yml puts BOTH $(SRCROOT)/.. (i.e.
// eden/src) and $(SRCROOT)/../libretro_core on HEADER_SEARCH_PATHS, because
// retro_core.cpp itself uses both - it includes "libretro.h" bare and
// "libretro_core/eden_libretro.h" prefixed. The prefixed form is used here.
//
// Both core headers are PURE C - eden_libretro.h wraps itself in extern "C" and
// includes only <stdbool.h>; libretro.h is the vendored upstream ABI header. Clang's
// importer maps them with no shim, so every Swift file in the target sees the C
// functions, the RETRO_* integer macros and the retro_* structs with no `import`
// statement of its own.
//
// The symbols themselves come from libeden_libretro.a at link time.
// src/libretro_core/CMakeLists.txt:16 builds it STATIC, and its own comment says why:
// "iOS will not load a dynamic library the app did not ship inside its own signed
// bundle, and will not load one produced at runtime at all, so a libretro core on iOS
// is linked into the frontend executable."
//
// ---------------------------------------------------------------------------
// WHAT SWIFT SHOULD ACTUALLY CALL
// ---------------------------------------------------------------------------
// The raw libretro entry points are exposed here because the importer brings them in
// whether or not that is wanted, not because Swift should use them. Swift calls
// EdenCoreBridge.h. The ordering constraints that make retro_init/retro_load_game
// dangerous to call directly (the CAMetalLayer must exist BEFORE retro_init, not
// merely before retro_load_game - see EdenCoreBridge.h) live in one place, on the one
// thread allowed to run them. Calling retro_init from Swift bypasses all of it.

#ifndef EDEN_BRIDGING_HEADER_H
#define EDEN_BRIDGING_HEADER_H

// The extra C surface the iOS app is required to drive:
//   eden_libretro_set_metal_layer  - mandatory; a headless window ABORTS, see
//                                    docs/IOS_PORT_NOTES.md #1
//   eden_libretro_resize           - main thread only
//   eden_libretro_set_visible      - background / foreground
//   eden_libretro_set_data_root    - mandatory on iOS; Common::FS::SetAppDirectory
//                                    does not work here, see IOS_PORT_NOTES.md #2
// These four are the ENTIRE eden_libretro.h surface. There is no eden_libretro_pause,
// no eden_libretro_get_screen_rect and no PresentedFrameCount export; anything that
// needs them has to add them to the core first.
#import "libretro_core/eden_libretro.h"

// The standard libretro ABI: retro_init, retro_run, retro_load_game, the
// RETRO_DEVICE_ID_JOYPAD_* constants the on-screen controls use, and the
// retro_game_info / retro_variable / retro_message structs.
#import "libretro_core/libretro.h"

// The app's own bridge - what Swift is expected to use.
#import "EdenCoreBridge.h"
#import "EdenJIT.h"
#import "EdenMetalLayerView.h"

// Key/firmware status and install - the firmware setup screen's entire C surface
// (eden_content_snapshot, eden_content_install_keys, eden_content_install_firmware,
// eden_content_verify_firmware, the EdenContentPathId/EdenContentSnapshot/
// EdenFirmwareCheck types). Declared in src/ios/App/EdenContentBridge.h, which
// existed on disk but was never imported here - every Swift file that called these
// failed with "cannot find ... in scope" because the bridging header, not the
// missing declarations, is what the Swift compiler actually consults.
#import "EdenContentBridge.h"

#endif // EDEN_BRIDGING_HEADER_H
