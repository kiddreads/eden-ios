// SPDX-FileCopyrightText: Copyright 2026 eden-ios contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Derived from suyu's src/libretro_core/retro_core.cpp audio block
// (suyu-emu/suyu-v0.0.4 retro_core.cpp:366-389) and src/audio_core/sink/libretro_sink.h,
// GPL-3.0-or-later, which derive from yuzu (GPL-2.0-or-later).
// SPDX-FileCopyrightText: Copyright 2024 suyu Emulator Project
// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project

#pragma once

#include <cstddef>

#include "libretro.h"

namespace LibretroCore::Audio {

/// AudioCore::TargetSampleRate (src/audio_core/common/common.h:70). retro_audio.cpp
/// static_asserts these two against each other - there is no resampler in this path.
constexpr unsigned kSampleRate = 48000;

/// One 60 Hz frame of stereo audio. retro_get_system_av_info declares
/// timing.fps = 60.0 and timing.sample_rate = 48000.0, and a libretro frontend paces
/// retro_run against those, so it expects roughly sample_rate/fps frames per call.
constexpr std::size_t kFramesPerRun = kSampleRate / 60;

/// Eden's sinks all report device_channels = 2 (sdl3_sink.cpp:238), the base
/// SinkStream::AppendBuffer downmixes 5.1 to that (sink_stream.cpp:37-61), and
/// retro_audio_sample_batch_t is interleaved stereo (libretro.h:7655-7670). Two, everywhere.
constexpr std::size_t kChannels = 2;

/// Installs the libretro audio sink factory into audio_core. Call once, before the
/// first Core::System::Load - AudioCore (and therefore the sink) is constructed inside
/// Load, at Impl::SetupForApplicationProcess (core.cpp:296), not at System::Initialize.
void Init();

/// Removes the factory and releases the staging buffers. Call at retro_deinit.
void Shutdown();

/// Drops any audio still staged from the title that just went away.
void OnGameUnloaded();

/// Called once per retro_run, on the thread that runs retro_run and no other.
/// Always emits exactly kFramesPerRun stereo frames, and always drains Eden even when
/// `cb` is null - the ADSP blocks on this drain (see retro_audio.cpp, "Overrun").
void Pump(retro_audio_sample_batch_t cb);

/// True once Eden has actually constructed a libretro sink, i.e. audio is coming from
/// the emulator rather than from the silence fallback. False before the first load,
/// after unload, and in a build without the audio_core hook.
bool IsLive();

} // namespace LibretroCore::Audio
