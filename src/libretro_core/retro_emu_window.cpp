// SPDX-FileCopyrightText: Copyright 2026 eden-ios contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Derived from suyu's src/libretro_core/retro_emu_window.cpp (suyu-emu/suyu-v0.0.4),
// GPL-3.0-or-later, which derives from yuzu (GPL-2.0-or-later).
// SPDX-FileCopyrightText: Copyright 2024 suyu Emulator Project
// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project

#include "common/logging.h" // Eden has no src/common/logging/ - it is one header
#include "core/frontend/graphics_context.h"
#include "libretro_core/retro_emu_window.h"

namespace LibretroCore {

namespace {
[[nodiscard]] u32 SanitizeDim(u32 value) {
    return value == 0 ? 1u : value;
}
} // namespace

RetroEmuWindow::RetroEmuWindow(void* metal_layer, u32 width, u32 height) {
#if defined(__APPLE__)
    // Core::Frontend::WindowSystemType (src/core/frontend/emu_window.h:21-28) has no
    // iOS or Metal member: Headless, Windows, X11, Wayland, Cocoa, Android, Xcb.
    // Cocoa is the Apple arm in both vulkan_instance.cpp:50-53 and
    // vulkan_surface.cpp:33-47, and both are guarded on plain `defined(__APPLE__)`,
    // so reusing it is correct for iphoneos and costs no Eden source change.
    // Adding an iOS enumerator would mean editing those branches for zero gain.
    window_info.type = Core::Frontend::WindowSystemType::Cocoa;
#else
    window_info.type = Core::Frontend::WindowSystemType::Headless;
#endif
    window_info.render_surface = metal_layer;
    // The frontend passes physical pixels, so the renderer works in physical pixels
    // and this stays 1.0.
    window_info.render_surface_scale = 1.0f;
    // src/core/frontend/emu_window.h:181. No GL context-sharing rules to honour.
    strict_context_required = false;

    if (metal_layer == nullptr) {
        LOG_CRITICAL(Frontend,
                     "libretro: no CAMetalLayer supplied. Eden's RendererVulkan ctor "
                     "calls CreateSurface unconditionally (renderer_vulkan.cpp:143) and "
                     "CreateSurface throws for an unmatched window type "
                     "(vulkan_surface.cpp:109-112). GPU init will fail.");
    }

    const u32 w = SanitizeDim(width);
    const u32 h = SanitizeDim(height);
    NotifyClientAreaSizeChanged({w, h}); // emu_window.h:163
    UpdateCurrentFramebufferLayout(w, h); // emu_window.h:126
}

RetroEmuWindow::~RetroEmuWindow() {
    // Release anyone parked in WaitForFramePresented so teardown cannot deadlock.
    {
        std::lock_guard lock{frame_mutex};
        ++frame_sequence;
    }
    frame_cv.notify_all();
}

std::unique_ptr<Core::Frontend::GraphicsContext> RetroEmuWindow::CreateSharedContext() const {
    // Core::Frontend::GraphicsContext is concrete: SwapBuffers, MakeCurrent,
    // DoneCurrent and GetDriverLibrary all have bodies
    // (src/core/frontend/graphics_context.h:20-31). Vulkan needs no host context and
    // Vulkan::OpenLibrary ignores the context off Android.
    return std::make_unique<Core::Frontend::GraphicsContext>();
}

bool RetroEmuWindow::IsShown() const {
    return shown.load(std::memory_order_relaxed);
}

void RetroEmuWindow::OnFrameDisplayed() {
    presented_frames.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard lock{frame_mutex};
        ++frame_sequence;
    }
    frame_cv.notify_all();
}

bool RetroEmuWindow::WaitForFramePresented(std::chrono::milliseconds timeout) {
    std::unique_lock lock{frame_mutex};
    const bool arrived = frame_cv.wait_for(lock, timeout, [this] {
        return frame_sequence != last_seen_sequence;
    });
    last_seen_sequence = frame_sequence;
    return arrived;
}

void RetroEmuWindow::SetRenderSurface(void* metal_layer) {
    window_info.render_surface = metal_layer;
}

void RetroEmuWindow::Resize(u32 width, u32 height) {
    const u32 w = SanitizeDim(width);
    const u32 h = SanitizeDim(height);
    NotifyClientAreaSizeChanged({w, h});
    UpdateCurrentFramebufferLayout(w, h);
    // No explicit swapchain kick: PresentManager compares the swapchain extent against
    // the incoming frame and recreates on mismatch, and the frame is sized from
    // render_window.GetFramebufferLayout() inside RendererVulkan::Composite.
}

void RetroEmuWindow::SetShown(bool value) {
    shown.store(value, std::memory_order_relaxed);
}

} // namespace LibretroCore
