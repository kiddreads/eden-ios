// SPDX-FileCopyrightText: Copyright 2026 eden-ios contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Derived from suyu's src/libretro_core/retro_core.cpp input block
// (suyu-emu/suyu-v0.0.4 retro_core.cpp:295-347), GPL-3.0-or-later, which derives
// from yuzu (GPL-2.0-or-later).
// SPDX-FileCopyrightText: Copyright 2024 suyu Emulator Project
// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project

#pragma once

#include <array>
#include <chrono>
#include <cstddef>

#include "libretro.h"

namespace Core {
class System;
} // namespace Core

namespace InputCommon {
class InputSubsystem;
} // namespace InputCommon

namespace LibretroCore {

/// Neither engine this class drives needs any user configuration:
/// EmulatedController::LoadDevices() calls LoadVirtualGamepadParams() unconditionally
/// (src/hid_core/frontend/emulated_controller.cpp:202 -> :283), and
/// EmulatedConsole::SetTouchParams() maps 16 "engine:touch" fingers unconditionally
/// (src/hid_core/frontend/emulated_console.cpp:39-46). Both drivers are compiled on
/// every platform (src/input_common/CMakeLists.txt) with no ANDROID or SDL guard.
class RetroInput {
public:
    static constexpr std::size_t MaxPlayers = 8;

    /// InputCommon::VirtualGamepad::VirtualButton has 20 enumerators
    /// (src/input_common/drivers/virtual_gamepad.h:15-34).
    static constexpr std::size_t NumVirtualButtons = 20;

    /// TouchScreen::MAX_FINGER_COUNT (src/input_common/drivers/touch_screen.h:52).
    static constexpr std::size_t MaxTouchPoints = 16;

    /// NpadIdTypeToIndex(NpadIdType::Handheld) == 8 (src/hid_core/hid_util.h:90-91).
    /// This is both the Settings::values.players index of the handheld controller and
    /// the VirtualGamepad port LoadVirtualGamepadParams binds it to.
    static constexpr std::size_t HandheldPort = 8;

    void SetEnvironment(retro_environment_t cb);

    void SetPollCallback(retro_input_poll_t cb) {
        poll_cb = cb;
    }
    void SetStateCallback(retro_input_state_t cb) {
        state_cb = cb;
    }

    void SetPortDevice(unsigned port, unsigned device);

    /// Call after InputSubsystem::Initialize() and after a successful
    /// Core::System::Load(), before the first retro_run.
    void OnGameLoaded(Core::System& system_, InputCommon::InputSubsystem& subsystem);

    void OnGameUnloaded();

    /// First statement of retro_run. Calls the frontend's poll callback itself.
    void Poll();

private:
    void PublishDescriptors();
    void ConnectPlayers();
    std::size_t VirtualPortFor(unsigned retro_port) const;
    void PollPad(unsigned retro_port);
    void PollTouch();
    void PollMotion();

    retro_environment_t environ_cb{};
    retro_input_poll_t poll_cb{};
    retro_input_state_t state_cb{};

    Core::System* system{};
    InputCommon::InputSubsystem* input{};
    bool loaded{false};

    retro_sensor_interface sensors{};
    bool sensors_available{false};
    std::chrono::steady_clock::time_point last_motion{};

    std::array<std::array<bool, NumVirtualButtons>, MaxPlayers> prev_buttons{};
    std::array<bool, MaxPlayers> port_enabled{};
};

RetroInput& GetRetroInput();

} // namespace LibretroCore
