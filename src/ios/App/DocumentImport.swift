// SPDX-FileCopyrightText: Copyright 2026 eden-ios contributors
// SPDX-License-Identifier: GPL-3.0-or-later

import Foundation

/// Copying keys and firmware into the places the core actually reads them from.
///
/// Every destination here is read out of the core's own source rather than assumed.
enum DocumentImport {

    // MARK: - keys

    /// Install prod.keys (or a sibling) into the drop-off the core adopts from.
    ///
    /// Writes to `keys_import`, NOT to `keys`. The reason is in AdoptKeys
    /// (retro_content.cpp:131-166), which runs at the start of every LoadGameInternal:
    ///
    ///   * it copies prod.keys, title.keys, console.keys and key_retail.bin from
    ///     `<root>/keys_import` into `<root>/keys`, skipping any file that already
    ///     exists at the destination (retro_content.cpp:146);
    ///   * and then calls `KeyManager::Instance().ReloadKeys()` UNCONDITIONALLY
    ///     (retro_content.cpp:165), whether or not anything was copied.
    ///
    /// That unconditional reload is why writing here works even for a replacement: the
    /// skip-if-exists rule would otherwise strand a corrected prod.keys forever. So
    /// this function clears the stale destination copy itself, then writes the new file
    /// into keys_import and lets the core do the install on the next load.
    ///
    /// The reload also matters for a subtler reason the core documents: KeyManager is a
    /// function-local static whose constructor calls ReloadKeys(), so anything that
    /// touched KeyManager::Instance() before the EdenPath table was repointed cached
    /// keys read from the wrong directory for the whole session.
    static func installKeys(from urls: [URL]) -> String {
        let fm = FileManager.default
        var installed: [String] = []
        var rejected: [String] = []

        // The four names AdoptKeys knows about (retro_content.cpp:143). A file called
        // anything else is silently ignored by the core, so reject it here where the
        // user can still see why.
        let accepted = ["prod.keys", "title.keys", "console.keys", "key_retail.bin"]

        for url in urls {
            let name = url.lastPathComponent
            guard accepted.contains(name.lowercased()) else {
                rejected.append(name)
                continue
            }

            let scoped = url.startAccessingSecurityScopedResource()
            defer { if scoped { url.stopAccessingSecurityScopedResource() } }

            do {
                try fm.createDirectory(at: EdenPaths.keysImportDirectory,
                                       withIntermediateDirectories: true)

                let staging = EdenPaths.keysImportDirectory.appendingPathComponent(name)
                if fm.fileExists(atPath: staging.path) {
                    try fm.removeItem(at: staging)
                }
                try fm.copyItem(at: url, to: staging)

                // Clear the already-installed copy, or AdoptKeys' skip-if-exists rule
                // means a replacement never takes effect.
                let installedPath = EdenPaths.keysDirectory.appendingPathComponent(name)
                if fm.fileExists(atPath: installedPath.path) {
                    try fm.removeItem(at: installedPath)
                }

                installed.append(name)
            } catch {
                rejected.append("\(name) (\(error.localizedDescription))")
            }
        }

        var lines: [String] = []
        if !installed.isEmpty {
            lines.append("Staged: \(installed.joined(separator: ", ")).")
            lines.append("They are installed and reloaded the next time a game starts.")
        }
        if !rejected.isEmpty {
            lines.append("Ignored: \(rejected.joined(separator: ", ")). "
                         + "Only prod.keys, title.keys, console.keys and key_retail.bin are read.")
        }
        return lines.isEmpty ? "Nothing to install." : lines.joined(separator: "\n\n")
    }

    // MARK: - firmware

    /// Copy firmware NCAs into nand/system/Contents/registered.
    ///
    /// The naming rule is not a convention, it is a regex. RegisteredCache's
    /// FollowsNcaIdFormat (registered_cache.cpp:56-63) accepts exactly two shapes,
    /// case-insensitively:
    ///
    ///     [0-9A-F]{32}.nca        - 36 characters
    ///     [0-9A-F]{32}.cnmt.nca   - 41 characters
    ///
    /// and AccumulateFiles picks up flat files in the directory that match
    /// (registered_cache.cpp:650-654). Anything else is invisible to the core with no
    /// error anywhere, which is why this validates the names and reports what it
    /// refused rather than copying everything and hoping.
    ///
    /// HONEST LIMITS, both real:
    ///   1. This does NOT clear the destination first. A partial or mixed-version
    ///      firmware set is therefore possible and its behaviour is unverified.
    ///   2. Installed firmware does not take effect until the next load, because
    ///      GetFileSystemController().CreateFactories() runs inside LoadGameInternal
    ///      (retro_core.cpp:263) and reads the directory at call time.
    ///   3. The core's own firmware handling is an acknowledged TODO
    ///      (retro_core.cpp:279-286) - frontend_common's FirmwareManager is not linked,
    ///      so nothing verifies the set is complete or coherent. Most titles boot
    ///      without firmware, which is why this is not on the critical path.
    static func installFirmware(from urls: [URL]) -> String {
        let fm = FileManager.default
        var copied = 0
        var rejected: [String] = []

        do {
            try fm.createDirectory(at: EdenPaths.firmwareDirectory,
                                   withIntermediateDirectories: true)
        } catch {
            return "Could not create the firmware directory: \(error.localizedDescription)"
        }

        for url in urls {
            let name = url.lastPathComponent
            guard isValidNcaName(name) else {
                rejected.append(name)
                continue
            }

            let scoped = url.startAccessingSecurityScopedResource()
            defer { if scoped { url.stopAccessingSecurityScopedResource() } }

            let destination = EdenPaths.firmwareDirectory.appendingPathComponent(name)
            do {
                if fm.fileExists(atPath: destination.path) {
                    try fm.removeItem(at: destination)
                }
                try fm.copyItem(at: url, to: destination)
                copied += 1
            } catch {
                rejected.append("\(name) (\(error.localizedDescription))")
            }
        }

        var lines = ["Installed \(copied) NCA\(copied == 1 ? "" : "s")."]
        if !rejected.isEmpty {
            let shown = rejected.prefix(8).joined(separator: ", ")
            let more = rejected.count > 8 ? " and \(rejected.count - 8) more" : ""
            lines.append("Skipped \(rejected.count): \(shown)\(more).")
            lines.append("Firmware files must be named as 32 hexadecimal characters "
                         + "followed by .nca or .cnmt.nca - that is what the core scans for. "
                         + "Extract the firmware zip without renaming anything.")
        }
        if copied > 0 {
            lines.append("Firmware is picked up the next time a game starts.")
        }
        return lines.joined(separator: "\n\n")
    }

    /// Mirrors FollowsNcaIdFormat (registered_cache.cpp:56-63) exactly, including its
    /// case-insensitivity and its two length checks.
    static func isValidNcaName(_ name: String) -> Bool {
        let lower = name.lowercased()
        let stem: String
        if lower.hasSuffix(".cnmt.nca") {
            guard name.count == 41 else { return false }
            stem = String(lower.dropLast(".cnmt.nca".count))
        } else if lower.hasSuffix(".nca") {
            guard name.count == 36 else { return false }
            stem = String(lower.dropLast(".nca".count))
        } else {
            return false
        }
        guard stem.count == 32 else { return false }
        return stem.allSatisfy { $0.isHexDigit }
    }
}
