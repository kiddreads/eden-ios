// SPDX-FileCopyrightText: Copyright 2026 eden-ios contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Derived from suyu's src/libretro_core/retro_core.cpp (suyu-emu/suyu-v0.0.4),
// GPL-3.0-or-later, which derives from yuzu (GPL-2.0-or-later).
// SPDX-FileCopyrightText: Copyright 2024 suyu Emulator Project
// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project

#pragma once

#include <string>

#include "core/core.h"

namespace LibretroCore::Content {

/// Point every Common::FS::EdenPath at a writable root and create the directories.
/// Idempotent; safe to call before every load. Returns false if no root is known.
bool SetupUserPaths();

/// Copy prod/title/console keys out of <root>/keys_import if Eden's own key directory
/// does not have them yet, then reload the KeyManager.
void AdoptKeys();

/// !Core::Crypto::KeyManager::Instance().BaseDeriveNecessary().
bool KeysPresent();

/// RETRO_REGION_NTSC or RETRO_REGION_PAL, derived from Settings::values.region_index.
unsigned RetroRegion();

/// Human-readable form of the composite status Core::System::Load returns.
std::string DescribeLoadFailure(Core::SystemResultStatus status);

} // namespace LibretroCore::Content
