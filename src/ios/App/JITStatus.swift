// SPDX-FileCopyrightText: Copyright 2026 eden-ios contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Telling the user the truth about JIT.
//
// The copy in this file is the whole point of the piece. docs/IOS_PORT_NOTES.md:
// "There is no interpreter. HAS_NCE is gated on ARCHITECTURE_arm64 AND (ANDROID OR
// LINUX), and KProcess::InitializeInterfaces constructs ArmDynarmic64/ArmDynarmic32
// unconditionally with no error path. No JIT does not mean slow emulation; it means no
// emulation."
//
// So there is no honest version of this screen that offers to continue anyway.

import Combine
import Foundation
import SwiftUI
import UIKit

/// Identifiable so the refusal screen can be presented with `.sheet(item:)`, which
/// carries the status that caused the refusal rather than reading a second source.
enum JITStatus: Equatable, Identifiable {

    var id: String {
        switch self {
        case .viaEntitlement: return "entitlement"
        case .viaDebugger:    return "debugger"
        case .unavailable:    return "unavailable"
        }
    }

    /// dynamic-codesigning in the signature. TrollStore-class install. Survives relaunch.
    case viaEntitlement
    /// CS_DEBUGGED. StikDebug/SideStore-class. This launch only.
    case viaDebugger
    /// Nothing found. No emulation is possible.
    case unavailable

    var permitsEmulation: Bool { self != .unavailable }

    static func current() -> JITStatus {
        switch eden_jit_report().verdict {
        case EdenJITViaEntitlement: return .viaEntitlement
        case EdenJITViaDebugger:    return .viaDebugger
        default:                    return .unavailable
        }
    }

    var headline: String {
        switch self {
        case .viaEntitlement: return "JIT is available"
        case .viaDebugger:    return "JIT is available for this launch"
        case .unavailable:    return "Eden cannot run without JIT"
        }
    }

    var detail: String {
        switch self {
        case .viaEntitlement:
            return """
            This build carries the dynamic-codesigning entitlement, so the recompiler \
            can make memory executable. It will keep working after a relaunch.

            One caveat worth knowing: the probe only confirmed that the mapping calls \
            succeed. It deliberately never executed anything, because on iOS a \
            code-signing violation arrives as an uncatchable signal - a failed test \
            would be indistinguishable from the app crashing at launch. If a game dies \
            the moment it starts running code, that is the case this check cannot see.
            """

        case .viaDebugger:
            return """
            A debugger is attached, which sets CS_DEBUGGED and lets the recompiler make \
            memory executable.

            This lasts only until the app is force-quit. The next launch has to go \
            through the same tool again, or JIT will be gone and games will not start.
            """

        case .unavailable:
            return """
            Eden translates Switch code into ARM code while it runs, and that requires \
            permission to make memory executable. iOS does not grant that to an \
            ordinary app.

            There is no slower fallback. Eden has no interpreter, so without JIT there \
            is no emulation at all - not reduced performance, nothing.

            Two things are known to grant it:

            • A debugger attaching at launch (StikDebug and similar tools do this \
            deliberately). Available on newer iOS. JIT lasts until you force-quit the \
            app, so the tool has to be used at every launch.

            • The dynamic-codesigning entitlement, which TrollStore can give an app on \
            the older iOS versions where it works. This survives relaunches.

            Which applies depends on your exact iOS version, and the supported ranges \
            move - check the tool's own README rather than trusting a number here. \
            Install the matching IPA variant for whichever route you use.
            """
        }
    }

    /// Which of the three IPAs to install. Named, because getting this wrong is the
    /// most common way a correct build still refuses to run.
    var recommendedVariant: String? {
        switch self {
        case .unavailable:
            return "Eden-TrollStore.ipa for a TrollStore install; Eden.ipa for a "
                 + "sideload where a debugger attaches at launch."
        case .viaDebugger, .viaEntitlement:
            return nil
        }
    }

    var symbolName: String {
        switch self {
        case .viaEntitlement: return "checkmark.seal"
        case .viaDebugger:    return "ant.circle"
        case .unavailable:    return "exclamationmark.triangle"
        }
    }

    var tint: Color {
        switch self {
        case .viaEntitlement: return .green
        case .viaDebugger:    return .yellow
        case .unavailable:    return .red
        }
    }
}

/// Observable wrapper with re-probe support.
///
/// "Check again" is a real button, not decoration: CS_DEBUGGED can appear mid-process
/// when a tool attaches to an already-running app, so a user who launched Eden first
/// and StikDebug second can fix their situation without relaunching. The verdict is a
/// snapshot valid at the moment it was taken and nothing here pretends otherwise.
@MainActor
final class JITAvailability: ObservableObject {

    @Published private(set) var status: JITStatus
    @Published private(set) var diagnostics: String

    init() {
        status = JITStatus.current()
        diagnostics = JITAvailability.readDiagnostics()
    }

    func recheck() {
        // eden_jit_probe re-runs the syscalls unless the verdict has been locked, which
        // happens once emulation starts. Probing on a timer is deliberately not offered:
        // Apple documents an arm64 process as having a single JIT region and it is
        // unverified whether a released probe mapping frees it cleanly.
        _ = eden_jit_probe()
        status = JITStatus.current()
        diagnostics = JITAvailability.readDiagnostics()
    }

    private static func readDiagnostics() -> String {
        var buffer = [CChar](repeating: 0, count: 2048)
        eden_jit_copy_diagnostics(&buffer, buffer.count)
        return String(cString: buffer)
    }
}

// MARK: - the refusal screen

struct JITRequiredView: View {

    let status: JITStatus
    @EnvironmentObject private var jit: JITAvailability
    @Environment(\.dismiss) private var dismiss
    @State private var showDiagnostics = false

    var body: some View {
        NavigationView {
            ScrollView {
                VStack(alignment: .leading, spacing: 20) {

                    HStack(spacing: 12) {
                        Image(systemName: status.symbolName)
                            .font(.system(size: 34))
                            .foregroundColor(status.tint)
                        Text(status.headline)
                            .font(.title2.weight(.semibold))
                    }

                    Text(status.detail)
                        .font(.callout)
                        .fixedSize(horizontal: false, vertical: true)

                    if let variant = status.recommendedVariant {
                        VStack(alignment: .leading, spacing: 6) {
                            Text("Which build to install")
                                .font(.subheadline.weight(.semibold))
                            Text(variant)
                                .font(.callout)
                                .fixedSize(horizontal: false, vertical: true)
                        }
                        .padding(12)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .background(Color.secondary.opacity(0.12))
                        .cornerRadius(10)
                    }

                    HStack {
                        Button {
                            jit.recheck()
                        } label: {
                            Label("Check again", systemImage: "arrow.clockwise")
                        }
                        .buttonStyle(.bordered)

                        Spacer()

                        Button {
                            UIPasteboard.general.string = jit.diagnostics
                        } label: {
                            Label("Copy diagnostics", systemImage: "doc.on.doc")
                        }
                        .buttonStyle(.bordered)
                    }

                    DisclosureGroup("Probe output", isExpanded: $showDiagnostics) {
                        Text(jit.diagnostics)
                            .font(.system(.caption, design: .monospaced))
                            .textSelection(.enabled)
                            .frame(maxWidth: .infinity, alignment: .leading)
                    }
                    .font(.subheadline)
                }
                .padding(20)
            }
            .navigationTitle("JIT")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Close") { dismiss() }
                }
            }
        }
        .navigationViewStyle(.stack)
    }
}
