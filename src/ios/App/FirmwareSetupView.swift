// SPDX-FileCopyrightText: Copyright 2026 eden-ios contributors
// SPDX-License-Identifier: GPL-3.0-or-later

import SwiftUI
import UniformTypeIdentifiers

/// The keys-and-firmware walkthrough.
///
/// This screen exists because getting keys and firmware wrong is the single most common
/// reason people conclude an emulator is broken, and every part of that is invisible by
/// default:
///
///   * A prod.keys that is present but too old for the game looks identical to a good
///     one in Files.app. Eden derives what it can and reports nothing
///     (key_manager.cpp:589-592 early-returns on a missing file, with no diagnostic).
///   * A firmware NCA renamed by a file manager is invisible to the content index -
///     RegisteredCache matches "32 hex digits + .nca" and quietly ignores anything else
///     (registered_cache.cpp:56-63).
///   * Missing firmware is reported by NOTHING at load time. It surfaces much later as a
///     service failure inside the guest, which reads as "the game crashed".
///
/// So every row here shows a state read from the core, and names the real absolute path
/// the file has to be at. No row says "should be fine" - it either checked or it says it
/// did not check.
///
/// Presented from ContentView. SetupGuideView remains the static "where things live"
/// reference; this is the live one with the buttons.
struct FirmwareSetupView: View {

    @StateObject private var model = EdenSetupModel()
    @Environment(\.dismiss) private var dismiss

    @State private var pick: SetupPick?
    @State private var replaceExistingFirmware = true

    var body: some View {
        NavigationView {
            List {
                statusSection
                keysSection
                firmwareSection
                pathsSection
                provenanceSection
            }
            .listStyle(.insetGrouped)
            .navigationTitle("Keys and firmware")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") { dismiss() }
                }
            }
        }
        .navigationViewStyle(.stack)
        .onAppear { model.refresh() }
        .sheet(item: $pick) { kind in
            DocumentPicker(contentTypes: kind.contentTypes, allowsMultiple: false) { urls in
                guard let url = urls.first else { return }
                switch kind {
                case .keys:
                    model.installKeys(from: url)
                case .firmware:
                    model.installFirmware(from: url, replaceExisting: replaceExistingFirmware)
                }
            }
        }
        .alert("Setup", isPresented: Binding(
            get: { model.lastMessage != nil },
            set: { if !$0 { model.lastMessage = nil } }
        )) {
            Button("OK", role: .cancel) { model.lastMessage = nil }
        } message: {
            Text(model.lastMessage ?? "")
        }
    }

    // MARK: - status

    private var statusSection: some View {
        Section {
            HStack(alignment: .top, spacing: 12) {
                Image(systemName: model.stage.symbolName)
                    .font(.title2)
                    .foregroundColor(model.stage.tint)
                VStack(alignment: .leading, spacing: 4) {
                    Text(model.stage.headline)
                        .font(.headline)
                    if !model.detail.isEmpty {
                        Text(model.detail)
                            .font(.caption)
                            .foregroundColor(.secondary)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                }
            }
            .padding(.vertical, 4)

            if let busy = model.busy {
                HStack(spacing: 10) {
                    ProgressView()
                    Text(busy).font(.callout).foregroundColor(.secondary)
                }
            }

            if model.coreIsBusy {
                Text("A game is running. Keys and firmware cannot be changed until it stops - "
                     + "the core owns the keyring while it has a game loaded.")
                    .font(.caption)
                    .foregroundColor(.orange)
            }
        } header: {
            Text("Status")
        } footer: {
            if model.stage.blocksPlaying {
                Text("No game will start in this state.")
            }
        }
    }

    // MARK: - keys

    private var keysSection: some View {
        Section {
            CheckRow(done: model.baseKeyFilePresent,
                     title: "\(model.baseKeyFileName) is in place",
                     note: model.baseKeyFilePresent
                         ? "The file is on disk."
                         : "Required. Nothing encrypted opens without it.")

            CheckRow(done: model.keysUsable,
                     title: "Eden can derive a complete keyring",
                     note: keyringNote)

            CheckRow(done: model.titleKeyFilePresent,
                     title: "title.keys is in place",
                     note: "Optional. Only personalised or eShop dumps need it; a cartridge "
                         + "dump does not.",
                     optional: true)

            CheckRow(done: model.amiiboKeyFilePresent,
                     title: "key_retail.bin is in place",
                     note: "Optional. Amiibo only - it never affects whether a game runs.",
                     optional: true)

            Button {
                pick = .keys
            } label: {
                Label("Install keys from a file or folder", systemImage: "key")
            }
            .disabled(model.busy != nil || model.coreIsBusy)
        } header: {
            Text("1. Keys - required")
        } footer: {
            Text("Pick the key file itself, or a folder holding it. Eden reads exactly these "
                 + "names and silently ignores everything else: prod.keys, dev.keys, "
                 + "title.keys, console.keys, key_retail.bin. A file named anything else - "
                 + "including prod.keys.txt, which is what some browsers save - is invisible "
                 + "to it.")
        }
    }

    private var keyringNote: String {
        if model.keysUsable {
            return "Checked against the keyring Eden actually built, not just the filename."
        }
        if model.baseKeyFilePresent {
            // The case a naive "does the file exist" check gets wrong, and the reason
            // this screen exists at all.
            return "The file is there but incomplete - it is usually truncated, or from an "
                 + "older system version than the games you want to run. Installing a newer "
                 + "one replaces it."
        }
        return "Nothing to derive from yet."
    }

    // MARK: - firmware

    private var firmwareSection: some View {
        Section {
            CheckRow(done: model.firmwareFileCount > 0,
                     title: firmwareCountTitle,
                     note: model.firmwareFileCount > 0
                         ? "Counted by name. It says nothing about whether they parse - use "
                           + "Check installed firmware for that."
                         : "Most games boot without firmware. Install it if a game asks for "
                           + "system files, a shared font, or a system applet.",
                     optional: true)

            if model.firmwareChecked {
                CheckRow(done: !model.firmwareVersion.isEmpty,
                         title: model.firmwareVersion.isEmpty
                             ? "Version could not be read"
                             : "Firmware \(model.firmwareVersion) installed",
                         note: model.firmwareDetail.isEmpty
                             ? "This build emulates Horizon OS \(model.targetVersion)."
                             : model.firmwareDetail,
                         optional: true)
            }

            Button {
                model.verifyFirmware()
            } label: {
                Label("Check installed firmware", systemImage: "magnifyingglass")
            }
            .disabled(model.busy != nil || model.coreIsBusy || model.firmwareFileCount == 0)

            Toggle("Replace existing firmware", isOn: $replaceExistingFirmware)
                .font(.callout)

            Button {
                pick = .firmware
            } label: {
                Label("Install firmware from a .zip or folder", systemImage: "cpu")
            }
            .disabled(model.busy != nil || model.coreIsBusy)
        } header: {
            Text("2. Firmware - usually optional")
        } footer: {
            Text("A firmware .zip is unpacked in place - nothing is copied twice and nothing "
                 + "is renamed, which matters because Eden's content index matches file names "
                 + "of exactly 32 hexadecimal characters plus .nca or .cnmt.nca and ignores "
                 + "anything else without saying so. Every file is checked against its "
                 + "CRC-32 before it is installed.\n\n"
                 + "Leave Replace existing firmware on unless you know you are adding to a "
                 + "set: two firmware versions mixed in one folder is a state nothing here "
                 + "can reason about.\n\n"
                 + "Firmware takes effect the next time a game starts.")
        }
    }

    private var firmwareCountTitle: String {
        let count = model.firmwareFileCount
        if count == 0 {
            return "No firmware installed"
        }
        return "\(count) firmware file\(count == 1 ? "" : "s") installed"
    }

    // MARK: - paths

    private var pathsSection: some View {
        Section {
            PathDisclosure(title: "Keys are read from", path: model.keysPath)
            PathDisclosure(title: "Drop-off for the Files app", path: model.keysImportPath)
            PathDisclosure(title: "Firmware is read from", path: model.firmwarePath)
            PathDisclosure(title: "Everything lives under", path: model.rootPath)
        } header: {
            Text("Where these files actually go")
        } footer: {
            Text("These are the paths the core reported, not paths this screen assumed. You "
                 + "can reach them from \(EdenPaths.filesAppHint) and copy files in by hand "
                 + "instead of using the buttons above - a key file dropped into the drop-off "
                 + "folder is installed the next time a game starts.")
        }
    }

    // MARK: - provenance

    private var provenanceSection: some View {
        Section {
            Text("Eden ships no keys, no firmware and no games, and cannot obtain them. "
                 + "Nothing on this screen downloads anything. Every file it installs came "
                 + "from one you picked, and every destination is inside this app's own "
                 + "folder.")
                .font(.caption)
                .foregroundColor(.secondary)
        }
    }
}

// MARK: - rows

private struct CheckRow: View {

    let done: Bool
    let title: String
    let note: String
    var optional: Bool = false

    var body: some View {
        HStack(alignment: .top, spacing: 10) {
            Image(systemName: done ? "checkmark.circle.fill"
                                   : (optional ? "circle" : "exclamationmark.circle"))
                .foregroundColor(done ? .green : (optional ? .secondary : .red))
            VStack(alignment: .leading, spacing: 3) {
                Text(title).font(.subheadline)
                Text(note)
                    .font(.caption)
                    .foregroundColor(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
        .padding(.vertical, 2)
    }
}

/// A real absolute path, selectable so it can be copied, and wrapped rather than
/// truncated - a path the user cannot read in full is not a path they can act on.
private struct PathDisclosure: View {

    let title: String
    let path: String

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(title).font(.caption).foregroundColor(.secondary)
            Text(path.isEmpty ? "(not known yet)" : path)
                .font(.system(.caption, design: .monospaced))
                .textSelection(.enabled)
                .fixedSize(horizontal: false, vertical: true)
        }
        .padding(.vertical, 2)
    }
}

// MARK: - picker plumbing

/// What the one document picker is being opened for.
///
/// Deliberately NOT reusing ContentView's ImportKind: that one also covers ROMs, allows
/// multiple selection, and offers no folder type. Both file AND folder must be
/// selectable here, because a user who has already unzipped their firmware has a folder
/// and nothing else.
private enum SetupPick: String, Identifiable {
    case keys, firmware
    var id: String { rawValue }

    var contentTypes: [UTType] {
        switch self {
        case .keys:
            // .plainText: prod.keys is a text file, and that is what the system usually
            // types it as. .data and .folder keep a file the system has not typed, and a
            // whole folder, selectable.
            return [.plainText, .data, .folder]
        case .firmware:
            return [.zip, .folder, .data]
        }
    }
}
