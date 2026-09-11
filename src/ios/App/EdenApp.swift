// SPDX-FileCopyrightText: Copyright 2026 eden-ios contributors
// SPDX-License-Identifier: GPL-3.0-or-later

import SwiftUI
import UIKit

@main
struct EdenApp: App {

    @StateObject private var session = EmulatorSession()
    @StateObject private var jit = JITAvailability()
    @Environment(\.scenePhase) private var scenePhase

    init() {
        // Create the two directories the app itself owns, so that Files.app has
        // somewhere to show the user before they have ever launched a game. The core
        // creates the rest of the tree inside retro_init. Failing here is not fatal -
        // the setup guide reports it rather than the app refusing to start.
        do {
            try EdenPaths.prepare()
        } catch {
            NSLog("eden: could not prepare the data root: \(error.localizedDescription)")
        }

        // NOT a static initialiser, and that distinction is load-bearing.
        //
        // docs/IOS_PORT_NOTES.md: "JIT memory is no longer allocated during static
        // initialisation - SpinLockImpl was a namespace-scope global whose constructor
        // mmapped executable memory at dyld load, before main(), so a device without
        // JIT permission killed the app before any UI could explain why."
        //
        // App.init() runs after dyld has finished and after main(), so a refusal here
        // is survivable and can be shown to the user. Probing from a +load or a
        // file-scope `let` would reintroduce exactly the bug that was removed.
        _ = eden_jit_probe()
    }

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(session)
                .environmentObject(jit)
                .preferredColorScheme(.dark)
        }
        .onChange(of: scenePhase) { phase in
            // Scene phase, not UIApplicationDelegate.
            //
            // eden_libretro.h:45-46 names applicationDidEnterBackground /
            // applicationWillEnterForeground, but a SwiftUI App is scene-based and
            // UIKit routes scene lifecycle to scenes; the UIApplication notifications
            // still fire, but scenePhase is the one that is authoritative for this app
            // shape and does not need a delegate at all. Info.plist sets
            // UIApplicationSupportsMultipleScenes = false, so there is exactly one
            // scene and exactly one answer.
            //
            // Both directions land on eden_bridge_set_visible, which forwards to
            // eden_libretro_set_visible -> RetroEmuWindow::SetShown. That is what makes
            // RendererVulkan::Composite early-return instead of presenting against a
            // drawable iOS has taken away. The bridge additionally parks its run loop.
            switch phase {
            case .active:
                eden_bridge_set_visible(true)
                UIApplication.shared.isIdleTimerDisabled = session.isRunning
            case .inactive, .background:
                eden_bridge_set_visible(false)
                UIApplication.shared.isIdleTimerDisabled = false
            @unknown default:
                eden_bridge_set_visible(false)
            }
        }
    }
}
