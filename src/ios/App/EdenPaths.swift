// SPDX-FileCopyrightText: Copyright 2026 eden-ios contributors
// SPDX-License-Identifier: GPL-3.0-or-later

import Foundation
import UIKit

/// Where Eden's data tree lives, and the exact paths the setup guide quotes.
///
/// docs/IOS_PORT_NOTES.md #2 is the requirement: "Call it before loading anything, with
/// a directory inside the app container that is visible to Files.app." Visible means
/// UIFileSharingEnabled + LSSupportsOpeningDocumentsInPlace in Info.plist, and a path
/// that is not dot-hidden. The note explains what happens otherwise: Eden's own
/// PathManagerImpl::Reinitialize discards its argument on Apple platforms and resolves
/// through XDG into `$HOME/.local/share/eden` inside the container - writable, so
/// nothing looks broken, but dot-hidden and therefore invisible in Files.app, which is
/// exactly where the user has to put prod.keys.
enum EdenPaths {

    /// THE DATA ROOT IS `Documents` ITSELF, not a subfolder of it.
    ///
    /// This was a real disagreement worth settling. The core's own fallback,
    /// ResolveRoot (retro_content.cpp:47-58), appends "/eden" to whatever
    /// RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY returns, which argues for
    /// `Documents/eden`. But that fallback only runs when the override is empty, and
    /// the bridge always sets the override before retro_init, so it never runs.
    ///
    /// Against that: Content::SetupUserPaths fans ~20 directories out under the root
    /// (retro_content.cpp:102-124 - amiibo, cache, config, crash_dumps, dump, icons,
    /// keys, load, log, lossless, nand, play_time, post_presets, post_shaders,
    /// screenshots, sdmc, tas, plus keys_import). Nesting them one level deeper adds a
    /// tap to the exact navigation users already get wrong, on the one task - putting
    /// prod.keys somewhere - that the app cannot do for them. Documents itself wins.
    static var dataRoot: URL {
        FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
    }

    /// Where the core looks for prod.keys after it has adopted them.
    /// `set_path(EdenPath::KeysDir, root / "keys")` - retro_content.cpp:109.
    static var keysDirectory: URL { dataRoot.appendingPathComponent("keys") }

    /// The sanctioned drop-off. `void(FS::CreateDirs(root / "keys_import"))` -
    /// retro_content.cpp:124, and AdoptKeys (retro_content.cpp:131-166) copies
    /// prod.keys, title.keys, console.keys and key_retail.bin out of here into
    /// keysDirectory at every load.
    ///
    /// The importer writes HERE rather than straight into `keys` for one reason worth
    /// stating: AdoptKeys skips any file that already exists at the destination
    /// (`if (!FS::Exists(src) || FS::Exists(dst)) continue;`, retro_content.cpp:146),
    /// but then calls `KeyManager::Instance().ReloadKeys()` UNCONDITIONALLY
    /// (retro_content.cpp:165). So dropping a file here always forces a reload even
    /// when nothing was copied, which is what makes replacing a bad prod.keys work.
    static var keysImportDirectory: URL { dataRoot.appendingPathComponent("keys_import") }

    /// Firmware. `set_path(EdenPath::NANDDir, root / "nand")` (retro_content.cpp:113)
    /// and Eden's BIS factory puts registered system content under
    /// nand/system/Contents/registered.
    static var firmwareDirectory: URL {
        dataRoot
            .appendingPathComponent("nand")
            .appendingPathComponent("system")
            .appendingPathComponent("Contents")
            .appendingPathComponent("registered")
    }

    /// Where the game list looks for ROMs copied into the container.
    /// Not a core path - the core takes a full path to whatever it is handed
    /// (need_fullpath is true, retro_core.cpp:349). This is purely the app's own
    /// convention, and it sits beside the core's directories rather than inside one so
    /// that SetupUserPaths never has an opinion about it.
    static var romsDirectory: URL { dataRoot.appendingPathComponent("roms") }

    static var logDirectory: URL { dataRoot.appendingPathComponent("log") }

    /// Create the two directories the app itself owns.
    ///
    /// The core creates its own tree - SetupUserPaths calls CreateDirs for every
    /// EdenPath (retro_content.cpp:97-100) - but only once retro_init has run, and the
    /// setup guide has to be able to show the user a real folder BEFORE they have ever
    /// launched a game. Creating keys_import and roms up front is what makes the first
    /// run possible at all: otherwise Files.app shows an empty Eden folder and there is
    /// nowhere to put prod.keys.
    ///
    /// Deliberately does NOT pre-create the other ~18. Letting the core own those keeps
    /// one source of truth for the layout; duplicating the list here is exactly the
    /// drift that makes a rebase silently wrong.
    static func prepare() throws {
        let fm = FileManager.default
        for url in [keysImportDirectory, romsDirectory] {
            try fm.createDirectory(at: url, withIntermediateDirectories: true)
        }
    }

    /// Filesystem path for the bridge, which takes a C string.
    static var dataRootPath: String { dataRoot.path }

    /// Human-readable location for the setup guide, e.g. "On My iPhone / Eden".
    /// The real container name shown in Files.app is the app's display name, so this
    /// is descriptive text rather than a resolvable path.
    static var filesAppHint: String {
        let idiom = UIDevice.current.userInterfaceIdiom == .pad ? "iPad" : "iPhone"
        return "Files -> On My \(idiom) -> Eden"
    }
}
