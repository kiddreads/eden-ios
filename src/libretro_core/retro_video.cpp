// SPDX-FileCopyrightText: Copyright 2026 eden-ios contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Derived from suyu's src/libretro_core/retro_core.cpp video block
// (suyu-emu/suyu-v0.0.4 retro_core.cpp:391-415), GPL-3.0-or-later, which derives
// from yuzu (GPL-2.0-or-later).
// SPDX-FileCopyrightText: Copyright 2024 suyu Emulator Project
// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
//
// WHAT CHANGED VS SUYU
//   suyu's retro_run reads the frame back with renderer.IsHeadless(),
//   GetLastRenderedFrame(), GetHeadlessWidth() and GetHeadlessHeight()
//   (suyu retro_core.cpp:393-404, declared suyu renderer_base.h:51-57). Those four
//   are suyu additions to its own RendererBase and DO NOT EXIST IN EDEN:
//   `grep -rn "GetHeadless" /Volumes/EdenWork/eden/src` returns nothing, and
//   src/video_core/renderer_base.h declares no headless virtuals at all.
//   The whole block is dropped rather than ported.

#include "common/logging.h"
#include "core/frontend/framebuffer_layout.h"
#include "libretro_core/retro_core_state.h"
#include "libretro_core/retro_emu_window.h"
#include "libretro_core/retro_video.h"

namespace LibretroCore::Video {

namespace {

// Nominal libretro geometry. Eden renders at whatever the guest plus
// Settings::values.resolution_setup produce; the frontend is told the real size
// through the framebuffer layout and resizes within max_width/max_height.
constexpr unsigned kBaseWidth = 1280;
constexpr unsigned kBaseHeight = 720;
constexpr unsigned kMaxWidth = 1920 * 4;
constexpr unsigned kMaxHeight = 1080 * 4;

bool g_format_ok = false;

} // namespace

void Init(retro_environment_t cb) {
    if (cb == nullptr) {
        return;
    }
    // Kept even though the direct-to-layer path never hands libretro real pixels:
    // a frontend that refuses XRGB8888 has no way to display a readback frame either,
    // and the negotiation is how we would find that out.
    //
    // The format is the right one if readback is ever implemented:
    // RendererVulkan::RenderScreenshot renders VK_FORMAT_B8G8R8A8_UNORM
    // (src/video_core/renderer_vulkan/renderer_vulkan.cpp:306), which is B,G,R,A
    // ascending; libretro XRGB8888 is the word 0xXXRRGGBB, i.e. B,G,R,X on a
    // little-endian host. Same bytes, no swizzle.
    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
    g_format_ok = cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt);
    if (!g_format_ok) {
        LOG_WARNING(Frontend, "libretro: frontend refused XRGB8888");
    }
}

void Shutdown() {
    g_format_ok = false;
}

void OnGameLoaded() {}

void OnGameUnloaded() {}

void Present() {
    if (g_video_cb == nullptr) {
        return;
    }

    unsigned width = kBaseWidth;
    unsigned height = kBaseHeight;
    if (g_emu_window) {
        const auto& layout = g_emu_window->GetFramebufferLayout(); // emu_window.h:118
        if (layout.width != 0 && layout.height != 0) {
            width = layout.width;
            height = layout.height;
        }
    }

    // Eden owns the swapchain on the app's CAMetalLayer and presents through
    // Vulkan::PresentManager on its own thread, so there is nothing for libretro to
    // deliver. A null data pointer is libretro's documented "duplicate the previous
    // frame" signal, which keeps the frontend from treating the core as hung or
    // clearing the screen.
    //
    // TODO(video): CPU readback for any frontend that must own presentation.
    // UNRESOLVED, and NOT a port of suyu's code. Eden's only readback is
    // VideoCore::RendererBase::RequestScreenshot (src/video_core/renderer_base.h:96-99,
    // impl renderer_base.cpp:38-54), serviced by RendererVulkan::RenderScreenshot
    // (renderer_vulkan.cpp:297-313) through RenderToBuffer (renderer_vulkan.cpp:268-295).
    // Three properties make it awkward and none of them apply to suyu's synchronous
    // GetLastRenderedFrame():
    //   1. RenderScreenshot only runs inside Composite on the GPU thread
    //      (renderer_vulkan.cpp:215), so pixels land at least one guest frame after
    //      the request - retro_run cannot synchronously produce a frame.
    //   2. RequestScreenshot wraps the completion callback in a detached std::thread
    //      (renderer_base.cpp:45-48), so completion is signalled from a thread we do
    //      not own.
    //   3. It is single-slot and self-clearing: a second request while one is in
    //      flight is logged and dropped (renderer_base.cpp:41-44).
    // A correct implementation needs a double buffer plus an atomic latch and
    // must re-arm every frame. Not attempted in cut one.
    g_video_cb(nullptr, width, height, 0);
}

unsigned BaseWidth() {
    return kBaseWidth;
}

unsigned BaseHeight() {
    return kBaseHeight;
}

unsigned MaxWidth() {
    return kMaxWidth;
}

unsigned MaxHeight() {
    return kMaxHeight;
}

} // namespace LibretroCore::Video
