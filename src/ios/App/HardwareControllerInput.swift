// SPDX-FileCopyrightText: Copyright 2026 eden-ios contributors
// SPDX-License-Identifier: GPL-3.0-or-later

import Foundation
import GameController

/// Physical controllers, through the GameController framework.
///
/// GameController.framework is already linked (src/ios/project.yml:221, listed there
/// because SDL3's iOS backend references it), and needs no Info.plist key and no
/// permission prompt. A controller the user has already paired in Settings shows up as
/// a `.GCControllerDidConnect` notification with no discovery call; the explicit
/// `startWirelessControllerDiscovery` dance is only for putting a legacy MFi controller
/// into pairing mode, which iOS Settings does better, so it is not called here.
///
/// ONE CONTROLLER, DELIBERATELY. The bridge keeps a single button word and one pair of
/// sticks (EdenCoreBridge.m:96-101), i.e. libretro port 0 and nothing else, so a second
/// controller has nowhere to go. It is adopted as the active one only if no other is.
/// Local multiplayer needs per-port state in the bridge first - see the note at the
/// bottom of this file.
///
/// SDL3 IS NOT USED FOR THIS, and that is worth a sentence because docs/IOS_PORT_NOTES.md
/// rightly says to prefer it. SDL3 is linked into the CORE (audio_core and input_common
/// link it unconditionally) and would deliver controller events into Eden's own SDL
/// input engine - but this app is a libretro FRONTEND: the core's input comes from
/// `state_cb` through RetroInput, not from Eden's engines directly, and RetroInput binds
/// the VirtualGamepad engine rather than the SDL one. Feeding SDL as well would produce
/// two engines writing the same EmulatedController. The frontend's job is to reach the
/// libretro ABI, and GameController is the framework-native way to do that.
final class HardwareControllerInput {

    static let shared = HardwareControllerInput()

    private init() {}

    /// True while an extended gamepad is attached and driving input.
    private(set) var isControllerAttached = false

    /// Called on the main queue whenever that changes. The on-screen pad uses it to get
    /// out of the way.
    var onAttachmentChanged: ((Bool) -> Void)?

    /// Display name of the active controller, for a HUD or a settings screen.
    private(set) var activeControllerName: String?

    private var started = false
    private var observers: [NSObjectProtocol] = []
    /// Weak: GCController keeps connected controllers alive itself, and a disconnected
    /// one must not be kept alive by us.
    private weak var active: GCController?

    // MARK: - lifecycle

    /// Idempotent. Safe to call from a view appearing.
    func start() {
        guard !started else { return }
        started = true

        let centre = NotificationCenter.default
        observers.append(centre.addObserver(forName: .GCControllerDidConnect,
                                            object: nil,
                                            queue: .main) { [weak self] note in
            self?.handleConnect(note.object as? GCController)
        })
        observers.append(centre.addObserver(forName: .GCControllerDidDisconnect,
                                            object: nil,
                                            queue: .main) { [weak self] note in
            self?.handleDisconnect(note.object as? GCController)
        })

        adoptFirstAvailable()
    }

    func stop() {
        guard started else { return }
        started = false
        for observer in observers {
            NotificationCenter.default.removeObserver(observer)
        }
        observers.removeAll()
        detach()
    }

    // MARK: - attachment

    private func handleConnect(_ controller: GCController?) {
        guard let controller = controller else { return }
        guard active == nil else { return }   // one at a time; see the type comment
        attach(controller)
    }

    private func handleDisconnect(_ controller: GCController?) {
        guard let controller = controller, controller === active else { return }
        detach()
        // Fall back to another controller that is still connected rather than dropping
        // the user to on-screen controls because one of two pads went flat.
        adoptFirstAvailable()
    }

    private func adoptFirstAvailable() {
        guard active == nil else { return }
        for controller in GCController.controllers() where controller.extendedGamepad != nil {
            attach(controller)
            return
        }
    }

    private func attach(_ controller: GCController) {
        guard let pad = controller.extendedGamepad else { return }

        // Everything downstream (EdenInputMixer) is main-thread state. The default is
        // already the main queue; setting it explicitly means a future change of that
        // default cannot silently introduce a data race.
        controller.handlerQueue = .main
        controller.playerIndex = .index1

        active = controller
        activeControllerName = controller.vendorName

        pad.valueChangedHandler = { [weak self] gamepad, _ in
            self?.publish(gamepad)
        }

        // Publish once immediately: a button already held when the app came forward
        // would otherwise not be reported until it changed.
        publish(pad)
        setAttached(true)
    }

    private func detach() {
        active?.extendedGamepad?.valueChangedHandler = nil
        active = nil
        activeControllerName = nil
        EdenInputMixer.shared.release(.hardware)
        setAttached(false)
    }

    private func setAttached(_ attached: Bool) {
        guard attached != isControllerAttached else { return }
        isControllerAttached = attached
        onAttachmentChanged?(attached)
    }

    // MARK: - publishing

    private func publish(_ pad: GCExtendedGamepad) {
        var mask: UInt32 = 0
        func press(_ id: UInt32, _ on: Bool) {
            if on, id < EdenInputMixer.buttonCount {
                mask |= (1 << id)
            }
        }

        // FACE BUTTONS ARE MAPPED BY POSITION, NOT BY LETTER, and the two happen to
        // disagree between the two vocabularies:
        //
        //   GameController names them by PHYSICAL POSITION - buttonA is always the
        //   BOTTOM one, buttonB the RIGHT one, whatever is silkscreened on the pad.
        //   Apple remaps Nintendo-layout controllers into that scheme.
        //
        //   libretro also names them by position, with the SNES arrangement:
        //   RETRO_DEVICE_ID_JOYPAD_B is the bottom one and _A the right one
        //   (libretro.h:320-347).
        //
        //   The Switch labels the RIGHT button A and the BOTTOM one B.
        //
        // So bottom -> bottom and right -> right is the whole rule, which spells out as
        // GC buttonA -> retro B and GC buttonB -> retro A. retro_input.cpp:59-62 makes
        // the matching point on its own side: "libretro's face buttons are POSITIONAL
        // ... so A->A .. Y->Y is correct and must NOT get the SNES-style diagonal swap."
        press(RetroPadID.b, pad.buttonA.isPressed) // bottom
        press(RetroPadID.a, pad.buttonB.isPressed) // right
        press(RetroPadID.y, pad.buttonX.isPressed) // left
        press(RetroPadID.x, pad.buttonY.isPressed) // top

        press(RetroPadID.l, pad.leftShoulder.isPressed)
        press(RetroPadID.r, pad.rightShoulder.isPressed)

        // The Switch's ZL/ZR are digital even on a Pro Controller, so the analog value
        // an MFi trigger reports is thresholded by GameController into `isPressed` and
        // that is all Eden can use: VirtualGamepad has no analog trigger input -
        // TriggerZL and TriggerZR are VirtualButtons (virtual_gamepad.h:24-25).
        press(RetroPadID.zl, pad.leftTrigger.isPressed)
        press(RetroPadID.zr, pad.rightTrigger.isPressed)

        press(RetroPadID.plus, pad.buttonMenu.isPressed)
        press(RetroPadID.minus, pad.buttonOptions?.isPressed ?? false)

        press(RetroPadID.lStick, pad.leftThumbstickButton?.isPressed ?? false)
        press(RetroPadID.rStick, pad.rightThumbstickButton?.isPressed ?? false)

        press(RetroPadID.up, pad.dpad.up.isPressed)
        press(RetroPadID.down, pad.dpad.down.isPressed)
        press(RetroPadID.left, pad.dpad.left.isPressed)
        press(RetroPadID.right, pad.dpad.right.isPressed)

        // pad.buttonHome exists (iOS 14+) and the Switch has a Home button, but there
        // is no libretro id left to carry it: RETRO_DEVICE_ID_JOYPAD_* runs 0...15 and
        // retro_input.cpp's kPadMap already spends all sixteen. Eden would accept it -
        // VirtualButton::ButtonHome exists and ConnectPlayers() calls
        // EnableSystemButtons() - so the missing piece is a frontend-specific producer,
        // not anything in hid_core. Same for Capture.

        EdenInputMixer.shared.setButtons(mask, from: .hardware)

        // STICK SIGN. GameController's thumbstick yAxis is +1 UP. The bridge and
        // libretro both want +1 DOWN (retro_input.cpp negates y once on the way into
        // Eden), so y is negated here exactly once.
        EdenInputMixer.shared.setStick(RetroPadID.analogLeft,
                                       x: pad.leftThumbstick.xAxis.value,
                                       y: -pad.leftThumbstick.yAxis.value,
                                       from: .hardware)
        EdenInputMixer.shared.setStick(RetroPadID.analogRight,
                                       x: pad.rightThumbstick.xAxis.value,
                                       y: -pad.rightThumbstick.yAxis.value,
                                       from: .hardware)

        // MOTION IS NOT FORWARDED, and cannot be from here.
        //
        // `active?.motion` is a GCMotion and a DualSense/DualShock/Joy-Con reports real
        // gyro through it. RetroInput reads motion ONLY through the libretro sensor
        // interface (`sensors.get_sensor_input`, retro_input.cpp PollMotion), which the
        // frontend supplies by answering RETRO_ENVIRONMENT_GET_SENSOR_INTERFACE - and
        // EdenCoreBridge.m currently returns false for that env call ("NOT IMPLEMENTED
        // IN CUT ONE"). There is no other door: no `eden_input_set_motion` exists.
        // Closing this gap is a bridge change, described in the handover notes.
    }
}
