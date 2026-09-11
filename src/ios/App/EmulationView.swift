// SPDX-FileCopyrightText: Copyright 2026 eden-ios contributors
// SPDX-License-Identifier: GPL-3.0-or-later

import SwiftUI
import UIKit

/// Hosts the one EdenMetalLayerView and the on-screen controls.
struct EmulationView: View {

    @EnvironmentObject private var session: EmulatorSession
    @State private var showOverlay = true

    var body: some View {
        ZStack {
            Color.black.ignoresSafeArea()

            MetalSurface()
                .ignoresSafeArea()

            TouchControls()
                .opacity(showOverlay ? 1 : 0)
                .allowsHitTesting(showOverlay)

            VStack {
                hud
                Spacer()
            }
            .padding(.horizontal, 16)
        }
        .statusBarHidden(true)
        .onTapGesture(count: 2) {
            withAnimation { showOverlay.toggle() }
        }
    }

    private var hud: some View {
        HStack(spacing: 12) {
            Button {
                session.stop()
            } label: {
                Image(systemName: "stop.circle.fill")
                    .font(.title2)
            }

            VStack(alignment: .leading, spacing: 1) {
                Text(session.currentGame?.name ?? "Eden")
                    .font(.caption.weight(.semibold))
                    .lineLimit(1)
                Text(session.status)
                    .font(.caption2)
                    .foregroundColor(.secondary)
                    .lineLimit(1)
            }

            Spacer()

            // Labelled "it/s", not "fps", and deliberately. retro_run returns either
            // when a frame is presented or when kFrameWaitTimeout (50 ms,
            // retro_core.cpp:132) elapses, so a completely stalled guest still reports
            // 20. Calling that "20 fps" would make a hung emulator look like a slow one.
            Text(session.rateDescription)
                .font(.caption.monospacedDigit())
                .foregroundColor(.secondary)
        }
        .padding(10)
        .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 12))
        .foregroundColor(.white)
        .padding(.top, 8)
    }
}

// MARK: - the surface

/// Reparents the single shared EdenMetalLayerView into SwiftUI's hierarchy.
///
/// It is NOT created here. The C side holds the layer unretained - retro_core.cpp:97
/// is a bare `void* g_metal_layer` and RetroEmuWindow copies it into
/// window_info.render_surface at construction (retro_emu_window.cpp:33) - and
/// eden_libretro.h:33 states plainly that "The layer must outlive Core::System".
/// SwiftUI recreates UIViewRepresentable bodies freely on view-identity changes, so a
/// view made in makeUIView could be deallocated under a running renderer. One shared
/// instance, reparented, removes that whole class of bug.
struct MetalSurface: UIViewRepresentable {

    func makeUIView(context: Context) -> UIView {
        let container = UIView()
        container.backgroundColor = .black

        let surface = EdenMetalLayerView.sharedView
        surface.removeFromSuperview()
        surface.translatesAutoresizingMaskIntoConstraints = false
        // Touches reach the guest touchscreen only while this view is on screen.
        surface.forwardsTouchesToGuest = true
        container.addSubview(surface)

        NSLayoutConstraint.activate([
            surface.leadingAnchor.constraint(equalTo: container.leadingAnchor),
            surface.trailingAnchor.constraint(equalTo: container.trailingAnchor),
            surface.topAnchor.constraint(equalTo: container.topAnchor),
            surface.bottomAnchor.constraint(equalTo: container.bottomAnchor),
        ])
        return container
    }

    func updateUIView(_ view: UIView, context: Context) {}

    static func dismantleUIView(_ view: UIView, coordinator: ()) {
        // Stop forwarding, but do NOT tear the view down or detach the layer: the core
        // may still be presenting into it while teardown runs.
        EdenMetalLayerView.sharedView.forwardsTouchesToGuest = false
    }
}

// MARK: - on-screen controls

/// Minimal fixed control layer.
///
/// Face-button mapping is POSITIONAL and matches the core's own, which is worth being
/// explicit about because the obvious alternative is wrong. retro_input.cpp:59-62:
/// "libretro's face buttons are POSITIONAL (B bottom, A right, Y left, X top), which is
/// the Switch layout, so A->A .. Y->Y is correct and must NOT get the SNES-style
/// diagonal swap."
struct TouchControls: View {

    var body: some View {
        HStack {
            StickPad()
                .frame(width: 140, height: 140)
                .padding(.leading, 28)

            Spacer()

            VStack(spacing: 6) {
                PadButton("X", RETRO_DEVICE_ID_JOYPAD_X)
                HStack(spacing: 40) {
                    PadButton("Y", RETRO_DEVICE_ID_JOYPAD_Y)
                    PadButton("A", RETRO_DEVICE_ID_JOYPAD_A)
                }
                PadButton("B", RETRO_DEVICE_ID_JOYPAD_B)
            }
            .padding(.trailing, 28)
        }
        .overlay(alignment: .top) {
            HStack(spacing: 16) {
                PadButton("ZL", RETRO_DEVICE_ID_JOYPAD_L2)
                PadButton("L",  RETRO_DEVICE_ID_JOYPAD_L)
                Spacer().frame(width: 80)
                PadButton("R",  RETRO_DEVICE_ID_JOYPAD_R)
                PadButton("ZR", RETRO_DEVICE_ID_JOYPAD_R2)
            }
            .padding(.top, 70)
        }
        .overlay(alignment: .bottom) {
            HStack(spacing: 24) {
                PadButton("-", RETRO_DEVICE_ID_JOYPAD_SELECT)
                PadButton("+", RETRO_DEVICE_ID_JOYPAD_START)
            }
            .padding(.bottom, 24)
        }
    }
}

private struct PadButton: View {

    private let title: String
    private let id: UInt32
    @State private var pressed = false

    // Generic over BinaryInteger rather than typed Int32. The RETRO_DEVICE_ID_JOYPAD_*
    // values are plain integer #defines (libretro.h:320-380), and which fixed-width
    // Swift type clang's importer gives such a macro is not something to rely on -
    // it has been Int32 and Int in different toolchains. This compiles either way.
    init<T: BinaryInteger>(_ title: String, _ id: T) {
        self.title = title
        self.id = UInt32(id)
    }

    var body: some View {
        Text(title)
            .font(.headline)
            .frame(width: 52, height: 52)
            .background(Circle().fill(Color.white.opacity(pressed ? 0.45 : 0.18)))
            .foregroundColor(.white)
            .contentShape(Circle())
            // minimumDistance 0 so the press registers on touch-down rather than after
            // a drag threshold - a fighting-game input must not wait for movement.
            .gesture(
                DragGesture(minimumDistance: 0)
                    .onChanged { _ in
                        guard !pressed else { return }
                        pressed = true
                        eden_input_set_button(id, true)
                    }
                    .onEnded { _ in
                        pressed = false
                        eden_input_set_button(id, false)
                    }
            )
    }
}

/// Left analog stick.
///
/// Y is passed to the bridge UNNEGATED, i.e. in UIKit's convention where down is
/// positive, because that is also libretro's convention. retro_input.cpp:285-292 does
/// the flip on the way into Eden ("libretro's +Y is down; Eden's stick +Y is up"), so
/// negating here as well would cancel it out and invert every stick in every game.
private struct StickPad: View {

    @State private var offset: CGSize = .zero
    private let radius: CGFloat = 60

    var body: some View {
        ZStack {
            Circle().fill(Color.white.opacity(0.12))
            Circle()
                .fill(Color.white.opacity(0.35))
                .frame(width: 52, height: 52)
                .offset(offset)
        }
        .contentShape(Circle())
        .gesture(
            DragGesture(minimumDistance: 0)
                .onChanged { value in
                    var dx = value.translation.width
                    var dy = value.translation.height
                    let distance = sqrt(dx * dx + dy * dy)
                    if distance > radius {
                        dx = dx / distance * radius
                        dy = dy / distance * radius
                    }
                    offset = CGSize(width: dx, height: dy)
                    eden_input_set_stick(UInt32(RETRO_DEVICE_INDEX_ANALOG_LEFT),
                                         Float(dx / radius),
                                         Float(dy / radius))
                }
                .onEnded { _ in
                    offset = .zero
                    eden_input_set_stick(UInt32(RETRO_DEVICE_INDEX_ANALOG_LEFT), 0, 0)
                }
        )
    }
}
