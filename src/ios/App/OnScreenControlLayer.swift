// SPDX-FileCopyrightText: Copyright 2026 eden-ios contributors
// SPDX-License-Identifier: GPL-3.0-or-later

import SwiftUI
import UIKit

// ---------------------------------------------------------------------------
// WHY THIS IS A UIView AND NOT A SwiftUI VIEW TREE
// ---------------------------------------------------------------------------
// A Switch pad needs several fingers down at once - a thumb on the stick, a thumb on
// B, an index finger on ZR is the normal case, not an edge case. SwiftUI's gesture
// system is built around a single logical gesture per view and arbitrates between
// recognisers; `DragGesture(minimumDistance: 0)` on each button, which is what the
// first cut of this layer used, gives one finger at a time in practice and no way to
// ask for more.
//
// UIKit already has the right primitive: one view, `isMultipleTouchEnabled = true`,
// and touchesBegan/Moved/Ended handing out every UITouch separately. The view owns a
// touch-to-control map, so N fingers drive N controls with no arbitration at all.
//
// The second reason is hit testing, and it matters more. The Switch HAS a touchscreen
// and Eden models it (EmulatedConsole maps sixteen "engine:touch" fingers
// unconditionally, emulated_console.cpp:39-46), so the bare parts of the screen must
// keep reaching the guest. `point(inside:with:)` below returns true ONLY over an
// actual control, which makes UIKit's hit test fall straight through to the
// EdenMetalLayerView underneath for everything else - and that view forwards to
// `eden_input_set_touch` (EdenMetalLayerView.m:120-139). A SwiftUI overlay with a
// `contentShape` big enough to be convenient would swallow the touchscreen.

/// The on-screen pad. One view, many fingers, drawn with CALayers.
final class EdenPadView: UIView {

    // MARK: - per-control state

    private final class ControlNode {
        let placed: PlacedPadControl
        let shape = CAShapeLayer()
        var label: UILabel?
        var knob: CAShapeLayer?
        /// D-pad direction pips, in the order up, down, left, right.
        var pips: [CAShapeLayer] = []
        var isPressed = false
        var dpadIDs: Set<UInt32> = []

        init(placed: PlacedPadControl) {
            self.placed = placed
        }
    }

    private struct LayoutSignature: Equatable {
        var size: CGSize
        var insets: UIEdgeInsets
        var scale: CGFloat
        var opacity: CGFloat
        var enabled: Bool
    }

    // MARK: - stored state

    private var nodes: [ControlNode] = []
    /// UITouch is a reference type and the same object is handed back for every phase
    /// of one finger, so its identity is the finger's identity.
    private var ownership: [ObjectIdentifier: Int] = [:]
    private var lastSignature: LayoutSignature?
    private var hiddenForController = false
    private var lifecycleObservers: [NSObjectProtocol] = []
    private lazy var haptics = UIImpactFeedbackGenerator(style: .rigid)

    private var settings: TouchControlSettings { TouchControlSettings.shared }

    // MARK: - init

    override init(frame: CGRect) {
        super.init(frame: frame)
        backgroundColor = .clear
        isOpaque = false
        isMultipleTouchEnabled = true
        // Not exclusive: another view (the HUD) must still be able to take a touch of
        // its own while a button here is held.
        isExclusiveTouch = false
        clipsToBounds = false
    }

    required init?(coder: NSCoder) {
        fatalError("EdenPadView is created in code only")
    }

    deinit {
        for observer in lifecycleObservers {
            NotificationCenter.default.removeObserver(observer)
        }
    }

    // MARK: - lifecycle

    override func didMoveToWindow() {
        super.didMoveToWindow()
        if window != nil {
            // Started here rather than in EdenApp so that every part of the input path
            // lives in files this layer owns. A better home is the app's own start-up;
            // start() is idempotent, so moving it later costs nothing.
            HardwareControllerInput.shared.start()
            HardwareControllerInput.shared.onAttachmentChanged = { [weak self] attached in
                self?.applyControllerAttachment(attached)
            }
            applyControllerAttachment(HardwareControllerInput.shared.isControllerAttached)
            registerLifecycleObservers()
            if settings.hapticsEnabled {
                haptics.prepare()
            }
        } else {
            releaseAll()
            HardwareControllerInput.shared.onAttachmentChanged = nil
        }
    }

    private func registerLifecycleObservers() {
        guard lifecycleObservers.isEmpty else { return }
        // A finger cannot send a touchesEnded through a phone call or an app switch.
        // Whatever is held at that moment would otherwise stay held: the mixer has no
        // reason to think it was ever let go, and the fix on the core side (see
        // retro_input.cpp defect 3) only covers a port change, not a vanished producer.
        let names: [NSNotification.Name] = [
            UIApplication.willResignActiveNotification,
            UIApplication.didEnterBackgroundNotification,
        ]
        for name in names {
            let token = NotificationCenter.default.addObserver(forName: name,
                                                               object: nil,
                                                               queue: .main) { [weak self] _ in
                self?.releaseAll()
            }
            lifecycleObservers.append(token)
        }
    }

    // MARK: - layout

    override func layoutSubviews() {
        super.layoutSubviews()
        let signature = LayoutSignature(size: bounds.size,
                                        insets: safeAreaInsets,
                                        scale: settings.scale,
                                        opacity: settings.opacity,
                                        enabled: settings.isEnabled)
        guard signature != lastSignature else { return }
        lastSignature = signature
        rebuild()
    }

    /// Re-read TouchControlSettings and rebuild if anything it owns changed.
    func refreshSettings() {
        setNeedsLayout()
    }

    private func rebuild() {
        // A rebuild invalidates every hit rect, so nothing may stay held across it.
        releaseAll()

        for node in nodes {
            node.shape.removeFromSuperlayer()
            node.knob?.removeFromSuperlayer()
            node.pips.forEach { $0.removeFromSuperlayer() }
            node.label?.removeFromSuperview()
        }
        nodes.removeAll()

        guard settings.isEnabled, bounds.width > 0, bounds.height > 0 else { return }

        let specs = PadLayout.specs(landscape: bounds.width > bounds.height)
        let placed = PadLayout.place(specs: specs,
                                     in: bounds,
                                     safeArea: safeAreaInsets,
                                     userScale: settings.scale)

        CATransaction.begin()
        CATransaction.setDisableActions(true)
        for item in placed {
            nodes.append(makeNode(item))
        }
        CATransaction.commit()

        applyHiddenState()
    }

    private func makeNode(_ placed: PlacedPadControl) -> ControlNode {
        let node = ControlNode(placed: placed)
        let frame = placed.frame
        let alpha = settings.opacity

        node.shape.frame = frame
        node.shape.path = path(for: placed.spec.shape, in: CGRect(origin: .zero, size: frame.size))
        node.shape.fillColor = UIColor.white.withAlphaComponent(0.17 * alpha).cgColor
        node.shape.strokeColor = UIColor.white.withAlphaComponent(0.45 * alpha).cgColor
        node.shape.lineWidth = 1.5
        layer.addSublayer(node.shape)

        switch placed.spec.kind {
        case .button:
            if !placed.spec.label.isEmpty {
                let label = UILabel(frame: frame)
                label.text = placed.spec.label
                label.textAlignment = .center
                label.textColor = UIColor.white.withAlphaComponent(min(1.0, 0.85 + alpha * 0.15))
                label.font = .systemFont(ofSize: max(13, frame.height * 0.42), weight: .semibold)
                label.isUserInteractionEnabled = false
                label.backgroundColor = .clear
                addSubview(label)
                node.label = label
            }

        case .stick:
            let knobSize = frame.width * 0.44
            let knob = CAShapeLayer()
            knob.bounds = CGRect(x: 0, y: 0, width: knobSize, height: knobSize)
            knob.position = CGPoint(x: frame.midX, y: frame.midY)
            knob.path = UIBezierPath(ovalIn: knob.bounds).cgPath
            knob.fillColor = UIColor.white.withAlphaComponent(0.38 * alpha).cgColor
            knob.strokeColor = UIColor.white.withAlphaComponent(0.6 * alpha).cgColor
            knob.lineWidth = 1.5
            layer.addSublayer(knob)
            node.knob = knob

        case .dpad:
            // Four pips rather than a single highlighted cross: a diagonal lights two
            // of them, which is the only cheap way to show that diagonals exist.
            let pipSize = frame.width * 0.18
            let offset = frame.width * 0.31
            let centres = [
                CGPoint(x: frame.midX, y: frame.midY - offset), // up
                CGPoint(x: frame.midX, y: frame.midY + offset), // down
                CGPoint(x: frame.midX - offset, y: frame.midY), // left
                CGPoint(x: frame.midX + offset, y: frame.midY), // right
            ]
            for centre in centres {
                let pip = CAShapeLayer()
                pip.bounds = CGRect(x: 0, y: 0, width: pipSize, height: pipSize)
                pip.position = centre
                pip.path = UIBezierPath(ovalIn: pip.bounds).cgPath
                pip.fillColor = UIColor.white.withAlphaComponent(0.22 * alpha).cgColor
                layer.addSublayer(pip)
                node.pips.append(pip)
            }
        }

        return node
    }

    private func path(for shape: PadControlShape, in rect: CGRect) -> CGPath {
        switch shape {
        case .circle:
            return UIBezierPath(ovalIn: rect).cgPath
        case .capsule:
            return UIBezierPath(roundedRect: rect, cornerRadius: rect.height / 2).cgPath
        case .cross:
            // Two overlapping rounded bars. Same fill, so the overlap is invisible.
            let thickness = rect.width * 0.36
            let radius = thickness * 0.28
            let vertical = CGRect(x: rect.midX - thickness / 2, y: rect.minY,
                                  width: thickness, height: rect.height)
            let horizontal = CGRect(x: rect.minX, y: rect.midY - thickness / 2,
                                    width: rect.width, height: thickness)
            let combined = UIBezierPath(roundedRect: vertical, cornerRadius: radius)
            combined.append(UIBezierPath(roundedRect: horizontal, cornerRadius: radius))
            return combined.cgPath
        }
    }

    // MARK: - hit testing

    override func point(inside point: CGPoint, with event: UIEvent?) -> Bool {
        guard isUserInteractionEnabled, !isHidden, alpha > 0.01, !hiddenForController else {
            return false
        }
        // THE LOAD-BEARING LINE. Everything that is not a control is not ours, and
        // falls through to EdenMetalLayerView -> eden_input_set_touch -> the guest
        // touchscreen.
        return nodes.contains { $0.placed.hitFrame.contains(point) }
    }

    // MARK: - touches

    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
        for touch in touches {
            acquire(touch, allowSticks: true)
        }
        publishButtons()
    }

    override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) {
        for touch in touches {
            let key = ObjectIdentifier(touch)
            let point = touch.location(in: self)

            guard let index = ownership[key] else {
                // A finger that started on bare screen belongs to the guest
                // touchscreen; it must not steal a button by sliding onto one.
                continue
            }
            let node = nodes[index]

            switch node.placed.spec.kind {
            case .stick(let stickIndex):
                update(stick: node, stickIndex: stickIndex, at: point)

            case .dpad:
                if node.placed.hitFrame.contains(point) {
                    update(dpad: node, at: point)
                } else {
                    // Sliding off the d-pad neutralises it rather than latching the
                    // last direction, which is what a physical pad does.
                    setDpad(node, ids: [])
                }

            case .button:
                if !node.placed.hitFrame.contains(point) {
                    // Slide-off releases, and slide-on to a neighbour presses - rolling
                    // A into B with one thumb is a normal way to play.
                    release(index)
                    ownership[key] = nil
                    acquire(touch, allowSticks: false)
                }
            }
        }
        publishButtons()
    }

    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) {
        endTouches(touches)
    }

    override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) {
        endTouches(touches)
    }

    private func endTouches(_ touches: Set<UITouch>) {
        for touch in touches {
            let key = ObjectIdentifier(touch)
            if let index = ownership[key] {
                release(index)
                ownership[key] = nil
            }
        }
        publishButtons()
    }

    // MARK: - acquire / release

    private func acquire(_ touch: UITouch, allowSticks: Bool) {
        let point = touch.location(in: self)
        guard let index = nodeIndex(at: point, allowSticks: allowSticks) else { return }
        ownership[ObjectIdentifier(touch)] = index

        let node = nodes[index]
        switch node.placed.spec.kind {
        case .button:
            node.isPressed = true
            setHighlight(node, pressed: true)
            fireHaptic()
        case .dpad:
            update(dpad: node, at: point)
        case .stick(let stickIndex):
            update(stick: node, stickIndex: stickIndex, at: point)
        }
    }

    /// Nearest-centre wins, so the slop rings of two adjacent buttons resolve the same
    /// way every time instead of depending on array order.
    private func nodeIndex(at point: CGPoint, allowSticks: Bool) -> Int? {
        let owned = Set(ownership.values)
        var best: Int?
        var bestDistance = CGFloat.greatestFiniteMagnitude

        for (index, node) in nodes.enumerated() {
            if case .stick = node.placed.spec.kind, !allowSticks { continue }
            // One control, one finger. Two fingers sharing a button would otherwise
            // release it when the first of them lifts.
            if owned.contains(index) { continue }
            guard node.placed.hitFrame.contains(point) else { continue }
            let centre = CGPoint(x: node.placed.frame.midX, y: node.placed.frame.midY)
            let distance = hypot(point.x - centre.x, point.y - centre.y)
            if distance < bestDistance {
                bestDistance = distance
                best = index
            }
        }
        return best
    }

    private func release(_ index: Int) {
        let node = nodes[index]
        switch node.placed.spec.kind {
        case .button:
            node.isPressed = false
            setHighlight(node, pressed: false)
        case .dpad:
            setDpad(node, ids: [])
        case .stick(let stickIndex):
            CATransaction.begin()
            CATransaction.setDisableActions(true)
            node.knob?.position = CGPoint(x: node.placed.frame.midX, y: node.placed.frame.midY)
            CATransaction.commit()
            EdenInputMixer.shared.setStick(stickIndex, x: 0, y: 0, from: .onScreen)
        }
    }

    /// Drop every held control. Safe to call at any time.
    func releaseAll() {
        for index in nodes.indices {
            release(index)
        }
        ownership.removeAll()
        publishButtons()
        EdenInputMixer.shared.release(.onScreen)
    }

    // MARK: - control updates

    private func update(stick node: ControlNode, stickIndex: Int, at point: CGPoint) {
        let centre = CGPoint(x: node.placed.frame.midX, y: node.placed.frame.midY)
        let radius = node.placed.frame.width / 2
        guard radius > 0 else { return }

        var dx = point.x - centre.x
        var dy = point.y - centre.y
        let distance = hypot(dx, dy)
        if distance > radius, distance > 0 {
            dx = dx / distance * radius
            dy = dy / distance * radius
        }

        CATransaction.begin()
        CATransaction.setDisableActions(true)
        node.knob?.position = CGPoint(x: centre.x + dx, y: centre.y + dy)
        CATransaction.commit()

        var nx = Float(dx / radius)
        var ny = Float(dy / radius)
        let magnitude = (nx * nx + ny * ny).squareRoot()
        let dead = PadLayout.stickDeadFraction
        if magnitude <= dead {
            nx = 0
            ny = 0
        } else {
            // Rescale so the usable travel still reaches full deflection; without this
            // the dead zone would simply cost the top of the range instead of the
            // bottom of it.
            let scaled = min((magnitude - dead) / (1 - dead), 1)
            nx = nx / magnitude * scaled
            ny = ny / magnitude * scaled
        }

        // y is NOT negated. UIKit's +y is down and so is libretro's; retro_input.cpp
        // flips it once on the way into Eden ("libretro's +Y is down; Eden's stick +Y
        // is up"). Flipping here as well would cancel that out and invert every stick.
        EdenInputMixer.shared.setStick(stickIndex, x: nx, y: ny, from: .onScreen)
    }

    private func update(dpad node: ControlNode, at point: CGPoint) {
        let centre = CGPoint(x: node.placed.frame.midX, y: node.placed.frame.midY)
        let half = node.placed.frame.width / 2
        let dx = point.x - centre.x
        let dy = point.y - centre.y
        let distance = hypot(dx, dy)

        var ids: Set<UInt32> = []
        if distance > half * PadLayout.dpadDeadFraction {
            // Eight 45-degree sectors, rotated by half a sector so that a sector is
            // CENTRED on each cardinal and diagonal rather than straddling it.
            // atan2 gives 0 to the right and +pi/2 downwards, matching UIKit's axes.
            let sectorWidth = CGFloat.pi / 4
            let twoPi = CGFloat.pi * 2
            let angle = atan2(dy, dx)
            let rotated = (angle + twoPi + sectorWidth / 2).truncatingRemainder(dividingBy: twoPi)
            switch Int(rotated / sectorWidth) % 8 {
            case 0: ids = [RetroPadID.right]
            case 1: ids = [RetroPadID.right, RetroPadID.down]
            case 2: ids = [RetroPadID.down]
            case 3: ids = [RetroPadID.down, RetroPadID.left]
            case 4: ids = [RetroPadID.left]
            case 5: ids = [RetroPadID.left, RetroPadID.up]
            case 6: ids = [RetroPadID.up]
            default: ids = [RetroPadID.up, RetroPadID.right]
            }
        }
        setDpad(node, ids: ids)
    }

    private func setDpad(_ node: ControlNode, ids: Set<UInt32>) {
        guard ids != node.dpadIDs else { return }
        let wasEmpty = node.dpadIDs.isEmpty
        node.dpadIDs = ids

        let order = [RetroPadID.up, RetroPadID.down, RetroPadID.left, RetroPadID.right]
        let alpha = settings.opacity
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        for (pipIndex, pip) in node.pips.enumerated() where pipIndex < order.count {
            let on = ids.contains(order[pipIndex])
            pip.fillColor = UIColor.white.withAlphaComponent((on ? 0.75 : 0.22) * alpha).cgColor
        }
        CATransaction.commit()

        if wasEmpty, !ids.isEmpty {
            fireHaptic()
        }
    }

    private func setHighlight(_ node: ControlNode, pressed: Bool) {
        let alpha = settings.opacity
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        node.shape.fillColor = UIColor.white
            .withAlphaComponent((pressed ? 0.5 : 0.17) * alpha).cgColor
        CATransaction.commit()
    }

    private func publishButtons() {
        var mask: UInt32 = 0
        for node in nodes {
            switch node.placed.spec.kind {
            case .button(let id):
                if node.isPressed, id < EdenInputMixer.buttonCount {
                    mask |= (1 << id)
                }
            case .dpad:
                for id in node.dpadIDs where id < EdenInputMixer.buttonCount {
                    mask |= (1 << id)
                }
            case .stick:
                break
            }
        }
        EdenInputMixer.shared.setButtons(mask, from: .onScreen)
    }

    private func fireHaptic() {
        guard settings.hapticsEnabled else { return }
        haptics.impactOccurred(intensity: 0.6)
        haptics.prepare()
    }

    // MARK: - hardware controller

    private func applyControllerAttachment(_ attached: Bool) {
        let shouldHide = attached && settings.hideWhenControllerAttached
        guard shouldHide != hiddenForController else { return }
        hiddenForController = shouldHide
        if shouldHide {
            releaseAll()
        }
        applyHiddenState()
    }

    private func applyHiddenState() {
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        for node in nodes {
            node.shape.isHidden = hiddenForController
            node.knob?.isHidden = hiddenForController
            node.pips.forEach { $0.isHidden = hiddenForController }
            node.label?.isHidden = hiddenForController
        }
        CATransaction.commit()
    }
}

// ---------------------------------------------------------------------------
// SwiftUI wrapper
// ---------------------------------------------------------------------------

/// Drop-in replacement for the placeholder `TouchControls` in EmulationView.swift.
///
/// HOOK-UP (EmulationView.swift is not this lane's file to edit):
///
///     TouchControls()                  ->   OnScreenControlLayer()
///         .opacity(showOverlay ? 1 : 0)         .opacity(showOverlay ? 1 : 0)
///         .allowsHitTesting(showOverlay)        .allowsHitTesting(showOverlay)
///         .ignoresSafeArea()                    .ignoresSafeArea()
///
/// `.ignoresSafeArea()` is the one addition, and it is not cosmetic: the layer places
/// controls against the safe-area insets ITSELF, from `safeAreaInsets`, because it needs
/// the full-bleed rect to know where the guest image is. Letting SwiftUI inset the view
/// instead would apply the home indicator margin twice.
///
/// Deleting the old `TouchControls`, `PadButton` and `StickPad` from EmulationView.swift
/// is part of the swap, not optional: `PadButton` calls `eden_input_set_button`, which
/// read-modify-writes the same shared word that this layer STORES through
/// `eden_input_set_button_mask` (EdenCoreBridge.m:699-715). Left in place, whichever
/// wrote last wins and buttons drop at random.
struct OnScreenControlLayer: UIViewRepresentable {

    func makeUIView(context: Context) -> EdenPadView {
        EdenPadView(frame: .zero)
    }

    func updateUIView(_ view: EdenPadView, context: Context) {
        view.refreshSettings()
    }

    static func dismantleUIView(_ view: EdenPadView, coordinator: ()) {
        view.releaseAll()
    }
}
