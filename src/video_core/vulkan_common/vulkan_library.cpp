// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <string>

#include "common/dynamic_library.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "video_core/vulkan_common/vulkan_library.h"

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

namespace Vulkan {

std::shared_ptr<Common::DynamicLibrary> OpenLibrary(
    [[maybe_unused]] Core::Frontend::GraphicsContext* context) {
    LOG_DEBUG(Render_Vulkan, "Looking for a Vulkan library");
#if defined(__ANDROID__) && defined(ARCHITECTURE_arm64)
    // Android manages its Vulkan driver from the frontend.
    return context->GetDriverLibrary();
#else
    auto library = std::make_shared<Common::DynamicLibrary>();
#if defined(__APPLE__) && TARGET_OS_IPHONE
    // An iOS bundle is flat. Frameworks live at <App>.app/Frameworks with no Contents/
    // level, so the macOS paths below never resolve here.
    const auto libmoltenvk_filename =
        Common::FS::GetBundleDirectory() / "Frameworks/libMoltenVK.dylib";
    const auto libvulkan_filename =
        Common::FS::GetBundleDirectory() / "Frameworks/libvulkan.1.dylib";
    const char* library_paths[] = {std::getenv("LIBVULKAN_PATH"), libmoltenvk_filename.c_str(),
                                   libvulkan_filename.c_str()};
    bool opened = false;
    for (const auto& library_path : library_paths) {
        if (library_path && library->Open(library_path)) {
            opened = true;
            break;
        }
    }
    if (!opened) {
        // MoltenVK is usually linked statically into the executable on iOS, since iOS will
        // not load a dynamic library the app did not ship signed inside its own bundle. In
        // that case there is no file to open and dlopen(nullptr) - a handle to the main
        // program itself - is what resolves the Vulkan entry points.
        if (library->Open(nullptr)) {
            LOG_INFO(Render_Vulkan, "Using MoltenVK linked into the executable");
        } else {
            LOG_ERROR(Render_Vulkan, "No Vulkan library found: not in the bundle and not "
                                     "linked into the executable");
        }
    }
#elif defined(__APPLE__)
    const auto libvulkan_filename =
        Common::FS::GetBundleDirectory() / "Contents/Frameworks/libvulkan.1.dylib";
    const auto libmoltenvk_filename =
        Common::FS::GetBundleDirectory() / "Contents/Frameworks/libMoltenVK.dylib";
    const char* library_paths[] = {std::getenv("LIBVULKAN_PATH"), libvulkan_filename.c_str(),
                                   libmoltenvk_filename.c_str()};
    // Check if a path to a specific Vulkan library has been specified.
    for (const auto& library_path : library_paths) {
        if (library_path && library->Open(library_path)) {
            break;
        }
    }
#else
    std::string filename = Common::DynamicLibrary::GetVersionedFilename("vulkan", 1);
    LOG_DEBUG(Render_Vulkan, "Trying Vulkan library: {}", filename);
    if (!library->Open(filename.c_str())) {
        // Android devices may not have libvulkan.so.1, only libvulkan.so.
        filename = Common::DynamicLibrary::GetVersionedFilename("vulkan");
        LOG_DEBUG(Render_Vulkan, "Trying Vulkan library (second attempt): {}", filename);
        void(library->Open(filename.c_str()));
    }
#endif
    return library;
#endif
}

} // namespace Vulkan
