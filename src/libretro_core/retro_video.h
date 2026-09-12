// SPDX-FileCopyrightText: Copyright 2026 eden-ios contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Derived from suyu's src/libretro_core/retro_core.cpp video block
// (suyu-emu/suyu-v0.0.4 retro_core.cpp:391-415), GPL-3.0-or-later, which derives
// from yuzu (GPL-2.0-or-later).
// SPDX-FileCopyrightText: Copyright 2024 suyu Emulator Project
// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
//
// ============================================================================
// HOW A FRAME REACHES THE SCREEN
// ============================================================================
//
// There are two independent paths, and on iOS they are NOT alternatives - the
// second one is only reachable while the first one is running.
//
// 1. DIRECT-TO-LAYER (always on, this is what you actually see)
//    Eden owns a Vulkan swapchain on the app's CAMetalLayer and presents through
//    Vulkan::PresentManager on its own thread. libretro is not involved. This is
//    mandatory, not a choice: RendererVulkan's ctor calls Vulkan::CreateSurface in
//    its member-initialiser list (renderer_vulkan.cpp:143) and CreateSurface throws
//    vk::Exception(VK_ERROR_INITIALIZATION_FAILED) when no window-system branch
//    matched (vulkan_surface.cpp:109-112). WindowSystemType::Headless matches none,
//    so there is no headless Eden and never a "libretro owns the display" mode.
//
// 2. PER-FRAME CPU READBACK (optional, off by default, costs a GPU stall per frame)
//    Verified to exist and to be usable from retro_run's thread - see the block
//    comment in retro_video.cpp. Source is Tegra::GPU::GetAppletCaptureBuffer()
//    (gpu.h:225), NOT RendererBase::RequestScreenshot. Fixed 1280x720, BGRA8888,
//    Tegra block-linear, and it makes the GPU thread do a full scheduler.Finish()
//    every time it is called.
//
// When readback is off, retro_video_refresh_t is fed libretro's documented frame-dupe
// signal (NULL) rather than pixels, after checking RETRO_ENVIRONMENT_GET_CAN_DUPE.
// If the frontend cannot dupe, readback turns itself on, because a frontend that
// cannot dupe must be handed a buffer every single frame and black forever is worse
// than real pixels at a cost.
//
// Everything the frontend is told about geometry describes the frame this file
// actually delivers. The reported size and the delivered size never disagree.
// ============================================================================

#pragma once

#include "libretro.h"

namespace LibretroCore::Video {

/// Called from retro_set_environment (retro_core.cpp:403), before retro_init.
/// Negotiates the pixel format and records whether the frontend supports frame
/// duping. Safe to call more than once; the last call wins.
void Init(retro_environment_t cb);

/// Called from retro_deinit. Drops the environment callback and frees the
/// readback/blank staging buffers.
void Shutdown();

/// Called after Core::System::Load succeeds. Re-reads the "eden_video_readback"
/// core option and sizes the staging buffers.
void OnGameLoaded();

/// Called from retro_unload_game. Frees staging buffers and stops readback.
void OnGameUnloaded();

/// Called once per retro_run, after the frame wait. Delivers exactly one frame to
/// retro_video_refresh_t: real pixels if readback is active, otherwise the dupe
/// signal, otherwise a correctly sized blank buffer.
///
/// Safe to call with no game loaded - it reports the base geometry and dupes.
void Present();

/// Nominal geometry for retro_get_system_av_info (retro_core.cpp:363-366).
/// BaseWidth/BaseHeight are the readback size, so a frontend that enables readback
/// never sees a geometry change on the first frame.
unsigned BaseWidth();
unsigned BaseHeight();

/// Upper bound on anything Present() will ever pass to retro_video_refresh_t.
/// Present() clamps to these, so base <= max always holds for SET_GEOMETRY.
unsigned MaxWidth();
unsigned MaxHeight();

} // namespace LibretroCore::Video
