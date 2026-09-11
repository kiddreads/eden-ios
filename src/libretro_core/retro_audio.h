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

/// AudioCore::TargetSampleRate (src/audio_core/common/common.h:70).
constexpr unsigned kSampleRate = 48000;

/// One 60 Hz frame of stereo audio. retro_get_system_av_info declares
/// timing.fps = 60.0 and timing.sample_rate = 48000.0, and a libretro frontend paces
/// retro_run against those, so it expects roughly sample_rate/fps frames per call.
constexpr std::size_t kFramesPerRun = kSampleRate / 60;
constexpr std::size_t kChannels = 2;

void Init();
void Shutdown();
void OnGameUnloaded();

/// Called once per retro_run. Emits exactly kFramesPerRun stereo frames.
void Pump(retro_audio_sample_batch_t cb);

} // namespace LibretroCore::Audio
