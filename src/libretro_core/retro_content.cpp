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

#include <filesystem>
#include <system_error>

#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "common/settings_enums.h"
#include "core/crypto/key_manager.h"
#include "core/loader/loader.h"
#include "libretro.h"
#include "libretro_core/retro_content.h"
#include "libretro_core/retro_core_state.h"

namespace LibretroCore::Content {

namespace {

bool g_paths_ready = false;

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

} // namespace

bool SetupUserPaths() {
    namespace FS = Common::FS;
    using FS::EdenPath;

    if (g_paths_ready) {
        return true;
    }

    const auto root = ResolveRoot();
    if (root.empty()) {
        LOG_WARNING(Frontend,
                    "libretro: no data root supplied; falling back to Eden's default. On iOS "
                    "that is $HOME/.local/share/eden inside the container - writable, but "
                    "dot-hidden and invisible to Files.app.");
        return false;
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

    LOG_INFO(Frontend, "libretro: Eden data root = {}", FS::PathToUTF8String(root));
    g_paths_ready = true;
    return true;
}

void AdoptKeys() {
    namespace FS = Common::FS;

    const auto root = ResolveRoot();
    if (root.empty()) {
        return;
    }

    const auto src_dir = root / "keys_import";
    const auto dst_dir = FS::GetEdenPath(FS::EdenPath::KeysDir);
    if (FS::IsDir(src_dir)) {
        void(FS::CreateDirs(dst_dir));
        for (const char* name : {"prod.keys", "title.keys", "console.keys", "key_retail.bin"}) {
            const auto src = src_dir / name;
            const auto dst = dst_dir / name;
            if (!FS::Exists(src) || FS::Exists(dst)) {
                continue;
            }
            std::error_code ec;
            std::filesystem::copy_file(src, dst, ec);
            if (ec) {
                LOG_ERROR(Frontend, "libretro: failed to install {}: {}", name, ec.message());
            } else {
                LOG_INFO(Frontend, "libretro: installed {}", name);
            }
        }
    }

    // Core::Crypto::KeyManager is a function-local static whose CONSTRUCTOR calls
    // ReloadKeys() (src/core/crypto/key_manager.cpp:558-560). Anything that touched
    // KeyManager::Instance() before the EdenPath table was repointed cached keys read
    // from the wrong directory for the whole session, so reload unconditionally here.
    // The keys directory must also stay WRITABLE: KeyManager writes derived keys back
    // as *.keys_autogenerated into it and reads them on the next boot.
    Core::Crypto::KeyManager::Instance().ReloadKeys(); // key_manager.h:298
}

bool KeysPresent() {
    // Equivalent to frontend_common's ContentManager::AreKeysPresent(), reached
    // directly so this target does not have to link frontend_common. Returns false
    // unless the S256 header key and, for every crypto revision, the master,
    // key-area and titlekek keys are loaded.
    return !Core::Crypto::KeyManager::Instance().BaseDeriveNecessary(); // key_manager.h:285
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
    case Loader::ResultStatus::ErrorMissingProductionKeyFile:
    case Loader::ResultStatus::ErrorMissingHeaderKey:
    case Loader::ResultStatus::ErrorIncorrectHeaderKey:
    case Loader::ResultStatus::ErrorMissingTitlekey:
    case Loader::ResultStatus::ErrorMissingTitlekek:
    case Loader::ResultStatus::ErrorMissingKeyAreaKey:
    case Loader::ResultStatus::ErrorIncorrectKeyAreaKey:
    case Loader::ResultStatus::ErrorIncorrectTitlekeyOrTitlekek:
        return "decryption keys are missing or wrong - install prod.keys";
    default:
        return "loader error " + std::to_string(raw - loader_base);
    }
}

} // namespace LibretroCore::Content
