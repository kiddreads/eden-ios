// SPDX-FileCopyrightText: Copyright 2026 eden-ios contributors
// SPDX-License-Identifier: GPL-3.0-or-later

import Combine
import Foundation
import SwiftUI
import UIKit

/// The Swift face of EdenCoreBridge. Owns the security-scoped resource for the session
/// and the JIT gate; owns no libretro state of its own.
@MainActor
final class EmulatorSession: ObservableObject {

    enum Phase: Equatable {
        case idle
        case starting
        case running
        case stopping
        case failed(String)
    }

    @Published private(set) var phase: Phase = .idle
    @Published private(set) var status: String = "idle"
    @Published private(set) var iterationsPerSecond: Double = 0
    @Published private(set) var currentGame: GameEntry?

    /// Set when a launch is refused because JIT is not available, so the UI can present
    /// JITRequiredView instead of a generic error.
    @Published var jitRefusal: JITStatus?

    var isRunning: Bool {
        if case .running = phase { return true }
        return false
    }

    /// The security-scoped URL held open for the whole session.
    ///
    /// docs/IOS_PORT_NOTES.md #4: "A ROM outside the container needs its
    /// security-scoped resource held for the whole session, not just at open time."
    /// The core opens the file lazily and repeatedly - retro_get_system_info sets
    /// need_fullpath = true (retro_core.cpp:349) and Core::System::Load walks split
    /// dumps and NCA containers long after retro_load_game returned - so releasing the
    /// scope at the end of the launch call would revoke access midway through play.
    private var scopedURL: URL?

    private var pollTimer: AnyCancellable?

    // MARK: - launching

    func start(_ game: GameEntry) {
        guard case .idle = phase else { return }

        // THE JIT GATE, and it goes here - before anything touches the core.
        //
        // docs/IOS_PORT_NOTES.md: "There is no interpreter. ... No JIT does not mean
        // slow emulation; it means no emulation." Without this check the first thing
        // the user sees is the app disappearing partway into a load, because dynarmic
        // dies at its first code emission and a code-signing violation on iOS is an
        // uncatchable signal.
        //
        // Checked on the main thread before the emulation thread is created, rather
        // than inside the bridge: by the time the bridge is running, presenting a
        // SwiftUI explanation means hopping back here anyway.
        let jitStatus = JITStatus.current()
        guard jitStatus.permitsEmulation else {
            jitRefusal = jitStatus
            return
        }
        // Freeze the verdict: no more probe mappings once oaknut may be allocating its
        // own. See eden_jit_lock_verdict's comment on the single-JIT-region question.
        eden_jit_lock_verdict()

        // Hold the scope BEFORE the bridge is told the path.
        if game.needsSecurityScope {
            guard game.url.startAccessingSecurityScopedResource() else {
                phase = .failed("iOS refused access to that file. Re-import it from Files.")
                return
            }
            scopedURL = game.url
        }

        currentGame = game
        phase = .starting
        status = "starting"
        jitRefusal = nil

        let started = game.url.path.withCString { rom in
            EdenPaths.dataRootPath.withCString { root in
                eden_bridge_start(rom, root)
            }
        }

        guard started else {
            releaseScope()
            phase = .failed(readStatus())
            status = readStatus()
            currentGame = nil
            return
        }

        UIApplication.shared.isIdleTimerDisabled = true
        beginPolling()
    }

    func stop() {
        guard phase != .idle else { return }
        phase = .stopping
        status = "shutting down"

        // eden_bridge_stop joins the emulation thread, and that join can be long: a
        // stop requested while retro_load_game is still inside Core::System::Load
        // cannot take effect until the load returns, because there is no cancellation
        // path through Eden's loader. Doing it on a background queue keeps the main
        // thread responsive - which is the same rule as #3 in IOS_PORT_NOTES.md, for
        // the same reason.
        Task.detached(priority: .userInitiated) {
            eden_bridge_stop()
            await MainActor.run {
                self.releaseScope()
                self.endPolling()
                self.phase = .idle
                self.status = self.readStatus()
                self.currentGame = nil
                self.iterationsPerSecond = 0
                UIApplication.shared.isIdleTimerDisabled = false
            }
        }
    }

    private func releaseScope() {
        if let url = scopedURL {
            url.stopAccessingSecurityScopedResource()
            scopedURL = nil
        }
    }

    // MARK: - polling
    //
    // Pull, not push. The bridge could take a callback, but every one of these values
    // is produced on the emulation thread and consumed by SwiftUI on the main thread,
    // and a 4 Hz poll of three scalars is cheaper and far easier to reason about than
    // marshalling callbacks across that boundary sixty times a second.

    private func beginPolling() {
        pollTimer = Timer.publish(every: 0.25, on: .main, in: .common)
            .autoconnect()
            .sink { [weak self] _ in
                self?.refresh()
            }
    }

    private func endPolling() {
        pollTimer?.cancel()
        pollTimer = nil
    }

    private func refresh() {
        status = readStatus()
        iterationsPerSecond = eden_bridge_iterations_per_second()

        switch eden_bridge_state() {
        case EdenBridgeStateRunning:
            phase = .running
        case EdenBridgeStateStarting:
            phase = .starting
        case EdenBridgeStateStopping:
            phase = .stopping
        case EdenBridgeStateFailed:
            phase = .failed(status)
            endPolling()
            releaseScope()
            UIApplication.shared.isIdleTimerDisabled = false
        case EdenBridgeStateIdle:
            // The core asked to shut down on its own - RETRO_ENVIRONMENT_SHUTDOWN,
            // raised either by the exit callback registered at load
            // (retro_core.cpp:317-323) or by the GetExitRequested poll in retro_run
            // (retro_core.cpp:558-560).
            if phase != .idle {
                phase = .idle
                releaseScope()
                endPolling()
                currentGame = nil
                UIApplication.shared.isIdleTimerDisabled = false
            }
        default:
            break
        }
    }

    private func readStatus() -> String {
        var buffer = [CChar](repeating: 0, count: 512)
        eden_bridge_copy_status(&buffer, buffer.count)
        return String(cString: buffer)
    }

    /// Frames-per-second text for the HUD, with the caveat spelled out.
    ///
    /// This is FRONTEND ITERATIONS, not guest frames. retro_run returns either when a
    /// frame is presented or when kFrameWaitTimeout (50 ms, retro_core.cpp:132)
    /// elapses, so a completely stalled guest still reports 20. Labelling it "fps"
    /// without that caveat would make a hung emulator look like a slow one.
    var rateDescription: String {
        String(format: "%.0f it/s", iterationsPerSecond)
    }
}
