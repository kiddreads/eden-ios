// SPDX-FileCopyrightText: Copyright 2026 eden-ios contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Derived from suyu's src/libretro_core/retro_core.cpp (suyu-emu/suyu-v0.0.4),
// GPL-3.0-or-later, which derives from yuzu (GPL-2.0-or-later).
// SPDX-FileCopyrightText: Copyright 2024 suyu Emulator Project
// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
//
// WHAT CHANGED VS SUYU
//  * Common::FS::GetSuyuPath / SuyuPath -> GetEdenPath / EdenPath
//    (src/common/fs/path_util.h:16-37, :231).
//  * suyu's scan of sibling emulators' roaming directories for keys
//    ({suyu,yuzu,sudachi,citron,Ryujinx}, suyu retro_core.cpp:182-207) is DELETED,
//    not ported. Every iOS app has its own container and cannot read another's, so
//    every candidate path is guaranteed absent.
//  * Common::FS::CreateDir / CreateDirs are [[nodiscard]] in Eden
//    (src/common/fs/fs.h:157, :185); suyu calls them bare, which is a build failure
//    under Eden's -Werror=all. Wrapped as void(...) - the idiom Eden itself uses.
//  * NEW: the whole SetupUserPaths function. suyu never repoints its data root; it
//    relies on GetSuyuPath's OS default. That cannot work on iOS - see below.
//  * NEW: the status report (Describe/StatusToken/DescribeStatus) and the firmware
//    scan. suyu has neither; it fails at load with a bare number.
//
// THIS FILE CONTAINS NO KEYS, NO FIRMWARE AND NO GAME DATA, AND FETCHES NONE.
// It only reports on, and copies between, directories the user populated themselves.
// The one copy it performs is <root>/keys_import -> <root>/keys, both inside the app's
// own container. There is no network access anywhere in this translation unit.

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

#include "common/fs/fs.h"
#include "common/fs/fs_util.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "common/settings_enums.h"
#include "core/crypto/key_manager.h"
#include "core/file_sys/nca_metadata.h"
#include "core/file_sys/registered_cache.h"
#include "core/hle/service/am/am_types.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/loader/loader.h"
#include "libretro.h"
#include "libretro_core/retro_content.h"
#include "libretro_core/retro_core_state.h"

namespace LibretroCore::Content {

namespace {

/// The root that is actually installed in the Common::FS EdenPath table.
///
/// This replaces the old `bool g_paths_ready` latch. The latch was a real bug: it made
/// the SECOND call to SetupUserPaths() a no-op whatever the root had become, while
/// AdoptKeys() re-ran ResolveRoot() live. An app that called
/// eden_libretro_set_data_root() after retro_init therefore had its keys copied out of
/// the NEW root's keys_import and into the OLD root's keys directory - where Eden,
/// still pointed at the old root, would read them, so it half-worked, which is worse
/// than failing. Everything now resolves against this one variable.
std::filesystem::path g_applied_root;

/// Where the data tree should live. Preference order:
///   1. eden_libretro_set_data_root() - what the iOS app should call.
///   2. RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY + "/eden".
/// Empty if neither is available, in which case Eden's own default applies and, on
/// iOS, lands somewhere dot-hidden - see the header comment in SetupUserPaths.
std::filesystem::path ResolveRoot() {
    if (!g_data_root_override.empty()) {
        return std::filesystem::path{g_data_root_override};
    }
    if (g_environ_cb != nullptr) {
        const char* dir = nullptr;
        if (g_environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &dir) && dir != nullptr) {
            return std::filesystem::path{dir} / "eden";
        }
    }
    return {};
}

/// The base key file Eden will actually open. KeyManager::ReloadKeys
/// (key_manager.cpp:568-576) reads dev.keys when Settings::values.use_dev_keys
/// (settings.h:920) is set and prod.keys otherwise; KeyManager::KeyFileExists
/// (key_manager.cpp:847-853) applies the same rule. Reproduced rather than guessed so
/// the path shown to the user is the path Eden reads.
const char* BaseKeyFileName() {
    return Settings::values.use_dev_keys.GetValue() ? "dev.keys" : "prod.keys";
}

/// The file names AdoptKeys will move out of keys_import. Anything else is ignored,
/// because these are the only names anything in Eden ever opens:
///   prod.keys / dev.keys / title.keys / console.keys - key_manager.cpp:570-580
///   key_retail.bin (amiibo only)                     - amiibo_crypto.cpp:284, :307
constexpr const char* ADOPTABLE_KEY_FILES[] = {
    "prod.keys", "dev.keys", "title.keys", "console.keys", "key_retail.bin",
};

bool IsHexDigit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/// The name test FileSys::RegisteredCache applies when it accumulates content
/// (registered_cache.cpp:56-63): 32 hex digits followed by ".nca", or by ".cnmt.nca".
/// Reimplemented without <regex> - the original compiles two static std::regex objects,
/// which is a lot of code size and a first-call cost for a string comparison.
bool FollowsNcaIdFormat(std::string_view name) {
    constexpr std::size_t kIdLen = 32;
    constexpr std::string_view kNca = ".nca";
    constexpr std::string_view kCnmtNca = ".cnmt.nca";

    const bool plain = name.size() == kIdLen + kNca.size() && name.substr(kIdLen) == kNca;
    const bool cnmt = name.size() == kIdLen + kCnmtNca.size() && name.substr(kIdLen) == kCnmtNca;
    if (!plain && !cnmt) {
        return false;
    }
    for (std::size_t i = 0; i < kIdLen; ++i) {
        if (!IsHexDigit(name[i])) {
            return false;
        }
    }
    return true;
}

/// The second layout the cache accepts: a "000000XX" bucket directory holding NCAs
/// (registered_cache.cpp:50-54).
bool FollowsTwoDigitDirFormat(std::string_view name) {
    constexpr std::string_view kPrefix = "000000";
    if (name.size() != kPrefix.size() + 2 || name.substr(0, kPrefix.size()) != kPrefix) {
        return false;
    }
    return IsHexDigit(name[6]) && IsHexDigit(name[7]);
}

/// Count NCA-shaped entries directly inside `dir`, without descending.
/// Both files and directories count: an installed NCA can be a plain
/// "<id>.nca" file or an "<id>.nca/" directory holding numbered chunks
/// (registered_cache.cpp:622-655 accepts both).
std::size_t CountNcasIn(const std::filesystem::path& dir) {
    if (!Common::FS::IsDir(dir)) {
        return 0;
    }
    std::size_t count = 0;
    std::error_code ec;
    for (std::filesystem::directory_iterator it{dir, ec}, end; !ec && it != end;
         it.increment(ec)) {
        if (FollowsNcaIdFormat(it->path().filename().string())) {
            ++count;
        }
    }
    if (ec) {
        LOG_WARNING(Frontend, "libretro: could not read {}: {}",
                    Common::FS::PathToUTF8String(dir), ec.message());
    }
    return count;
}

} // namespace

const std::filesystem::path& AppliedRoot() {
    return g_applied_root;
}

bool SetupUserPaths() {
    namespace FS = Common::FS;
    using FS::EdenPath;

    const auto root = ResolveRoot();

    if (root.empty()) {
        // Nothing new to apply. If a root was applied earlier the table is still valid;
        // only report failure when there has never been one.
        if (!g_applied_root.empty()) {
            return true;
        }
        LOG_WARNING(Frontend,
                    "libretro: no data root supplied; falling back to Eden's default. On iOS "
                    "that is $HOME/.local/share/eden inside the container - writable, but "
                    "dot-hidden and invisible to Files.app.");
        return false;
    }

    if (root == g_applied_root) {
        return true; // Idempotent: the table already points here.
    }

    // Common::FS::SetAppDirectory (path_util.h:222) does NOT work here.
    // PathManagerImpl::Reinitialize's non-Windows, non-Android branch
    // (src/common/fs/path_util.cpp:133-142) assigns over its own eden_path argument
    // with GetCurrentDir()/"user" and then $XDG_DATA_HOME/eden, so the argument is
    // discarded on every Apple platform; __ANDROID__ is the only branch that honours
    // it. Until path_util.cpp grows a TARGET_OS_IPHONE branch, each path is set by hand.
    //
    // Common::FS::SetEdenPath (path_util.cpp:302-309) logs an error and does nothing
    // if the new path is not ALREADY a directory, so every directory is created first.
    // Nothing else creates them: Common::FS::CreateEdenPaths() is called only from
    // src/qt_common/qt_common.cpp and src/yuzu/main_window.cpp, and path_util.cpp
    // explicitly defers creation.
    if (!FS::CreateDirs(root)) {
        LOG_CRITICAL(Frontend, "libretro: could not create Eden root at {}",
                     FS::PathToUTF8String(root));
        return false;
    }

    const auto set_path = [](EdenPath id, const std::filesystem::path& path) {
        void(FS::CreateDirs(path)); // [[nodiscard]] - src/common/fs/fs.h:185
        FS::SetEdenPath(id, path);
    };

    set_path(EdenPath::EdenDir, root);
    set_path(EdenPath::AmiiboDir, root / "amiibo");
    set_path(EdenPath::CacheDir, root / "cache");
    set_path(EdenPath::ConfigDir, root / "config");
    set_path(EdenPath::CrashDumpsDir, root / "crash_dumps");
    set_path(EdenPath::DumpDir, root / "dump");
    set_path(EdenPath::IconsDir, root / "icons");
    set_path(EdenPath::KeysDir, root / "keys");
    set_path(EdenPath::LoadDir, root / "load");
    set_path(EdenPath::LogDir, root / "log");
    set_path(EdenPath::LosslessDir, root / "lossless");
    set_path(EdenPath::NANDDir, root / "nand");
    set_path(EdenPath::PlayTimeDir, root / "play_time");
    set_path(EdenPath::PostPresetDir, root / "post_presets");
    set_path(EdenPath::PostShaderDir, root / "post_shaders");
    set_path(EdenPath::SaveDir, root / "nand"); // SaveDir aliases NANDDir upstream
    set_path(EdenPath::SDMCDir, root / "sdmc");
    set_path(EdenPath::ScreenshotsDir, root / "screenshots");
    set_path(EdenPath::ShaderDir, root / "cache" / "shader");
    set_path(EdenPath::TASDir, root / "tas");

    // The drop-off the app writes an imported prod.keys into.
    void(FS::CreateDirs(root / "keys_import"));

    // The firmware drop-off. FileSys::BISFactory creates this itself
    // (bis_factory.cpp:19-20, GetOrCreateDirectoryRelative(nand_root,
    // "/system/Contents/registered")) - but only once CreateFactories runs, which is
    // during a game load. Creating it up front is what lets the user find the folder in
    // Files.app BEFORE the first launch, which is the only moment they need it.
    void(FS::CreateDirs(root / "nand" / "system" / "Contents" / "registered"));

    if (!g_applied_root.empty()) {
        LOG_INFO(Frontend, "libretro: Eden data root moved {} -> {}",
                 FS::PathToUTF8String(g_applied_root), FS::PathToUTF8String(root));
    } else {
        LOG_INFO(Frontend, "libretro: Eden data root = {}", FS::PathToUTF8String(root));
    }
    g_applied_root = root;
    return true;
}

Paths GetPaths() {
    namespace FS = Common::FS;

    Paths paths{};
    if (g_applied_root.empty()) {
        paths.valid = false;
        return paths;
    }

    paths.valid = true;
    paths.root = g_applied_root;
    // Read back out of the EdenPath table rather than re-deriving root/"keys": the
    // table is what Eden itself will use, and if anything ever re-points it behind our
    // back the user must be told the truth, not our assumption.
    paths.keys_dir = FS::GetEdenPath(FS::EdenPath::KeysDir);          // path_util.h:231
    paths.keys_import_dir = g_applied_root / "keys_import";
    paths.firmware_dir =
        FS::GetEdenPath(FS::EdenPath::NANDDir) / "system" / "Contents" / "registered";
    paths.base_key_file = paths.keys_dir / BaseKeyFileName();
    return paths;
}

void AdoptKeys() {
    namespace FS = Common::FS;

    // static_cast, not void(...): `void(Qualified::Name())` on a no-argument call is
    // the vexing parse and is ill-formed inside a function body.
    // Ordering matters and is enforced here rather than documented: AdoptKeys must
    // never resolve a root that SetupUserPaths has not applied to the EdenPath table.
    static_cast<void>(SetupUserPaths());
    if (g_applied_root.empty()) {
        return;
    }

    const auto src_dir = g_applied_root / "keys_import";
    const auto dst_dir = FS::GetEdenPath(FS::EdenPath::KeysDir);
    if (FS::IsDir(src_dir)) {
        void(FS::CreateDirs(dst_dir));
        for (const char* const name : ADOPTABLE_KEY_FILES) {
            const auto src = src_dir / name;
            const auto dst = dst_dir / name;
            if (!FS::Exists(src)) {
                continue;
            }

            // The old rule was "skip if the destination exists", which silently ignored
            // a NEWER prod.keys dropped into keys_import - exactly what a user does
            // when their keys turn out to be for an older firmware generation. Copy
            // when the destination is missing, or when the source differs from it.
            // std::error_code overloads throughout: a filesystem_error escaping into
            // retro_load_game would abort the app.
            bool replace = true;
            if (FS::Exists(dst)) {
                // One error_code per query: the std::filesystem overloads CLEAR the code
                // on success, so sharing one between four calls lets a later success
                // erase an earlier failure.
                std::error_code src_size_ec;
                std::error_code dst_size_ec;
                std::error_code src_time_ec;
                std::error_code dst_time_ec;
                const auto src_size = std::filesystem::file_size(src, src_size_ec);
                const auto dst_size = std::filesystem::file_size(dst, dst_size_ec);
                const auto src_time = std::filesystem::last_write_time(src, src_time_ec);
                const auto dst_time = std::filesystem::last_write_time(dst, dst_time_ec);
                if (src_size_ec || dst_size_ec || src_time_ec || dst_time_ec) {
                    replace = true; // Could not compare: prefer the drop-off.
                } else {
                    replace = src_size != dst_size || src_time > dst_time;
                }
            }
            if (!replace) {
                continue;
            }

            std::error_code copy_ec;
            std::filesystem::copy_file(src, dst, std::filesystem::copy_options::overwrite_existing,
                                       copy_ec);
            if (copy_ec) {
                LOG_ERROR(Frontend, "libretro: failed to install {}: {}", name, copy_ec.message());
            } else {
                LOG_INFO(Frontend, "libretro: installed {} into {}", name,
                         FS::PathToUTF8String(dst_dir));
            }
        }
    }

    // Core::Crypto::KeyManager is a function-local static whose CONSTRUCTOR calls
    // ReloadKeys() (src/core/crypto/key_manager.cpp:559-561). Anything that touched
    // KeyManager::Instance() before the EdenPath table was repointed cached keys read
    // from the wrong directory for the whole session, so reload unconditionally here.
    // The keys directory must also stay WRITABLE: KeyManager writes derived keys back
    // as *.keys_autogenerated into it and reads them on the next boot.
    Core::Crypto::KeyManager::Instance().ReloadKeys(); // key_manager.h:298
}

bool KeysPresent() {
    // Equivalent to frontend_common's ContentManager::AreKeysPresent()
    // (content_manager.h:379-381), reached directly so this target does not have to
    // link frontend_common. BaseDeriveNecessary (key_manager.cpp:692-710) returns true
    // - i.e. keys are NOT usable - unless the S256 header key is loaded AND, for every
    // crypto revision below CURRENT_CRYPTO_REVISION (key_manager.cpp:36, currently 5),
    // the master key, all three key-area keys (application/ocean/system) and the
    // titlekek are loaded. A prod.keys from an older firmware generation satisfies the
    // header check and fails the loop: that is the "stale keys" case, and it is why
    // "the file exists" is not the same question as "the keys work".
    return !Core::Crypto::KeyManager::Instance().BaseDeriveNecessary(); // key_manager.h:285
}

std::size_t CountFirmwareNcas() {
    const auto paths = GetPaths();
    if (!paths.valid) {
        return 0;
    }

    std::size_t count = CountNcasIn(paths.firmware_dir);

    // Plus the bucketed layout: <registered>/000000XX/<id>.nca.
    if (Common::FS::IsDir(paths.firmware_dir)) {
        std::error_code ec;
        for (std::filesystem::directory_iterator it{paths.firmware_dir, ec}, end;
             !ec && it != end; it.increment(ec)) {
            if (!it->is_directory(ec) || ec) {
                ec.clear();
                continue;
            }
            if (FollowsTwoDigitDirFormat(it->path().filename().string())) {
                count += CountNcasIn(it->path());
            }
        }
    }
    return count;
}

bool FirmwarePresent() {
    return CountFirmwareNcas() != 0;
}

bool FirmwareRegistered(Core::System& system) {
    // FirmwareManager::CheckFirmwarePresence (frontend_common/firmware_manager.h:66-82)
    // asks the system-NAND RegisteredCache for the Mii Edit applet's Program NCA. The
    // same predicate, without linking frontend_common - whose header pulls in
    // core/hle/service/am/frontend/applet_mii_edit.h.
    //
    // HasEntry (registered_cache.cpp:716-718) is `GetEntryRaw(...) != nullptr`, and
    // FirmwareManager's GetEntry (registered_cache.cpp:744-750) returns nullptr on
    // exactly that condition, so the two agree while costing one fewer NCA parse.
    //
    // Service::AM::AppletProgramId::MiiEdit is 0x0100000000001009 (am_types.h:110).
    // am_types.h is pulled in only for this enum, and it is cheap: common_funcs.h and
    // common_types.h are its only includes. retro_core.cpp already includes it too.
    constexpr u64 kMiiEditProgramId = static_cast<u64>(Service::AM::AppletProgramId::MiiEdit);

    const auto* cache = system.GetFileSystemController().GetSystemNANDContents(); // core.h:364
    if (cache == nullptr) {
        // bis_factory is null until FileSystemController::CreateFactories runs
        // (filesystem.cpp:517-523), i.e. before the first load. Not an error; the
        // caller wanted CountFirmwareNcas().
        return false;
    }
    return cache->HasEntry(kMiiEditProgramId, FileSys::ContentRecordType::Program);
}

Report Describe() {
    Report report{};

    // Resolve paths first: a status computed against a half-initialised EdenPath table
    // would name directories Eden is not using. Idempotent once a root is applied.
    static_cast<void>(SetupUserPaths());
    report.paths = GetPaths();

    if (!report.paths.valid) {
        report.status = Status::NoDataRoot;
        return report;
    }

    namespace FS = Common::FS;
    report.base_key_file_present = FS::Exists(report.paths.base_key_file);
    report.title_key_file_present = FS::Exists(report.paths.keys_dir / "title.keys");
    report.amiibo_key_file_present = FS::Exists(report.paths.keys_dir / "key_retail.bin");
    report.keys_usable = KeysPresent();

    // The one case where re-reading from disk can change the answer from wrong to
    // right: a key file is sitting there that the in-memory keyring has not been told
    // about, because the user dropped it in through Files.app after the KeyManager
    // singleton was first constructed. Costs one small file read, and only in the
    // failure state the user is actively trying to get out of.
    if (!report.keys_usable && report.base_key_file_present) {
        Core::Crypto::KeyManager::Instance().ReloadKeys(); // key_manager.h:298
        report.keys_usable = KeysPresent();
    }

    report.firmware_nca_count = CountFirmwareNcas();

    if (report.keys_usable) {
        report.status = report.firmware_nca_count != 0 ? Status::Ready : Status::NoFirmware;
    } else if (report.base_key_file_present) {
        report.status = Status::BadKeys;
    } else {
        report.status = Status::NoKeys;
    }
    return report;
}

const char* StatusToken(Status status) {
    switch (status) {
    case Status::NoDataRoot:
        return "no_data_root";
    case Status::NoKeys:
        return "no_keys";
    case Status::BadKeys:
        return "bad_keys";
    case Status::NoFirmware:
        return "no_firmware";
    case Status::Ready:
        return "ready";
    }
    return "no_data_root";
}

std::string DescribeStatus(const Report& report) {
    const auto keys_path = Common::FS::PathToUTF8String(report.paths.keys_import_dir);
    const auto firmware_path = Common::FS::PathToUTF8String(report.paths.firmware_dir);
    const std::string key_file{BaseKeyFileName()};

    switch (report.status) {
    case Status::NoDataRoot:
        return "Eden has nowhere to store its data. The app must call "
               "eden_libretro_set_data_root() before retro_init.";
    case Status::NoKeys:
        return "No " + key_file + " found. Put your own " + key_file + " in " + keys_path +
               " - nothing encrypted can be opened without it. Eden does not supply keys and "
               "cannot obtain them for you.";
    case Status::BadKeys:
        return key_file +
               " is there but Eden could not derive a complete keyring from it. It is usually "
               "truncated, or from an older system version than the games you want to run. "
               "Replace it in " + keys_path + " with a complete, current one.";
    case Status::NoFirmware:
        return "Keys are good. No firmware is installed - put the firmware's .nca files in " +
               firmware_path +
               ". Most games run without it; install it if a game asks for system files, a "
               "shared font, or a system applet.";
    case Status::Ready:
        return "Keys are good and firmware is installed (" +
               std::to_string(report.firmware_nca_count) + " NCA files in " + firmware_path + ").";
    }
    return "Unknown content status.";
}

unsigned RetroRegion() {
    // Switch content is region free, so there is no per-ROM region to report; suyu
    // hardcodes NTSC. Eden does model the emulated console's region
    // (Settings::values.region_index, src/common/settings.h:765; enum
    // Settings::Region at src/common/settings_enums.h:126), so report that instead.
    switch (Settings::values.region_index.GetValue()) {
    case Settings::Region::Europe:
    case Settings::Region::Australia:
        return RETRO_REGION_PAL;
    default:
        return RETRO_REGION_NTSC;
    }
}

std::string DescribeLoadFailure(Core::SystemResultStatus status) {
    // Mirrors the decode Eden's own headless frontend does in src/yuzu_cmd/yuzu.cpp:
    // anything at or above ErrorLoader is ErrorLoader + (u16)Loader::ResultStatus.
    // suyu only logged the raw u32, which is how a missing prod.keys becomes an
    // opaque number instead of a sentence.
    switch (status) {
    case Core::SystemResultStatus::ErrorGetLoader:
        return "no loader could handle this file";
    case Core::SystemResultStatus::ErrorNotInitialized:
        return "CPU core not initialized";
    // ErrorSystemFiles (core.h:137) and ErrorSharedFont (core.h:138) are declared but
    // returned by nothing in src/ - a grep finds them only in core.h itself and in the
    // Android frontend's Kotlin mirror (NativeLibrary.kt:279, :382-383). Missing
    // firmware therefore does NOT surface as a distinct load status; it surfaces later,
    // as a service failure inside the guest. Mapped anyway in case that changes, but do
    // not build a firmware check on top of it - Content::Describe() is the check.
    case Core::SystemResultStatus::ErrorSystemFiles:
        return "missing system files - install firmware";
    case Core::SystemResultStatus::ErrorSharedFont:
        return "missing shared font - install firmware";
    case Core::SystemResultStatus::ErrorVideoCore:
        return "video core failed to initialize";
    case Core::SystemResultStatus::ErrorUnknown:
        return "unknown error";
    default:
        break;
    }

    const auto loader_base = static_cast<u32>(Core::SystemResultStatus::ErrorLoader);
    const auto raw = static_cast<u32>(status);
    if (raw < loader_base) {
        return "unknown error";
    }
    switch (static_cast<Loader::ResultStatus>(raw - loader_base)) {
    // loader.h:103-113. These are the statuses that mean "the key situation is wrong",
    // as opposed to "this particular title needs a title key you do not have":
    // ErrorMissingProductionKeyFile is raised by xci.cpp:77, nsp.cpp:116 and nax.cpp:55
    // when KeyManager::KeyFileExists(false) is false at open time.
    case Loader::ResultStatus::ErrorMissingProductionKeyFile:
    case Loader::ResultStatus::ErrorMissingHeaderKey:
    case Loader::ResultStatus::ErrorIncorrectHeaderKey:
    case Loader::ResultStatus::ErrorMissingKeyAreaKey:
    case Loader::ResultStatus::ErrorIncorrectKeyAreaKey:
        return "decryption keys are missing or wrong - install a current prod.keys";
    // These mean the keyring is fine but this specific title's key is absent, which is
    // a different sentence: a complete prod.keys will not fix it, title.keys might.
    case Loader::ResultStatus::ErrorMissingTitlekey:
    case Loader::ResultStatus::ErrorMissingTitlekek:
    case Loader::ResultStatus::ErrorIncorrectTitlekeyOrTitlekek:
        return "this title's key is missing or wrong - it needs a matching title.keys entry";
    default:
        return "loader error " + std::to_string(raw - loader_base);
    }
}

} // namespace LibretroCore::Content
