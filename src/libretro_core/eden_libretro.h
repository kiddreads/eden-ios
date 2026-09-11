// SPDX-FileCopyrightText: Copyright 2026 eden-ios contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Derived from suyu's src/libretro_core (suyu-emu/suyu-v0.0.4), GPL-3.0-or-later.
// SPDX-FileCopyrightText: Copyright 2024 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// suyu in turn derives from yuzu:
// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Hand the core the CAMetalLayer it should render into, and the layer's size in
 * PHYSICAL pixels (already multiplied by contentsScale).
 *
 * This is mandatory, not optional. Eden's RendererVulkan ctor calls CreateSurface
 * unconditionally (src/video_core/renderer_vulkan/renderer_vulkan.cpp:143) and
 * Vulkan::CreateSurface throws vk::Exception(VK_ERROR_INITIALIZATION_FAILED) when no
 * window-system branch produced a surface (vulkan_common/vulkan_surface.cpp:109-112).
 * WindowSystemType::Headless matches no branch, so there is no headless fallback.
 *
 * The pointer must be an actual CAMetalLayer, not a UIView: vulkan_surface.cpp:39
 * passes it straight to VkMetalSurfaceCreateInfoEXT::pLayer. On iOS the simplest
 * correct source is a UIView whose +layerClass is CAMetalLayer, then view.layer.
 *
 * Call before retro_load_game. The layer must outlive Core::System.
 */
void eden_libretro_set_metal_layer(void* metal_layer, unsigned width, unsigned height);

/**
 * Call from layoutSubviews / rotation with the new PHYSICAL pixel size.
 * Must be called on the main thread: MoltenVK touches the layer from whichever
 * thread calls vkAcquireNextImageKHR, and the layer's geometry must not be mutated
 * concurrently from elsewhere.
 */
void eden_libretro_resize(unsigned width, unsigned height);

/**
 * Call from applicationDidEnterBackground / applicationWillEnterForeground.
 * RendererVulkan::Composite early-returns when the window reports not-shown, which
 * is how presentation stops against a layer iOS has taken away.
 */
void eden_libretro_set_visible(bool visible);

/**
 * Point Eden's data tree (keys, NAND, sdmc, shader cache, logs) at a writable
 * directory inside the app container.
 *
 * Required on iOS. Common::FS::SetAppDirectory() does NOT work here:
 * PathManagerImpl::Reinitialize's non-Windows, non-Android branch
 * (src/common/fs/path_util.cpp:133-142) assigns over its own argument with
 * GetCurrentDir()/"user" and then $XDG_DATA_HOME/eden, so the argument is discarded
 * on Apple platforms. Without this call Eden writes to $HOME/.local/share/eden
 * inside the container - writable, but dot-hidden and invisible to Files.app.
 *
 * Call before retro_load_game. If it is never called, the core falls back to
 * RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, and failing that to Eden's own default.
 */
void eden_libretro_set_data_root(const char* path);

#ifdef __cplusplus
}
#endif
