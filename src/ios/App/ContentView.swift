// SPDX-FileCopyrightText: Copyright 2026 eden-ios contributors
// SPDX-License-Identifier: GPL-3.0-or-later

import SwiftUI
import UIKit
import UniformTypeIdentifiers

struct ContentView: View {

    @EnvironmentObject private var session: EmulatorSession
    @EnvironmentObject private var jit: JITAvailability
    @StateObject private var library = GameLibrary()

    @State private var importKind: ImportKind?
    @State private var showSetupGuide = false
    @State private var importReport: String?

    var body: some View {
        NavigationView {
            List {
                jitSection
                gamesSection
                importSection
            }
            .listStyle(.insetGrouped)
            .navigationTitle("Eden")
            .toolbar {
                ToolbarItem(placement: .navigationBarTrailing) {
                    Button {
                        showSetupGuide = true
                    } label: {
                        Image(systemName: "questionmark.circle")
                    }
                }
            }
        }
        .navigationViewStyle(.stack)
        // Full-screen, not a sheet: the CAMetalLayer must not be resized or partly
        // covered while the core is presenting into it.
        .fullScreenCover(isPresented: .constant(session.isRunning || session.phase == .starting)) {
            EmulationView()
                .environmentObject(session)
        }
        .sheet(isPresented: $showSetupGuide) {
            SetupGuideView()
        }
        .sheet(item: $session.jitRefusal) { status in
            JITRequiredView(status: status)
                .environmentObject(jit)
        }
        .sheet(item: $importKind) { kind in
            DocumentPicker(contentTypes: kind.contentTypes, allowsMultiple: kind.allowsMultiple) { urls in
                handleImport(kind: kind, urls: urls)
            }
        }
        .alert("Import", isPresented: Binding(
            get: { importReport != nil },
            set: { if !$0 { importReport = nil } }
        )) {
            Button("OK", role: .cancel) { importReport = nil }
        } message: {
            Text(importReport ?? "")
        }
    }

    // MARK: - sections

    private var jitSection: some View {
        Section {
            Button {
                session.jitRefusal = jit.status
            } label: {
                HStack {
                    Image(systemName: jit.status.symbolName)
                        .foregroundColor(jit.status.tint)
                    VStack(alignment: .leading, spacing: 2) {
                        Text(jit.status.headline)
                            .foregroundColor(.primary)
                        if jit.status == .unavailable {
                            Text("Games will not start. Tap for what to do.")
                                .font(.caption)
                                .foregroundColor(.secondary)
                        }
                    }
                    Spacer()
                    Image(systemName: "chevron.right")
                        .font(.caption)
                        .foregroundColor(.secondary)
                }
            }
        } header: {
            Text("Status")
        } footer: {
            if jit.status == .unavailable {
                // Not softened. IOS_PORT_NOTES.md: no JIT means no emulation, not slow
                // emulation, and there is no interpreter to fall back to.
                Text("Eden has no interpreter. Without JIT there is no emulation at all.")
            }
        }
    }

    private var gamesSection: some View {
        Section("Games") {
            if library.games.isEmpty {
                VStack(alignment: .leading, spacing: 6) {
                    Text("No games yet")
                        .font(.headline)
                    Text("Add one below, or copy a .nsp/.xci into \(EdenPaths.filesAppHint)/roms.")
                        .font(.caption)
                        .foregroundColor(.secondary)
                }
                .padding(.vertical, 4)
            } else {
                ForEach(library.games) { game in
                    Button {
                        session.start(game)
                    } label: {
                        HStack {
                            VStack(alignment: .leading, spacing: 2) {
                                Text(game.name).foregroundColor(.primary)
                                Text("\(game.ext.uppercased()) - \(game.sizeDescription)"
                                     + (game.needsSecurityScope ? " - outside the app" : ""))
                                    .font(.caption)
                                    .foregroundColor(.secondary)
                            }
                            Spacer()
                            Image(systemName: "play.circle")
                        }
                    }
                    .swipeActions {
                        if game.needsSecurityScope {
                            Button("Forget", role: .destructive) { library.forget(game) }
                        }
                    }
                }
            }
        }
    }

    private var importSection: some View {
        Section {
            Button { importKind = .rom }      label: { Label("Add a game", systemImage: "plus.circle") }
            Button { importKind = .keys }     label: { Label("Install prod.keys", systemImage: "key") }
            Button { importKind = .firmware } label: { Label("Install firmware (NCAs)", systemImage: "cpu") }
            Button { library.refresh() }      label: { Label("Rescan", systemImage: "arrow.clockwise") }
        } header: {
            Text("Set up")
        } footer: {
            Text("Keys and firmware can also be dropped straight into \(EdenPaths.filesAppHint).")
        }
    }

    // MARK: - import handling

    private func handleImport(kind: ImportKind, urls: [URL]) {
        switch kind {
        case .rom:
            // Bookmark rather than copy. See GameLibrary.addBookmark.
            for url in urls { library.addBookmark(for: url) }
            importReport = "Added \(urls.count) game\(urls.count == 1 ? "" : "s")."

        case .keys:
            importReport = DocumentImport.installKeys(from: urls)

        case .firmware:
            importReport = DocumentImport.installFirmware(from: urls)
        }
        library.refresh()
    }
}

// MARK: - picker plumbing

enum ImportKind: String, Identifiable {
    case rom, keys, firmware
    var id: String { rawValue }

    var allowsMultiple: Bool { self != .keys }

    var contentTypes: [UTType] {
        switch self {
        case .rom:
            // The exported UTI from Info.plist, plus a plain-data fallback so a file
            // the system has not yet associated is still selectable.
            return [UTType(exportedAs: "dev.edenios.switch-rom"), .data]
        case .keys:
            return [.plainText, .data]
        case .firmware:
            return [.data]
        }
    }
}

/// UIDocumentPickerViewController wrapper.
///
/// forImporting/asCopy: false, so a picked ROM stays where it is and comes back as a
/// security-scoped URL. That is what makes "outside the app" entries possible at all -
/// copying a 32 GB XCI into the container is often not an option on a phone.
struct DocumentPicker: UIViewControllerRepresentable {

    let contentTypes: [UTType]
    let allowsMultiple: Bool
    let onPick: ([URL]) -> Void

    func makeCoordinator() -> Coordinator { Coordinator(onPick: onPick) }

    func makeUIViewController(context: Context) -> UIDocumentPickerViewController {
        let picker = UIDocumentPickerViewController(forOpeningContentTypes: contentTypes,
                                                    asCopy: false)
        picker.allowsMultipleSelection = allowsMultiple
        picker.delegate = context.coordinator
        return picker
    }

    func updateUIViewController(_ controller: UIDocumentPickerViewController, context: Context) {}

    final class Coordinator: NSObject, UIDocumentPickerDelegate {
        private let onPick: ([URL]) -> Void
        init(onPick: @escaping ([URL]) -> Void) { self.onPick = onPick }

        func documentPicker(_ controller: UIDocumentPickerViewController,
                            didPickDocumentsAt urls: [URL]) {
            onPick(urls)
        }
    }
}
