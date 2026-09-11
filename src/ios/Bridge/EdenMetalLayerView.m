// SPDX-FileCopyrightText: Copyright 2026 eden-ios contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#import "EdenMetalLayerView.h"

#import <QuartzCore/CAMetalLayer.h>

#include <math.h>   // lround

#import "EdenCoreBridge.h"

@interface EdenMetalLayerView ()
/// Same object as `self.layer`, typed. +layerClass guarantees the cast is sound.
@property (nonatomic, readonly) CAMetalLayer *metalLayer;
@end

@implementation EdenMetalLayerView {
    CGSize _lastReportedSize;
}

+ (Class)layerClass {
    // The whole reason this class exists. docs/IOS_PORT_NOTES.md #1.
    return [CAMetalLayer class];
}

+ (EdenMetalLayerView *)sharedView {
    static EdenMetalLayerView *shared;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        shared = [[EdenMetalLayerView alloc] initWithFrame:CGRectZero];
    });
    return shared;
}

- (instancetype)initWithFrame:(CGRect)frame {
    self = [super initWithFrame:frame];
    if (self != nil) {
        _lastReportedSize = CGSizeZero;
        _forwardsTouchesToGuest = NO;
        self.opaque = YES;
        self.backgroundColor = [UIColor blackColor];
        self.multipleTouchEnabled = NO;   // one pointer in cut one
        self.contentMode = UIViewContentModeRedraw;
    }
    return self;
}

- (CAMetalLayer *)metalLayer {
    return (CAMetalLayer *)self.layer;
}

- (CGSize)physicalPixelSize {
    const CGFloat scale = self.layer.contentsScale;
    return CGSizeMake(self.bounds.size.width * scale, self.bounds.size.height * scale);
}

- (void)didMoveToWindow {
    [super didMoveToWindow];
    if (self.window == nil) {
        return;
    }

    // nativeScale, not scale.
    //
    // nativeScale is true panel pixels and avoids a resample by the compositor. The
    // cost: on a Display-Zoomed Plus-class iPhone it is non-integral (about 2.608), so
    // the drawable size is a rounded value and the swapchain extent will not be an
    // exact multiple of the guest resolution. Whether that matters to MoltenVK's
    // surface capabilities, or whether `scale` would be the safer choice, needs a
    // device. This is the first thing to change if the swapchain misbehaves on one.
    self.layer.contentsScale = self.window.screen.nativeScale;

    // NOT SET, on purpose: device, pixelFormat, framebufferOnly, maximumDrawableCount.
    // The assumption is that MoltenVK configures all of them when it creates the
    // swapchain over this layer through vkCreateMetalSurfaceEXT. That assumption is
    // UNVERIFIED - docs/IOS_PORT_NOTES.md is explicit that "Nothing in this port has
    // read MoltenVK's source or run it", and neither have I. If surface creation fails
    // with the layer left unconfigured, setting `device` to MTLCreateSystemDefaultDevice()
    // is the first thing to try.

    [self setNeedsLayout];
}

- (void)layoutSubviews {
    [super layoutSubviews];

    const CGSize px = self.physicalPixelSize;
    const unsigned w = (unsigned)lround(px.width);
    const unsigned h = (unsigned)lround(px.height);

    if (w == 0 || h == 0) {
        // Do not forward. SanitizeDim (retro_emu_window.cpp:16-18) turns 0 into 1, so
        // passing a transient zero would rebuild the swapchain at 1x1 partway through
        // every rotation.
        return;
    }
    if (w == (unsigned)lround(_lastReportedSize.width) &&
        h == (unsigned)lround(_lastReportedSize.height)) {
        return;
    }
    _lastReportedSize = CGSizeMake(w, h);

    // MoltenVK reads the layer from whichever thread calls vkAcquireNextImageKHR, and
    // eden_libretro.h:38-42 requires the geometry to be mutated on the main thread
    // only. layoutSubviews already is the main thread; this assignment and the
    // bridge call must both stay here.
    self.metalLayer.drawableSize = CGSizeMake(w, h);

    // Attach is idempotent and cheap; doing it here as well as in didMoveToWindow
    // means the bridge always has a valid pointer and a current size before any
    // eden_bridge_start, which is the precondition it refuses on.
    eden_bridge_attach_layer((__bridge void *)self.metalLayer, w, h);
    eden_bridge_layer_did_resize(w, h);
}

// ---------------------------------------------------------------------------
// touch forwarding
// ---------------------------------------------------------------------------

- (void)forwardTouch:(UITouch *)touch pressed:(BOOL)pressed {
    if (!_forwardsTouchesToGuest) {
        return;
    }
    if (!pressed) {
        eden_input_set_touch(false, 0.0f, 0.0f);
        return;
    }
    const CGPoint p = [touch locationInView:self];
    const CGSize size = self.bounds.size;
    if (size.width <= 0.0 || size.height <= 0.0) {
        return;
    }
    // Normalised LAYER coordinates. The bridge does the layer -> guest-screen
    // letterbox conversion itself, because that is where the 16:9 assumption is
    // documented and where libretro pointer space is produced.
    eden_input_set_touch(true,
                         (float)(p.x / size.width),
                         (float)(p.y / size.height));
}

- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    [super touchesBegan:touches withEvent:event];
    [self forwardTouch:touches.anyObject pressed:YES];
}

- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    [super touchesMoved:touches withEvent:event];
    [self forwardTouch:touches.anyObject pressed:YES];
}

- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    [super touchesEnded:touches withEvent:event];
    [self forwardTouch:touches.anyObject pressed:NO];
}

- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    [super touchesCancelled:touches withEvent:event];
    [self forwardTouch:touches.anyObject pressed:NO];
}

@end
