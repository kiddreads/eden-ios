// SPDX-FileCopyrightText: Copyright 2026 eden-ios contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Derived from suyu's src/libretro_core/retro_emu_window.h (suyu-emu/suyu-v0.0.4),
// GPL-3.0-or-later, which derives from yuzu (GPL-2.0-or-later).
// SPDX-FileCopyrightText: Copyright 2024 suyu Emulator Project
// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
//
// WHAT CHANGED VS SUYU
//   suyu sets window_info.type = WindowSystemType::Headless with
//   render_surface = nullptr (suyu retro_emu_window.cpp:10-11). On Eden that aborts
//   GPU init: RendererVulkan's ctor calls Vulkan::CreateSurface unconditionally
//   (src/video_core/renderer_vulkan/renderer_vulkan.cpp:143) and CreateSurface throws
//   vk::Exception(VK_ERROR_INITIALIZATION_FAILED) when no window-system branch matched
//   (src/video_core/vulkan_common/vulkan_surface.cpp:109-112). Headless matches none.
//   We report Cocoa instead, which is the arm Eden's own Apple code already takes:
//   vulkan_surface.cpp:33-47 calls vkCreateMetalSurfaceEXT with
//   .pLayer = static_cast<const CAMetalLayer*>(window_info.render_surface), and
//   vulkan_common/vulkan_instance.cpp:50-53 pushes VK_EXT_METAL_SURFACE_EXTENSION_NAME,
//   both under plain `defined(__APPLE__)` so they compile for iphoneos unchanged.
//
//   suyu also never paces on presentation. We override OnFrameDisplayed()
//   (src/core/frontend/emu_window.h:77), which RendererVulkan::Composite raises at
//   renderer_vulkan.cpp:206, and expose a timed wait for retro_run.

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>

#include "common/common_types.h"
#include "core/frontend/emu_window.h"

namespace LibretroCore {

class RetroEmuWindow final : public Core::Frontend::EmuWindow {
public:
    explicit RetroEmuWindow(void* metal_layer, u32 width, u32 height);
    ~RetroEmuWindow() override;

    // The only two pure virtuals on Core::Frontend::EmuWindow
    // (src/core/frontend/emu_window.h:81 and :84).
    std::unique_ptr<Core::Frontend::GraphicsContext> CreateSharedContext() const override;
    bool IsShown() const override;

    /// Raised on the GPU thread at the end of RendererVulkan::Composite
    /// (src/video_core/renderer_vulkan/renderer_vulkan.cpp:206). Must stay cheap and
    /// must never call back into libretro.
    void OnFrameDisplayed() override;

    /// Blocks until the next OnFrameDisplayed(), or until the timeout expires.
    /// Returns true if a frame arrived. This is how retro_run returns at guest cadence:
    /// Core::System::Run() (src/core/core.h:168) is not a step function - its whole body
    /// is SuspendEmulation(false) + SyncPause(false) (src/core/core.cpp:218-224) - so
    /// without this wait retro_run spins at full CPU whenever the guest is slower.
    bool WaitForFramePresented(std::chrono::milliseconds timeout);

    /// Main thread only. iOS hands us 0x0 during rotation and on backgrounding;
    /// Layout::DefaultFrameLayout opens with ASSERT(width > 0 && height > 0)
    /// (src/core/frontend/framebuffer_layout.cpp), so the dimensions are clamped.
    void Resize(u32 width, u32 height);

    /// The layer normally arrives after retro_init has already constructed the window,
    /// so it has to be assigned separately. It MUST land before Core::System::Load:
    /// RendererVulkan's constructor reads window_info.render_surface exactly once, in
    /// its member initialiser list, and the surface is never recreated on this platform.
    /// There is no second chance.
    void SetRenderSurface(void* metal_layer);

    void SetShown(bool value);

    u64 PresentedFrameCount() const {
        return presented_frames.load(std::memory_order_relaxed);
    }

private:
    std::mutex frame_mutex;
    std::condition_variable frame_cv;
    u64 frame_sequence{0};
    u64 last_seen_sequence{0};

    std::atomic<bool> shown{true};
    std::atomic<u64> presented_frames{0};
};

} // namespace LibretroCore
