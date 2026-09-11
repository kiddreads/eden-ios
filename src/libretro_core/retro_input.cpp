// SPDX-FileCopyrightText: Copyright 2026 eden-ios contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Derived from suyu's src/libretro_core/retro_core.cpp input block
// (suyu-emu/suyu-v0.0.4 retro_core.cpp:295-347), GPL-3.0-or-later, which derives
// from yuzu (GPL-2.0-or-later).
// SPDX-FileCopyrightText: Copyright 2024 suyu Emulator Project
// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
//
// TWO VERIFIED DEFECTS IN SUYU'S VERSION ARE FIXED HERE, NOT PORTED:
//
//  1. WRONG PORT IN HANDHELD MODE. suyu hardcodes VirtualGamepad player_index 0
//     (suyu retro_core.cpp:335, :341, :345). EmulatedController::LoadVirtualGamepadParams
//     binds each controller to port = NpadIdTypeToIndex(npad_id_type)
//     (src/hid_core/frontend/emulated_controller.cpp:284-286), and
//     NpadIdTypeToIndex(NpadIdType::Handheld) is 8 (src/hid_core/hid_util.h:90-91).
//     An undocked build is handheld, and iOS is undocked by default, so suyu's port-0
//     writes reach nothing. Eden's own Android overlay switches on exactly this
//     (android/.../overlay/InputOverlay.kt:236-239).
//
//  2. NO CONTROLLER IS EVER CONNECTED. suyu calls HIDCore().ReloadInputDevices()
//     (suyu retro_core.cpp:249) and nothing else. That fans out to
//     EmulatedController::ReloadFromSettings(), whose tail is
//     `Disconnect(); if (player.connected) Connect();`. Settings::values.players is an
//     InputSetting whose storage is value-initialised (`Type global{}`,
//     src/common/settings.h:130) and PlayerInput::connected has no default member
//     initialiser (src/common/settings_input.h:383-384), so with no config file every
//     controller is connected == false and that call actively disconnects everything
//     it just built. ConnectPlayers() fills the settings in first.

#include <algorithm>
#include <vector>

#include "common/common_types.h"
#include "common/logging.h"
#include "common/settings.h"
#include "common/settings_input.h"
#include "core/core.h"
#include "hid_core/frontend/emulated_controller.h"
#include "hid_core/hid_core.h"
#include "hid_core/hid_types.h"
#include "input_common/drivers/touch_screen.h"
#include "input_common/drivers/virtual_gamepad.h"
#include "input_common/main.h"
#include "libretro_core/retro_input.h"

namespace LibretroCore {
namespace {

using VB = InputCommon::VirtualGamepad::VirtualButton;
using VS = InputCommon::VirtualGamepad::VirtualStick;

struct PadEntry {
    unsigned retro_id;
    VB virtual_button;
    const char* desc;
};

// libretro's face buttons are POSITIONAL (B bottom, A right, Y left, X top), which is
// the Switch layout, so A->A .. Y->Y is correct and must NOT get the SNES-style
// diagonal swap. VirtualButton's own ordering (virtual_gamepad.h:15-34) differs from
// Settings::NativeButton's, so always pass the enumerator, never a raw index.
constexpr PadEntry kPadMap[] = {
    {RETRO_DEVICE_ID_JOYPAD_A, VB::ButtonA, "A"},
    {RETRO_DEVICE_ID_JOYPAD_B, VB::ButtonB, "B"},
    {RETRO_DEVICE_ID_JOYPAD_X, VB::ButtonX, "X"},
    {RETRO_DEVICE_ID_JOYPAD_Y, VB::ButtonY, "Y"},
    {RETRO_DEVICE_ID_JOYPAD_L, VB::TriggerL, "L"},
    {RETRO_DEVICE_ID_JOYPAD_R, VB::TriggerR, "R"},
    {RETRO_DEVICE_ID_JOYPAD_L2, VB::TriggerZL, "ZL"},
    {RETRO_DEVICE_ID_JOYPAD_R2, VB::TriggerZR, "ZR"},
    {RETRO_DEVICE_ID_JOYPAD_L3, VB::StickL, "Left Stick Press"},
    {RETRO_DEVICE_ID_JOYPAD_R3, VB::StickR, "Right Stick Press"},
    {RETRO_DEVICE_ID_JOYPAD_START, VB::ButtonPlus, "Plus"},
    {RETRO_DEVICE_ID_JOYPAD_SELECT, VB::ButtonMinus, "Minus"},
    {RETRO_DEVICE_ID_JOYPAD_UP, VB::ButtonUp, "D-Pad Up"},
    {RETRO_DEVICE_ID_JOYPAD_DOWN, VB::ButtonDown, "D-Pad Down"},
    {RETRO_DEVICE_ID_JOYPAD_LEFT, VB::ButtonLeft, "D-Pad Left"},
    {RETRO_DEVICE_ID_JOYPAD_RIGHT, VB::ButtonRight, "D-Pad Right"},
};

constexpr retro_controller_description kControllerTypes[] = {
    {"Switch Controller", RETRO_DEVICE_JOYPAD},
};

constexpr retro_controller_info kControllerInfo[] = {
    {kControllerTypes, 1},
    {nullptr, 0},
};

// MotionInput clamps gyro at GyroMaxValue = 5.0f and integrates it as
// `rotations += gyro * seconds`, where rotations is documented as "Number of full
// rotations in each axis" (src/hid_core/frontend/motion_input.h:25, :87). The unit is
// REVOLUTIONS per second; the "radians/s" comment at motion_input.h:93 is stale -
// 5 rad/s would be a clamp below one turn per second. libretro's sensor gyro is rad/s.
constexpr float kRadPerSecToRevPerSec = 1.0f / 6.28318530717958647692f;

// MotionInput wants G (motion_input.h:90, AccelMaxValue 7.0f); libretro's is m/s^2.
constexpr float kMetresPerSecSqToG = 1.0f / 9.80665f;

RetroInput g_retro_input;

} // namespace

RetroInput& GetRetroInput() {
    return g_retro_input;
}

void RetroInput::SetEnvironment(retro_environment_t cb) {
    environ_cb = cb;
    if (environ_cb == nullptr) {
        return;
    }

    PublishDescriptors();
    environ_cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO,
               const_cast<retro_controller_info*>(kControllerInfo));

    // Optional. Absent on most frontends; we then simply run without six-axis.
    if (environ_cb(RETRO_ENVIRONMENT_GET_SENSOR_INTERFACE, &sensors) &&
        sensors.get_sensor_input != nullptr) {
        sensors_available = true;
    }
}

void RetroInput::PublishDescriptors() {
    // libretro requires these pointers to stay valid until retro_unload_game, hence
    // function-static storage.
    static std::vector<retro_input_descriptor> descriptors;
    if (descriptors.empty()) {
        for (unsigned port = 0; port < MaxPlayers; ++port) {
            for (const auto& entry : kPadMap) {
                descriptors.push_back({port, RETRO_DEVICE_JOYPAD, 0, entry.retro_id, entry.desc});
            }
            descriptors.push_back({port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
                                   RETRO_DEVICE_ID_ANALOG_X, "Left Stick X"});
            descriptors.push_back({port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
                                   RETRO_DEVICE_ID_ANALOG_Y, "Left Stick Y"});
            descriptors.push_back({port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT,
                                   RETRO_DEVICE_ID_ANALOG_X, "Right Stick X"});
            descriptors.push_back({port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT,
                                   RETRO_DEVICE_ID_ANALOG_Y, "Right Stick Y"});
        }
        descriptors.push_back({0, 0, 0, 0, nullptr});
    }
    environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, descriptors.data());
}

void RetroInput::SetPortDevice(unsigned port, unsigned device) {
    if (port >= MaxPlayers) {
        return;
    }
    port_enabled[port] = (device != RETRO_DEVICE_NONE);
    if (loaded) {
        ConnectPlayers();
    }
}

void RetroInput::OnGameLoaded(Core::System& system_, InputCommon::InputSubsystem& subsystem) {
    system = &system_;
    input = &subsystem;
    prev_buttons = {};
    port_enabled[0] = true; // port 0 is always present

    ConnectPlayers();

    if (sensors_available && sensors.set_sensor_state != nullptr) {
        sensors.set_sensor_state(0, RETRO_SENSOR_ACCELEROMETER_ENABLE, 60);
        sensors.set_sensor_state(0, RETRO_SENSOR_GYROSCOPE_ENABLE, 60);
    }
    last_motion = std::chrono::steady_clock::now();
    loaded = true;
}

void RetroInput::ConnectPlayers() {
    if (system == nullptr) {
        return;
    }
    auto& hid = system->HIDCore(); // src/core/core.h:308

    // EmulatedController::Connect() refuses any style the controller's
    // supported_style_tag does not allow, so open everything up first. This is exactly
    // what Eden's Android frontend does at android/app/src/main/jni/native.cpp:372.
    // A running title narrows it again through SetSupportedNpadStyleSet.
    hid.SetSupportedStyleTag({Core::HID::NpadStyleSet::All}); // hid_core.h:46

    const bool handheld = !Settings::IsDockedMode(); // src/common/settings.h:1016

    // Fill the settings in BEFORE the reload - see the header comment, defect 2.
    // Settings::values.players is InputSetting<std::array<PlayerInput, 10>>
    // (src/common/settings.h:806); index 8 is the handheld controller.
    auto& players = Settings::values.players.GetValue();

    players[0].controller_type =
        handheld ? Settings::ControllerType::Handheld : Settings::ControllerType::ProController;
    players[0].connected = port_enabled[0] && !handheld;

    for (std::size_t i = 1; i < MaxPlayers; ++i) {
        players[i].controller_type = Settings::ControllerType::ProController;
        players[i].connected = port_enabled[i];
    }

    players[HandheldPort].controller_type = Settings::ControllerType::Handheld;
    players[HandheldPort].connected = port_enabled[0] && handheld;

    // EmulatedConsole maps the "touch" engine regardless of this flag
    // (emulated_console.cpp:39-46); set it anyway so the frontend state is consistent.
    Settings::values.touchscreen.enabled = true; // src/common/settings.h:870

    hid.ReloadInputDevices(); // hid_core.h:71

    // VirtualButton::ButtonHome and ButtonCapture are dropped on the floor unless
    // system buttons are enabled: EmulatedController::SetButton returns early for
    // NativeButton::Home and NativeButton::Screenshot.
    for (std::size_t i = 0; i < MaxPlayers; ++i) {
        if (auto* controller = hid.GetEmulatedControllerByIndex(i)) { // hid_core.h:37
            controller->EnableSystemButtons(); // emulated_controller.h:246
        }
    }
    if (auto* handheld_controller = hid.GetEmulatedController(Core::HID::NpadIdType::Handheld)) {
        handheld_controller->EnableSystemButtons();
    }

    LOG_INFO(Frontend, "libretro: input bridged ({}), libretro port 0 -> virtual port {}",
             handheld ? "handheld" : "docked", VirtualPortFor(0));
}

std::size_t RetroInput::VirtualPortFor(unsigned retro_port) const {
    // See the header comment, defect 1. Re-read live each call so a mid-session
    // docked/handheld change is followed without re-entering this function's caller.
    if (retro_port != 0 || system == nullptr) {
        return retro_port;
    }
    const auto* player_one = system->HIDCore().GetEmulatedController(Core::HID::NpadIdType::Player1);
    if (player_one != nullptr &&
        player_one->GetNpadStyleIndex() == Core::HID::NpadStyleIndex::Handheld) {
        return HandheldPort;
    }
    return 0;
}

void RetroInput::Poll() {
    if (poll_cb != nullptr) {
        poll_cb();
    }
    if (!loaded || state_cb == nullptr || input == nullptr || system == nullptr) {
        return;
    }
    for (unsigned port = 0; port < MaxPlayers; ++port) {
        if (port_enabled[port]) {
            PollPad(port);
        }
    }
    PollTouch();
    PollMotion();
}

void RetroInput::PollPad(unsigned retro_port) {
    auto* vgp = input->GetVirtualGamepad(); // src/input_common/main.h:123
    if (vgp == nullptr) {
        return;
    }
    const std::size_t virtual_port = VirtualPortFor(retro_port);
    auto& prev = prev_buttons[retro_port];

    for (const auto& entry : kPadMap) {
        const bool pressed = state_cb(retro_port, RETRO_DEVICE_JOYPAD, 0, entry.retro_id) != 0;
        const auto index = static_cast<std::size_t>(entry.virtual_button);
        if (pressed == prev[index]) {
            continue;
        }
        prev[index] = pressed;
        vgp->SetButtonState(virtual_port, entry.virtual_button, pressed); // virtual_gamepad.h:51
    }

    // Full-scale libretro analog is +32767, not 32768. suyu divides by 32768 so full
    // deflection never quite reaches 1.0. LoadVirtualGamepadParams sets deadzone 0.0
    // and range 1.0 (emulated_controller.cpp:326-329), so whatever we pass reaches the
    // game unmodified - clamp it ourselves.
    const auto read_axis = [this, retro_port](unsigned index, unsigned id) {
        const float raw = static_cast<float>(state_cb(retro_port, RETRO_DEVICE_ANALOG, index, id));
        return std::clamp(raw / 32767.0f, -1.0f, 1.0f);
    };

    // libretro's +Y is down; Eden's stick +Y is up.
    vgp->SetStickPosition(
        virtual_port, VS::Left, read_axis(RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X),
        -read_axis(RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y));
    vgp->SetStickPosition(
        virtual_port, VS::Right,
        read_axis(RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X),
        -read_axis(RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y));
}

void RetroInput::PollTouch() {
    auto* touch = input->GetTouchScreen(); // src/input_common/main.h:93
    if (touch == nullptr) {
        return;
    }

    // libretro gives a per-frame snapshot with no press/release edges, so use
    // TouchScreen's polled-source protocol: clear the active flags, re-assert every
    // finger that is still down, then release whatever did not come back
    // (touch_screen.h:43, :34, :46).
    touch->ClearActiveFlag();

    const auto count =
        static_cast<int>(state_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_COUNT));

    for (int i = 0; i < count && i < static_cast<int>(MaxTouchPoints); ++i) {
        const auto idx = static_cast<unsigned>(i);
        if (state_cb(0, RETRO_DEVICE_POINTER, idx, RETRO_DEVICE_ID_POINTER_PRESSED) == 0) {
            continue;
        }
        const auto raw_x = state_cb(0, RETRO_DEVICE_POINTER, idx, RETRO_DEVICE_ID_POINTER_X);
        const auto raw_y = state_cb(0, RETRO_DEVICE_POINTER, idx, RETRO_DEVICE_ID_POINTER_Y);

        // libretro pointer space is [-0x7fff, 0x7fff] across the viewport, and
        // TouchScreen::TouchMoved feeds x/y straight into SetAxis expecting [0, 1].
        // EmuWindow::MapToTouchScreen is not needed - libretro already gave us
        // viewport-relative coordinates.
        const float x = (static_cast<float>(raw_x) + 32767.0f) / 65534.0f;
        const float y = (static_cast<float>(raw_y) + 32767.0f) / 65534.0f;
        if (x < 0.0f || x > 1.0f || y < 0.0f || y > 1.0f) {
            continue; // outside the emulated screen
        }
        touch->TouchPressed(x, y, static_cast<std::size_t>(i));
    }

    touch->ReleaseInactiveTouch();
}

void RetroInput::PollMotion() {
    if (!sensors_available) {
        return;
    }
    auto* vgp = input->GetVirtualGamepad();
    if (vgp == nullptr) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto delta_us =
        std::chrono::duration_cast<std::chrono::microseconds>(now - last_motion).count();
    last_motion = now;

    // MotionInput::UpdateRotation ignores any sample period over 0.1 s.
    if (delta_us <= 0 || delta_us > 100000) {
        return;
    }
    const auto delta = static_cast<u64>(delta_us);

    const float gyro_x = sensors.get_sensor_input(0, RETRO_SENSOR_GYROSCOPE_X) * kRadPerSecToRevPerSec;
    const float gyro_y = sensors.get_sensor_input(0, RETRO_SENSOR_GYROSCOPE_Y) * kRadPerSecToRevPerSec;
    const float gyro_z = sensors.get_sensor_input(0, RETRO_SENSOR_GYROSCOPE_Z) * kRadPerSecToRevPerSec;
    const float accel_x =
        sensors.get_sensor_input(0, RETRO_SENSOR_ACCELEROMETER_X) * kMetresPerSecSqToG;
    const float accel_y =
        sensors.get_sensor_input(0, RETRO_SENSOR_ACCELEROMETER_Y) * kMetresPerSecSqToG;
    const float accel_z =
        sensors.get_sensor_input(0, RETRO_SENSOR_ACCELEROMETER_Z) * kMetresPerSecSqToG;

    // LoadVirtualGamepadParams maps both MotionLeft and MotionRight to motion:0
    // (emulated_controller.cpp:334-335), so one call drives both Joy-Cons.
    const std::size_t pad_port = VirtualPortFor(0);
    vgp->SetMotionState(pad_port, delta, gyro_x, gyro_y, gyro_z, accel_x, accel_y, accel_z);

    // Console six-axis. EmulatedConsole::ReloadInput() hardcodes
    // "engine:virtual_gamepad,port:8,motion:0" (emulated_console.cpp:81) - port 8 is a
    // literal in Eden's source, not a setting.
    if (pad_port != HandheldPort) {
        vgp->SetMotionState(HandheldPort, delta, gyro_x, gyro_y, gyro_z, accel_x, accel_y, accel_z);
    }

    // TODO(input): axis orientation between the sensor frame and the Joy-Con frame is
    // UNVERIFIED. The units above are read out of motion_input.h; no axis convention is
    // documented anywhere in Eden's source. Expect to determine the sign and axis
    // permutation empirically against a title with gyro aiming.
}

void RetroInput::OnGameUnloaded() {
    if (input != nullptr) {
        if (auto* vgp = input->GetVirtualGamepad()) {
            vgp->ResetControllers(); // virtual_gamepad.h:76
        }
        if (auto* touch = input->GetTouchScreen()) {
            touch->ReleaseAllTouch(); // touch_screen.h:49
        }
    }
    if (sensors_available && sensors.set_sensor_state != nullptr) {
        sensors.set_sensor_state(0, RETRO_SENSOR_ACCELEROMETER_DISABLE, 0);
        sensors.set_sensor_state(0, RETRO_SENSOR_GYROSCOPE_DISABLE, 0);
    }
    // suyu never clears its button state on unload, so stale presses survive into the
    // next load. It also uses one shared array for all ports.
    prev_buttons = {};
    loaded = false;
    system = nullptr;
    input = nullptr;
}

} // namespace LibretroCore
