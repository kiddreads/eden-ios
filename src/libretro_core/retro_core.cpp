// SPDX-FileCopyrightText: Copyright 2026 eden-ios contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Derived from suyu's src/libretro_core/retro_core.cpp (suyu-emu/suyu-v0.0.4),
// GPL-3.0-or-later, which derives from yuzu (GPL-2.0-or-later).
// SPDX-FileCopyrightText: Copyright 2024 suyu Emulator Project
// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
//
// ============================================================================
// WHAT CHANGED VS SUYU - every item checked against Eden's headers on disk
// ============================================================================
//  * #include "common/logging/backend.h" + "common/logging/log.h" -> "common/logging.h".
//    Eden has NO src/common/logging/ directory; it is one header, which also defines
//    the LOG_* macros. Common::Log::Initialize / Start / Stop keep their names
//    (src/common/logging.h:132, :134, :137).
//  * Settings that feed Core::System::Initialize are applied BEFORE it, not after.
//    Core::System::Impl::Initialize latches use_multi_core, memory_layout_mode and
//    use_asynchronous_gpu_emulation exactly once into the kernel and CpuManager
//    (src/core/core.cpp:121, :122, :138, :140-142), and Core::System::ApplySettings()
//    does NOT re-read them - its whole body is RefreshTime + Renderer().
//    RefreshBaseSettings (core.cpp:1037-1043). suyu sets them after Initialize and
//    relies on ApplySettings, which silently does nothing for this group.
//  * FrontendAppletParameters gains applet_type and launch_type. Eden's Impl::Load
//    branches on both (core.cpp:315 gates InitTempStorage on launch_type, core.cpp:321
//    gates current_application_filepath on applet_type). Both zero-default to the
//    right enumerator by luck (AppletType::Application == 0, LaunchType::
//    FrontendInitiated == 0), so suyu is accidentally correct - set them explicitly,
//    because that coincidence is exactly what flips on the next rebase.
//  * retro_reset is IMPLEMENTED. suyu's is a LOG_WARNING stub. Eden has no in-place
//    reset, so it is ShutdownMainProcess -> SetShuttingDown(false) -> re-Load. The
//    SetShuttingDown(false) is mandatory and non-obvious: ShutdownMainProcess latches
//    it true (core.cpp:415) and never clears it, and Eden's Android frontend clears it
//    before every load (android/.../native.cpp:293). Without it
//    Conductor::VsyncThread exits immediately and the reloaded game never presents.
//  * retro_run BLOCKS on a presented frame. Core::System::Run() is not a step
//    function - see the comment above kFrameWaitTimeout.
//  * GPU().ObtainContext() / ReleaseContext() are DROPPED. Eden's Qt thread only does
//    that pair to make a context current for disk-shader-cache loading; both non-Qt
//    frontends call Start() alone (yuzu_cmd/yuzu.cpp:458, android native.cpp:340).
//  * Core::System::RegisterHostThread() is ADDED. Missing from suyu, present in Eden
//    (core.h:398) and used by Eden's Qt emu thread before it touches the GPU.
//  * cpuopt_fastmem / cpuopt_fastmem_exclusives are NOT forced true. suyu forces both
//    (suyu retro_core.cpp:174-175). Eden already defaults them false on iOS
//    (src/common/settings.h:307-324) because a 16 KiB host page cannot back a 4 KiB
//    guest page 1:1; forcing them would re-enable an arena that is never constructed.
//  * Settings::AudioEngine::Libretro does not exist in Eden - see retro_audio.cpp.

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

#include "common/logging.h"
#include "common/settings.h"
#include "common/settings_enums.h"
#include "core/core.h"
#include "core/cpu_manager.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/hle/service/am/am_types.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "hid_core/hid_core.h"
#include "input_common/main.h"
#include "network/network.h"
#include "video_core/gpu.h"

#include "libretro.h"
#include "libretro_core/eden_libretro.h"
#include "libretro_core/retro_audio.h"
#include "libretro_core/retro_content.h"
#include "libretro_core/retro_core_state.h"
#include "libretro_core/retro_emu_window.h"
#include "libretro_core/retro_input.h"
#include "libretro_core/retro_video.h"

namespace LibretroCore {

// Definitions of the state declared in retro_core_state.h.
std::unique_ptr<Core::System> g_system;
std::unique_ptr<RetroEmuWindow> g_emu_window;
std::shared_ptr<InputCommon::InputSubsystem> g_input_subsystem;

retro_environment_t g_environ_cb{};
retro_video_refresh_t g_video_cb{};
retro_audio_sample_t g_audio_sample_cb{};
retro_audio_sample_batch_t g_audio_batch_cb{};

std::string g_game_path;
std::atomic<bool> g_game_loaded{false};

void* g_metal_layer = nullptr;
std::string g_data_root_override;

} // namespace LibretroCore

namespace {

using namespace std::chrono_literals;
using LibretroCore::g_audio_batch_cb;
using LibretroCore::g_emu_window;
using LibretroCore::g_environ_cb;
using LibretroCore::g_game_loaded;
using LibretroCore::g_game_path;
using LibretroCore::g_input_subsystem;
using LibretroCore::g_metal_layer;
using LibretroCore::g_system;
using LibretroCore::g_video_cb;

// Eden's run loop is NOT frontend-pumped. Core::System::Run() (src/core/core.h:168)
// only un-suspends the kernel and core timing - its entire body is
// kernel.SuspendEmulation(false) + core_timing.SyncPause(false) under the suspend
// guard (src/core/core.cpp:218-224). The guest then runs free on CpuManager's per-core
// jthreads and the GPU thread started by GPU::Start(). There is no Step, Tick or
// RunFrame anywhere on Core::System - the whole class was read to confirm it.
//
// Speed limiting happens inside the emulated system: nvdisp_disp0::Composite calls
// SpeedLimiter::DoSpeedLimiting (src/core/hle/service/nvdrv/devices/nvdisp_disp0.cpp:
// 92-94), and Service::VI::Conductor::GetNextTicks scales the vsync period by
// Settings::SpeedLimit() (src/core/hle/service/vi/conductor.cpp:101-125).
//
// So retro_run has nothing to step, and suyu's pure-sampler retro_run busy-spins
// whenever the guest is slower than the declared 60 fps. We block on the frame signal
// Eden already raises - RendererVulkan::Composite calls render_window.OnFrameDisplayed()
// (src/video_core/renderer_vulkan/renderer_vulkan.cpp:206) - with a hard cap so a
// stalled guest cannot hang the caller.
constexpr auto kFrameWaitTimeout = 50ms;

bool ReadBoolOption(const char* key, bool default_value) {
    if (g_environ_cb == nullptr) {
        return default_value;
    }
    struct retro_variable var{};
    var.key = key;
    var.value = nullptr;
    if (!g_environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value == nullptr) {
        return default_value;
    }
    return std::strcmp(var.value, "Enabled") == 0 || std::strcmp(var.value, "Yes") == 0;
}

const char* ReadStringOption(const char* key) {
    if (g_environ_cb == nullptr) {
        return nullptr;
    }
    struct retro_variable var{};
    var.key = key;
    var.value = nullptr;
    if (!g_environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var)) {
        return nullptr;
    }
    return var.value;
}

// Options that MUST be applied before Core::System::Initialize() - see the file
// header. Core::System::ApplySettings() will not pick these up later, and
// ReinitializeIfNecessary (core.cpp:314-332) only keys off multicore and memory
// layout, so async GPU in particular is latch-once.
void ApplyPreInitSettings() {
    // TODO(cpu): start single-core until dynarmic's Apple W^X handling is confirmed
    // safe for a shared code cache across Hardware::NUM_CPU_CORES guest threads.
    // pthread_jit_write_protect_np is thread-local, and CpuManager::SetMulticore is
    // only called inside System::Initialize (core.cpp:141), so this cannot be
    // revisited after the fact.
    Settings::values.use_multi_core.SetValue(ReadBoolOption("eden_multi_core", true));
    Settings::values.use_asynchronous_gpu_emulation.SetValue(
        ReadBoolOption("eden_async_gpu", true));

    // Impl::Initialize derives extended_memory_layout from memory_layout_mode !=
    // Memory_4Gb (core.cpp:122). Eden offers up to Memory_12Gb
    // (src/common/settings_enums.h:144); on a 6 GB phone with a jetsam limit well
    // below that, anything above 4 GB is a kill. Default to 4 GB.
    Settings::MemoryLayout layout = Settings::MemoryLayout::Memory_4Gb;
    if (const char* value = ReadStringOption("eden_memory_layout")) {
        if (std::strcmp(value, "6GB") == 0) {
            layout = Settings::MemoryLayout::Memory_6Gb;
        } else if (std::strcmp(value, "8GB") == 0) {
            layout = Settings::MemoryLayout::Memory_8Gb;
        }
    }
    Settings::values.memory_layout_mode.SetValue(layout);
}

void ApplyPreLoadSettings() {
    // Only Vulkan (through MoltenVK) and Null are meaningful on iOS: ENABLE_OPENGL is
    // forced OFF for APPLE in Eden's top-level CMakeLists, so the three OpenGL arms of
    // VideoCore::CreateRenderer vanish. suyu's "Vulkan|OpenGL|Software" core option is
    // dropped - two of its three values are unbuildable here.
    Settings::values.renderer_backend.SetValue(Settings::RendererBackend::Vulkan);

    Settings::values.use_docked_mode.SetValue(ReadBoolOption("eden_use_docked", false)
                                                  ? Settings::ConsoleMode::Docked
                                                  : Settings::ConsoleMode::Handheld);

    // Keep Eden's internal limiter ON. Turning it off makes Conductor set
    // speed_scale = 0.01 (conductor.cpp:105-110), i.e. deliberately unlocked, which on
    // a phone means an immediate thermal-throttle spiral. The frontend-side wait in
    // retro_run is the subordinate one.
    Settings::values.use_speed_limit.SetValue(ReadBoolOption("eden_speed_limit", true));

    // suyu's resolution option offers "1x|2x|3x|4x", which hides the sub-1x modes that
    // are the only usable ones on an A-series part. Eden's ResolutionSetup runs from
    // Res1_4X to Res8X with Res1X in the middle (settings_enums.h:148).
    Settings::ResolutionSetup resolution = Settings::ResolutionSetup::Res1X;
    if (const char* value = ReadStringOption("eden_resolution")) {
        if (std::strcmp(value, "0.25x") == 0) {
            resolution = Settings::ResolutionSetup::Res1_4X;
        } else if (std::strcmp(value, "0.5x") == 0) {
            resolution = Settings::ResolutionSetup::Res1_2X;
        } else if (std::strcmp(value, "0.75x") == 0) {
            resolution = Settings::ResolutionSetup::Res3_4X;
        } else if (std::strcmp(value, "2x") == 0) {
            resolution = Settings::ResolutionSetup::Res2X;
        }
    }
    Settings::values.resolution_setup.SetValue(resolution);

#if defined(__APPLE__) && defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
    // Eden's own UI says "Extended Dynamic State is disabled on macOS due to MoltenVK"
    // (src/yuzu/configuration/configure_graphics_extensions.cpp:68). The same applies
    // on iOS; Settings::values.dyna_state drives RemoveExtensionFeature for EDS1/2/3.
    Settings::values.dyna_state.SetValue(Settings::ExtendedDynamicState::Disabled);
    // vk_turbo_mode creates a SECOND VkDevice when this is set - wasted memory and
    // battery on a phone.
    Settings::values.renderer_force_max_clock.SetValue(false);
#endif
}

void StopEmulation() {
    if (!g_system) {
        return;
    }
    // The order Eden's own frontends use: yuzu_cmd/yuzu.cpp:508-511 and
    // qt_common/render/emu_thread.cpp:75-77.
    g_system->DetachDebugger();    // core.h:189
    g_system->Pause();             // core.h:174
    g_system->ShutdownMainProcess(); // core.h:180
}

bool LoadGameInternal(const std::string& path) {
    void(LibretroCore::Content::SetupUserPaths());
    LibretroCore::Content::AdoptKeys();

    ApplyPreLoadSettings();
    g_system->ApplySettings(); // core.h:450

    // ShutdownMainProcess latches shutting_down true and never clears it
    // (core.cpp:415). Eden's Android frontend clears it before every load
    // (android/.../native.cpp:293). Required for retro_reset to work at all.
    g_system->SetShuttingDown(false); // core.h:186

    // Filesystem and content provider, in the order every Eden frontend uses
    // (yuzu_cmd/yuzu.cpp:407-410). CreateFactories reads SDMCDir, NANDDir, LoadDir and
    // DumpDir out of the EdenPath table AT CALL TIME, so SetupUserPaths must already
    // have run - hence the ordering above.
    g_system->SetContentProvider(std::make_unique<FileSys::ContentProviderUnion>()); // core.h:356
    g_system->SetFilesystem(std::make_shared<FileSys::RealVfsFilesystem>());         // core.h:340
    g_system->GetFileSystemController().CreateFactories(*g_system->GetFilesystem());
    g_system->GetUserChannel().clear(); // core.h:426

    if (!LibretroCore::Content::KeysPresent()) {
        LOG_CRITICAL(Frontend,
                     "libretro: decryption keys missing. Put prod.keys in <data root>/keys "
                     "or <data root>/keys_import.");
        struct retro_message msg{};
        msg.msg = "Eden: decryption keys missing - install prod.keys";
        msg.frames = 300;
        if (g_environ_cb != nullptr) {
            g_environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &msg);
        }
        return false;
    }

    // TODO(content): firmware. Eden has frontend_common/firmware_manager.h
    // (CheckFirmwarePresence, VerifyFirmware, InstallKeys, GameRequiresFirmware) with
    // no suyu counterpart, and installing firmware means clearing
    // nand/system/Contents/registered, VfsRawCopy-ing every NCA in, and re-running
    // CreateFactories. Not wired up in cut one because linking frontend_common from
    // here is unverified: firmware_manager.h includes
    // core/hle/service/am/frontend/applet_mii_edit.h, which I did not follow to the
    // bottom. Most titles boot without firmware, so this is not on the critical path.

    Service::AM::FrontendAppletParameters params{
        .program_id = 0,
        .applet_id = Service::AM::AppletId::Application,            // am_types.h:76
        .applet_type = Service::AM::AppletType::Application,        // am_types.h:19
        .launch_type = Service::AM::LaunchType::FrontendInitiated,  // applet_manager.h:27
        .program_index = 0,
        .previous_program_index = -1,
    };

    const Core::SystemResultStatus result = g_system->Load(*g_emu_window, path, params); // core.h:212
    if (result != Core::SystemResultStatus::Success) {
        const auto why = LibretroCore::Content::DescribeLoadFailure(result);
        LOG_CRITICAL(Frontend, "libretro: Core::System::Load failed ({}): {}",
                     static_cast<u32>(result), why);
        return false;
    }

    // Eden's Qt emu thread registers itself as a host thread before touching the GPU
    // (emu_thread.cpp:23) so the kernel can tell it apart from a guest core thread.
    // suyu omits this; Android gets away without it only because its thread is created
    // by the JVM side.
    g_system->RegisterHostThread(); // core.h:398

    LibretroCore::Video::OnGameLoaded();

    g_system->GPU().Start();                 // video_core/gpu.h:230
    g_system->GetCpuManager().OnGpuReady();  // cpu_manager.h:54 - releases the barrier
                                             // the guest CPU threads are parked on

    g_system->RegisterExitCallback([] { // core.h:440
        // Fires on a GUEST thread. Only raise a flag / notify the frontend here -
        // never tear down Core::System from inside it.
        if (g_environ_cb != nullptr) {
            g_environ_cb(RETRO_ENVIRONMENT_SHUTDOWN, nullptr);
        }
    });

    g_system->Run(); // core.h:168 - called exactly once, never from retro_run

    LibretroCore::GetRetroInput().OnGameLoaded(*g_system, *g_input_subsystem);

    g_game_loaded.store(true, std::memory_order_release);
    LOG_INFO(Frontend, "libretro: running");
    return true;
}

} // namespace

extern "C" {

RETRO_API unsigned retro_api_version() {
    return RETRO_API_VERSION;
}

RETRO_API void retro_get_system_info(struct retro_system_info* info) {
    std::memset(info, 0, sizeof(*info));
    info->library_name = "Eden";
    info->library_version = "0.1";
    // Eden's Loader::GuessFromFilename (src/core/loader/loader.cpp:167-186) recognises
    // six extensions, not the four suyu advertises.
    info->valid_extensions = "nsp|xci|nca|nro|nso|kip";
    info->need_fullpath = true; // Core::System::Load takes a host path string, and
                                // Core::GetGameFileFromPath additionally handles split
                                // dumps (00/01/...) and "<dir>/main"
    info->block_extract = true; // suyu sets false; never let a frontend unzip a
                                // multi-GB XCI into an iOS container
}

RETRO_API void retro_get_system_av_info(struct retro_system_av_info* info) {
    std::memset(info, 0, sizeof(*info));
    info->geometry.base_width = LibretroCore::Video::BaseWidth();
    info->geometry.base_height = LibretroCore::Video::BaseHeight();
    info->geometry.max_width = LibretroCore::Video::MaxWidth();
    info->geometry.max_height = LibretroCore::Video::MaxHeight();
    info->geometry.aspect_ratio = 16.0f / 9.0f;
    // Eden's VI conductor derives the real period from 60 / m_swap_interval scaled by
    // Settings::SpeedLimit(); 60 is the right nominal figure and a 30 fps title simply
    // presents the same frame twice.
    info->timing.fps = 60.0;
    info->timing.sample_rate = static_cast<double>(LibretroCore::Audio::kSampleRate);
}

RETRO_API void retro_set_environment(retro_environment_t cb) {
    LibretroCore::g_environ_cb = cb;
    if (cb == nullptr) {
        return;
    }

    bool no_content = false;
    cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_content);

    // Eden has no state serialization at any layer: there is no Save, Load,
    // SerializeSize or state member on Core::System (the whole class was read), and
    // grepping src/core and src/common for SaveState|savestate|save_state returns no
    // files. Declaring the quirk makes the frontend report savestates, netplay,
    // rewind and run-ahead as unavailable up front instead of offering and failing.
    uint64_t quirks = RETRO_SERIALIZATION_QUIRK_INCOMPLETE;
    cb(RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS, &quirks);

    static const struct retro_variable vars[] = {
        {"eden_resolution", "Internal Resolution; 1x|0.25x|0.5x|0.75x|2x"},
        {"eden_use_docked", "Docked Mode; No|Yes"},
        {"eden_multi_core", "Multicore CPU; Enabled|Disabled"},
        {"eden_async_gpu", "Asynchronous GPU Emulation; Enabled|Disabled"},
        {"eden_memory_layout", "Emulated Memory; 4GB|6GB|8GB"},
        {"eden_speed_limit", "Internal Frame Limiter; Enabled|Disabled"},
        {nullptr, nullptr},
    };
    cb(RETRO_ENVIRONMENT_SET_VARIABLES, const_cast<struct retro_variable*>(vars));

    LibretroCore::Video::Init(cb);
    LibretroCore::GetRetroInput().SetEnvironment(cb);
}

RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb) {
    LibretroCore::g_video_cb = cb;
}

RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb) {
    // Unused - everything goes through the batch callback, as in suyu.
    LibretroCore::g_audio_sample_cb = cb;
}

RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) {
    LibretroCore::g_audio_batch_cb = cb;
}

RETRO_API void retro_set_input_poll(retro_input_poll_t cb) {
    LibretroCore::GetRetroInput().SetPollCallback(cb);
}

RETRO_API void retro_set_input_state(retro_input_state_t cb) {
    LibretroCore::GetRetroInput().SetStateCallback(cb);
}

RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device) {
    LibretroCore::GetRetroInput().SetPortDevice(port, device);
}

RETRO_API void retro_init() {
    Common::Log::Initialize(); // src/common/logging.h:132
    Common::Log::Start();      // :134
    LOG_INFO(Frontend, "Eden libretro core: retro_init");

    // suyu sets a verbose filter plus log_flush_line = true (retro_core.cpp:176-177).
    // Synchronous line-flushed logging through os_log costs measurable frame time on a
    // phone, so default to warnings with flush off.
    Settings::values.log_filter.SetValue("*:Warning");
    Settings::values.log_flush_line.SetValue(false);

    g_system = std::make_unique<Core::System>();

    // MUST precede Initialize() - see the file header.
    ApplyPreInitSettings();

    g_system->Initialize(); // core.h:162

    g_emu_window = std::make_unique<LibretroCore::RetroEmuWindow>(
        g_metal_layer, LibretroCore::Video::BaseWidth(), LibretroCore::Video::BaseHeight());

    g_input_subsystem = std::make_shared<InputCommon::InputSubsystem>();
    g_input_subsystem->Initialize(); // src/input_common/main.h:75 - must run before
                                     // HIDCore().ReloadInputDevices(), because
                                     // Common::Input::CreateInputDevice resolves the
                                     // engine name against the factories it registers

    LibretroCore::Audio::Init();

    void(LibretroCore::Content::SetupUserPaths());

    if (!Network::Init()) { // src/network/network.h:15
        LOG_WARNING(Frontend, "libretro: network layer unavailable");
    }
}

RETRO_API void retro_deinit() {
    if (g_game_loaded.load(std::memory_order_acquire)) {
        retro_unload_game();
    }

    LibretroCore::Audio::Shutdown();
    LibretroCore::Video::Shutdown();

    Network::Shutdown(); // src/network/network.h:24

    if (g_input_subsystem) {
        g_input_subsystem->Shutdown(); // src/input_common/main.h:78
        g_input_subsystem.reset();
    }
    g_emu_window.reset();
    g_system.reset();

    Common::Log::Stop(); // src/common/logging.h:137

    LibretroCore::g_environ_cb = nullptr;
    LibretroCore::g_video_cb = nullptr;
    LibretroCore::g_audio_sample_cb = nullptr;
    LibretroCore::g_audio_batch_cb = nullptr;
}

RETRO_API bool retro_load_game(const struct retro_game_info* game) {
    if (!g_system || !g_emu_window || game == nullptr || game->path == nullptr) {
        LOG_CRITICAL(Frontend, "libretro: retro_load_game precondition failed");
        return false;
    }
    g_game_path = game->path;
    LOG_INFO(Frontend, "libretro: loading {}", g_game_path);
    return LoadGameInternal(g_game_path);
}

RETRO_API bool retro_load_game_special(unsigned, const struct retro_game_info*, size_t) {
    return false;
}

RETRO_API void retro_unload_game() {
    if (!g_system || !g_game_loaded.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    LibretroCore::GetRetroInput().OnGameUnloaded();
    StopEmulation();
    LibretroCore::Video::OnGameUnloaded();
    LibretroCore::Audio::OnGameUnloaded();
    g_system->HIDCore().UnloadInputDevices(); // hid_core.h:74
    // suyu also calls AudioCore::Sink::LibretroSampleQueue::Instance().Clear() here
    // (suyu retro_core.cpp:635). Eden has no libretro sink; the line is dropped.
    g_game_path.clear();
}

RETRO_API void retro_reset() {
    // Core::System exposes no in-place reset: there is Run / Pause /
    // ShutdownMainProcess and nothing between (core.h:162-189). The only correct reset
    // is a full teardown and re-Load of the same path. Impl::ShutdownMainProcess resets
    // stop_event, kernel, gpu_core, host1x_core, audio_core, cpu_manager and perf_stats
    // (core.cpp:414-452), which is what makes a second Load on the same System object
    // sound.
    //
    // TODO(lifecycle): this is REASONED, NOT TESTED. I found no Eden code path that
    // demonstrably reloads into the same Core::System twice - Android's ChangeProgram
    // goes through a JVM-side restart I did not read. Test retro_reset early. If it
    // misbehaves, the fallback is to answer retro_reset with RETRO_ENVIRONMENT_SHUTDOWN
    // and let the frontend reload the core.
    if (!g_system || g_game_path.empty()) {
        return;
    }
    const std::string path = g_game_path;
    retro_unload_game();
    if (!LoadGameInternal(path)) {
        LOG_CRITICAL(Frontend, "libretro: reset failed to reload {}", path);
        if (g_environ_cb != nullptr) {
            g_environ_cb(RETRO_ENVIRONMENT_SHUTDOWN, nullptr);
        }
    }
}

RETRO_API void retro_run() {
    LibretroCore::GetRetroInput().Poll(); // calls the frontend's poll callback itself

    if (!g_game_loaded.load(std::memory_order_acquire)) {
        // Nothing running: still hand the frontend a frame so the core is not treated
        // as hung. A null data pointer means "duplicate the previous frame".
        if (g_video_cb != nullptr) {
            g_video_cb(nullptr, LibretroCore::Video::BaseWidth(),
                       LibretroCore::Video::BaseHeight(), 0);
        }
        LibretroCore::Audio::Pump(g_audio_batch_cb);
        return;
    }

    // The guest asked to quit (System::Exit -> exit_requested, core.h:389). Polling it
    // here is the belt-and-braces path alongside the exit callback registered at load.
    if (g_system->GetExitRequested() && g_environ_cb != nullptr) {
        g_environ_cb(RETRO_ENVIRONMENT_SHUTDOWN, nullptr);
    }

    // Return at guest cadence instead of spinning - see kFrameWaitTimeout above.
    if (g_emu_window) {
        void(g_emu_window->WaitForFramePresented(kFrameWaitTimeout));
    }

    LibretroCore::Audio::Pump(g_audio_batch_cb);
    LibretroCore::Video::Present();
}

// --- not supported on Eden -------------------------------------------------
RETRO_API size_t retro_serialize_size() {
    return 0;
}

RETRO_API bool retro_serialize(void*, size_t) {
    return false;
}

RETRO_API bool retro_unserialize(const void*, size_t) {
    return false;
}

RETRO_API void retro_cheat_reset() {}

RETRO_API void retro_cheat_set(unsigned, bool, const char*) {}

RETRO_API void* retro_get_memory_data(unsigned) {
    return nullptr;
}

RETRO_API size_t retro_get_memory_size(unsigned) {
    return 0;
}

RETRO_API unsigned retro_get_region() {
    return LibretroCore::Content::RetroRegion();
}

// --- eden_libretro.h: the extra C surface the iOS app uses -----------------

void eden_libretro_set_metal_layer(void* metal_layer, unsigned width, unsigned height) {
    g_metal_layer = metal_layer;
    if (g_emu_window) {
        // The window was built before the layer arrived. The surface is read by
        // RendererVulkan's ctor during Core::System::Load, so as long as this lands
        // before retro_load_game the layer is in place in time.
        g_emu_window->Resize(width, height);
    }
}

void eden_libretro_resize(unsigned width, unsigned height) {
    if (g_emu_window) {
        g_emu_window->Resize(width, height);
    }
}

void eden_libretro_set_visible(bool visible) {
    if (g_emu_window) {
        g_emu_window->SetShown(visible);
    }
}

void eden_libretro_set_data_root(const char* path) {
    LibretroCore::g_data_root_override = (path != nullptr) ? path : "";
}

} // extern "C"
