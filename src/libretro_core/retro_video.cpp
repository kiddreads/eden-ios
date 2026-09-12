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
// WHAT CHANGED VS SUYU
// ============================================================================
//   suyu's retro_run reads the frame back with renderer.IsHeadless(),
//   GetLastRenderedFrame(), GetHeadlessWidth() and GetHeadlessHeight()
//   (suyu retro_core.cpp:393-404, declared suyu renderer_base.h:51-57). Those four
//   are suyu additions to its own RendererBase and DO NOT EXIST IN EDEN:
//   `grep -rn "GetHeadless" /Volumes/EdenWork/eden/src` returns nothing, and
//   src/video_core/renderer_base.h declares no headless virtuals at all.
//   The block is not ported. The readback below is a different mechanism found in
//   Eden's own tree, not a reconstruction of suyu's.
//
// ============================================================================
// READBACK: WHAT IS ACTUALLY POSSIBLE, VERIFIED AGAINST THE TREE
// ============================================================================
//
// There are exactly two readback mechanisms in video_core. Only the second is a
// per-frame API.
//
// ---- Mechanism A: RendererBase::RequestScreenshot - REJECTED ----------------
// Declared renderer_base.h:96-99, implemented renderer_base.cpp:38-54, serviced by
// RendererVulkan::RenderScreenshot (renderer_vulkan.cpp:297-313) through
// RenderToBuffer (renderer_vulkan.cpp:268-295).
//
// It is a one-shot, self-clearing, detached-thread affair, and it cannot be driven
// per frame without changing video_core. Confirmed, with the specific reasons:
//
//   A1. Single-slot. renderer_base.cpp:41-44 drops any request made while one is in
//       flight and logs LOG_ERROR. It self-clears at renderer_vulkan.cpp:312 - AFTER
//       the completion callback fires at :311 - so a re-arm driven from the callback
//       races the clear and is dropped roughly whenever it wins that race.
//   A2. One detached std::thread per completed capture (renderer_base.cpp:45-48).
//       Per frame at 60 Hz that is 60 thread creations a second, on iOS.
//   A3. RenderToBuffer allocates a fresh VkImage, VkImageView, VkFramebuffer and a
//       download VkBuffer on every call (renderer_vulkan.cpp:271-279) and ends with
//       scheduler.Finish() (:292) - a full pipeline stall. Nothing is pooled.
//   A4. It contends with the guest. caps_su.cpp:79 - the Capture applet, i.e. the
//       player pressing the Switch's own screenshot button - uses the same single
//       slot. A core holding it permanently breaks guest screenshots outright.
//   A5. It is gated on presentation anyway. RenderScreenshot is called at
//       renderer_vulkan.cpp:215, BELOW the `if (!render_window.IsShown()) return;`
//       at :211-213. It can never be a substitute for the direct-to-layer path.
//
// A per-frame path built on it would need these video_core changes: a ring of
// pre-allocated capture images and download buffers instead of per-call creation;
// the detached thread replaced with a caller-supplied completion policy; the
// single `screenshot_requested` bool replaced with a queue or a second slot so the
// guest's Capture applet still works; and the clear moved before the callback.
// That is a real video_core redesign, so this file does not go near it.
//
// ---- Mechanism B: GPU::GetAppletCaptureBuffer - USED --------------------------
// Declared gpu.h:225, implemented gpu.cpp:469-471 -> gpu.cpp:294-303, serviced by
// RendererVulkan::GetAppletCaptureBuffer (renderer_vulkan.cpp:315-341).
//
// The previous pass on this file missed it. It is genuinely per-frame:
//
//   B1. It is fed every frame, unconditionally. RendererVulkan::Composite calls
//       RenderAppletCaptureLayer at renderer_vulkan.cpp:209, ABOVE the IsShown()
//       guard at :211-213. That function filters for LayerStackId::LastFrame
//       (renderer_vulkan.cpp:345-346), and DefaultLayerStackMask already contains
//       LayerStackBit(LayerStackId::LastFrame) (hwc_layer.h:44-46) - so an ordinary
//       game layer matches and applet_frame.image holds the last composited frame.
//   B2. It is callable from this thread. gpu.cpp:294-303 marshals the work onto the
//       GPU thread with RequestSyncOperation + TickGPU + WaitForSyncOperation and
//       blocks until it is done. There is an in-tree precedent for calling it from
//       an arbitrary frontend thread: android/app/src/main/jni/native.cpp:949-972.
//   B3. It is synchronous and returns the pixels, so no latch, no callback, no
//       double buffer, no re-arm.
//
// The code below is a straight adaptation of the Android call site, which is the
// only existing consumer of this API outside the guest's own vi service.
//
// What it costs, honestly, per frame:
//   * a 3,932,160-byte download VkBuffer allocated and freed (renderer_vulkan.cpp:
//     324-325), plus scheduler.Finish() at :333 - a full GPU pipeline stall;
//   * a CPU block-linear SWIZZLE of that buffer inside video_core
//     (renderer_vulkan.cpp:337-338), returned by value as a ~3.9 MB std::vector;
//   * our CPU UNSWIZZLE of the same data straight back to linear, below.
// The swizzle/unswizzle pair is pure waste: video_core tiles the frame only because
// its one caller, vi's shared_buffer_manager.cpp:525, wants Tegra tiling. Removing
// it needs a video_core change (a GetAppletCaptureBufferLinear(), or a variant
// taking a caller buffer and skipping SwizzleTexture). Listed, not attempted.
//
// What it cannot do, and no amount of code here changes:
//   * The size is fixed at 1280x720 by VideoCore::Capture::Layout (capture.h:29-34,
//     LinearWidth/LinearHeight at capture.h:20-21 = Layout::ScreenUndocked). It is
//     NOT the window layout and NOT affected by the internal resolution scale, so a
//     libretro frontend gets 720p no matter what the device or the scale setting is.
//     Raising it means making VideoCore::Capture::Layout non-constexpr - video_core.
//   * It still requires GPU init, which still requires a real CAMetalLayer. This is
//     a way to COPY the frame, not a way to run without a surface.
//
// Because of the stall it is off by default. It turns on when the "eden_video_readback"
// core option says so, or automatically when the frontend cannot frame-dupe.
// ============================================================================

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <vector>

#include "common/logging.h"
#include "core/core.h"
#include "core/frontend/framebuffer_layout.h"
#include "video_core/capture.h"
#include "video_core/gpu.h"
#include "video_core/textures/decoders.h"

#include "libretro_core/retro_core_state.h"
#include "libretro_core/retro_emu_window.h"
#include "libretro_core/retro_video.h"

namespace LibretroCore::Video {

namespace {

// The readback source is fixed size (capture.h:20-21), so the nominal libretro
// geometry is that size. A frontend that turns readback on therefore sees no
// geometry change on its first frame.
constexpr unsigned kBaseWidth = static_cast<unsigned>(VideoCore::Capture::LinearWidth);
constexpr unsigned kBaseHeight = static_cast<unsigned>(VideoCore::Capture::LinearHeight);

// Upper bound on anything Present() hands to retro_video_refresh_t. The value that
// matters is the window layout, which on this platform is the CAMetalLayer size in
// physical pixels (retro_emu_window.cpp:97-101 feeds Resize() straight into
// UpdateCurrentFramebufferLayout), so it is bounded by the display, not by the
// internal resolution scale - blit_swapchain downscales the scaled render target to
// the layout before presenting. 4K covers every panel iPadOS will drive.
//
// This was 7680x4320 (1920*4 x 1080*4), which reasoned about the internal render
// size rather than the layout. A frontend that preallocates max_width*max_height*4
// would have reserved 132 MB for a frame that can never be that large; at 4K it
// reserves 33 MB. If a larger display ever appears, Present() clamps and the frame
// is reported smaller than the layer - harmless while duping, and the point at which
// RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO (32) would be needed, since SET_GEOMETRY
// explicitly ignores max_width/max_height (libretro.h:1552-1553).
constexpr unsigned kMaxWidth = 3840;
constexpr unsigned kMaxHeight = 2160;

/// Core option key. NOT yet registered in retro_core.cpp's SET_VARIABLES list
/// (retro_core.cpp:392-400), so RETRO_ENVIRONMENT_GET_VARIABLE returns false or a
/// null value and readback stays off. That is the intended default; registering the
/// key is all that is needed to expose the switch.
constexpr const char* kReadbackOption = "eden_video_readback";

retro_environment_t s_environ{};

/// The format actually in effect. libretro's default is 0RGB1555 (libretro.h:858),
/// which is what we are left with if every SET_PIXEL_FORMAT is refused.
retro_pixel_format s_format = RETRO_PIXEL_FORMAT_0RGB1555;
unsigned s_bytes_per_pixel = 2;

/// RETRO_ENVIRONMENT_GET_CAN_DUPE (libretro.h:767). False means passing NULL to the
/// video callback is not allowed and every frame must carry a buffer.
bool s_can_dupe = false;

/// Set by the core option; ORed with "the frontend cannot dupe".
bool s_readback_requested = false;

/// Geometry last reported to the frontend. Seeded with what retro_get_system_av_info
/// already said (retro_core.cpp:363-366) so the first Present() is silent unless the
/// size really differs.
unsigned s_reported_width = kBaseWidth;
unsigned s_reported_height = kBaseHeight;

std::vector<u8> s_linear; ///< Unswizzled readback destination, BGRA8888.
std::vector<u8> s_blank;  ///< All-zero frame for frontends that cannot dupe.

bool s_logged_readback_unavailable = false;
bool s_logged_readback_active = false;

/// Readback output is B,G,R,A ascending (VK_FORMAT_B8G8R8A8_UNORM via
/// VideoCore::Capture::PixelFormat, capture.h:18), i.e. 0xAARRGGBB as a
/// little-endian word. libretro XRGB8888 is 0xXXRRGGBB with the top byte ignored
/// (libretro.h:5802). Same bytes, no swizzle, no alpha fixup. Any other negotiated
/// format would need a per-pixel conversion we are not going to pay for.
bool FormatAllowsReadback() {
    return s_format == RETRO_PIXEL_FORMAT_XRGB8888;
}

bool ReadbackWanted() {
    return (s_readback_requested || !s_can_dupe) && FormatAllowsReadback();
}

/// `target` is deliberately not called `fmt`: common/logging.h pulls in fmt/ranges.h,
/// and a local of that name hides the fmt namespace for the rest of the function.
bool TryNegotiateFormat(retro_environment_t cb, retro_pixel_format target, unsigned bpp) {
    // SET_PIXEL_FORMAT takes a non-const pointer through the void* environment ABI,
    // so the request needs its own mutable copy.
    retro_pixel_format request = target;
    if (!cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &request)) {
        return false;
    }
    s_format = target;
    s_bytes_per_pixel = bpp;
    return true;
}

void ReadOption() {
    s_readback_requested = false;
    if (s_environ == nullptr) {
        return;
    }
    retro_variable var{};
    var.key = kReadbackOption;
    if (!s_environ(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value == nullptr) {
        return;
    }
    s_readback_requested = std::strcmp(var.value, "Enabled") == 0 ||
                           std::strcmp(var.value, "enabled") == 0 ||
                           std::strcmp(var.value, "true") == 0 ||
                           std::strcmp(var.value, "1") == 0;
}

/// Tell the frontend the size of the frames we are about to deliver, but only when
/// it changes. SET_GEOMETRY (libretro.h:1558) is the cheap call - it does not
/// reinitialise the frontend's A/V pipeline the way SET_SYSTEM_AV_INFO does.
void ReportGeometry(unsigned width, unsigned height) {
    if (width == s_reported_width && height == s_reported_height) {
        return;
    }
    s_reported_width = width;
    s_reported_height = height;

    if (s_environ == nullptr) {
        return;
    }
    retro_game_geometry geom{};
    geom.base_width = width;
    geom.base_height = height;
    // Ignored by SET_GEOMETRY (libretro.h:1552-1553); filled so the struct is never
    // half-initialised if it is ever reused for SET_SYSTEM_AV_INFO.
    geom.max_width = kMaxWidth;
    geom.max_height = kMaxHeight;
    // Zero means "assume base_width / base_height" (libretro.h:6450-6453). That is
    // the truth in both modes: the readback frame is 1280x720 square-pixel, and the
    // direct-to-layer frame is the CAMetalLayer, also square-pixel. Hardcoding 16:9
    // here (as retro_get_system_av_info does at retro_core.cpp:367) would letterbox a
    // second time an image Eden's DefaultFrameLayout already letterboxed into a
    // non-16:9 window.
    geom.aspect_ratio = 0.0f;
    void(s_environ(RETRO_ENVIRONMENT_SET_GEOMETRY, &geom));
}

/// Pull the last composited frame out of the GPU and unswizzle it into s_linear.
/// Mirrors android/app/src/main/jni/native.cpp:949-972, the only existing frontend
/// consumer of this API. Returns false if no frame could be obtained, in which case
/// s_linear is untouched and the caller falls back to the dupe/blank path.
bool ReadbackFrame() {
    using namespace VideoCore::Capture;

    if (!g_system || !g_game_loaded.load(std::memory_order_acquire)) {
        return false;
    }
    // GPU() dereferences impl->gpu_core unconditionally (core.cpp:712-714 for
    // Renderer(); GPU() is the same shape), so it must not be called before Load.
    if (!g_system->IsPoweredOn()) {
        return false;
    }

    // Blocks until the GPU thread services the request (gpu.cpp:294-303). Returns an
    // EMPTY vector if the GPU is shutting down - WaitForSyncOperation bails on
    // shutting_down (gpu.cpp:288-291) without the operation having run - and a
    // zero-filled TiledSize vector if nothing has been composited yet
    // (renderer_vulkan.cpp:318-322). The size check catches the first case; the
    // second is legitimately a black frame and is delivered as one.
    const std::vector<u8> tiled = g_system->GPU().GetAppletCaptureBuffer();
    if (tiled.size() < TiledSize) {
        return false;
    }

    const std::size_t linear_size =
        static_cast<std::size_t>(LinearWidth) * LinearHeight * BytesPerPixel;
    if (s_linear.size() != linear_size) {
        s_linear.assign(linear_size, 0);
    }

    Tegra::Texture::UnswizzleTexture(s_linear, tiled, BytesPerPixel, LinearWidth, LinearHeight,
                                     LinearDepth, BlockHeight, BlockDepth);
    return true;
}

/// A correctly sized, correctly formatted black frame. Only reachable when the
/// frontend cannot dupe AND readback produced nothing; it is never larger than
/// kBaseWidth x kBaseHeight, so it cannot be used to allocate a 4K buffer.
const void* BlankFrame(unsigned width, unsigned height) {
    const std::size_t needed =
        static_cast<std::size_t>(width) * height * s_bytes_per_pixel;
    if (s_blank.size() != needed) {
        // Black is all-zero in every format we can negotiate: XRGB8888 0x00000000,
        // RGB565 0x0000, 0RGB1555 0x0000.
        s_blank.assign(needed, 0);
    }
    return s_blank.data();
}

} // namespace

void Init(retro_environment_t cb) {
    if (cb == nullptr) {
        return;
    }
    s_environ = cb;

    // XRGB8888 first: it is the only format the readback path can feed without a
    // per-pixel conversion, and it is what every modern frontend supports. RGB565 is
    // the documented fallback for anything that refuses it (libretro.h:5793 -
    // "New code should use RETRO_PIXEL_FORMAT_RGB565" in preference to 0RGB1555).
    if (!TryNegotiateFormat(cb, RETRO_PIXEL_FORMAT_XRGB8888, 4)) {
        LOG_WARNING(Frontend, "libretro: frontend refused XRGB8888, trying RGB565");
        if (!TryNegotiateFormat(cb, RETRO_PIXEL_FORMAT_RGB565, 2)) {
            LOG_WARNING(Frontend,
                        "libretro: frontend refused RGB565 too; staying on the default "
                        "0RGB1555. Frame readback is unavailable in this format.");
            s_format = RETRO_PIXEL_FORMAT_0RGB1555;
            s_bytes_per_pixel = 2;
        }
    }

    // Without this, passing NULL to retro_video_refresh_t is undefined - the whole
    // point of the callback (libretro.h:758-767). The previous version of this file
    // passed NULL unconditionally.
    bool can_dupe = false;
    s_can_dupe = cb(RETRO_ENVIRONMENT_GET_CAN_DUPE, &can_dupe) && can_dupe;
    if (!s_can_dupe) {
        LOG_INFO(Frontend,
                 "libretro: frontend cannot frame-dupe; enabling per-frame CPU readback "
                 "(one GPU stall per frame)");
    }

    ReadOption();
}

void Shutdown() {
    s_environ = nullptr;
    s_format = RETRO_PIXEL_FORMAT_0RGB1555;
    s_bytes_per_pixel = 2;
    s_can_dupe = false;
    s_readback_requested = false;
    s_reported_width = kBaseWidth;
    s_reported_height = kBaseHeight;
    s_linear.clear();
    s_linear.shrink_to_fit();
    s_blank.clear();
    s_blank.shrink_to_fit();
    s_logged_readback_unavailable = false;
    s_logged_readback_active = false;
}

void OnGameLoaded() {
    // Options are applied at load time, so this is where the switch is picked up.
    // Deliberately does NOT poll RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE (17): that
    // flag is a one-shot latch, and reading it here would steal the notification
    // from any future option handling in retro_core.cpp.
    ReadOption();
    s_logged_readback_unavailable = false;
    s_logged_readback_active = false;
}

void OnGameUnloaded() {
    s_linear.clear();
    s_linear.shrink_to_fit();
    s_blank.clear();
    s_blank.shrink_to_fit();
    s_logged_readback_unavailable = false;
    s_logged_readback_active = false;
}

void Present() {
    if (g_video_cb == nullptr) {
        return;
    }

    // Exactly one frame leaves this function, and the geometry reported is always the
    // geometry of that frame. Decide all three first, report, then deliver.
    const void* data = nullptr;
    unsigned width = kBaseWidth;
    unsigned height = kBaseHeight;
    std::size_t pitch = 0;

    const bool want_readback = ReadbackWanted();

    if (want_readback && ReadbackFrame()) {
        // The readback size is fixed (capture.h:20-21) and is NOT the window layout.
        // Reporting anything else would be a lie about the buffer we hand over.
        if (!s_logged_readback_active) {
            s_logged_readback_active = true;
            LOG_INFO(Frontend, "libretro: per-frame CPU readback active at {}x{}", kBaseWidth,
                     kBaseHeight);
        }
        data = s_linear.data();
        pitch = static_cast<std::size_t>(kBaseWidth) * VideoCore::Capture::BytesPerPixel;
    } else if (s_can_dupe) {
        // Direct-to-layer. Eden owns the swapchain on the app's CAMetalLayer and
        // presents through Vulkan::PresentManager on its own thread, so there are no
        // pixels for libretro to carry. NULL is libretro's documented frame-dupe
        // signal (libretro.h:5211), which keeps a frontend from treating the core as
        // hung or clearing the screen. The size still has to be right: it is what the
        // frontend sizes its window and its aspect from, and on this platform it is
        // the CAMetalLayer size in physical pixels.
        //
        // data stays null and pitch stays 0; both are ignored for a dupe.
        if (want_readback) {
            // Readback is on but this particular frame was not available (not powered
            // on yet, or shutting down). Dupe, but keep reporting the readback
            // geometry: switching to the layout size and back would make the frontend
            // resize twice for a transient miss.
            if (!s_logged_readback_unavailable) {
                s_logged_readback_unavailable = true;
                LOG_WARNING(Frontend,
                            "libretro: frame readback unavailable (no GPU frame yet, or the "
                            "GPU is shutting down); duplicating instead");
            }
        } else if (g_emu_window) {
            const Layout::FramebufferLayout& layout =
                g_emu_window->GetFramebufferLayout(); // emu_window.h:121
            if (layout.width != 0 && layout.height != 0) {
                width = std::min<unsigned>(layout.width, kMaxWidth);
                height = std::min<unsigned>(layout.height, kMaxHeight);
            }
        }
    } else {
        // Cannot dupe and could not read back - either no frame exists yet, or the
        // negotiated pixel format rules readback out. A buffer must be delivered
        // every frame, so deliver black at the base size rather than at the layer
        // size: nothing is displayed either way, and a 4K all-zero buffer would cost
        // 33 MB for no reason.
        if (!s_logged_readback_unavailable) {
            s_logged_readback_unavailable = true;
            LOG_WARNING(Frontend,
                        "libretro: frontend cannot frame-dupe and no frame is available; "
                        "delivering blank {}x{} frames", kBaseWidth, kBaseHeight);
        }
        data = BlankFrame(kBaseWidth, kBaseHeight);
        pitch = static_cast<std::size_t>(kBaseWidth) * s_bytes_per_pixel;
    }

    ReportGeometry(width, height);
    g_video_cb(data, width, height, pitch);
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
