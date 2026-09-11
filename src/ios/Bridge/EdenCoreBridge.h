// SPDX-FileCopyrightText: Copyright 2026 eden-ios contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The whole libretro lifecycle, so Swift never touches it.
//
// ===========================================================================
// THE ORDERING FINDING THIS FILE EXISTS TO ENFORCE
// ===========================================================================
// src/libretro_core/eden_libretro.h:33 says "Call before retro_load_game" for
// eden_libretro_set_metal_layer, and retro_core.cpp:605-608 repeats it in a comment
// ("The window was built before the layer arrived ... as long as this lands before
// retro_load_game the layer is in place in time").
//
// BOTH ARE WRONG, and the failure is an abort, not a degradation:
//
//   * retro_core.cpp:445-446   retro_init() does
//                                g_emu_window = make_unique<RetroEmuWindow>(g_metal_layer, ...)
//   * retro_emu_window.cpp:33  the constructor does
//                                window_info.render_surface = metal_layer;
//     i.e. it COPIES the pointer at construction time.
//   * retro_core.cpp:602-610   eden_libretro_set_metal_layer only assigns g_metal_layer
//                              and calls g_emu_window->Resize(). Resize()
//                              (retro_emu_window.cpp:93-101) touches the framebuffer
//                              layout and NEVER re-assigns render_surface.
//
// So a layer supplied after retro_init leaves render_surface == nullptr forever.
// CreateSurface then matches no branch and RendererVulkan's ctor throws
// VK_ERROR_INITIALIZATION_FAILED from its member-initialiser list during
// Core::System::Load - the exact abort docs/IOS_PORT_NOTES.md #1 describes.
//
// The same argument applies to eden_libretro_set_data_root: retro_init's tail calls
// Content::SetupUserPaths() (retro_core.cpp:456), which latches a file-scope
// g_paths_ready on the first successful resolve (retro_content.cpp:40, :66-68) and
// returns early forever after. A data root supplied afterwards is read by nothing.
//
// THEREFORE: set_metal_layer and set_data_root BOTH run before retro_init.
// The header comment is necessary but not sufficient; this is the sufficient order.
//
// ===========================================================================
// THREADING
// ===========================================================================
// docs/IOS_PORT_NOTES.md #3: "Keep retro_run off the main thread. It blocks waiting on
// the frame signal. iOS kills an app whose main thread stops responding." The block is
// real and bounded - retro_run calls WaitForFramePresented(kFrameWaitTimeout) with
// kFrameWaitTimeout = 50ms (retro_core.cpp:132, :562-565).
//
// Everything from retro_init to retro_deinit runs on ONE dedicated pthread, because
// LoadGameInternal calls g_system->RegisterHostThread() (retro_core.cpp:309) on
// whatever thread called retro_load_game. Load, run and unload must therefore all be
// that same thread.
//
// A pthread and not a dispatch queue: a Dispatch worker is a pooled thread the runtime
// expects back, and parking one for the process lifetime is the thread-explosion case
// Dispatch documents against; Dispatch also cannot set a stack size, and
// retro_load_game runs Eden's NCA/VFS loader deep on this thread.
//
// Functions are annotated MAIN THREAD ONLY or ANY THREAD. The main thread only ever
// does attach_layer / layer_did_resize / set_visible.

#ifndef EDEN_CORE_BRIDGE_H
#define EDEN_CORE_BRIDGE_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Lifecycle state, polled by the Swift UI layer.
typedef enum {
    EdenBridgeStateIdle = 0,
    EdenBridgeStateStarting,   ///< thread up, before retro_load_game returned
    EdenBridgeStateRunning,    ///< inside the retro_run loop
    EdenBridgeStateStopping,
    EdenBridgeStateFailed,     ///< see eden_bridge_copy_status
} EdenBridgeState;

// --- surface ---------------------------------------------------------------

/**
 * Hand the bridge the CAMetalLayer to render into, plus its size in PHYSICAL pixels.
 *
 * MAIN THREAD ONLY. Must be called BEFORE eden_bridge_start - the layer has to exist
 * before retro_init, per the file header.
 *
 * `layer` must be an actual CAMetalLayer, not a UIView. eden_libretro.h:29-31 and
 * vulkan_surface.cpp pass it straight to VkMetalSurfaceCreateInfoEXT::pLayer. Pass
 * EdenMetalLayerView's `.layer`, never the view.
 *
 * The bridge stores the pointer UNRETAINED, matching the core: retro_core.cpp:97
 * holds `void* g_metal_layer` and RetroEmuWindow copies it into window_info. The view
 * must outlive Core::System, which is why EmulatorSurface keeps exactly one for the
 * process lifetime.
 */
void eden_bridge_attach_layer(void *metal_layer, unsigned width, unsigned height);

/**
 * New PHYSICAL pixel size from layoutSubviews / rotation.
 * MAIN THREAD ONLY - eden_libretro.h:39-42 requires it ("MoltenVK touches the layer
 * from whichever thread calls vkAcquireNextImageKHR").
 * A zero width or height is ignored rather than forwarded: SanitizeDim
 * (retro_emu_window.cpp:16-18) clamps 0 to 1, which would rebuild the swapchain at
 * 1x1 in the middle of every rotation.
 */
void eden_bridge_layer_did_resize(unsigned width, unsigned height);

/**
 * Background / foreground. MAIN THREAD ONLY.
 * Forwards to eden_libretro_set_visible, which makes RendererVulkan::Composite
 * early-return, and additionally parks the run loop so nothing polls input or pumps
 * audio while suspended.
 */
void eden_bridge_set_visible(bool visible);

// --- lifecycle -------------------------------------------------------------

/**
 * Start emulation. MAIN THREAD (it creates the emulation thread and returns at once).
 *
 * @param rom_path   host path; retro_get_system_info sets need_fullpath = true
 *                   (retro_core.cpp:349) so the core opens this itself.
 * @param data_root  writable directory visible to Files.app. Passed to
 *                   eden_libretro_set_data_root before retro_init.
 * @return false if a session is already live, if no layer has been attached, or if the
 *         thread could not be created. Never blocks on the load.
 *
 * The CALLER must already hold any security-scoped resource for rom_path and must keep
 * holding it until eden_bridge_stop returns - docs/IOS_PORT_NOTES.md #4.
 * The CALLER must also have created data_root; the bridge does not mkdir. (The core
 * does create the ~20 subdirectories beneath it - retro_content.cpp:91-124 - but only
 * if the root itself resolves.)
 */
bool eden_bridge_start(const char *rom_path, const char *data_root);

/**
 * Ask the emulation thread to leave the run loop, then join it.
 * MAIN THREAD. Blocks until retro_deinit has returned, or until the timeout.
 *
 * A stop requested while retro_load_game is still running cannot take effect until the
 * load returns - there is no cancellation inside Core::System::Load. For a large XCI
 * that can be a long time. The timeout is a guess, not a measurement.
 */
void eden_bridge_stop(void);

EdenBridgeState eden_bridge_state(void);

/// Copies a human-readable status / last-error line into `buf`. ANY THREAD.
void eden_bridge_copy_status(char *buf, size_t len);

/// Frontend run-loop iterations per second. ANY THREAD.
///
/// NOT guest frames per second. retro_run returns either when a frame is presented or
/// when kFrameWaitTimeout (50ms) elapses, so a stalled guest floors this at 20. Telling
/// the two apart needs RetroEmuWindow::PresentedFrameCount(), which exists
/// (retro_emu_window.h) but is not exported through eden_libretro.h.
double eden_bridge_iterations_per_second(void);

// --- input (producer side; ANY THREAD) -------------------------------------
//
// UIKit touches and GCController events arrive on the main thread; the core reads
// input from the emulation thread inside retro_run -> RetroInput::Poll ->
// state_cb(...). The bridge keeps one shared atomic word plus two stick pairs, and
// input_poll latches them into an emu-thread-private snapshot that input_state then
// reads with no lock. That is the entire reason libretro has a separate poll callback.

/// `id` is a RETRO_DEVICE_ID_JOYPAD_* value (libretro.h:355-380).
void eden_input_set_button(unsigned id, bool pressed);

/// Replace the whole button bitmask at once (bit N = RETRO_DEVICE_ID_JOYPAD_N).
void eden_input_set_button_mask(unsigned mask);

/// `index` is RETRO_DEVICE_INDEX_ANALOG_LEFT (0) or _RIGHT (1). x/y in [-1, 1],
/// libretro's axis convention (+Y is DOWN). retro_input.cpp:285-292 negates Y on the
/// way into Eden, so pass UIKit-natural values: down is positive.
void eden_input_set_stick(unsigned index, float x, float y);

/**
 * One touch, in NORMALISED LAYER coordinates: (0,0) top-left, (1,1) bottom-right of
 * the CAMetalLayer.
 *
 * The bridge converts to libretro pointer space itself, and that conversion is not a
 * straight rescale. retro_input.cpp:322-326 maps libretro pointer coordinates to
 * [0,1] and DROPS anything outside, with the comment "EmuWindow::MapToTouchScreen is
 * not needed - libretro already gave us viewport-relative coordinates". So libretro
 * pointer space is the GUEST SCREEN, not the layer, and the letterbox bars are the
 * frontend's problem. The bridge maps layer -> 16:9 guest rect and reports
 * not-pressed for a touch on a bar.
 */
void eden_input_set_touch(bool pressed, float layer_x, float layer_y);

#ifdef __cplusplus
}
#endif

#endif // EDEN_CORE_BRIDGE_H
