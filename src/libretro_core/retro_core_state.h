// SPDX-FileCopyrightText: Copyright 2026 eden-ios contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Derived from suyu's src/libretro_core/retro_core.cpp (suyu-emu/suyu-v0.0.4),
// GPL-3.0-or-later, which derives from yuzu (GPL-2.0-or-later).
// SPDX-FileCopyrightText: Copyright 2024 suyu Emulator Project
// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project

#pragma once

#include <atomic>
#include <memory>
#include <string>

#include "libretro.h"

namespace Core {
class System;
} // namespace Core

namespace InputCommon {
class InputSubsystem;
} // namespace InputCommon

namespace LibretroCore {

class RetroEmuWindow;

// Defined in retro_core.cpp. suyu held all of this in an anonymous namespace in one
// translation unit (suyu retro_core.cpp:70-90), which works only while the core is a
// single file. It is split here, so the state needs external linkage.
extern std::unique_ptr<Core::System> g_system;
extern std::unique_ptr<RetroEmuWindow> g_emu_window;
extern std::shared_ptr<InputCommon::InputSubsystem> g_input_subsystem;

extern retro_environment_t g_environ_cb;
extern retro_video_refresh_t g_video_cb;
extern retro_audio_sample_t g_audio_sample_cb;
extern retro_audio_sample_batch_t g_audio_batch_cb;

extern std::string g_game_path;
extern std::atomic<bool> g_game_loaded;

/// The CAMetalLayer handed over by eden_libretro_set_metal_layer(). Owned by the app.
extern void* g_metal_layer;

/// Data root handed over by eden_libretro_set_data_root(), if any.
extern std::string g_data_root_override;

} // namespace LibretroCore
