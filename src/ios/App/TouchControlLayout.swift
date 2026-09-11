// SPDX-FileCopyrightText: Copyright 2026 eden-ios contributors
// SPDX-License-Identifier: GPL-3.0-or-later

import CoreGraphics
import UIKit

/// libretro joypad ids, as UInt32, in one place.
///
/// The RETRO_DEVICE_ID_JOYPAD_* names are plain integer `#define`s (libretro.h:320-371)
/// and which fixed-width Swift type clang's importer gives such a macro is not stable
/// across toolchains - EmulationView.swift already carries a note that it has been both
/// Int32 and Int. `UInt32(_:)` accepts either, so converting once here means the rest of
/// this file can be written in the type the bridge actually takes.
///
/// The naming is POSITIONAL and matches both the core and the Switch: B is the BOTTOM
/// button, A the RIGHT one, Y the LEFT one, X the TOP one. retro_input.cpp:59-62 spells
/// out why this must not get the SNES-style diagonal swap.
enum RetroPadID {
    static let b = UInt32(RETRO_DEVICE_ID_JOYPAD_B)
    static let a = UInt32(RETRO_DEVICE_ID_JOYPAD_A)
    static let x = UInt32(RETRO_DEVICE_ID_JOYPAD_X)
    static let y = UInt32(RETRO_DEVICE_ID_JOYPAD_Y)
    static let l = UInt32(RETRO_DEVICE_ID_JOYPAD_L)
    static let r = UInt32(RETRO_DEVICE_ID_JOYPAD_R)
    static let zl = UInt32(RETRO_DEVICE_ID_JOYPAD_L2)
    static let zr = UInt32(RETRO_DEVICE_ID_JOYPAD_R2)
    static let lStick = UInt32(RETRO_DEVICE_ID_JOYPAD_L3)
    static let rStick = UInt32(RETRO_DEVICE_ID_JOYPAD_R3)
    static let plus = UInt32(RETRO_DEVICE_ID_JOYPAD_START)
    static let minus = UInt32(RETRO_DEVICE_ID_JOYPAD_SELECT)
    static let up = UInt32(RETRO_DEVICE_ID_JOYPAD_UP)
    static let down = UInt32(RETRO_DEVICE_ID_JOYPAD_DOWN)
    static let left = UInt32(RETRO_DEVICE_ID_JOYPAD_LEFT)
    static let right = UInt32(RETRO_DEVICE_ID_JOYPAD_RIGHT)

    static let analogLeft = Int(RETRO_DEVICE_INDEX_ANALOG_LEFT)
    static let analogRight = Int(RETRO_DEVICE_INDEX_ANALOG_RIGHT)
}

/// What one on-screen control does.
enum PadControlKind {
    /// A momentary button carrying one libretro id.
    case button(UInt32)
    /// Four ids driven together from one finger, so diagonals work.
    case dpad
    /// An analog stick. The payload is RETRO_DEVICE_INDEX_ANALOG_LEFT / _RIGHT.
    case stick(Int)
}

/// Visual treatment. Purely cosmetic; hit testing always uses the control's rect.
enum PadControlShape {
    case circle
    case capsule
    case cross
}

/// Which screen corner the control is measured from.
///
/// Anchors rather than normalised centres, deliberately. A normalised layout tuned on a
/// 19.5:9 iPhone puts the shoulder buttons in the middle of an 4:3 iPad and the sticks
/// off the bottom of a Display-Zoomed SE. Distances from the nearest corner of the safe
/// area are what a thumb actually cares about, and they survive every aspect ratio.
enum PadAnchor {
    case bottomLeading
    case bottomTrailing
    case topLeading
    case topTrailing
    /// Horizontally from the middle of the view, vertically from the top safe inset.
    case topCentreLeading
    case topCentreTrailing
}

struct PadControlSpec {
    let kind: PadControlKind
    let shape: PadControlShape
    let label: String
    let anchor: PadAnchor
    /// Centre offset in points, measured INWARDS from the anchor.
    let inset: CGPoint
    /// Untransformed size in points.
    let size: CGSize
}

/// A spec with its frame resolved against a concrete view.
struct PlacedPadControl {
    let spec: PadControlSpec
    /// Where it is drawn.
    let frame: CGRect
    /// Where it responds. Larger than `frame`: a 52 pt circle is already at Apple's
    /// minimum target size, and a thumb held over a game does not aim like a fingertip
    /// over a list row.
    let hitFrame: CGRect
}

/// The default control arrangement, and the arithmetic that places it.
enum PadLayout {

    /// Extra hit area around every control, in points, before scaling.
    static let hitSlop: CGFloat = 8

    /// How far a finger must leave the centre of the d-pad before any direction is
    /// reported, as a fraction of the d-pad's half-width. Without it, resting a thumb
    /// dead centre chatters between opposite directions.
    static let dpadDeadFraction: CGFloat = 0.22

    /// Radial dead zone on the analog sticks, as a fraction of the travel radius.
    ///
    /// Eden applies NONE of its own: LoadVirtualGamepadParams sets deadzone 0.0 and
    /// range 1.0 (emulated_controller.cpp:326-329) precisely so a virtual pad's value
    /// reaches the game untouched. A touch stick has no mechanical centre detent, so
    /// whatever is not filtered here reaches the guest as a slow permanent walk.
    static let stickDeadFraction: Float = 0.06

    // MARK: - specs

    static func specs(landscape: Bool) -> [PadControlSpec] {
        landscape ? landscapeSpecs() : portraitSpecs()
    }

    /// Landscape, which is how a Switch title is meant to be held.
    ///
    /// Left thumb: stick low and outboard, d-pad inboard and slightly higher, matching
    /// the left Joy-Con. Right thumb: the ABXY diamond outboard, right stick inboard and
    /// lower, matching the right Joy-Con. Shoulders along the top edge where an index
    /// finger already rests when the device is gripped.
    private static func landscapeSpecs() -> [PadControlSpec] {
        let stickSize = CGSize(width: 132, height: 132)
        let faceSize = CGSize(width: 56, height: 56)
        let shoulderSize = CGSize(width: 74, height: 42)
        let systemSize = CGSize(width: 46, height: 34)
        let dpadSize = CGSize(width: 126, height: 126)

        // Centre of the ABXY diamond, and how far each button sits from it.
        let faceCentre = CGPoint(x: 108, y: 112)
        let faceSpread: CGFloat = 46

        return [
            PadControlSpec(kind: .stick(RetroPadID.analogLeft), shape: .circle, label: "",
                           anchor: .bottomLeading, inset: CGPoint(x: 100, y: 106),
                           size: stickSize),
            PadControlSpec(kind: .dpad, shape: .cross, label: "",
                           anchor: .bottomLeading, inset: CGPoint(x: 232, y: 82),
                           size: dpadSize),

            PadControlSpec(kind: .button(RetroPadID.x), shape: .circle, label: "X",
                           anchor: .bottomTrailing,
                           inset: CGPoint(x: faceCentre.x, y: faceCentre.y + faceSpread),
                           size: faceSize),
            PadControlSpec(kind: .button(RetroPadID.b), shape: .circle, label: "B",
                           anchor: .bottomTrailing,
                           inset: CGPoint(x: faceCentre.x, y: faceCentre.y - faceSpread),
                           size: faceSize),
            // Trailing anchor measures inwards, so the LARGER inset.x is the LEFTMOST
            // button. Y is the left-hand face button on a Switch.
            PadControlSpec(kind: .button(RetroPadID.y), shape: .circle, label: "Y",
                           anchor: .bottomTrailing,
                           inset: CGPoint(x: faceCentre.x + faceSpread, y: faceCentre.y),
                           size: faceSize),
            PadControlSpec(kind: .button(RetroPadID.a), shape: .circle, label: "A",
                           anchor: .bottomTrailing,
                           inset: CGPoint(x: faceCentre.x - faceSpread, y: faceCentre.y),
                           size: faceSize),

            PadControlSpec(kind: .stick(RetroPadID.analogRight), shape: .circle, label: "",
                           anchor: .bottomTrailing, inset: CGPoint(x: 238, y: 78),
                           size: stickSize),

            PadControlSpec(kind: .button(RetroPadID.zl), shape: .capsule, label: "ZL",
                           anchor: .topLeading, inset: CGPoint(x: 62, y: 40),
                           size: shoulderSize),
            PadControlSpec(kind: .button(RetroPadID.l), shape: .capsule, label: "L",
                           anchor: .topLeading, inset: CGPoint(x: 150, y: 40),
                           size: shoulderSize),
            PadControlSpec(kind: .button(RetroPadID.zr), shape: .capsule, label: "ZR",
                           anchor: .topTrailing, inset: CGPoint(x: 62, y: 40),
                           size: shoulderSize),
            PadControlSpec(kind: .button(RetroPadID.r), shape: .capsule, label: "R",
                           anchor: .topTrailing, inset: CGPoint(x: 150, y: 40),
                           size: shoulderSize),

            PadControlSpec(kind: .button(RetroPadID.minus), shape: .capsule, label: "\u{2212}",
                           anchor: .topCentreLeading, inset: CGPoint(x: 72, y: 40),
                           size: systemSize),
            PadControlSpec(kind: .button(RetroPadID.plus), shape: .capsule, label: "+",
                           anchor: .topCentreTrailing, inset: CGPoint(x: 72, y: 40),
                           size: systemSize),

            // Stacked under the shoulders rather than beside the sticks: a stick click
            // placed next to the stick is pressed by accident constantly, and there is
            // no room under the left stick that does not overlap its travel circle.
            PadControlSpec(kind: .button(RetroPadID.lStick), shape: .capsule, label: "L3",
                           anchor: .topLeading, inset: CGPoint(x: 62, y: 96),
                           size: systemSize),
            PadControlSpec(kind: .button(RetroPadID.rStick), shape: .capsule, label: "R3",
                           anchor: .topTrailing, inset: CGPoint(x: 62, y: 96),
                           size: systemSize),
        ]
    }

    /// Portrait. Everything is pushed into the lower third so the guest image, which is
    /// letterboxed into a wide band on a tall screen, stays uncovered - and so that the
    /// bare middle of the screen is left free for the GUEST TOUCHSCREEN, which is the
    /// whole reason the control layer is hit-tested per control rather than as a slab.
    private static func portraitSpecs() -> [PadControlSpec] {
        let stickSize = CGSize(width: 124, height: 124)
        let faceSize = CGSize(width: 54, height: 54)
        let shoulderSize = CGSize(width: 68, height: 40)
        let systemSize = CGSize(width: 44, height: 32)
        let dpadSize = CGSize(width: 118, height: 118)

        let faceCentre = CGPoint(x: 76, y: 232)
        let faceSpread: CGFloat = 44

        return [
            PadControlSpec(kind: .stick(RetroPadID.analogLeft), shape: .circle, label: "",
                           anchor: .bottomLeading, inset: CGPoint(x: 84, y: 226),
                           size: stickSize),
            PadControlSpec(kind: .dpad, shape: .cross, label: "",
                           anchor: .bottomLeading, inset: CGPoint(x: 82, y: 92),
                           size: dpadSize),

            PadControlSpec(kind: .button(RetroPadID.x), shape: .circle, label: "X",
                           anchor: .bottomTrailing,
                           inset: CGPoint(x: faceCentre.x, y: faceCentre.y + faceSpread),
                           size: faceSize),
            PadControlSpec(kind: .button(RetroPadID.b), shape: .circle, label: "B",
                           anchor: .bottomTrailing,
                           inset: CGPoint(x: faceCentre.x, y: faceCentre.y - faceSpread),
                           size: faceSize),
            PadControlSpec(kind: .button(RetroPadID.y), shape: .circle, label: "Y",
                           anchor: .bottomTrailing,
                           inset: CGPoint(x: faceCentre.x + faceSpread, y: faceCentre.y),
                           size: faceSize),
            PadControlSpec(kind: .button(RetroPadID.a), shape: .circle, label: "A",
                           anchor: .bottomTrailing,
                           inset: CGPoint(x: faceCentre.x - faceSpread, y: faceCentre.y),
                           size: faceSize),

            PadControlSpec(kind: .stick(RetroPadID.analogRight), shape: .circle, label: "",
                           anchor: .bottomTrailing, inset: CGPoint(x: 80, y: 92),
                           size: stickSize),

            PadControlSpec(kind: .button(RetroPadID.zl), shape: .capsule, label: "ZL",
                           anchor: .bottomLeading, inset: CGPoint(x: 58, y: 330),
                           size: shoulderSize),
            PadControlSpec(kind: .button(RetroPadID.l), shape: .capsule, label: "L",
                           anchor: .bottomLeading, inset: CGPoint(x: 138, y: 330),
                           size: shoulderSize),
            PadControlSpec(kind: .button(RetroPadID.zr), shape: .capsule, label: "ZR",
                           anchor: .bottomTrailing, inset: CGPoint(x: 58, y: 330),
                           size: shoulderSize),
            PadControlSpec(kind: .button(RetroPadID.r), shape: .capsule, label: "R",
                           anchor: .bottomTrailing, inset: CGPoint(x: 138, y: 330),
                           size: shoulderSize),

            PadControlSpec(kind: .button(RetroPadID.minus), shape: .capsule, label: "\u{2212}",
                           anchor: .topCentreLeading, inset: CGPoint(x: 60, y: 34),
                           size: systemSize),
            PadControlSpec(kind: .button(RetroPadID.plus), shape: .capsule, label: "+",
                           anchor: .topCentreTrailing, inset: CGPoint(x: 60, y: 34),
                           size: systemSize),

            PadControlSpec(kind: .button(RetroPadID.lStick), shape: .capsule, label: "L3",
                           anchor: .topCentreLeading, inset: CGPoint(x: 60, y: 82),
                           size: systemSize),
            PadControlSpec(kind: .button(RetroPadID.rStick), shape: .capsule, label: "R3",
                           anchor: .topCentreTrailing, inset: CGPoint(x: 60, y: 82),
                           size: systemSize),
        ]
    }

    // MARK: - placement

    /// Resolve every spec into a frame inside `bounds`.
    ///
    /// `userScale` comes from TouchControlSettings and multiplies both the control sizes
    /// and their distances from the anchor, so scaling up does not push a control off
    /// the far edge of a small screen - the clamp at the end catches what is left.
    static func place(specs: [PadControlSpec],
                      in bounds: CGRect,
                      safeArea: UIEdgeInsets,
                      userScale: CGFloat) -> [PlacedPadControl] {

        guard bounds.width > 0, bounds.height > 0 else { return [] }

        // Reference is the 390 pt short edge of a modern phone. Clamped so an iPad does
        // not get cartoonishly large controls and a small phone does not get unusable
        // ones.
        let shortEdge = min(bounds.width, bounds.height)
        let deviceScale = min(max(shortEdge / 390, 0.88), 1.45)
        let scale = deviceScale * userScale

        let left = bounds.minX + safeArea.left
        let right = bounds.maxX - safeArea.right
        let top = bounds.minY + safeArea.top
        let bottom = bounds.maxY - safeArea.bottom

        return specs.map { spec in
            let size = CGSize(width: spec.size.width * scale, height: spec.size.height * scale)
            let dx = spec.inset.x * scale
            let dy = spec.inset.y * scale

            var centre: CGPoint
            switch spec.anchor {
            case .bottomLeading:
                centre = CGPoint(x: left + dx, y: bottom - dy)
            case .bottomTrailing:
                centre = CGPoint(x: right - dx, y: bottom - dy)
            case .topLeading:
                centre = CGPoint(x: left + dx, y: top + dy)
            case .topTrailing:
                centre = CGPoint(x: right - dx, y: top + dy)
            case .topCentreLeading:
                centre = CGPoint(x: bounds.midX - dx, y: top + dy)
            case .topCentreTrailing:
                centre = CGPoint(x: bounds.midX + dx, y: top + dy)
            }

            // Keep the whole control on screen even when a large userScale or a small
            // device would otherwise push it past an edge.
            centre.x = min(max(centre.x, bounds.minX + size.width / 2),
                           bounds.maxX - size.width / 2)
            centre.y = min(max(centre.y, bounds.minY + size.height / 2),
                           bounds.maxY - size.height / 2)

            let frame = CGRect(x: centre.x - size.width / 2,
                               y: centre.y - size.height / 2,
                               width: size.width,
                               height: size.height)
            let slop = hitSlop * scale
            return PlacedPadControl(spec: spec,
                                    frame: frame,
                                    hitFrame: frame.insetBy(dx: -slop, dy: -slop))
        }
    }
}

/// User-adjustable presentation, persisted per device.
///
/// Deliberately small: opacity, size and whether the layer is shown at all. Anything
/// more (drag-to-reposition, per-control toggles) needs an editing mode and a place to
/// store a custom layout, which is a feature rather than a default.
final class TouchControlSettings {

    static let shared = TouchControlSettings()

    private enum Key {
        static let opacity = "eden.touchControls.opacity"
        static let scale = "eden.touchControls.scale"
        static let enabled = "eden.touchControls.enabled"
        static let haptics = "eden.touchControls.haptics"
        static let hideWithController = "eden.touchControls.hideWithController"
    }

    private let defaults = UserDefaults.standard

    private init() {
        defaults.register(defaults: [
            Key.opacity: 0.55,
            Key.scale: 1.0,
            Key.enabled: true,
            Key.haptics: true,
            Key.hideWithController: true,
        ])
    }

    var opacity: CGFloat {
        get { CGFloat(min(max(defaults.double(forKey: Key.opacity), 0.1), 1.0)) }
        set { defaults.set(Double(newValue), forKey: Key.opacity) }
    }

    var scale: CGFloat {
        get { CGFloat(min(max(defaults.double(forKey: Key.scale), 0.7), 1.4)) }
        set { defaults.set(Double(newValue), forKey: Key.scale) }
    }

    var isEnabled: Bool {
        get { defaults.bool(forKey: Key.enabled) }
        set { defaults.set(newValue, forKey: Key.enabled) }
    }

    var hapticsEnabled: Bool {
        get { defaults.bool(forKey: Key.haptics) }
        set { defaults.set(newValue, forKey: Key.haptics) }
    }

    /// Hide the on-screen pad while a physical controller is attached. The guest
    /// touchscreen keeps working either way - hiding the pad actually widens it, because
    /// the control layer stops claiming those rectangles.
    var hideWhenControllerAttached: Bool {
        get { defaults.bool(forKey: Key.hideWithController) }
        set { defaults.set(newValue, forKey: Key.hideWithController) }
    }
}
