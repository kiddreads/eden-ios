// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#ifdef ENABLE_LIBRETRO
#include <atomic>
#endif

#include "audio_core/sink/sink_details.h"
#ifdef HAVE_CUBEB
#include "audio_core/sink/cubeb_sink.h"
#endif
#ifdef HAVE_SDL3
#include "audio_core/sink/sdl3_sink.h"
#endif
#include "audio_core/sink/null_sink.h"
#include "common/logging.h"
#include "common/settings_enums.h"

namespace AudioCore::Sink {

#ifdef ENABLE_LIBRETRO
// ---------------------------------------------------------------------------
// FRONTEND SINK OVERRIDE - libretro builds only (ENABLE_LIBRETRO, CMakeLists.txt:226;
// src/CMakeLists.txt:275-277 only adds libretro_core under it, and
// src/audio_core/CMakeLists.txt turns it into a compile definition for this file).
//
// A libretro core has to *be* the sink: every path that produces audio reaches hardware
// through a SinkStream from Sink::AcquireSinkStream, a Sink is only ever constructed by
// AudioCore::CreateSinks (audio_core.cpp:30-31) through CreateSinkFromID, and Sink has
// no way to enumerate the streams it already handed out. See the long comment at the top
// of src/libretro_core/retro_audio.cpp.
//
// The alternative - a Settings::AudioEngine::Libretro value plus a row in sink_details
// below - was rejected: it means three edits in common/settings_enums.h (the enum, the
// hand-written Canonicalizations(), and GetLast(), which hard-returns Null) and it leaks
// a "libretro" entry into the desktop Qt audio dropdown that would be silence if anyone
// picked it. A runtime pointer, set by the frontend that is actually driving, costs the
// desktop and Android builds nothing: with ENABLE_LIBRETRO off not one line below this
// point changes.
//
// DECLARATION SITE. This alias and this declaration belong in sink_details.h next to
// CreateSinkFromID. They are here because that header is outside this change's lane, and
// -Werror=missing-declarations (src/CMakeLists.txt:162) needs a declaration to precede
// the definition. src/libretro_core/retro_audio.cpp carries the matching declaration for
// the same reason. Moving the pair into the header is a redeclaration of the same
// function type in both places, so it is safe to do later without touching either .cpp.
// ---------------------------------------------------------------------------

/// Signature of a frontend-supplied sink factory. Identical to SinkDetails::FactoryFn
/// below, which cannot be named from another translation unit - SinkDetails lives in
/// this file's anonymous namespace.
using SinkFactoryFn = std::unique_ptr<Sink> (*)(std::string_view);

/// Install a frontend-supplied sink factory, overriding Settings sink selection
/// entirely. nullptr clears it and restores the normal table lookup.
///
/// Call before the first Core::System::Load: AudioCore, and with it CreateSinks, is
/// constructed inside Load at Impl::SetupForApplicationProcess (core.cpp:296), not at
/// System::Initialize.
void SetSinkOverride(SinkFactoryFn factory);
#endif

namespace {
struct SinkDetails {
    using FactoryFn = std::unique_ptr<Sink> (*)(std::string_view);
    using ListDevicesFn = std::vector<std::string> (*)(bool);
    using LatencyFn = u32 (*)(); // REINTRODUCED FROM 3833 - DIABLO 3 FIX
    // using SuitableFn = bool (*)(); // REVERTED FOR ABOVE - DIABLO 3 FIX

    /// Name for this sink.
    Settings::AudioEngine id;
    /// A method to call to construct an instance of this type of sink.
    FactoryFn factory;
    /// A method to call to list available devices.
    ListDevicesFn list_devices;
    /// Method to get the latency of this backend - REINTRODUCED FROM 3833 - DIABLO 3 FIX
    LatencyFn latency;
    /// Check whether this backend is suitable to be used.
    /// SuitableFn is_suitable; // REVERTED FOR LatencyFn latency ABOVE - DIABLO 3 FIX
};

#ifdef ENABLE_LIBRETRO
/// Written once by the frontend in retro_init and cleared in retro_deinit; read on
/// whatever thread runs Core::System::Load. Atomic rather than plain because those are
/// not guaranteed to be the same thread, and an acquire load on a path that runs twice
/// per title costs nothing.
std::atomic<SinkFactoryFn> g_sink_override{nullptr};
#endif

// sink_details is ordered in terms of desirability, with the best choice at the top.
constexpr SinkDetails sink_details[] = {
#ifdef HAVE_CUBEB
    SinkDetails{
        Settings::AudioEngine::Cubeb,
        [](std::string_view device_id) -> std::unique_ptr<Sink> {
            return std::make_unique<CubebSink>(device_id);
        },
        &ListCubebSinkDevices,
        &GetCubebLatency,
    },
#endif
#ifdef HAVE_SDL3
    SinkDetails{
        Settings::AudioEngine::Sdl3,
        [](std::string_view device_id) -> std::unique_ptr<Sink> {
            return std::make_unique<SDLSink>(device_id);
        },
        &ListSDLSinkDevices,
        &GetSDLLatency,
    },
#endif
    SinkDetails{
        Settings::AudioEngine::Null,
        [](std::string_view device_id) -> std::unique_ptr<Sink> {
            return std::make_unique<NullSink>(device_id);
        },
        [](bool capture) { return std::vector<std::string>{"null"}; },
        []() { return 0u; },
    },
};

const SinkDetails& GetOutputSinkDetails(Settings::AudioEngine sink_id) {
    const auto find_backend{[](Settings::AudioEngine id) {
        return std::find_if(std::begin(sink_details), std::end(sink_details),
                            [&id](const auto& sink_detail) { return sink_detail.id == id; });
    }};

    auto iter = find_backend(sink_id);

    if (sink_id == Settings::AudioEngine::Auto) {
        // REVERTED TO 3833 BELOW - DIABLO 3 FIX
        /*
        // Auto-select a backend. Use the sink details ordering, preferring cubeb first, checking
        // that the backend is available and suitable to use.
        for (auto& details : sink_details) {
            if (details.is_suitable()) {
                iter = &details;
                break;
            }
        }
        */ // END REVERTED CODE - DIABLO 3 FIX

        // BEGIN REINTRODUCED FROM 3833 - REPLACED CODE BLOCK ABOVE - DIABLO 3 FIX
        // Auto-select a backend. Prefer CubeB, but it may report a large minimum latency which
        // causes audio issues, in that case go with SDL.
#if defined(HAVE_CUBEB) && defined(HAVE_SDL3)
        iter = find_backend(Settings::AudioEngine::Cubeb);
        if (iter->latency() > TargetSampleCount * 3) {
        iter = find_backend(Settings::AudioEngine::Sdl3);
        }
#else
        iter = std::begin(sink_details);
#endif
        // END REINTRODUCED SECTION FROM 3833 - DIABLO 3 FIX
        LOG_INFO(Service_Audio, "Auto-selecting the {} backend",
                 Settings::CanonicalizeEnum(iter->id));
    /* BEGIN REMOVED - REVERTING BACK TO 3833, this didn't exist at all. - DIABLO 3 FIX
    } else {
        if (iter != std::end(sink_details) && !iter->is_suitable()) {
            LOG_ERROR(Service_Audio, "Selected backend {} is not suitable, falling back to null",
                      Settings::CanonicalizeEnum(iter->id));
            iter = find_backend(Settings::AudioEngine::Null);
        } */ // END REMOVED REVERT - DIABLO 3 FIX
    }

    if (iter == std::end(sink_details)) {
        LOG_ERROR(Audio, "Invalid sink_id {}", Settings::CanonicalizeEnum(sink_id));
        iter = find_backend(Settings::AudioEngine::Null);
    }

    return *iter;
}
} // Anonymous namespace

#ifdef ENABLE_LIBRETRO
void SetSinkOverride(SinkFactoryFn factory) {
    g_sink_override.store(factory, std::memory_order_release);
    if (factory != nullptr) {
        LOG_INFO(Audio_Sink, "Frontend sink override installed; Settings sink_id ignored");
    } else {
        LOG_INFO(Audio_Sink, "Frontend sink override cleared");
    }
}
#endif

std::vector<Settings::AudioEngine> GetSinkIDs() {
    // Deliberately NOT filtered by the override. Its only caller is the desktop Qt audio
    // page (yuzu/configuration/configure_audio.cpp:293), which is not built in a libretro
    // configuration, and the list is of engines the user may pick - the override is not
    // one of those, it outranks all of them.
    std::vector<Settings::AudioEngine> sink_ids(std::size(sink_details));

    std::transform(std::begin(sink_details), std::end(sink_details), std::begin(sink_ids),
                   [](const auto& sink) { return sink.id; });

    return sink_ids;
}

std::vector<std::string> GetDeviceListForSink(Settings::AudioEngine sink_id, bool capture) {
#ifdef ENABLE_LIBRETRO
    if (g_sink_override.load(std::memory_order_acquire) != nullptr) {
        // Do not enumerate a backend that cannot be selected. Falling through would run
        // ListSDLSinkDevices -> SDL_GetAudioRecordingDevices (sdl3_sink.cpp:286-307),
        // which starts SDL's audio subsystem - and on iOS that means an AVAudioSession
        // and a microphone permission prompt - purely to answer a question whose answer
        // can no longer change what CreateSinkFromID returns.
        //
        // The value is not cosmetic on the capture side: AudioIn::Manager::GetDeviceNames
        // reports a "Uac" input device to the guest if and only if this list is non-empty
        // (audio_in_manager.cpp:84-89). Empty is the truthful answer for the libretro
        // path - there is no capture implementation there, only the silence that
        // LibretroSink::FeedInputSilence hands an In stream that a game opens anyway.
        if (capture) {
            return {};
        }
        return {std::string{"libretro"}};
    }
#endif
    return GetOutputSinkDetails(sink_id).list_devices(capture);
}

std::unique_ptr<Sink> CreateSinkFromID(Settings::AudioEngine sink_id, std::string_view device_id) {
#ifdef ENABLE_LIBRETRO
    // Checked BEFORE GetOutputSinkDetails, not after: for AudioEngine::Auto that function
    // calls latency() on the cubeb row, and GetCubebLatency opens a real cubeb context.
    // The point of the override is that no host audio device is touched at all.
    if (const SinkFactoryFn factory = g_sink_override.load(std::memory_order_acquire);
        factory != nullptr) {
        return factory(device_id);
    }
#endif
    return GetOutputSinkDetails(sink_id).factory(device_id);
}

} // namespace AudioCore::Sink
