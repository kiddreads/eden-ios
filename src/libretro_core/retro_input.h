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
#include <limits>

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
///
/// ---------------------------------------------------------------------------
/// WHAT THE SWITCH HAS THAT A LIBRETRO PAD DOES NOT
/// ---------------------------------------------------------------------------
/// Carried here rather than in the frontend because these are the gaps the *core*
/// side can see, and three of the four cannot be closed from Swift alone.
///
///  - TOUCHSCREEN. Not on RETRO_DEVICE_JOYPAD at all; it arrives on
///    RETRO_DEVICE_POINTER, which is a separate device the frontend has to report.
///    PollTouch() below handles up to MaxTouchPoints fingers. See the pointer-index
///    contract on that function - it is a real requirement on the frontend, not a
///    formality.
///  - MOTION (six-axis). Not in the libretro pad ABI either; the only route is the
///    OPTIONAL RETRO_ENVIRONMENT_GET_SENSOR_INTERFACE, and a frontend that does not
///    implement it silently has no gyro. `sensors_available` records which case we
///    are in. A Switch game with gyro aiming is unplayable without it.
///  - THE SECOND STICK exists in the libretro ABI (RETRO_DEVICE_ANALOG index
///    RETRO_DEVICE_INDEX_ANALOG_RIGHT) and is wired up in PollPad(). This one is fine.
///  - HOME AND CAPTURE. VirtualGamepad has ButtonHome and ButtonCapture
///    (virtual_gamepad.h:34-35) and ConnectPlayers() calls EnableSystemButtons() so
///    they would be accepted - but RETRO_DEVICE_ID_JOYPAD_* only runs 0..15
///    (libretro.h:320-371) and all sixteen ids are already spoken for by kPadMap.
///    There is no spare libretro button id, so these two are unreachable until a
///    frontend-specific path is added.
///  - HANDHELD vs DOCKED decides which virtual port the pad has to be written to
///    (0 vs 8), and the guest can change it underneath us at runtime -
///    NPad::UpdateControllerAt calls EmulatedController::SetNpadStyleIndex
///    (src/hid_core/resources/npad/npad.cpp:815). VirtualPortFor() re-reads it every
///    poll and PollPad() hands the binding over cleanly; see `bound_port`.
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

    /// "No virtual port is currently bound to this libretro port."
    static constexpr std::size_t UnboundPort = std::numeric_limits<std::size_t>::max();

    /// Finger ids handed to TouchScreen are slot + 1, never 0, and that is LOAD
    /// BEARING. TouchScreen::fingers is value-initialised, so every unused slot holds
    /// finger_id == 0 (touch_screen.h:54-58, :64). ReleaseInactiveTouch() walks EVERY
    /// slot including the unused ones and calls TouchReleased(finger.finger_id)
    /// (touch_screen.cpp:91-97), and TouchReleased looks that id up among the
    /// *enabled* fingers (touch_screen.cpp:51-52 -> :63-74). So a live finger whose id
    /// is 0 is found by the lookup performed on behalf of an unused slot and released
    /// on the same call that pressed it. Biasing by one makes every id we ever pass
    /// non-zero, so those lookups find nothing and the stale release is inert.
    static constexpr std::size_t FingerIdBias = 1;

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

    /// Drives every button and both sticks of one VirtualGamepad port to neutral.
    /// The per-port equivalent of VirtualGamepad::ResetControllers(), which clears all
    /// ten ports (virtual_gamepad.cpp:58-84) and would therefore also clear ports that
    /// are still in use.
    void ReleaseVirtualPort(std::size_t virtual_port);

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

    /// The VirtualGamepad port each libretro port is currently writing to, so that a
    /// change of binding can be noticed and the port it is leaving can be released.
    /// Filled with UnboundPort by OnGameLoaded / OnGameUnloaded - the value-initialised
    /// 0 would be a legitimate port number and would suppress the first hand-over.
    std::array<std::size_t, MaxPlayers> bound_port{};
};

RetroInput& GetRetroInput();

} // namespace LibretroCore
