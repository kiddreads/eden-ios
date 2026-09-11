// SPDX-FileCopyrightText: Copyright 2026 eden-ios contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Derived from suyu's src/libretro_core/retro_core.cpp audio block
// (suyu-emu/suyu-v0.0.4 retro_core.cpp:366-389) and src/audio_core/sink/libretro_sink.h,
// GPL-3.0-or-later, which derive from yuzu (GPL-2.0-or-later).
// SPDX-FileCopyrightText: Copyright 2024 suyu Emulator Project
// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
//
// STATUS: SILENCE. This emits correctly-sized silent batches so the frontend's audio
// clock keeps running and nothing downstream sees a starved core. There is no audio.
//
// WHY IT IS NOT IMPLEMENTED IN CUT ONE
//   suyu routes audio through AudioCore::Sink::LibretroSampleQueue and
//   Settings::AudioEngine::Libretro (suyu retro_core.cpp:41, :372, :512-515, :635).
//   NEITHER EXISTS IN EDEN. Eden's sinks are cubeb, sdl3 and null only
//   (src/audio_core/sink/ contains cubeb_sink.{h,cpp}, sdl3_sink.{h,cpp}, null_sink.h,
//   sink.h, sink_details.{h,cpp}, sink_stream.{h,cpp} - no oboe, no sdl2, no libretro),
//   and Settings::AudioEngine is { Auto, Cubeb, Sdl3, Null }
//   (src/common/settings_enums.h:95). Building the bridge means editing four
//   Eden-proper files - settings_enums.h in three places (the enum at :95, the
//   hand-written EnumMetadata<AudioEngine>::Canonicalizations at :97-104, and
//   GetLast() at :115-117, which hard-returns AudioEngine::Null rather than computing
//   it), sink_details.cpp, audio_core/CMakeLists.txt - for zero progress toward
//   "it compiles", plus it leaks a selectable "libretro" entry into the desktop Qt
//   audio dropdown that would produce silence if a desktop user picked it.
//
// TODO(audio): the design that should replace this, verified against Eden's headers:
//   * A new AudioCore::Sink::LibretroSink implementing the six pure virtuals of
//     class Sink (src/audio_core/sink/sink.h:27): AcquireSinkStream, CloseStream,
//     CloseStreams, GetDeviceVolume, SetDeviceVolume, SetSystemVolume, plus a
//     std::string_view ctor, ListLibretroSinkDevices(bool) and GetLibretroLatency()
//     - Eden's SinkDetails has a LatencyFn field that upstream's does not.
//   * Its SinkStream must NOT override AppendBuffer the way suyu's does
//     (suyu libretro_sink.h:78-88). Eden's base AppendBuffer (sink_stream.cpp:25)
//     applies Settings::Volume(), downmixes 6 channels to 2, and fills the ring the
//     pull side drains; overriding it throws all three away.
//   * It MUST override Start/Stop. `paused` initialises to true (sink_stream.h:235)
//     and the base Start/Stop are empty, so a stream that does not override them
//     stays paused forever and WaitFreeSpace's predicate
//     `paused || queued_buffers < max_queue_size` is always true - no backpressure,
//     which is exactly the burstiness suyu documents at its retro_core.cpp:19-20.
//   * Pump() then pulls kFramesPerRun through the public non-virtual
//     SinkStream::ProcessAudioOutAndRender (sink_stream.h:205) - the same entry point
//     CubebSink's device callback uses - summing every non-In stream. That pull is
//     also what advances min/max_played_sample_count, without which
//     DeviceSession::IsBufferConsumed never returns true and AudioOut games stall.
//   The format needs no conversion: Eden emits interleaved host-endian s16 at
//   TargetSampleRate 48000 (audio_core/common/common.h:70) and
//   retro_audio_sample_batch_t is interleaved s16 stereo at the declared rate.

#include <array>
#include <cstring>

#include "common/common_types.h"
#include "libretro_core/retro_audio.h"

namespace LibretroCore::Audio {

namespace {
std::array<s16, kFramesPerRun * kChannels> g_silence{};
} // namespace

void Init() {
    g_silence.fill(0);
}

void Shutdown() {}

void OnGameUnloaded() {}

void Pump(retro_audio_sample_batch_t cb) {
    if (cb == nullptr) {
        return;
    }
    // g_audio_batch_cb takes FRAMES (L+R pairs), not samples. suyu's comment about
    // this (retro_core.cpp:359-360) is correct and worth preserving.
    cb(g_silence.data(), kFramesPerRun);
}

} // namespace LibretroCore::Audio
