// SPDX-FileCopyrightText: Copyright 2026 eden-ios contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Derived from suyu's src/libretro_core/retro_core.cpp audio block
// (suyu-emu/suyu-v0.0.4 retro_core.cpp:366-389) and src/audio_core/sink/libretro_sink.h,
// GPL-3.0-or-later, which derive from yuzu (GPL-2.0-or-later).
// SPDX-FileCopyrightText: Copyright 2024 suyu Emulator Project
// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
//
// ============================================================================
// HOW A FRONTEND GETS PCM OUT OF EDEN
// ============================================================================
//
// It registers a Sink. There is no other tap point, and this was checked rather than
// assumed:
//
//   * Everything that produces audio reaches hardware through a SinkStream obtained
//     from Sink::AcquireSinkStream. There are exactly two callers in the whole tree -
//     audio_core/device/device_session.cpp:53 (every AudioIn/AudioOut session) and
//     audio_core/adsp/apps/audio_renderer/audio_renderer.cpp:127 (MaxRendererSessions
//     = 2 render streams). Nothing else can hand out a stream.
//   * A Sink is only ever constructed by AudioCore::CreateSinks (audio_core.cpp:30-31)
//     via Sink::CreateSinkFromID, and CreateSinkFromID has no callers outside
//     audio_core.cpp. AudioCore exposes GetOutputSink() (audio_core.h:44) but Sink has
//     no way to enumerate the streams it already handed out (sink.h:27-103), so a
//     frontend cannot attach to a sink that already exists. It has to *be* the sink.
//
// suyu solved this with Settings::AudioEngine::Libretro plus a LibretroSink entry in
// the sink table. Eden has neither: the enum is { Auto, Cubeb, Sdl3, Null }
// (common/settings_enums.h:95) with a hand-written Canonicalizations() and a GetLast()
// that hard-returns Null, so adding a value means three edits in that header, one in
// sink_details.cpp and one in audio_core/CMakeLists.txt - and it leaks a "libretro"
// entry into the desktop Qt audio dropdown that would be silence if anyone picked it.
//
// So this file supplies the sink and audio_core grew one runtime hook instead, gated on
// ENABLE_LIBRETRO so nothing changes for desktop or Android:
//
//     namespace AudioCore::Sink {
//     using SinkFactoryFn = std::unique_ptr<Sink> (*)(std::string_view);
//     void SetSinkOverride(SinkFactoryFn factory);   // nullptr clears
//     }
//
// CreateSinkFromID consults it *before* GetOutputSinkDetails (which for
// AudioEngine::Auto calls GetCubebLatency, and that opens a cubeb context - no real
// device is to be touched at all), and GetDeviceListForSink short-circuits for the same
// reason. See src/audio_core/sink/sink_details.cpp.
//
// Init() installs the factory; from that point the emulator's audio comes out of Pump().
//
// FORMAT. No conversion anywhere on this path, and each half was read to confirm it:
//   rate     48000 - AudioCore::TargetSampleRate (audio_core/common/common.h:70),
//                    declared to the frontend as timing.sample_rate (retro_core.cpp:372).
//                    static_assert'd below.
//   channels 2     - our Sink reports device_channels = 2, so SinkStream::AppendBuffer
//                    downmixes a 5.1 game to stereo with yuzu's coefficients
//                    (sink_stream.cpp:37-61) before we ever see it.
//   sample   s16 host-endian interleaved - what AppendBuffer pushes and what
//                    retro_audio_sample_batch_t takes (libretro.h:7670-7671).
//   volume   already applied - AppendBuffer multiplies by
//                    system_volume * device_volume * Settings::Volume()
//                    (sink_stream.cpp:32-35). Do NOT apply it again here.
//
// ============================================================================
// BUFFERING: EDEN'S SCHEDULE vs. retro_run's
// ============================================================================
//
// Eden produces on the ADSP render thread, 240 frames (TargetSampleCount) per
// DeviceSinkCommand (renderer/command/sink/device.cpp:31-50), driven by the guest's
// audren service at 5 ms intervals. libretro consumes in one lump per retro_run:
// kFramesPerRun = 800 frames at the declared 60 fps / 48000 Hz.
//
// Between them sits Eden's own per-stream buffering, which is the right place for it
// and which this file deliberately does not duplicate: SinkStream owns a
// RingBuffer<s16, 0x10000> of samples plus an SPSCQueue of SinkBuffers, and
// ProcessAudioOutAndRender (sink_stream.h:205 - public, non-virtual, the same entry
// point CubebSink's and SDLSink's device callbacks use) is the drain. Pump() is
// therefore the "device callback", called from exactly one thread.
//
// On top of that this file keeps one small staging FIFO, for one reason only: the
// batch callback documents its return as "the number of frames that were processed"
// (libretro.h:7665), so a frontend may take fewer than it was offered and the
// remainder has to go somewhere.
//
// The three failure modes, and which are tolerated:
//
// UNDERRUN - Eden behind. ProcessAudioOutAndRender fills the shortfall by repeating
//   last_frame (sink_stream.cpp:189-194) and counts only real frames into
//   actual_frames_written (:207, :220), so min/max_played_sample_count - and therefore
//   DeviceSession::IsBufferConsumed - stay honest; the guest is never told it played
//   audio it did not produce. Audible as a short DC hold.
//   TOLERATED, and expected to be the COMMON case, not the exception: the emulator will
//   sit below 100% speed on a phone and the guest will produce fewer than 800 frames per
//   retro_run most of the time. Hiding this behind a resampler would also hide how far
//   below realtime the core is.
//
// OVERRUN - Eden ahead, or Pump not called. Bounded by Eden itself: the renderer calls
//   WaitFreeSpace before each command list (audio_renderer.cpp:197), whose predicate is
//   `paused || queued_buffers < max_queue_size` (sink_stream.cpp:238) and which
//   hard-blocks past max_queue_size + 3. Only ProcessAudioOutAndRender's
//   release_cv.notify_one (:199) releases it. Two consequences, both load-bearing:
//     - our stream MUST override Start/Stop. `paused` initialises to true
//       (sink_stream.h:235) and the base Start/Stop are empty (:75, :80), so a stream
//       that does not override them leaves the predicate permanently true: no
//       backpressure, unbounded latency, and exactly the burstiness suyu documents at
//       its retro_core.cpp:19-20.
//     - Pump() must keep draining even when the frontend gave us no batch callback,
//       or the ADSP render thread parks forever.
//   NOT tolerated as latency growth: the emulator throttles instead. That is the
//   correct trade on a phone.
//
// DRIFT - retro_run's real cadence vs. 48000 Hz. We push exactly kFramesPerRun every
//   run and never resample. retro_run waits on the guest's presentation
//   (retro_core.cpp:589), so when the core runs at, say, 40 fps the frontend receives
//   40*800 frames per wall second against a 48000 Hz device and its own dynamic rate
//   control has to absorb it - which is what libretro frontends do for every slow core.
//   TOLERATED DELIBERATELY. Rate-matching in the core would fight the frontend's DRC,
//   and tuning it needs measurements from hardware that has never run this.
//
// ============================================================================

#include <algorithm>
#include <cstddef>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "audio_core/common/common.h"
#include "audio_core/sink/sink.h"
#include "audio_core/sink/sink_details.h"
#include "audio_core/sink/sink_stream.h"
#include "common/common_types.h"
#include "common/logging.h"
#include "libretro_core/retro_audio.h"

namespace AudioCore::Sink {
// Defined in src/audio_core/sink/sink_details.cpp under ENABLE_LIBRETRO, which is the
// same condition under which this file is compiled at all (src/CMakeLists.txt:275-277
// only adds libretro_core when ENABLE_LIBRETRO is set), so there is no configuration in
// which this declaration resolves to nothing.
//
// Declared here rather than taken from sink_details.h because that header is outside
// this change's lane. Spelled with the function type written out instead of the
// SinkFactoryFn alias so the two declarations cannot drift; if the pair later moves into
// sink_details.h, this becomes a redeclaration of the same function and both compile.
void SetSinkOverride(std::unique_ptr<Sink> (*factory)(std::string_view));
} // namespace AudioCore::Sink

namespace LibretroCore::Audio {

// No resampler exists on this path in either direction, so the two rates must agree.
static_assert(kSampleRate == AudioCore::TargetSampleRate,
              "retro_get_system_av_info declares kSampleRate; Eden emits TargetSampleRate");
static_assert(kChannels == 2, "retro_audio_sample_batch_t is interleaved stereo");

namespace {

using AudioCore::Sink::SinkStream;
using AudioCore::Sink::StreamType;

constexpr std::size_t kSamplesPerRun = kFramesPerRun * kChannels;

/// Ceiling on frames held back because a frontend accepted less than it was offered.
/// Four runs ~= 66 ms. Past this the OLDEST frames are dropped: audio latency that
/// grows without bound is worse than a click, and dropping from the front keeps what
/// the frontend hears closest to what the emulator is doing now.
constexpr std::size_t kMaxStagingFrames = kFramesPerRun * 4;

constexpr s32 kSampleMin = static_cast<s32>((std::numeric_limits<s16>::min)());
constexpr s32 kSampleMax = static_cast<s32>((std::numeric_limits<s16>::max)());

// --------------------------------------------------------------------------
// The stream
// --------------------------------------------------------------------------

/// One Eden audio stream, owned by LibretroSink.
///
/// It deliberately overrides almost nothing. AppendBuffer stays the base version
/// because that is what applies Settings::Volume(), downmixes 5.1 to stereo and fills
/// the ring that the pull side drains (sink_stream.cpp:25-101) - suyu's override
/// (libretro_sink.h:78-88) throws all three away. Start/Stop are the exception, and
/// they are not optional: see "OVERRUN" in the header comment.
class LibretroSinkStream final : public SinkStream {
public:
    LibretroSinkStream(Core::System& system_, u32 device_channels_, u32 system_channels_,
                       const std::string& name_, StreamType type_)
        : SinkStream{system_, type_} {
        device_channels = device_channels_;
        system_channels = system_channels_;
        name = name_;
    }

    ~LibretroSinkStream() override = default;

    void Start(bool resume = false) override {
        paused = false;
    }

    void Stop() override {
        if (paused) {
            return;
        }
        // Sets paused and notifies release_cv, which is what unparks a producer that is
        // blocked in WaitFreeSpace (sink_stream.cpp:248-254).
        SignalPause();
    }
};

/// A stream plus the one piece of information SinkStream does not expose: its type is
/// protected (sink_stream.h:229) and there is no getter, and -fno-rtti rules out asking
/// after the fact, so record it at creation.
struct StreamEntry {
    AudioCore::Sink::SinkStreamPtr stream;
    StreamType type;
};

// --------------------------------------------------------------------------
// The sink
// --------------------------------------------------------------------------

class LibretroSink final : public AudioCore::Sink::Sink {
public:
    explicit LibretroSink(std::string_view /*device_id*/) {
        // Same as SDLSink (sdl3_sink.cpp:238). Drives the downmix in AppendBuffer and
        // the frame stride in ProcessAudioOutAndRender.
        device_channels = static_cast<u32>(kChannels);
    }

    ~LibretroSink() override;

    SinkStream* AcquireSinkStream(Core::System& system_, u32 system_channels_,
                                  const std::string& name_, StreamType type_) override;
    void CloseStream(SinkStream* stream_) override;
    void CloseStreams() override;

    f32 GetDeviceVolume() const override;
    void SetDeviceVolume(f32 volume) override;
    void SetSystemVolume(f32 volume) override;

    /// Pull `frames` stereo frames from every running output stream and add them into
    /// `accumulator`. `scratch` is caller-owned working space of at least
    /// frames * kChannels samples. Returns true if any stream contributed.
    bool MixInto(std::span<s32> accumulator, std::span<s16> scratch, std::size_t frames);

    /// Hand `frames` of silence to every running capture stream. See "AUDIO IN" below.
    void FeedInputSilence(std::span<const s16> silence, std::size_t frames);

private:
    mutable std::mutex streams_mutex;
    std::vector<StreamEntry> streams;
};

// --------------------------------------------------------------------------
// Registry
//
// AudioCore owns the sinks (audio_core.h:64-66) and destroys them on
// ShutdownMainProcess (core.cpp:439), which on this port happens inside
// retro_unload_game - possibly on a different thread from the one running retro_run.
// So the frontend side keeps observers, not owners, and every traversal holds
// g_sink_mutex for the whole pull; a sink cannot be destroyed mid-mix because
// ~LibretroSink takes the same lock to deregister.
//
// Lock order is always g_sink_mutex -> LibretroSink::streams_mutex, and nothing in the
// pull path waits on a producer, so there is no cycle: ProcessAudioOutAndRender only
// ever TryPops (sink_stream.cpp:189).
//
// CreateSinkFromID is called twice, for the output sink and then the input sink
// (audio_core.cpp:30-31), and nothing in its signature says which is which. Rather than
// depend on that order, both are registered and both are traversed - the input sink
// only ever holds StreamType::In streams, which the mixer skips by type anyway.
// --------------------------------------------------------------------------

std::mutex g_sink_mutex;
std::vector<LibretroSink*> g_sinks;

/// Interleaved stereo s16 waiting for the frontend. Touched only by the Pump thread.
std::vector<s16> g_staging;
/// Mix accumulator and per-stream scratch, preallocated so Pump does not allocate.
std::vector<s32> g_accumulator;
std::vector<s16> g_scratch;
std::vector<s16> g_silence;
/// One-shot "audio is actually flowing" marker, for bring-up. Pump thread only.
bool g_logged_live = false;

LibretroSink::~LibretroSink() {
    {
        std::scoped_lock lk{g_sink_mutex};
        for (std::size_t i = 0; i < g_sinks.size(); ++i) {
            if (g_sinks[i] == this) {
                g_sinks.erase(g_sinks.begin() + static_cast<std::ptrdiff_t>(i));
                break;
            }
        }
    }
    // Only now may `streams` be destroyed: past the line above no Pump can reach us,
    // and any Pump already inside MixInto held g_sink_mutex, so it has finished.
    std::scoped_lock lk{streams_mutex};
    streams.clear();
}

SinkStream* LibretroSink::AcquireSinkStream(Core::System& system_, u32 system_channels_,
                                            const std::string& name_, StreamType type_) {
    // Mirrors SDLSink::AcquireSinkStream (sdl3_sink.cpp:244-250). The trailing
    // underscores are not style: `system_channels` is a protected member of Sink
    // (sink.h:102) and a bare parameter name would be -Werror=shadow.
    system_channels = system_channels_;

    std::scoped_lock lk{streams_mutex};
    StreamEntry& entry = streams.emplace_back(StreamEntry{
        std::make_unique<LibretroSinkStream>(system_, device_channels, system_channels_, name_,
                                             type_),
        type_,
    });
    return entry.stream.get();
}

void LibretroSink::CloseStream(SinkStream* stream_) {
    std::scoped_lock lk{streams_mutex};
    for (std::size_t i = 0; i < streams.size(); ++i) {
        if (streams[i].stream.get() == stream_) {
            streams.erase(streams.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
    }
}

void LibretroSink::CloseStreams() {
    std::scoped_lock lk{streams_mutex};
    streams.clear();
}

f32 LibretroSink::GetDeviceVolume() const {
    std::scoped_lock lk{streams_mutex};
    if (streams.empty()) {
        return 1.0f;
    }
    return streams.front().stream->GetDeviceVolume();
}

void LibretroSink::SetDeviceVolume(f32 volume) {
    std::scoped_lock lk{streams_mutex};
    for (StreamEntry& entry : streams) {
        entry.stream->SetDeviceVolume(volume);
    }
}

void LibretroSink::SetSystemVolume(f32 volume) {
    std::scoped_lock lk{streams_mutex};
    for (StreamEntry& entry : streams) {
        entry.stream->SetSystemVolume(volume);
    }
}

bool LibretroSink::MixInto(std::span<s32> accumulator, std::span<s16> scratch,
                           std::size_t frames) {
    if (frames == 0) {
        // ProcessAudioOutAndRender indexes output_buffer[(frames_written - 1) * stride]
        // unconditionally at sink_stream.cpp:214. With num_frames == 0 that is an
        // underflowed index. Never call it with zero.
        return false;
    }

    const std::size_t samples = frames * kChannels;
    bool contributed = false;

    std::scoped_lock lk{streams_mutex};
    for (StreamEntry& entry : streams) {
        if (entry.type == StreamType::In) {
            continue;
        }
        SinkStream* const stream = entry.stream.get();

        // A paused stream is a stopped AudioOut session, or a renderer stream the guest
        // has not fed yet (DeviceSinkCommand starts it lazily, after its first append -
        // renderer/command/sink/device.cpp:52-54). Cubeb and SDL pause the *device* in
        // that state and stop getting callbacks; we have one output path for all
        // streams, so "no callback" has to mean "skip". Pulling anyway would mix in
        // ProcessAudioOutAndRender's underrun fill - last_frame repeated forever - and
        // a stream that stopped on a loud sample would add a constant DC offset to
        // everything else.
        if (stream->IsPaused()) {
            continue;
        }

        // Cannot currently fire: every stream here was built with device_channels =
        // kChannels above. It is checked anyway because being wrong writes
        // frames * device_channels samples into a frames * kChannels buffer.
        if (stream->GetDeviceChannels() != static_cast<u32>(kChannels)) {
            continue;
        }

        // ProcessAudioOutAndRender fills every frame it was asked for, but its Pop can
        // return short if the queue and the sample ring ever disagree (ClearQueue races
        // an append). Zeroing first makes that silence instead of whatever the previous
        // stream left in the scratch buffer.
        std::fill_n(scratch.begin(), samples, s16{0});
        stream->ProcessAudioOutAndRender(scratch.subspan(0, samples), frames);

        for (std::size_t i = 0; i < samples; ++i) {
            accumulator[i] += static_cast<s32>(scratch[i]);
        }
        contributed = true;
    }

    return contributed;
}

// AUDIO IN.
//
// The hook overrides CreateSinkFromID, which builds the input sink as well as the
// output one, so capture stops going to SDL. Nothing here records a microphone - iOS
// capture needs its own session category and a usage description, neither of which
// exists in this port yet. sink_details.cpp's GetDeviceListForSink also reports an empty
// capture device list while the override is installed, so a guest that asks first is
// told there is no microphone rather than being handed one that only produces zeros.
//
// Handing the capture streams silence is still not the same as doing nothing, for the
// guest that opens a session anyway. SinkStream::AppendBuffer returns immediately for
// StreamType::In (sink_stream.cpp:26-27), so an In stream's buffer bookkeeping advances
// only through ProcessAudioIn's counter update (sink_stream.cpp:159-164), and
// DeviceSession::IsBufferConsumed compares against exactly that (device_session.cpp:
// 119-121, :134). Never calling it pins played_sample_count at the constant
// GetExpectedPlayedSampleCount returns from zeroed counters (TargetSampleCount * 5), and
// a game that opens the mic waits forever for its first buffer back. Feeding zeros turns
// that hang into silence.
//
// REASONED FROM THE SOURCE, NOT RUN. No title that opens an AudioIn session has been
// tested here.
void LibretroSink::FeedInputSilence(std::span<const s16> silence, std::size_t frames) {
    if (frames == 0) {
        return;
    }

    std::scoped_lock lk{streams_mutex};
    for (StreamEntry& entry : streams) {
        if (entry.type != StreamType::In) {
            continue;
        }
        SinkStream* const stream = entry.stream.get();
        if (stream->IsPaused()) {
            continue;
        }
        const std::size_t samples = frames * stream->GetDeviceChannels();
        if (samples > silence.size()) {
            continue;
        }
        stream->ProcessAudioIn(silence.subspan(0, samples), frames);
    }
}

/// Drain `frames` frames out of Eden and append them to the staging FIFO, clamped to
/// s16. Produces silence when no sink exists yet, which is the before-first-load and
/// between-titles behaviour.
void PullFrames(std::size_t frames) {
    if (frames == 0) {
        return;
    }

    const std::size_t samples = frames * kChannels;
    if (g_accumulator.size() < samples) {
        g_accumulator.resize(samples);
    }
    if (g_scratch.size() < samples) {
        g_scratch.resize(samples);
    }
    if (g_silence.size() < samples) {
        g_silence.resize(samples, s16{0});
    }
    std::fill_n(g_accumulator.begin(), samples, s32{0});

    bool contributed = false;
    {
        // Held across the whole traversal: this is what keeps a sink alive while it is
        // being pulled. Nothing inside blocks on the producer.
        std::scoped_lock lk{g_sink_mutex};
        for (LibretroSink* const sink : g_sinks) {
            // Sum rather than pick. Cubeb and SDL open one device per stream and let
            // the OS mix them (sdl3_sink.cpp:108); there is exactly one output path
            // here, so the renderer's streams and every AudioOut session's stream have
            // to be added together, with the saturation done once at the end.
            if (sink->MixInto(g_accumulator, g_scratch, frames)) {
                contributed = true;
            }
            sink->FeedInputSilence(g_silence, frames);
        }
    }

    if (contributed && !g_logged_live) {
        g_logged_live = true;
        LOG_INFO(Audio_Sink, "libretro: audio is live - first frames pulled from Eden");
    }

    const std::size_t base = g_staging.size();
    g_staging.resize(base + samples);
    for (std::size_t i = 0; i < samples; ++i) {
        g_staging[base + i] = static_cast<s16>(std::clamp(g_accumulator[i], kSampleMin, kSampleMax));
    }
}

void DropFrontFrames(std::size_t frames) {
    const std::size_t samples = std::min(frames * kChannels, g_staging.size());
    g_staging.erase(g_staging.begin(), g_staging.begin() + static_cast<std::ptrdiff_t>(samples));
}

/// Handed to audio_core as a plain function pointer by Init(). Internal linkage: the only
/// thing that ever names it is SetSinkOverride's argument, and a hidden symbol is one
/// fewer name in a static library that gets linked straight into the app binary.
std::unique_ptr<AudioCore::Sink::Sink> CreateLibretroSink(std::string_view device_id) {
    auto sink = std::make_unique<LibretroSink>(device_id);
    {
        std::scoped_lock lk{g_sink_mutex};
        g_sinks.push_back(sink.get());
    }
    LOG_INFO(Audio_Sink, "libretro: sink created (device id \"{}\")", device_id);
    return sink;
}

} // namespace

void Init() {
    g_staging.clear();
    g_staging.reserve((kMaxStagingFrames + kFramesPerRun) * kChannels);
    g_accumulator.assign(kSamplesPerRun, s32{0});
    g_scratch.assign(kSamplesPerRun, s16{0});
    g_silence.assign(kSamplesPerRun, s16{0});
    g_logged_live = false;

    // Safe to do here even though retro_init has already called System::Initialize:
    // AudioCore, and with it CreateSinks, is constructed later, inside System::Load at
    // Impl::SetupForApplicationProcess (core.cpp:296).
    AudioCore::Sink::SetSinkOverride(&CreateLibretroSink);
    LOG_INFO(Audio_Sink, "libretro: audio sink override installed");
}

void Shutdown() {
    AudioCore::Sink::SetSinkOverride(nullptr);
    g_staging.clear();
    g_staging.shrink_to_fit();
    g_accumulator.clear();
    g_accumulator.shrink_to_fit();
    g_scratch.clear();
    g_scratch.shrink_to_fit();
    g_silence.clear();
    g_silence.shrink_to_fit();
}

void OnGameUnloaded() {
    // The sinks are already gone by the time this runs - retro_unload_game calls
    // StopEmulation (and so ShutdownMainProcess, which does audio_core.reset() at
    // core.cpp:439) before it calls here, and ~LibretroSink deregistered itself. All
    // that is left is whatever the last Pump could not hand over.
    g_staging.clear();
    g_logged_live = false;
}

bool IsLive() {
    std::scoped_lock lk{g_sink_mutex};
    return !g_sinks.empty();
}

void Pump(retro_audio_sample_batch_t cb) {
    // Drain first and unconditionally, before any early exit on `cb`. A frontend that
    // has not set a batch callback still must not park the ADSP render thread in
    // WaitFreeSpace - see "OVERRUN" in the header comment.
    const std::size_t staged = g_staging.size() / kChannels;
    if (staged < kFramesPerRun) {
        PullFrames(kFramesPerRun - staged);
    }

    if (cb == nullptr) {
        g_staging.clear();
        return;
    }

    // g_audio_batch_cb takes FRAMES (L+R pairs), not samples. suyu's comment about
    // this (retro_core.cpp:359-360) is correct and worth preserving.
    //
    // The callback is invoked with no lock held: it can block on the frontend's audio
    // device, and holding g_sink_mutex across that would stall CloseStream on the
    // emulation side for as long as the device takes.
    const std::size_t accepted = cb(g_staging.data(), kFramesPerRun);

    // The header documents the return as "the number of frames that were processed"
    // (libretro.h:7665) and RetroArch returns exactly what it was offered. But a great
    // many cores ignore this value outright, so frontends exist that return 0 as a stub
    // having consumed everything, and there is no way from here to tell that apart from
    // a frontend that genuinely took nothing. Zero is therefore read as "does not
    // report", not as "took none": treating it literally would re-offer the same stale
    // frames forever and permanently stall this core's audio against such a frontend,
    // whereas mis-reading a real zero only costs the dropout it was already heading for.
    // Any other short count is honoured, and the remainder carries to the next run.
    const std::size_t consumed = (accepted == 0) ? kFramesPerRun : std::min(accepted, kFramesPerRun);
    DropFrontFrames(consumed);

    // Whatever the frontend declined stays staged for the next run, up to the ceiling.
    const std::size_t remaining = g_staging.size() / kChannels;
    if (remaining > kMaxStagingFrames) {
        DropFrontFrames(remaining - kMaxStagingFrames);
    }
}

} // namespace LibretroCore::Audio

// ============================================================================
// WHAT IS WIRED, AND WHAT IS STILL OWED FROM ELSEWHERE
//
// DONE, in this change:
//   * src/audio_core/sink/sink_details.cpp - SetSinkOverride is defined there under
//     ENABLE_LIBRETRO, CreateSinkFromID consults it before GetOutputSinkDetails, and
//     GetDeviceListForSink short-circuits so no backend is enumerated once the frontend
//     owns the device.
//   * src/audio_core/CMakeLists.txt - turns the ENABLE_LIBRETRO *option* into a compile
//     definition for audio_core's own sources (PRIVATE). It was only ever an option
//     before, so sink_details.cpp could not have tested it.
//   * this file - the guard is gone. libretro_core is only added to the build under
//     ENABLE_LIBRETRO (src/CMakeLists.txt:275-277), so a second macro would have had no
//     configuration in which it could differ from it.
//
// STILL OWED, neither of which blocks audio working:
//
// 1. src/audio_core/sink/sink_details.h is where this pair belongs:
//
//        using SinkFactoryFn = std::unique_ptr<Sink> (*)(std::string_view);
//        /// Install a frontend-supplied sink factory, overriding sink_id. nullptr clears.
//        void SetSinkOverride(SinkFactoryFn factory);
//
//    Today the declaration is written out by hand in two places - near the top of this
//    file, and above the anonymous namespace in sink_details.cpp (it must be outside it;
//    the SinkDetails machinery is inside one) - because -Werror=missing-declarations
//    wants a declaration before the definition. Both spell the same function type, so
//    adding the header version is a redeclaration and needs no edit to either .cpp.
//
// 2. src/libretro_core/CMakeLists.txt could name the dependency it now has:
//
//        target_link_libraries(eden_libretro PRIVATE audio_core)
//
//    and the comment at CMakeLists.txt:42-44 ("audio_core would be needed only once a
//    real libretro sink exists") is no longer true. This is tidiness, not a fix: the
//    symbols already resolve, because eden_libretro links `core` PUBLIC and
//    src/core/CMakeLists.txt links audio_core PRIVATE, which reaches consumers as
//    LINK_ONLY and so lands after eden_libretro in the final static link line. That
//    already had to be true before this change - LibretroSinkStream's vtable references
//    SinkStream::AppendBuffer and ReleaseBuffer, both defined in sink_stream.cpp.
// ============================================================================
