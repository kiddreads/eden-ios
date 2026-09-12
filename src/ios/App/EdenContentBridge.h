// SPDX-FileCopyrightText: Copyright 2026 eden-ios contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The keys-and-firmware surface, in pure C, so Swift can see it.
//
// WHY THIS FILE IS HERE AND NOT IN src/libretro_core/
//   src/libretro_core/retro_content.h is C++ - it includes core/core.h - so Clang's
//   Swift importer cannot read it and src/ios/Bridge/Eden-Bridging-Header.h cannot
//   #import it. This is the C mirror. It is the ONE declaration of these entry points:
//   retro_content.cpp includes this same header and defines them, so the two cannot
//   drift the way a hand-copied prototype list would.
//
//   The spelling resolves identically on both sides:
//     * CMake  - src/CMakeLists.txt:8 is `include_directories(.)`, and libretro_core is
//                added at src/CMakeLists.txt:276, i.e. after it. So "ios/App/..."
//                resolves from src/.
//     * Xcode  - src/ios/project.yml:104-106 puts $(SRCROOT)/.. (= src/) on
//                HEADER_SEARCH_PATHS. Same spelling.
//
//   Nothing in here includes anything but <stdbool.h> and <stddef.h>, so including it
//   from the core costs nothing and keeps the "Bridge/ is Objective-C, never
//   Objective-C++" constraint in src/ios/project.yml:32-36 intact.
//
// TO MAKE SWIFT SEE THIS, src/ios/Bridge/Eden-Bridging-Header.h needs one line:
//     #import "ios/App/EdenContentBridge.h"
//   That file belongs to the bridge lane, not this one, so it is not edited here.
//
// THREADING - read this before calling anything below.
//   None of it is synchronised, and it cannot be: it reads process-wide globals
//   (LibretroCore::g_data_root_override), Common::FS's EdenPath table and the
//   Core::Crypto::KeyManager singleton, none of which has a lock. Call these only while
//   NO game is loaded - i.e. eden_bridge_state() is EdenBridgeStateIdle or
//   EdenBridgeStateFailed. Any one serial queue is fine; two at once is not.
//
//   Call eden_libretro_set_data_root() once before the first call here. Without it
//   there is no data root to resolve and everything reports
//   EdenContentStatusNoDataRoot: eden_bridge_start() is currently the only caller of
//   set_data_root, and it does not run until a game is launched - which is exactly the
//   wrong time for a SETUP screen to learn where the keys directory is.
//
// THIS PORT SHIPS NO KEYS, NO FIRMWARE AND NO GAMES, AND FETCHES NONE. Everything here
// reads, validates and copies files the user supplied, inside the app's own container.
// There is no network access behind any of these functions.

#ifndef EDEN_CONTENT_BRIDGE_H
#define EDEN_CONTENT_BRIDGE_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------

/// Mirrors LibretroCore::Content::Status. The numeric values are part of this ABI;
/// append, never reorder.
typedef enum {
    /// No writable data root is known. Call eden_libretro_set_data_root() first.
    EdenContentStatusNoDataRoot = 0,
    /// No prod.keys (or dev.keys under Settings::values.use_dev_keys) on disk.
    EdenContentStatusNoKeys = 1,
    /// The key file exists but Eden could not derive a complete keyring from it.
    EdenContentStatusBadKeys = 2,
    /// Keys are complete; no firmware NCAs are installed. NOT a blocker.
    EdenContentStatusNoFirmware = 3,
    /// Firmware files are present but could not be parsed into a usable system
    /// version - a partial extraction, a renamed archive, or a corrupt dump.
    EdenContentStatusFirmwareUnreadable = 4,
    /// Firmware parsed, and its major version is NEWER than the Horizon OS version
    /// this build of Eden emulates (see EdenContentSnapshot::target_major).
    EdenContentStatusFirmwareWrongVersion = 5,
    /// Keys are complete and firmware is installed.
    EdenContentStatusReady = 6,
} EdenContentStatus;

/// Selector for eden_content_path().
typedef enum {
    EdenContentPathRoot = 0,          ///< the data root itself
    EdenContentPathKeysDir = 1,       ///< where Eden READS keys
    EdenContentPathKeysImportDir = 2, ///< the Files.app drop-off AdoptKeys drains
    EdenContentPathFirmwareDir = 3,   ///< nand/system/Contents/registered
    EdenContentPathBaseKeyFile = 4,   ///< <keys>/prod.keys, or dev.keys
} EdenContentPathId;

/// The cheap, disk-only snapshot. Every field is recomputed on each call; there is no
/// cache, because the user can drop a file in from Files.app between two calls.
typedef struct {
    int status; ///< EdenContentStatus

    bool base_key_file_present;    ///< prod.keys (or dev.keys) exists
    bool title_key_file_present;   ///< title.keys exists - optional, per-title keys
    bool console_key_file_present; ///< console.keys exists - optional
    bool amiibo_key_file_present;  ///< key_retail.bin exists - amiibo only, never gates a game

    /// The keyring derived from those files is complete. This is the question that
    /// matters; "the file exists" is a different and weaker one.
    bool keys_usable;

    /// Files under the firmware directory whose names match the format the
    /// RegisteredCache scans for. A name scan, not a parse.
    unsigned long long firmware_nca_count;

    /// Set only after eden_content_verify_firmware() has run in this process.
    bool firmware_version_known;
    unsigned char firmware_major;
    unsigned char firmware_minor;
    unsigned char firmware_micro;

    /// The Horizon OS version THIS BUILD emulates, from src/core/hle/api_version.h.
    unsigned char target_major;
    unsigned char target_minor;
    unsigned char target_micro;
} EdenContentSnapshot;

/// Fill `out` with the current state. Cheap: file existence plus one directory scan.
/// Creates the data directories as a side effect (it runs the core's SetupUserPaths),
/// which is deliberate - the paths reported are then the paths Eden will use.
void eden_content_snapshot(EdenContentSnapshot* out);

/// Stable lowercase token for the last status computed by eden_content_snapshot():
/// "no_data_root", "no_keys", "bad_keys", "no_firmware", "firmware_unreadable",
/// "firmware_wrong_version", "ready". Returns the length that would have been written,
/// excluding the terminator.
size_t eden_content_status_token(char *buf, size_t len);

/// One or two sentences a human can act on, naming the real path a file must go to.
size_t eden_content_status_detail(char *buf, size_t len);

/// An absolute filesystem path, or an empty string when no data root is applied.
size_t eden_content_path(EdenContentPathId which, char *buf, size_t len);

// ---------------------------------------------------------------------------
// Installing
// ---------------------------------------------------------------------------

/// Result of eden_content_install_keys().
typedef enum {
    EdenKeyInstallOk = 0,            ///< installed, and the keyring is now complete
    EdenKeyInstallNoDataRoot = 1,    ///< no data root; nothing was written
    EdenKeyInstallSourceMissing = 2, ///< the path does not exist
    EdenKeyInstallWrongName = 3,     ///< nothing at that path is a key file Eden reads
    EdenKeyInstallCopyFailed = 4,    ///< the copy itself failed (permissions, space)
    EdenKeyInstallStillUnusable = 5, ///< files installed, but the keyring is STILL incomplete
} EdenKeyInstallResult;

/**
 * Install key files from a user-provided file or folder.
 *
 * `source_path` may be:
 *   * a single file named prod.keys, dev.keys, title.keys, console.keys or
 *     key_retail.bin (matched case-insensitively, written under the lowercase name
 *     Eden actually opens), or
 *   * a folder, which is searched recursively for any of those names.
 *
 * Writes straight into the keys directory and reloads the keyring immediately, so the
 * effect is visible in the very next eden_content_snapshot(). This is NOT the
 * keys_import drop-off, which only takes effect at the next game load.
 *
 * `message` receives a sentence for the user; pass NULL to skip it.
 * Returns an EdenKeyInstallResult.
 *
 * EdenKeyInstallStillUnusable is a success at copying and a failure at the real
 * question: it means the file parsed but does not carry every master / key-area /
 * titlekek pair Eden needs. Usually a prod.keys from an older system version.
 */
int eden_content_install_keys(const char *source_path, char *message, size_t message_len);

/// Result of eden_content_install_firmware().
typedef enum {
    EdenFirmwareInstallOk = 0,             ///< at least one NCA installed, none failed
    EdenFirmwareInstallNoDataRoot = 1,
    EdenFirmwareInstallSourceMissing = 2,  ///< the path does not exist
    EdenFirmwareInstallUnreadable = 3,     ///< not a folder, and not a ZIP this can read
    EdenFirmwareInstallNothingFound = 4,   ///< readable, but held no NCA-named files
    EdenFirmwareInstallPartial = 5,        ///< some NCAs installed, some failed
    EdenFirmwareInstallWriteFailed = 6,    ///< nothing could be written at all
} EdenFirmwareInstallResult;

/**
 * Install firmware from a user-provided ZIP archive or folder.
 *
 * `source_path` may be a .zip (read and decompressed in-process - deflate and stored
 * entries, every entry checked against its CRC-32) or a folder, which is searched
 * recursively. Only entries whose FILE NAME matches what Eden's RegisteredCache scans
 * for are installed: 32 hex digits followed by ".nca" or ".cnmt.nca". Directory
 * structure inside the archive is flattened, which is correct - the cache reads a flat
 * directory. Anything else in the archive is ignored and counted.
 *
 * `replace_existing` deletes the NCAs already in the firmware directory first. Prefer
 * true for a complete firmware archive: mixing two firmware versions in one directory
 * produces a set whose behaviour nothing here can predict. It only ever removes files
 * inside the app's own firmware directory.
 *
 * `message` receives a sentence for the user; pass NULL to skip it.
 * Returns an EdenFirmwareInstallResult.
 *
 * Installed firmware is picked up at the NEXT game load: the core builds its
 * RegisteredCache inside FileSystemController::CreateFactories during the load and
 * reads the directory at that moment.
 */
int eden_content_install_firmware(const char *source_path, bool replace_existing, char *message,
                                  size_t message_len);

// ---------------------------------------------------------------------------
// Verifying
// ---------------------------------------------------------------------------

/// Result of eden_content_verify_firmware().
typedef enum {
    EdenFirmwareGood = 0,          ///< parsed, and the version was read
    EdenFirmwareNoDataRoot = 1,
    EdenFirmwareNotInstalled = 2,  ///< no NCA-named files at all
    EdenFirmwareKeysMissing = 3,   ///< cannot parse an NCA without a complete keyring
    EdenFirmwareUnreadable = 4,    ///< files present, nothing parsed out of them
    EdenFirmwareIncomplete = 5,    ///< parsed, but the system titles Eden needs are absent
    EdenFirmwareWrongVersion = 6,  ///< parsed; newer than the Horizon OS this build emulates
} EdenFirmwareVerdict;

typedef struct {
    int verdict; ///< EdenFirmwareVerdict
    unsigned long long nca_count;
    bool version_known;
    unsigned char major;
    unsigned char minor;
    unsigned char micro;
    unsigned char target_major;
    unsigned char target_minor;
    unsigned char target_micro;
} EdenFirmwareCheck;

/**
 * Parse the installed firmware and read its system version.
 *
 * EXPENSIVE and BLOCKING. It builds a FileSys::RegisteredCache over the firmware
 * directory, which parses the header of every NCA in it with the KeyManager keyring.
 * Call it from a background queue, never from the main thread, and never while a game
 * is loaded.
 *
 * `out`, `version` ("19.0.1", or empty when unknown) and `detail` may each be NULL.
 * Returns an EdenFirmwareVerdict.
 *
 * A verdict here is about the firmware SET, not about any particular game. Most titles
 * boot with no firmware at all; EdenFirmwareNotInstalled is information, not an error.
 */
int eden_content_verify_firmware(EdenFirmwareCheck *out, char *version, size_t version_len,
                                 char *detail, size_t detail_len);

#ifdef __cplusplus
}
#endif

#endif // EDEN_CONTENT_BRIDGE_H
