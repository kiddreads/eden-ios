// SPDX-FileCopyrightText: Copyright 2026 eden-ios contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Derived from suyu's src/libretro_core/retro_core.cpp video block
// (suyu-emu/suyu-v0.0.4 retro_core.cpp:391-415), GPL-3.0-or-later, which derives
// from yuzu (GPL-2.0-or-later).
// SPDX-FileCopyrightText: Copyright 2024 suyu Emulator Project
// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project

#pragma once

#include "libretro.h"

namespace LibretroCore::Video {

/// From retro_set_environment. Negotiates the pixel format.
void Init(retro_environment_t cb);

void Shutdown();

void OnGameLoaded();
void OnGameUnloaded();

/// Called once per retro_run, after the frame wait.
void Present();

unsigned BaseWidth();
unsigned BaseHeight();
unsigned MaxWidth();
unsigned MaxHeight();

} // namespace LibretroCore::Video
