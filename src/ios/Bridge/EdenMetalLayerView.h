// SPDX-FileCopyrightText: Copyright 2026 eden-ios contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef EDEN_METAL_LAYER_VIEW_H
#define EDEN_METAL_LAYER_VIEW_H

#import <UIKit/UIKit.h>

NS_ASSUME_NONNULL_BEGIN

/**
 * A UIView whose +layerClass is CAMetalLayer.
 *
 * docs/IOS_PORT_NOTES.md #1 and eden_libretro.h:29-31 both require an actual
 * CAMetalLayer, not a UIView: vulkan_surface.cpp passes the pointer straight to
 * VkMetalSurfaceCreateInfoEXT::pLayer for WindowSystemType::Cocoa (which is the branch
 * iOS takes, because Eden's Apple guards are `#elif defined(__APPLE__)` and
 * RetroEmuWindow reports Cocoa - retro_emu_window.cpp:29). The note names this exact
 * construction as "the simplest correct source".
 *
 * The view calls eden_bridge_attach_layer / eden_bridge_layer_did_resize itself, from
 * layoutSubviews, always in PHYSICAL pixels.
 *
 * OWNERSHIP: the C side holds the layer UNRETAINED (retro_core.cpp:97 is a bare
 * `void* g_metal_layer`, and RetroEmuWindow copies it into window_info at
 * construction). eden_libretro.h:33 says "The layer must outlive Core::System". So
 * exactly one of these is created and kept alive for the process lifetime by
 * +sharedView, and SwiftUI reparents it rather than making new ones. Creating a second
 * one, or letting SwiftUI deallocate the first on a view-identity change, hands the
 * renderer a dangling pointer.
 */
@interface EdenMetalLayerView : UIView

/// The one instance the core is given. Main thread only.
@property (class, nonatomic, readonly) EdenMetalLayerView *sharedView;

/// Current drawable size in physical pixels, for the touch letterbox and diagnostics.
@property (nonatomic, readonly) CGSize physicalPixelSize;

/// Forwards touches to eden_input_set_touch in normalised layer coordinates.
/// Defaults to NO: the on-screen controls sit on top of this view, and a touch meant
/// for a button should not also land on the guest touchscreen.
@property (nonatomic) BOOL forwardsTouchesToGuest;

@end

NS_ASSUME_NONNULL_END

#endif // EDEN_METAL_LAYER_VIEW_H
