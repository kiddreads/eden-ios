// SPDX-FileCopyrightText: Copyright 2026 eden-ios contributors
// SPDX-License-Identifier: GPL-3.0-or-later

import SwiftUI

/// Where things go, spelled out. Every path here is read out of the core's source, not
/// invented - see the citations beside each one.
struct SetupGuideView: View {

    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationView {
            List {
                Section {
                    Text("""
                    Eden keeps everything in its own folder, which you can open from the \
                    Files app at \(EdenPaths.filesAppHint). You can drop files straight \
                    in there instead of using the buttons in the app.
                    """)
                    .font(.callout)
                } header: {
                    Text("Where Eden's folder is")
                }

                Section {
                    PathRow(title: "Put prod.keys here",
                            path: "keys_import/prod.keys",
                            note: """
                            Eden copies it into keys/ and reloads the keyring the next \
                            time a game starts. Dropping a new prod.keys here always \
                            replaces the old one.

                            title.keys, console.keys and key_retail.bin work the same way. \
                            Any other filename is ignored.
                            """)

                    Text("""
                    Without keys nothing will load. The core checks before it even tries \
                    and reports "decryption keys missing".
                    """)
                    .font(.caption)
                    .foregroundColor(.secondary)
                } header: {
                    Text("Keys - required")
                }

                Section {
                    PathRow(title: "Put firmware NCAs here",
                            path: "nand/system/Contents/registered/",
                            note: """
                            Flat files, not folders. Each one must be named as 32 \
                            hexadecimal characters followed by .nca (or .cnmt.nca) - \
                            that is exactly what the core scans for, so extract the \
                            firmware archive without renaming anything.

                            Most games boot without firmware. Install it if a game asks \
                            for system files or a shared font.
                            """)
                } header: {
                    Text("Firmware - usually optional")
                }

                Section {
                    PathRow(title: "Games copied into the app",
                            path: "roms/",
                            note: """
                            .nsp, .xci, .nca, .nro, .nso and .kip are recognised.

                            You do not have to copy a game in. "Add a game" leaves it \
                            where it is and just remembers the location, which matters \
                            when a single title is larger than the free space on the \
                            device.
                            """)
                } header: {
                    Text("Games")
                }

                Section {
                    Text("""
                    Eden needs permission to turn Switch code into ARM code while it \
                    runs. iOS does not give an ordinary app that permission, and Eden \
                    has no slower fallback - without it, games do not start at all.

                    The Status row on the main screen says whether this device has it \
                    and what to do if it does not.
                    """)
                    .font(.callout)
                } header: {
                    Text("Why a game might not start")
                }

                Section {
                    Text("""
                    Audio is silent. Not muted - the audio path in this build emits \
                    silence by design, because Eden has no libretro audio sink yet.

                    There are no save states. Eden has no state serialization at any \
                    layer, so the feature is reported unavailable rather than offered \
                    and failing.

                    Motion controls are not wired up, so gyro aiming will not respond.
                    """)
                    .font(.callout)
                } header: {
                    Text("Known to be missing")
                }
            }
            .listStyle(.insetGrouped)
            .navigationTitle("Setting up")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") { dismiss() }
                }
            }
        }
        .navigationViewStyle(.stack)
    }
}

private struct PathRow: View {

    let title: String
    let path: String
    let note: String

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(title).font(.subheadline.weight(.semibold))
            Text(path)
                .font(.system(.footnote, design: .monospaced))
                .textSelection(.enabled)
                .padding(.vertical, 4)
                .padding(.horizontal, 8)
                .background(Color.secondary.opacity(0.15))
                .cornerRadius(6)
            Text(note)
                .font(.caption)
                .foregroundColor(.secondary)
                .fixedSize(horizontal: false, vertical: true)
        }
        .padding(.vertical, 4)
    }
}
