// SPDX-FileCopyrightText: Copyright 2026 eden-ios contributors
// SPDX-License-Identifier: GPL-3.0-or-later

import Foundation

/// Where a pad input came from. Both sources drive the SAME libretro port.
enum EdenInputSource: Int, CaseIterable {
    /// The SwiftUI/UIKit on-screen control layer.
    case onScreen
    /// A physical MFi / Bluetooth controller, through the GameController framework.
    case hardware
}

/// The one writer of the bridge's shared pad state.
///
/// WHY THIS EXISTS. EdenCoreBridge keeps exactly one button word and two stick pairs
/// for libretro port 0 (EdenCoreBridge.m:96-101), and exposes three producers on top
/// of it: `eden_input_set_button` does a read-modify-write of one bit,
/// `eden_input_set_button_mask` STORES the whole word, and `eden_input_set_stick`
/// stores one stick (EdenCoreBridge.h:167-176).
///
/// So two independent producers cannot both write it and be correct. An MFi pad that
/// publishes its whole state with `set_button_mask` erases every on-screen button in
/// the same instant; a pad that instead publishes per-button would clear A on release
/// even while a thumb is still holding A on the screen. Neither source can see the
/// other's state, so the combination has to happen somewhere, and this is that place:
/// each source owns a private mask, the union is what reaches the bridge, and one
/// `set_button_mask` per change means the bridge's word is never a torn blend of two
/// writers.
///
/// STICKS are not OR-able, so they need a rule rather than a union: a hardware stick
/// wins whenever it is off-centre, otherwise the on-screen stick does. That makes
/// picking up a controller mid-game take effect immediately without the on-screen
/// stick's last value fighting it, and makes putting it down fall back cleanly.
///
/// THREADING. Everything here runs on the main thread: UIKit touches arrive there, and
/// `attach` in HardwareControllerInput pins every GameController handler to the main
/// queue for exactly this reason. The bridge functions themselves are safe from any
/// thread (they are C11 atomics), but the mask bookkeeping below is not, so the
/// precondition is real even though the call it guards is not.
final class EdenInputMixer {

    static let shared = EdenInputMixer()

    private init() {}

    private struct StickValue: Equatable {
        var x: Float = 0
        var y: Float = 0

        var isOffCentre: Bool {
            // Comfortably below any usable deflection, well above float noise.
            (x * x + y * y) > 0.0004
        }
    }

    /// libretro joypad ids are 0...15 (libretro.h:320-371) and
    /// `eden_input_set_button_mask` masks with 0xFFFF (EdenCoreBridge.m:713).
    static let buttonCount: UInt32 = 16

    private var masks = [UInt32](repeating: 0, count: EdenInputSource.allCases.count)
    private var sticks = [[StickValue]](
        repeating: [StickValue](repeating: StickValue(), count: 2),
        count: EdenInputSource.allCases.count)

    private var publishedMask: UInt32 = 0
    private var publishedSticks = [StickValue](repeating: StickValue(), count: 2)

    // MARK: - buttons

    func setButton(_ id: UInt32, pressed: Bool, from source: EdenInputSource) {
        assertMainThread()
        guard id < Self.buttonCount else { return }
        let bit: UInt32 = 1 << id
        if pressed {
            masks[source.rawValue] |= bit
        } else {
            masks[source.rawValue] &= ~bit
        }
        publishButtons()
    }

    /// Replace every button this source owns in one go. Cheaper and less error-prone
    /// than diffing sixteen bits at the call site, which is what a GameController
    /// value-changed handler would otherwise have to do.
    func setButtons(_ mask: UInt32, from source: EdenInputSource) {
        assertMainThread()
        masks[source.rawValue] = mask & 0xFFFF
        publishButtons()
    }

    // MARK: - sticks

    /// `index` is RETRO_DEVICE_INDEX_ANALOG_LEFT (0) or RETRO_DEVICE_INDEX_ANALOG_RIGHT
    /// (1). x/y are in [-1, 1] with **+y DOWN** - libretro's convention, which is also
    /// UIKit's. retro_input.cpp negates y on the way into Eden ("libretro's +Y is down;
    /// Eden's stick +Y is up"), so negating here as well would invert every stick.
    func setStick(_ index: Int, x: Float, y: Float, from source: EdenInputSource) {
        assertMainThread()
        guard index >= 0, index < 2 else { return }
        sticks[source.rawValue][index] = StickValue(x: clamp(x), y: clamp(y))
        publishStick(index)
    }

    // MARK: - release

    /// Drop everything one source is holding. Call it when that source goes away: a
    /// controller disconnecting, the control layer being torn down, the app being
    /// backgrounded. A held button that outlives its producer is stuck forever,
    /// because nothing will ever send its release.
    func release(_ source: EdenInputSource) {
        assertMainThread()
        masks[source.rawValue] = 0
        sticks[source.rawValue] = [StickValue](repeating: StickValue(), count: 2)
        publishButtons()
        publishStick(0)
        publishStick(1)
    }

    func releaseAll() {
        assertMainThread()
        for source in EdenInputSource.allCases {
            masks[source.rawValue] = 0
            sticks[source.rawValue] = [StickValue](repeating: StickValue(), count: 2)
        }
        publishButtons()
        publishStick(0)
        publishStick(1)
    }

    // MARK: - publishing

    private func publishButtons() {
        let combined = masks.reduce(UInt32(0)) { $0 | $1 }
        guard combined != publishedMask else { return }
        publishedMask = combined
        eden_input_set_button_mask(combined)
    }

    private func publishStick(_ index: Int) {
        let hardware = sticks[EdenInputSource.hardware.rawValue][index]
        let onScreen = sticks[EdenInputSource.onScreen.rawValue][index]
        let chosen = hardware.isOffCentre ? hardware : onScreen
        guard chosen != publishedSticks[index] else { return }
        publishedSticks[index] = chosen
        eden_input_set_stick(UInt32(index), chosen.x, chosen.y)
    }

    private func clamp(_ value: Float) -> Float {
        if value.isNaN { return 0 }
        return min(max(value, -1), 1)
    }

    private func assertMainThread() {
        #if DEBUG
        dispatchPrecondition(condition: .onQueue(.main))
        #endif
    }
}
