// SPDX-FileCopyrightText: Copyright 2026 eden-ios contributors
// SPDX-License-Identifier: GPL-3.0-or-later

import Foundation
import SwiftUI

/// Live keys-and-firmware state, read from the core rather than guessed in Swift.
///
/// WHY THIS DOES NOT JUST LOOK AT THE FILESYSTEM ITSELF
/// Two of the five states cannot be answered from Swift at all:
///
///   * "bad keys" is `KeyManager::BaseDeriveNecessary()` (key_manager.cpp:692-710) - a
///     prod.keys can exist, parse, and still be missing the master / key-area / titlekek
///     pair for a newer crypto revision. That is the single most common reason a user
///     believes an emulator is broken: the file is right there, so nothing looks wrong.
///   * "firmware present but wrong version" needs the SystemVersion title's NCA to be
///     decrypted and its RomFS parsed.
///
/// Both live behind eden_content_* in src/ios/App/EdenContentBridge.h. Everything here
/// is a thin wrapper over those calls.
///
/// THREADING. EdenContentBridge.h requires that these are called from one serial queue,
/// and never while a game is loaded: they touch process-wide globals, Common::FS's path
/// table and the KeyManager singleton, none of which is locked. `work` is that queue.
/// `guard eden_bridge_state() == ...` is the "no game loaded" half.
///
/// Nothing here ships, downloads or generates keys, firmware or games. Every path it
/// writes to is inside the app's own container, and every byte it writes came from a
/// file the user picked.
@MainActor
final class EdenSetupModel: ObservableObject {

    // MARK: - published state

    enum Stage: Equatable {
        case noDataRoot
        case noKeys
        case badKeys
        case noFirmware
        case firmwareUnreadable
        case firmwareWrongVersion
        case ready
        /// Nothing has been read yet.
        case unknown
    }

    @Published private(set) var stage: Stage = .unknown
    @Published private(set) var detail: String = ""

    @Published private(set) var baseKeyFilePresent = false
    @Published private(set) var titleKeyFilePresent = false
    @Published private(set) var amiiboKeyFilePresent = false
    @Published private(set) var keysUsable = false
    @Published private(set) var firmwareFileCount: UInt64 = 0

    /// Set only after `verifyFirmware()` has run at least once this session.
    @Published private(set) var firmwareVersion: String = ""
    @Published private(set) var firmwareChecked = false
    @Published private(set) var firmwareDetail: String = ""

    /// The Horizon OS version this build emulates, e.g. "23.0.0".
    @Published private(set) var targetVersion: String = ""

    @Published private(set) var rootPath: String = ""
    @Published private(set) var keysPath: String = ""
    @Published private(set) var keysImportPath: String = ""
    @Published private(set) var firmwarePath: String = ""
    @Published private(set) var baseKeyFileName: String = "prod.keys"

    /// A long operation is in flight; the UI disables its buttons and says which.
    @Published private(set) var busy: String?

    /// The result of the last install or check, for an alert.
    @Published var lastMessage: String?

    // MARK: - plumbing

    /// SERIAL, and that is the requirement rather than a preference: EdenContentBridge.h
    /// says these entry points must be called from one queue. Task.detached - which the
    /// rest of this app uses for background work - gives no such ordering.
    private let work = DispatchQueue(label: "dev.edenios.setup", qos: .userInitiated)

    /// True while the core owns the KeyManager and the path table, i.e. a game is up.
    /// Every entry point checks it: calling in anyway is not "probably fine", it is an
    /// unsynchronised read of state the emulation thread is writing.
    var coreIsBusy: Bool { !Self.coreIsIdle() }

    /// The same test, callable from the background queue. `g.state` is a C11 atomic
    /// (EdenCoreBridge.m:132, :425), so reading it off the main thread is fine.
    ///
    /// Checked AGAIN inside every background closure, not just before dispatching: the
    /// main-thread check and the core call are not one atomic step, and a game started
    /// in between would have the emulation thread writing the KeyManager and the path
    /// table while this reads them. The window is tiny - the setup sheet is modal, so
    /// the library is not reachable while it is up - but "tiny" is not "closed".
    nonisolated static func coreIsIdle() -> Bool {
        let state = eden_bridge_state()
        return state == EdenBridgeStateIdle || state == EdenBridgeStateFailed
    }

    // MARK: - reading

    /// Cheap: file existence plus one directory scan. Safe to call on every appear.
    func refresh() {
        guard busy == nil, !coreIsBusy else { return }
        work.async { [weak self] in
            guard Self.coreIsIdle() else { return }
            let snapshot = Self.readSnapshot(applyRoot: true)
            Task { @MainActor [weak self] in self?.apply(snapshot) }
        }
    }

    /// Expensive: parses every installed NCA header. Explicitly user-triggered, never
    /// automatic, because on a full firmware set it is seconds of work.
    func verifyFirmware() {
        guard busy == nil, !coreIsBusy else { return }
        busy = "Checking the installed firmware…"
        work.async { [weak self] in
            guard Self.coreIsIdle() else {
                Task { @MainActor [weak self] in
                    self?.busy = nil
                    self?.lastMessage = "A game started while this was checking. Stop it and "
                                      + "try again."
                }
                return
            }
            var check = EdenFirmwareCheck()
            var version = [CChar](repeating: 0, count: 64)
            var text = [CChar](repeating: 0, count: 2048)
            version.withUnsafeMutableBufferPointer { versionBuffer in
                text.withUnsafeMutableBufferPointer { textBuffer in
                    _ = eden_content_verify_firmware(&check,
                                                     versionBuffer.baseAddress, versionBuffer.count,
                                                     textBuffer.baseAddress, textBuffer.count)
                }
            }
            let readVersion = String(cString: version)
            let readDetail = String(cString: text)
            // Re-read the cheap snapshot too: verifying can change the headline state
            // from "ready" to "wrong version", and they must not disagree on screen.
            let snapshot = Self.readSnapshot(applyRoot: false, verified: check)

            Task { @MainActor [weak self] in
                guard let self else { return }
                self.busy = nil
                self.firmwareChecked = true
                self.firmwareDetail = readDetail
                self.apply(snapshot)
                // After apply, not before: apply falls back to major.minor.micro, and
                // the display_version string the firmware itself carries is the better
                // answer when there is one.
                if !readVersion.isEmpty {
                    self.firmwareVersion = readVersion
                }
                self.lastMessage = readDetail
            }
        }
    }

    // MARK: - installing

    /// `source` is a file or a folder the user picked. It may live outside the app, so
    /// its security-scoped resource is held across the whole background call - not just
    /// long enough to start it (docs/IOS_PORT_NOTES.md #4 makes the same point for ROMs).
    func installKeys(from source: URL) {
        run(describing: "Installing keys…", source: source) { path, message, capacity in
            eden_content_install_keys(path, message, capacity)
        }
    }

    func installFirmware(from source: URL, replaceExisting: Bool) {
        run(describing: "Installing firmware…", source: source) { path, message, capacity in
            eden_content_install_firmware(path, replaceExisting, message, capacity)
        }
    }

    private func run(describing label: String,
                     source: URL,
                     body: @escaping (UnsafePointer<CChar>, UnsafeMutablePointer<CChar>?, Int) -> Int32) {
        guard busy == nil else { return }
        guard !coreIsBusy else {
            lastMessage = "Stop the running game first. Keys and firmware cannot be changed "
                        + "while the core has a game loaded."
            return
        }

        busy = label
        let scoped = source.startAccessingSecurityScopedResource()
        let path = source.path

        work.async { [weak self] in
            guard Self.coreIsIdle() else {
                Task { @MainActor [weak self] in
                    if scoped { source.stopAccessingSecurityScopedResource() }
                    self?.busy = nil
                    self?.lastMessage = "A game started before this could run. Stop it and "
                                      + "try again."
                }
                return
            }
            var buffer = [CChar](repeating: 0, count: 4096)
            path.withCString { cPath in
                buffer.withUnsafeMutableBufferPointer { out in
                    _ = body(cPath, out.baseAddress, out.count)
                }
            }
            let message = String(cString: buffer)
            // Installing firmware invalidates any earlier verification.
            let snapshot = Self.readSnapshot(applyRoot: false)

            Task { @MainActor [weak self] in
                guard let self else { return }
                if scoped { source.stopAccessingSecurityScopedResource() }
                self.busy = nil
                self.firmwareChecked = false
                self.firmwareVersion = ""
                self.firmwareDetail = ""
                self.apply(snapshot)
                self.lastMessage = message
            }
        }
    }

    // MARK: - core calls (background queue only)

    private struct Snapshot {
        var raw = EdenContentSnapshot()
        var detail = ""
        var root = ""
        var keys = ""
        var keysImport = ""
        var firmware = ""
        var verified: EdenFirmwareCheck?
    }

    private nonisolated static func readSnapshot(applyRoot: Bool,
                                                 verified: EdenFirmwareCheck? = nil) -> Snapshot {
        if applyRoot {
            // Without this the core has no data root before the first game launch -
            // eden_bridge_start is the only other caller of set_data_root, and it does
            // not run until a game starts, which is exactly too late for a setup screen.
            // Assigning the same string twice is harmless (retro_core.cpp:652-654).
            EdenPaths.dataRootPath.withCString { eden_libretro_set_data_root($0) }
        }

        var snapshot = Snapshot()
        eden_content_snapshot(&snapshot.raw)
        snapshot.verified = verified
        snapshot.detail = Self.copyString(2048) { eden_content_status_detail($0, $1) }
        snapshot.root = Self.copyString { eden_content_path(EdenContentPathRoot, $0, $1) }
        snapshot.keys = Self.copyString { eden_content_path(EdenContentPathKeysDir, $0, $1) }
        snapshot.keysImport = Self.copyString {
            eden_content_path(EdenContentPathKeysImportDir, $0, $1)
        }
        snapshot.firmware = Self.copyString {
            eden_content_path(EdenContentPathFirmwareDir, $0, $1)
        }
        return snapshot
    }

    private nonisolated static func copyString(
        _ capacity: Int = 1024,
        _ body: (UnsafeMutablePointer<CChar>?, Int) -> Int
    ) -> String {
        var buffer = [CChar](repeating: 0, count: capacity)
        buffer.withUnsafeMutableBufferPointer { out in
            _ = body(out.baseAddress, out.count)
        }
        return String(cString: buffer)
    }

    // MARK: - applying

    private func apply(_ snapshot: Snapshot) {
        let raw = snapshot.raw
        stage = Self.stage(for: raw.status)
        detail = snapshot.detail

        baseKeyFilePresent = raw.base_key_file_present
        titleKeyFilePresent = raw.title_key_file_present
        amiiboKeyFilePresent = raw.amiibo_key_file_present
        keysUsable = raw.keys_usable
        firmwareFileCount = raw.firmware_nca_count

        targetVersion = "\(raw.target_major).\(raw.target_minor).\(raw.target_micro)"

        rootPath = snapshot.root
        keysPath = snapshot.keys
        keysImportPath = snapshot.keysImport
        firmwarePath = snapshot.firmware
        // The base key file is dev.keys when Settings::values.use_dev_keys is set, which
        // the core resolves; take the name from the path it reported rather than
        // assuming prod.keys here.
        let reportedKeyFile = Self.copyString(1024) {
            eden_content_path(EdenContentPathBaseKeyFile, $0, $1)
        }
        if let name = reportedKeyFile.split(separator: "/").last, !name.isEmpty {
            baseKeyFileName = String(name)
        }

        if let check = snapshot.verified, check.version_known {
            firmwareVersion = "\(check.major).\(check.minor).\(check.micro)"
        }
    }

    private static func stage(for status: Int32) -> Stage {
        switch status {
        case Int32(EdenContentStatusNoDataRoot.rawValue): return .noDataRoot
        case Int32(EdenContentStatusNoKeys.rawValue): return .noKeys
        case Int32(EdenContentStatusBadKeys.rawValue): return .badKeys
        case Int32(EdenContentStatusNoFirmware.rawValue): return .noFirmware
        case Int32(EdenContentStatusFirmwareUnreadable.rawValue): return .firmwareUnreadable
        case Int32(EdenContentStatusFirmwareWrongVersion.rawValue): return .firmwareWrongVersion
        case Int32(EdenContentStatusReady.rawValue): return .ready
        default: return .unknown
        }
    }
}

// MARK: - presentation

extension EdenSetupModel.Stage {

    var headline: String {
        switch self {
        case .unknown: return "Checking…"
        case .noDataRoot: return "Eden has nowhere to store its files"
        case .noKeys: return "No keys installed"
        case .badKeys: return "Keys are incomplete"
        case .noFirmware: return "Keys are good - no firmware"
        case .firmwareUnreadable: return "Firmware could not be read"
        case .firmwareWrongVersion: return "Firmware is newer than this build"
        case .ready: return "Ready"
        }
    }

    /// Whether a game can be expected to start at all in this state.
    var blocksPlaying: Bool {
        switch self {
        case .noDataRoot, .noKeys, .badKeys: return true
        // Deliberately NOT blocking. Most titles boot with no firmware installed, and
        // an emulator that refuses to start one because a firmware check is unhappy is
        // a worse failure than the one it is guarding against.
        case .noFirmware, .firmwareUnreadable, .firmwareWrongVersion, .ready, .unknown:
            return false
        }
    }

    var symbolName: String {
        switch self {
        case .ready: return "checkmark.circle.fill"
        case .unknown: return "clock"
        case .noFirmware: return "info.circle.fill"
        case .firmwareUnreadable, .firmwareWrongVersion: return "exclamationmark.triangle.fill"
        case .noDataRoot, .noKeys, .badKeys: return "xmark.octagon.fill"
        }
    }

    var tint: Color {
        switch self {
        case .ready: return .green
        case .unknown: return .secondary
        case .noFirmware: return .blue
        case .firmwareUnreadable, .firmwareWrongVersion: return .orange
        case .noDataRoot, .noKeys, .badKeys: return .red
        }
    }
}
