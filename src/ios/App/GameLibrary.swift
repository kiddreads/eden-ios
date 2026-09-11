// SPDX-FileCopyrightText: Copyright 2026 eden-ios contributors
// SPDX-License-Identifier: GPL-3.0-or-later

import Combine
import Foundation

/// One entry in the game list.
struct GameEntry: Identifiable, Equatable {

    let url: URL
    /// True for a file outside the app container, reached through a security-scoped
    /// bookmark. docs/IOS_PORT_NOTES.md #4 requires the scope to be held for the whole
    /// session for these, which EmulatorSession does.
    let needsSecurityScope: Bool
    let sizeBytes: Int64

    /// Identity is the resolved path.
    ///
    /// HONEST LIMIT: for a file inside the container this means renaming it in Files.app
    /// makes it a new entry. Nothing is orphaned today because nothing is keyed off it,
    /// but the moment per-game settings or save redirection exist, identity has to
    /// become something stable - a title ID read from the container, which needs the
    /// loader and therefore needs keys.
    var id: String { url.path }

    var name: String { url.deletingPathExtension().lastPathComponent }
    var ext: String { url.pathExtension.lowercased() }

    var sizeDescription: String {
        ByteCountFormatter.string(fromByteCount: sizeBytes, countStyle: .file)
    }
}

/// Scans the container for ROMs and remembers bookmarked ones from outside it.
@MainActor
final class GameLibrary: ObservableObject {

    /// Exactly what retro_get_system_info advertises (retro_core.cpp:348), which in
    /// turn mirrors Loader::GuessFromFilename (src/core/loader/loader.cpp:167-186).
    /// Six extensions, not the four suyu declares. Kept in one place so the importer,
    /// the scanner and the document picker cannot disagree.
    static let supportedExtensions: Set<String> = ["nsp", "xci", "nca", "nro", "nso", "kip"]

    @Published private(set) var games: [GameEntry] = []
    @Published private(set) var lastError: String?

    /// Security-scoped bookmarks for files outside the container, as raw Data.
    ///
    /// UserDefaults is the right size of tool here: a handful of small Data blobs, read
    /// once at launch. It is not where the ROMs live.
    private static let bookmarkKey = "dev.edenios.Eden.romBookmarks"

    init() {
        refresh()
    }

    func refresh() {
        var found: [GameEntry] = []
        found.append(contentsOf: scanContainer())
        found.append(contentsOf: resolveBookmarks())
        games = found.sorted { $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending }
    }

    // MARK: - container

    private func scanContainer() -> [GameEntry] {
        let fm = FileManager.default
        guard let items = try? fm.contentsOfDirectory(
            at: EdenPaths.romsDirectory,
            includingPropertiesForKeys: [.fileSizeKey],
            options: [.skipsHiddenFiles]
        ) else {
            return []
        }

        return items.compactMap { url in
            guard Self.supportedExtensions.contains(url.pathExtension.lowercased()) else {
                return nil
            }
            let size = (try? url.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0
            return GameEntry(url: url, needsSecurityScope: false, sizeBytes: Int64(size))
        }
    }

    // MARK: - bookmarks

    /// Record a file the user picked in place, rather than copying it in.
    ///
    /// Copying a 32 GB XCI into the container doubles its storage cost, which on a
    /// phone is often the difference between the game fitting and not. Info.plist sets
    /// LSSupportsOpeningDocumentsInPlace so the picker can return the original.
    func addBookmark(for url: URL) {
        do {
            // On iOS there is no .withSecurityScope option - it is macOS-only, and
            // passing it is a compile error here. A plain minimal bookmark to a
            // document-picker URL is security-scoped implicitly on iOS; the scope is
            // then entered with startAccessingSecurityScopedResource on the RESOLVED
            // url, which EmulatorSession does at launch.
            let data = try url.bookmarkData(options: [],
                                            includingResourceValuesForKeys: nil,
                                            relativeTo: nil)
            var all = Self.storedBookmarks()
            all.append(data)
            UserDefaults.standard.set(all, forKey: Self.bookmarkKey)
            refresh()
        } catch {
            lastError = "Could not remember that file: \(error.localizedDescription)"
        }
    }

    private static func storedBookmarks() -> [Data] {
        (UserDefaults.standard.array(forKey: bookmarkKey) as? [Data]) ?? []
    }

    private func resolveBookmarks() -> [GameEntry] {
        var surviving: [Data] = []
        var entries: [GameEntry] = []

        for data in Self.storedBookmarks() {
            var stale = false
            guard let url = try? URL(resolvingBookmarkData: data,
                                     options: [],
                                     relativeTo: nil,
                                     bookmarkDataIsStale: &stale) else {
                continue   // the file is gone; drop the bookmark
            }
            if stale {
                // A stale bookmark still resolves but should be rewritten. Not doing so
                // here because rewriting needs the scope open, and opening it outside a
                // session is the thing IOS_PORT_NOTES.md #4 warns about mismanaging.
                // It resolves, so it is kept.
                lastError = "A saved ROM location has moved; re-import it if it fails to load."
            }

            let size = (try? url.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0
            entries.append(GameEntry(url: url, needsSecurityScope: true, sizeBytes: Int64(size)))
            surviving.append(data)
        }

        if surviving.count != Self.storedBookmarks().count {
            UserDefaults.standard.set(surviving, forKey: Self.bookmarkKey)
        }
        return entries
    }

    func forget(_ game: GameEntry) {
        guard game.needsSecurityScope else { return }
        let remaining = Self.storedBookmarks().filter { data in
            var stale = false
            let url = try? URL(resolvingBookmarkData: data, options: [],
                               relativeTo: nil, bookmarkDataIsStale: &stale)
            return url?.path != game.url.path
        }
        UserDefaults.standard.set(remaining, forKey: Self.bookmarkKey)
        refresh()
    }
}
