// SPDX-FileCopyrightText: Copyright 2026 eden-ios contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// See EdenCoreBridge.h for the ordering finding that shapes this file.
//
// Objective-C, NOT Objective-C++, and that is deliberate. eden_libretro.h is wrapped
// in extern "C" and includes <stdbool.h>; libretro.h is plain C. Neither drags in a
// single Eden C++ header, so this target needs no C++ dialect setting, no prefix
// header and no replication of CMake's -D flags - the whole tangle
// cemu-ios-muffin/src/ios/project.yml spends 40 lines of comments on. Keep it that
// way: see the note in src/ios/project.yml.

#import <Foundation/Foundation.h>

#include <errno.h>
#include <limits.h>      // PATH_MAX
#include <math.h>
#include <os/log.h>
#include <pthread.h>
#include <stdarg.h>      // va_list, used by EdenSetStatus
#include <stdatomic.h>
#include <stdio.h>       // vsnprintf
#include <stdlib.h>
#include <string.h>      // strlcpy (Darwin)
#include <sys/qos.h>     // QOS_CLASS_USER_INTERACTIVE
#include <time.h>

#include "libretro_core/eden_libretro.h"
#include "libretro_core/libretro.h"

#import "EdenCoreBridge.h"

// ---------------------------------------------------------------------------
// logging
// ---------------------------------------------------------------------------

static os_log_t EdenLog(void) {
    static os_log_t log;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        log = os_log_create("dev.edenios.Eden", "bridge");
    });
    return log;
}

#define EDEN_LOG(fmt, ...)  os_log(EdenLog(), fmt, ##__VA_ARGS__)
#define EDEN_ERR(fmt, ...)  os_log_error(EdenLog(), fmt, ##__VA_ARGS__)

// ---------------------------------------------------------------------------
// shared state
// ---------------------------------------------------------------------------

#define EDEN_STATUS_MAX 512
#define EDEN_MAX_OPTIONS 32

typedef struct {
    char key[64];
    char value[64];   // the DEFAULT, i.e. the first alternative after ';'
} EdenOption;

static struct {
    // --- surface, written on the main thread only -------------------------
    void *layer;
    _Atomic unsigned layer_w;
    _Atomic unsigned layer_h;

    // --- lifecycle --------------------------------------------------------
    _Atomic int state;             // EdenBridgeState
    _Atomic bool should_run;       // cleared by eden_bridge_stop
    _Atomic bool visible;
    _Atomic bool shutdown_requested;  // RETRO_ENVIRONMENT_SHUTDOWN from the core
    pthread_t thread;
    bool thread_valid;

    // Parking the run loop while backgrounded. Not a pause of the emulated system -
    // see the note in the run loop.
    pthread_mutex_t gate_mutex;
    pthread_cond_t gate_cond;

    // --- session parameters, owned by the bridge --------------------------
    char rom_path[PATH_MAX];
    char data_root[PATH_MAX];

    // --- status -----------------------------------------------------------
    pthread_mutex_t status_mutex;
    char status[EDEN_STATUS_MAX];

    // --- core options, learned from RETRO_ENVIRONMENT_SET_VARIABLES -------
    EdenOption options[EDEN_MAX_OPTIONS];
    int option_count;

    // --- stats ------------------------------------------------------------
    _Atomic unsigned long long iterations;
    _Atomic double ips;

    // --- input producer side (any thread) ---------------------------------
    _Atomic unsigned button_mask;
    _Atomic int stick[2][2];       // [index][axis], libretro scale (+-32767)
    _Atomic bool touch_pressed;
    _Atomic int touch_x;           // libretro pointer space
    _Atomic int touch_y;

    // --- input consumer side (emulation thread only, no synchronisation) --
    unsigned snap_mask;
    int snap_stick[2][2];
    bool snap_touch_pressed;
    int snap_touch_x;
    int snap_touch_y;
} g;

static void EdenSetStatus(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void EdenSetStatus(const char *fmt, ...) {
    char tmp[EDEN_STATUS_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);

    pthread_mutex_lock(&g.status_mutex);
    strlcpy(g.status, tmp, sizeof(g.status));
    pthread_mutex_unlock(&g.status_mutex);
    EDEN_LOG("%{public}s", tmp);
}

static void EdenBridgeInitOnce(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        pthread_mutex_init(&g.status_mutex, NULL);
        pthread_mutex_init(&g.gate_mutex, NULL);
        pthread_cond_init(&g.gate_cond, NULL);
        atomic_store(&g.state, EdenBridgeStateIdle);
        atomic_store(&g.visible, true);
        strlcpy(g.status, "idle", sizeof(g.status));
    });
}

// ---------------------------------------------------------------------------
// core options
//
// The core reads six options through RETRO_ENVIRONMENT_GET_VARIABLE, and it reads
// them EARLY: ApplyPreInitSettings runs inside retro_init before
// Core::System::Initialize (retro_core.cpp:441-443), and Core::System::Impl::Initialize
// latches use_multi_core, memory_layout_mode and use_asynchronous_gpu_emulation exactly
// once - ApplySettings() does not re-read them (retro_core.cpp:16-22). Answering these
// late or wrongly silently changes the emulated machine with no error anywhere.
//
// The value strings the core compares against are literal: "Enabled"/"Yes" for bools
// (retro_core.cpp:144), "6GB"/"8GB" for memory, "0.25x".."2x" for resolution.
// ---------------------------------------------------------------------------

/// Parse "Internal Resolution; 1x|0.25x|0.5x|2x" -> "1x". libretro's convention is
/// that the first alternative after the ';' is the default.
static void EdenParseOptionDefault(const char *decl, char *out, size_t out_len) {
    out[0] = '\0';
    if (decl == NULL) {
        return;
    }
    const char *semi = strchr(decl, ';');
    if (semi == NULL) {
        return;
    }
    const char *p = semi + 1;
    while (*p == ' ') {
        ++p;
    }
    const char *bar = strchr(p, '|');
    size_t n = (bar != NULL) ? (size_t)(bar - p) : strlen(p);
    if (n >= out_len) {
        n = out_len - 1;
    }
    memcpy(out, p, n);
    out[n] = '\0';
}

static const char *EdenLookupOption(const char *key) {
    if (key == NULL) {
        return NULL;
    }
    for (int i = 0; i < g.option_count; ++i) {
        if (strcmp(g.options[i].key, key) == 0) {
            return g.options[i].value;
        }
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// libretro callbacks
// ---------------------------------------------------------------------------

static bool EdenEnvironmentCB(unsigned cmd, void *data) {
    switch (cmd) {

    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
        // retro_video.cpp:54-58 negotiates XRGB8888 and only warns on refusal. Accept
        // it: a frontend that cannot take the format has no way to show a readback
        // frame either, and readback is not implemented (retro_video.cpp:90-106).
        const enum retro_pixel_format *fmt = (const enum retro_pixel_format *)data;
        return fmt != NULL && *fmt == RETRO_PIXEL_FORMAT_XRGB8888;
    }

    case RETRO_ENVIRONMENT_SET_VARIABLES: {
        const struct retro_variable *vars = (const struct retro_variable *)data;
        g.option_count = 0;
        for (; vars != NULL && vars->key != NULL && g.option_count < EDEN_MAX_OPTIONS; ++vars) {
            EdenOption *opt = &g.options[g.option_count];
            strlcpy(opt->key, vars->key, sizeof(opt->key));
            EdenParseOptionDefault(vars->value, opt->value, sizeof(opt->value));
            ++g.option_count;
        }
        EDEN_LOG("core declared %d options", g.option_count);
        return true;
    }

    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        struct retro_variable *var = (struct retro_variable *)data;
        if (var == NULL) {
            return false;
        }
        var->value = EdenLookupOption(var->key);
        // Returning false makes the core fall back to the default it passed to
        // ReadBoolOption / ReadStringOption, which is the same answer. Both paths are
        // safe; this one is just honest about whether we knew the key.
        return var->value != NULL;
    }

    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: {
        bool *updated = (bool *)data;
        if (updated != NULL) {
            *updated = false;  // options are fixed for the life of a session
        }
        return true;
    }

    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY: {
        // Only a fallback. ResolveRoot (retro_content.cpp:47-58) prefers
        // g_data_root_override, which eden_bridge_start always sets before retro_init,
        // so this branch is never what decides the tree. If it ever were, note that
        // ResolveRoot appends "/eden" to whatever is returned here - i.e. it would
        // silently pick a DIFFERENT directory from the one the UI tells the user
        // about. Returning the same root keeps the divergence to one level rather
        // than to a different volume.
        const char **dir = (const char **)data;
        if (dir == NULL || g.data_root[0] == '\0') {
            return false;
        }
        *dir = g.data_root;
        return true;
    }

    case RETRO_ENVIRONMENT_SET_MESSAGE: {
        // This is how "decryption keys missing" reaches the UI - retro_core.cpp:270-275
        // raises it from LoadGameInternal when Content::KeysPresent() is false.
        const struct retro_message *msg = (const struct retro_message *)data;
        if (msg != NULL && msg->msg != NULL) {
            EdenSetStatus("%s", msg->msg);
        }
        return true;
    }

    case RETRO_ENVIRONMENT_SHUTDOWN: {
        // CAREFUL: retro_core.cpp:317-323 registers this as a Core::System exit
        // callback, whose own comment says "Fires on a GUEST thread. Only raise a flag
        // / notify the frontend here - never tear down Core::System from inside it."
        // So this sets a flag and nothing else. The run loop notices and unwinds on
        // the emulation thread, which is the only thread allowed to.
        atomic_store(&g.shutdown_requested, true);
        atomic_store(&g.should_run, false);
        return true;
    }

    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME: {
        // The core offers it (retro_core.cpp:376-377); this frontend always has a ROM.
        return true;
    }

    case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS: {
        // Eden has no state serialization at any layer (retro_core.cpp:382-385).
        // Acknowledging the quirk is how the frontend knows not to offer savestates.
        return true;
    }

    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS: {
        // Accepted and ignored. These describe the pad to a configurable frontend; the
        // on-screen controls here are fixed, and retro_input.cpp publishes them
        // unconditionally (retro_input.cpp:115-117, :146).
        return true;
    }

    case RETRO_ENVIRONMENT_GET_SENSOR_INTERFACE: {
        // NOT IMPLEMENTED IN CUT ONE. Returning false is handled: RetroInput::
        // SetEnvironment only sets sensors_available when this succeeds AND
        // get_sensor_input is non-null (retro_input.cpp:120-123), and PollMotion
        // returns immediately when it is false (retro_input.cpp:334-336). The cost is
        // no gyro, which affects a handful of titles.
        //
        // Wiring it up means a CMMotionManager plus resolving two unknowns
        // retro_input.cpp:375-378 already flags: the axis permutation between the
        // sensor frame and the Joy-Con frame, and the sign convention (libretro
        // documents accelerometer at rest as (0,0,1); CoreMotion reports a face-up
        // device as z = -1.0). Both need a device and a gyro-aiming title to settle,
        // so guessing them here would be inventing confidence.
        return false;
    }

    default:
        return false;
    }
}

static void EdenVideoRefreshCB(const void *data, unsigned width, unsigned height, size_t pitch) {
    (void)data;
    (void)width;
    (void)height;
    (void)pitch;
    // `data` is ALWAYS NULL. retro_video.cpp:107 calls g_video_cb(nullptr, w, h, 0),
    // libretro's documented "duplicate the previous frame" signal, because Eden owns
    // the swapchain on the app's CAMetalLayer and presents through Vulkan::
    // PresentManager on its own thread. There is nothing here to draw and this must
    // never try to.
    atomic_fetch_add(&g.iterations, 1ULL);
}

static size_t EdenAudioSampleBatchCB(const int16_t *data, size_t frames) {
    (void)data;
    // Consume and claim all of it. retro_audio.cpp is SILENCE by design and says so in
    // its header: Eden has no AudioCore::Sink::LibretroSink and Settings::AudioEngine
    // has no Libretro member, so Pump() emits kFramesPerRun (800) zeroed stereo frames
    // per run purely to keep the frontend's audio clock running.
    //
    // Opening an AudioQueue here would play silence at some power cost and prove
    // nothing, so there is no audio output path in cut one. When a real sink lands
    // (the design is spelled out at retro_audio.cpp:28-50), this is where it attaches.
    return frames;
}

static void EdenAudioSampleCB(int16_t left, int16_t right) {
    (void)left;
    (void)right;
    // Installed but never called: retro_core.cpp:406-409 stores the pointer and
    // documents that everything goes through the batch callback.
}

static void EdenInputPollCB(void) {
    // Latch the producer state into an emulation-thread-private snapshot. This is the
    // entire reason libretro separates poll from state: UIKit touches and GCController
    // events arrive on the main thread, and RetroInput::PollPad then calls state_cb
    // sixteen times per port (retro_input.cpp:266-274) expecting a consistent frame.
    g.snap_mask = atomic_load(&g.button_mask);
    for (int i = 0; i < 2; ++i) {
        g.snap_stick[i][0] = atomic_load(&g.stick[i][0]);
        g.snap_stick[i][1] = atomic_load(&g.stick[i][1]);
    }
    g.snap_touch_pressed = atomic_load(&g.touch_pressed);
    g.snap_touch_x = atomic_load(&g.touch_x);
    g.snap_touch_y = atomic_load(&g.touch_y);
}

static int16_t EdenInputStateCB(unsigned port, unsigned device, unsigned index, unsigned id) {
    if (port != 0) {
        return 0;   // one player in cut one
    }

    switch (device) {
    case RETRO_DEVICE_JOYPAD:
        if (id == RETRO_DEVICE_ID_JOYPAD_MASK) {
            return (int16_t)(g.snap_mask & 0xFFFF);
        }
        if (id < 16) {
            return (g.snap_mask & (1u << id)) ? 1 : 0;
        }
        return 0;

    case RETRO_DEVICE_ANALOG:
        if (index < 2 && id < 2) {
            return (int16_t)g.snap_stick[index][id];
        }
        return 0;

    case RETRO_DEVICE_POINTER:
        // retro_input.cpp:307-330 asks for COUNT first, then per-index PRESSED/X/Y.
        // One touch point in cut one.
        switch (id) {
        case RETRO_DEVICE_ID_POINTER_COUNT:
            return g.snap_touch_pressed ? 1 : 0;
        case RETRO_DEVICE_ID_POINTER_PRESSED:
            return (index == 0 && g.snap_touch_pressed) ? 1 : 0;
        case RETRO_DEVICE_ID_POINTER_X:
            return (index == 0) ? (int16_t)g.snap_touch_x : 0;
        case RETRO_DEVICE_ID_POINTER_Y:
            return (index == 0) ? (int16_t)g.snap_touch_y : 0;
        default:
            return 0;
        }

    default:
        return 0;
    }
}

// ---------------------------------------------------------------------------
// the emulation thread
// ---------------------------------------------------------------------------

static double EdenNowSeconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void EdenWaitWhileHidden(void) {
    pthread_mutex_lock(&g.gate_mutex);
    while (!atomic_load(&g.visible) && atomic_load(&g.should_run)) {
        pthread_cond_wait(&g.gate_cond, &g.gate_mutex);
    }
    pthread_mutex_unlock(&g.gate_mutex);
}

static void *EdenEmulationThread(void *arg) {
    (void)arg;
    pthread_setname_np("eden.emu");

    atomic_store(&g.state, EdenBridgeStateStarting);

    // === THE ORDER. Every step justified in EdenCoreBridge.h. ===============

    // 1. Callbacks first: retro_set_environment must precede retro_init because
    //    retro_init reads core options through it (ApplyPreInitSettings,
    //    retro_core.cpp:441), and retro_set_environment itself calls
    //    Video::Init(cb) and RetroInput::SetEnvironment(cb) (retro_core.cpp:398-399).
    retro_set_environment(EdenEnvironmentCB);
    retro_set_video_refresh(EdenVideoRefreshCB);
    retro_set_audio_sample(EdenAudioSampleCB);
    retro_set_audio_sample_batch(EdenAudioSampleBatchCB);
    retro_set_input_poll(EdenInputPollCB);
    retro_set_input_state(EdenInputStateCB);

    // 2. Data root BEFORE retro_init. retro_init's tail calls SetupUserPaths
    //    (retro_core.cpp:456), which latches g_paths_ready (retro_content.cpp:40)
    //    and returns early forever after.
    eden_libretro_set_data_root(g.data_root);

    // 3. Metal layer BEFORE retro_init. retro_init constructs RetroEmuWindow with
    //    g_metal_layer (retro_core.cpp:445-446) and the ctor COPIES it into
    //    window_info.render_surface (retro_emu_window.cpp:33). Setting it afterwards
    //    only calls Resize(), which never re-assigns render_surface - and a null
    //    render_surface means CreateSurface throws VK_ERROR_INITIALIZATION_FAILED
    //    out of RendererVulkan's member-initialiser list during Load.
    {
        const unsigned w = atomic_load(&g.layer_w);
        const unsigned h = atomic_load(&g.layer_h);
        eden_libretro_set_metal_layer(g.layer, w, h);
        EDEN_LOG("layer %p attached at %ux%u (physical px) before retro_init", g.layer, w, h);
    }

    // 4. Now the core may initialise.
    retro_init();

    // 5. Port device. SetPortDevice guards on `loaded` and only takes effect through
    //    ConnectPlayers once a game is up (retro_input.cpp:149-157), so before
    //    retro_load_game is the right time to declare it.
    retro_set_controller_port_device(0, RETRO_DEVICE_JOYPAD);

    // 6. Load. This is the long one - Eden's NCA/VFS loader runs deep on this stack,
    //    which is why the thread was created with 8 MiB rather than the 512 KiB
    //    secondary-thread default.
    EdenSetStatus("loading %s", g.rom_path);

    struct retro_game_info info;
    memset(&info, 0, sizeof(info));
    info.path = g.rom_path;
    info.data = NULL;   // need_fullpath is true (retro_core.cpp:349)
    info.size = 0;
    info.meta = NULL;

    const bool loaded = retro_load_game(&info);
    if (!loaded) {
        // retro_core.cpp logs the real reason through Content::DescribeLoadFailure
        // (retro_content.cpp:190-230) and, for the keys case, also raises
        // RETRO_ENVIRONMENT_SET_MESSAGE - which our environment callback has already
        // copied into g.status. Do not overwrite a specific message with a generic one.
        pthread_mutex_lock(&g.status_mutex);
        const bool have_specific = (strstr(g.status, "loading ") == g.status);
        pthread_mutex_unlock(&g.status_mutex);
        if (have_specific) {
            EdenSetStatus("failed to load. Check the log for the loader status; the "
                          "usual cause is missing or wrong prod.keys.");
        }
        atomic_store(&g.state, EdenBridgeStateFailed);
        retro_deinit();
        return NULL;
    }

    EdenSetStatus("running");
    atomic_store(&g.state, EdenBridgeStateRunning);

    // 7. The run loop.
    double window_start = EdenNowSeconds();
    unsigned long long window_base = atomic_load(&g.iterations);

    while (atomic_load(&g.should_run)) {
        // Backgrounded: stop calling retro_run entirely.
        //
        // HONEST LIMIT: this parks the FRONTEND loop, and eden_libretro_set_visible
        // makes RendererVulkan::Composite early-return, but nothing here stops Eden's
        // own guest CPU threads or its GPU thread. Core::System exposes Run / Pause /
        // ShutdownMainProcess and nothing that libretro can reach to suspend them
        // (retro_core.cpp:516-528 makes the same observation about reset). Whether
        // that is enough to survive a background transition without iOS killing the
        // app for GPU work is untested.
        if (!atomic_load(&g.visible)) {
            EdenWaitWhileHidden();
            if (!atomic_load(&g.should_run)) {
                break;
            }
            window_start = EdenNowSeconds();
            window_base = atomic_load(&g.iterations);
        }

        // Blocks up to kFrameWaitTimeout = 50ms on the frame signal
        // (retro_core.cpp:132, :562-565). This is exactly why it is not on the main
        // thread - docs/IOS_PORT_NOTES.md #3.
        retro_run();

        const double now = EdenNowSeconds();
        const double elapsed = now - window_start;
        if (elapsed >= 0.5) {
            const unsigned long long count = atomic_load(&g.iterations);
            atomic_store(&g.ips, (double)(count - window_base) / elapsed);
            window_start = now;
            window_base = count;
        }
    }

    // 8. Teardown, on this thread - the one that called retro_load_game and therefore
    //    the one Core::System::RegisterHostThread() (retro_core.cpp:309) registered.
    atomic_store(&g.state, EdenBridgeStateStopping);
    EdenSetStatus("shutting down");

    retro_unload_game();
    retro_deinit();

    atomic_store(&g.state, EdenBridgeStateIdle);
    atomic_store(&g.ips, 0.0);
    // "%s" rather than passing the choice straight in, so -Wformat-security has
    // nothing to say about a non-literal format string.
    EdenSetStatus("%s", atomic_load(&g.shutdown_requested) ? "the game exited" : "stopped");
    return NULL;
}

// ---------------------------------------------------------------------------
// public API
// ---------------------------------------------------------------------------

void eden_bridge_attach_layer(void *metal_layer, unsigned width, unsigned height) {
    EdenBridgeInitOnce();
    g.layer = metal_layer;   // unretained, matching retro_core.cpp:97's void*
    if (width > 0 && height > 0) {
        atomic_store(&g.layer_w, width);
        atomic_store(&g.layer_h, height);
    }
}

void eden_bridge_layer_did_resize(unsigned width, unsigned height) {
    EdenBridgeInitOnce();
    if (width == 0 || height == 0) {
        return;   // see the header: SanitizeDim would clamp to 1x1
    }
    atomic_store(&g.layer_w, width);
    atomic_store(&g.layer_h, height);
    // Safe before retro_init: eden_libretro_resize guards on g_emu_window
    // (retro_core.cpp:612-616) and does nothing if the window does not exist yet.
    eden_libretro_resize(width, height);
}

void eden_bridge_set_visible(bool visible) {
    EdenBridgeInitOnce();
    atomic_store(&g.visible, visible);
    eden_libretro_set_visible(visible);

    pthread_mutex_lock(&g.gate_mutex);
    pthread_cond_broadcast(&g.gate_cond);
    pthread_mutex_unlock(&g.gate_mutex);
}

bool eden_bridge_start(const char *rom_path, const char *data_root) {
    EdenBridgeInitOnce();

    if (atomic_load(&g.state) != EdenBridgeStateIdle) {
        EdenSetStatus("a session is already running");
        return false;
    }
    if (rom_path == NULL || rom_path[0] == '\0' || data_root == NULL || data_root[0] == '\0') {
        EdenSetStatus("start refused: missing ROM path or data root");
        return false;
    }
    if (g.layer == NULL) {
        // Refusing here is the whole point. docs/IOS_PORT_NOTES.md #1: a headless
        // window does not degrade, it ABORTS. Better a clear refusal than a
        // VK_ERROR_INITIALIZATION_FAILED throw out of a member-initialiser list.
        EdenSetStatus("start refused: no CAMetalLayer attached. The emulation view must "
                      "be on screen before a game can start.");
        atomic_store(&g.state, EdenBridgeStateFailed);
        return false;
    }

    strlcpy(g.rom_path, rom_path, sizeof(g.rom_path));
    strlcpy(g.data_root, data_root, sizeof(g.data_root));

    atomic_store(&g.should_run, true);
    atomic_store(&g.shutdown_requested, false);
    atomic_store(&g.visible, true);
    atomic_store(&g.iterations, 0ULL);
    atomic_store(&g.ips, 0.0);
    atomic_store(&g.button_mask, 0u);
    atomic_store(&g.touch_pressed, false);
    for (int i = 0; i < 2; ++i) {
        atomic_store(&g.stick[i][0], 0);
        atomic_store(&g.stick[i][1], 0);
    }

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    // 8 MiB. retro_load_game runs Eden's NCA/VFS loader on this stack and the default
    // for a secondary thread is 512 KiB.
    pthread_attr_setstacksize(&attr, 8 * 1024 * 1024);
    // QOS_CLASS_USER_INTERACTIVE is a choice, not a measurement: whether iOS throttles
    // it under thermal pressure, and whether this eventually wants a
    // thread_policy_set time-constraint policy the way audio render threads do, is
    // unmeasured.
    pthread_attr_set_qos_class_np(&attr, QOS_CLASS_USER_INTERACTIVE, 0);

    const int rc = pthread_create(&g.thread, &attr, EdenEmulationThread, NULL);
    pthread_attr_destroy(&attr);

    if (rc != 0) {
        atomic_store(&g.should_run, false);
        atomic_store(&g.state, EdenBridgeStateFailed);
        EdenSetStatus("could not create the emulation thread (errno %d)", rc);
        return false;
    }

    g.thread_valid = true;
    return true;
}

void eden_bridge_stop(void) {
    EdenBridgeInitOnce();

    if (!g.thread_valid) {
        return;
    }

    atomic_store(&g.should_run, false);

    // Release the thread if it is parked on the visibility gate, or it will never see
    // should_run change.
    atomic_store(&g.visible, true);
    pthread_mutex_lock(&g.gate_mutex);
    pthread_cond_broadcast(&g.gate_cond);
    pthread_mutex_unlock(&g.gate_mutex);

    // A plain join, deliberately. There is no way to interrupt retro_load_game - a
    // stop requested during a large XCI load cannot take effect until Core::System::
    // Load returns, and for a multi-GB title that can be a long time. The alternative
    // (giving up and leaving the thread running) would mean a second retro_init on a
    // live Core::System, which is far worse: docs/IOS_PORT_NOTES.md lists "whether a
    // second Core::System::Load after ShutdownMainProcess works" as reasoned but never
    // executed. Blocking the caller is the safe failure.
    pthread_join(g.thread, NULL);
    g.thread_valid = false;
    atomic_store(&g.state, EdenBridgeStateIdle);
}

EdenBridgeState eden_bridge_state(void) {
    EdenBridgeInitOnce();
    return (EdenBridgeState)atomic_load(&g.state);
}

void eden_bridge_copy_status(char *buf, size_t len) {
    EdenBridgeInitOnce();
    if (buf == NULL || len == 0) {
        return;
    }
    pthread_mutex_lock(&g.status_mutex);
    strlcpy(buf, g.status, len);
    pthread_mutex_unlock(&g.status_mutex);
}

double eden_bridge_iterations_per_second(void) {
    EdenBridgeInitOnce();
    return atomic_load(&g.ips);
}

// --- input producers -------------------------------------------------------

void eden_input_set_button(unsigned id, bool pressed) {
    EdenBridgeInitOnce();
    if (id >= 16) {
        return;
    }
    const unsigned bit = 1u << id;
    if (pressed) {
        atomic_fetch_or(&g.button_mask, bit);
    } else {
        atomic_fetch_and(&g.button_mask, ~bit);
    }
}

void eden_input_set_button_mask(unsigned mask) {
    EdenBridgeInitOnce();
    atomic_store(&g.button_mask, mask & 0xFFFFu);
}

void eden_input_set_stick(unsigned index, float x, float y) {
    EdenBridgeInitOnce();
    if (index >= 2) {
        return;
    }
    // Full scale is +32767, not 32768: retro_input.cpp:280-283 divides by 32767.0f
    // precisely so full deflection reaches 1.0, and notes that suyu's /32768 never
    // quite does. Clamp on this side so the core never sees an out-of-range value.
    if (x < -1.0f) { x = -1.0f; } else if (x > 1.0f) { x = 1.0f; }
    if (y < -1.0f) { y = -1.0f; } else if (y > 1.0f) { y = 1.0f; }
    atomic_store(&g.stick[index][0], (int)lrintf(x * 32767.0f));
    atomic_store(&g.stick[index][1], (int)lrintf(y * 32767.0f));
}

void eden_input_set_touch(bool pressed, float layer_x, float layer_y) {
    EdenBridgeInitOnce();

    if (!pressed) {
        atomic_store(&g.touch_pressed, false);
        return;
    }

    // Layer -> guest screen, then guest screen -> libretro pointer space.
    //
    // Why this is not a straight rescale: retro_input.cpp:322-326 converts the pointer
    // value back to [0,1] with (raw + 32767) / 65534 and DROPS anything outside,
    // commenting "EmuWindow::MapToTouchScreen is not needed - libretro already gave us
    // viewport-relative coordinates". Viewport means the GUEST SCREEN. The letterbox
    // bars around a 16:9 guest image inside a 19.5:9 phone layer are therefore the
    // frontend's problem, and a touch on a bar must read as no touch rather than as a
    // touch at the screen edge.
    //
    // This reproduces Layout::MaxRectangle against a 16:9 target - the aspect
    // retro_get_system_av_info declares (retro_core.cpp:362) and the only one the core
    // currently offers, since the libretro core exposes no aspect-ratio option. If one
    // is ever added, or if Settings::AspectRatio::Stretch becomes reachable, this
    // mapping breaks SILENTLY. Verified against the layout maths, not on a device.
    const double lw = (double)atomic_load(&g.layer_w);
    const double lh = (double)atomic_load(&g.layer_h);
    if (lw <= 0.0 || lh <= 0.0) {
        atomic_store(&g.touch_pressed, false);
        return;
    }

    const double target = 16.0 / 9.0;
    double gw, gh;
    if (lw / lh > target) {
        gh = lh;            // height-limited: bars on the left and right
        gw = lh * target;
    } else {
        gw = lw;            // width-limited: bars on the top and bottom
        gh = lw / target;
    }
    const double x0 = (lw - gw) * 0.5;
    const double y0 = (lh - gh) * 0.5;

    const double px = (double)layer_x * lw;
    const double py = (double)layer_y * lh;

    const double nx = (px - x0) / gw;
    const double ny = (py - y0) / gh;

    if (nx < 0.0 || nx > 1.0 || ny < 0.0 || ny > 1.0) {
        atomic_store(&g.touch_pressed, false);   // on a letterbox bar
        return;
    }

    atomic_store(&g.touch_x, (int)lrint(nx * 65534.0 - 32767.0));
    atomic_store(&g.touch_y, (int)lrint(ny * 65534.0 - 32767.0));
    atomic_store(&g.touch_pressed, true);
}
