// CAI macOS speech: gapless system-voice queue. See speech.h for the API.
//
// Why this exists: AVSpeechSynthesizer can speak an utterance itself, but its
// own playback queue starts and stops the audio output around every
// utterance. That leaves about 120 ms of silence per line for the compact
// voices and about 250 ms for Eloquence (measured in the Say the Spire 2 and
// Oxygen Not Included accessibility mods: four short lines took 1612 ms to
// play for 602 ms of speech). A screen reader user hears speech stopping and
// restarting on every line, and nothing on the utterance API changes it. So
// each line is rendered offline here and played back through a player of
// our own, back to back.
//
// Design (a port of MacSpeechStream.cs from the OniAccess mod, reworked so
// nothing depends on a per-frame Update() or on the main thread):
//
// - AVSpeechSynthesizer never plays audio here. Each line is rendered with
//   writeUtterance:toBufferCallback:, which delivers AVAudioPCMBuffer chunks
//   in the voice's native format (Eloquence 16 kHz, compact voices 22.05 kHz)
//   in a fraction of the line's playing time. Channel 0 is collected, the
//   silence AVSpeech leaves around the line is trimmed, the samples are
//   brought to -10 dBFS RMS under a peak limiter, and the result plus 50 ms
//   of silence is scheduled on an AVAudioPlayerNode attached to our own
//   AVAudioEngine. The player plays scheduled buffers back to back, so
//   consecutive lines have no gap beyond that 50 ms.
//
// - One render is in flight at a time; further lines wait in a pending queue.
//   A render cannot be aborted (stopSpeakingAtBoundary: does not touch a
//   write), so an interrupt "retires" the render in flight: its synthesizer
//   is parked until AVSpeech delivers both end markers (an empty buffer, sent
//   twice), and a fresh synthesizer takes the next line at once. The player
//   node's stop discards its scheduled buffers, which is the audible half of
//   the interrupt. A parked synthesizer is released on the main queue, where
//   AVSpeech runs its completion: releasing it from another thread while the
//   first end marker was still being delivered crashed the game inside the
//   TextToSpeech framework.
//
// - Threading: the buffer callback may arrive on any queue (in practice the
//   main queue). It only touches the Render it was created for, under that
//   Render's own mutex, and on the end marker it hands the Render to a
//   private serial dispatch queue, which does the trim/normalize/schedule and
//   starts the next render under the stream's mutex. The public API takes
//   the same mutex. No code here waits for the main run loop; if callbacks do
//   not arrive, the stall timer (1 s period) retires a render after
//   kStallTimeoutSeconds with no progress. Blocks hold only a weak pointer to
//   the stream, so one that runs after shutdown finds nothing to do.
//
// - Every entry point wraps its work in @autoreleasepool, because the game
//   thread that calls us may not drain one.

#import <AVFoundation/AVFoundation.h>
#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "speech.h"
#include "speech_samples.h"

namespace {

#ifndef CAI_SPEECH_INITIAL_RATE
#define CAI_SPEECH_INITIAL_RATE 22050       // build with another value to force the reconnect path in a test
#endif
constexpr double kInitialRate = CAI_SPEECH_INITIAL_RATE; // the player's format until the first render says otherwise
constexpr float kGapSeconds = 0.05f;        // silence between utterances, as a screen reader would pause
constexpr double kStallTimeoutSeconds = 30; // a render that delivers nothing for this long is abandoned (cold voice start can take seconds)
constexpr int kEndMarkers = 2;              // empty buffers AVSpeech sends after the last audio of an utterance
constexpr double kSlowRenderSeconds = 2;    // a render slower than this is noted in the log
constexpr double kMainQueueWarnSeconds = 2; // how long after init an undrained main queue is worth a warning

using Clock = std::chrono::steady_clock;

// ---- logging ----

std::atomic<cai_speech_log_fn> g_logFn{nullptr};
const Clock::time_point g_epoch = Clock::now();

double NowMs() {
    return std::chrono::duration<double, std::milli>(Clock::now() - g_epoch).count();
}

// The host's callback gets the bare line and stamps it itself; without a
// callback the line goes to stderr with the milliseconds since load.
void LogV(cai_speech_log_level level, const char* fmt, va_list args) {
    char body[1024];
    vsnprintf(body, sizeof body, fmt, args);
    if (cai_speech_log_fn fn = g_logFn.load()) {
        fn(level, body);
    } else {
        fprintf(stderr, "[%9.1f ms] %s%s\n", NowMs(), level >= CAI_SPEECH_LOG_WARNING ? "warning: " : "", body);
        fflush(stderr);
    }
}
// The normal flow of a render.
void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    LogV(CAI_SPEECH_LOG_INFO, fmt, args);
    va_end(args);
}
// A failure or a dropped line.
void Warn(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Warn(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    LogV(CAI_SPEECH_LOG_WARNING, fmt, args);
    va_end(args);
}

const char* QueueLabel() {
    const char* label = dispatch_queue_get_label(DISPATCH_CURRENT_QUEUE_LABEL);
    return label && *label ? label : "(unlabeled)";
}

// ---- one utterance being rendered ----

struct Render {
    std::mutex mutex;
    std::vector<float> samples;
    double sampleRate = 0;
    std::string problem;              // set when the delivered audio is unusable; the render still waits for the end marker
    std::atomic<bool> done{false};    // the end marker arrived
    std::atomic<int> endMarkers{0};
    std::atomic<int> chunks{0};
    const Clock::time_point started = Clock::now();
    std::atomic<Clock::time_point> lastProgress{Clock::now()};
    std::string text;                 // for the log only
    bool firstChunkLogged = false;

    // AVSpeech is finished with the synthesizer: both end markers arrived, or
    // the render ended and the second marker has not come for the stall
    // timeout, which is as long as anything else here waits for AVSpeech.
    bool SynthesizerReleasable() const {
        if (endMarkers.load() >= kEndMarkers) return true;
        return done.load() && SecondsSinceProgress() > kStallTimeoutSeconds;
    }

    double SecondsSinceStart() const {
        return std::chrono::duration<double>(Clock::now() - started).count();
    }
    double SecondsSinceProgress() const {
        return std::chrono::duration<double>(Clock::now() - lastProgress.load()).count();
    }

    void Fail(const std::string& why) {
        std::lock_guard<std::mutex> lock(mutex);
        if (problem.empty()) problem = why;
    }

    // AVSpeech hands over one buffer per call, then an empty one (twice) at the end.
    // Returns true the first time the end marker is seen.
    bool Append(AVAudioBuffer* buffer) {
        lastProgress.store(Clock::now());
        if (buffer == nil) return MarkDone();
        if (![buffer isKindOfClass:[AVAudioPCMBuffer class]]) {
            Fail("AVSpeech delivered a buffer that is not PCM");
            return false;
        }
        AVAudioPCMBuffer* pcm = (AVAudioPCMBuffer*)buffer;
        AVAudioFrameCount frames = pcm.frameLength;
        if (frames == 0) return MarkDone();

        AVAudioFormat* format = pcm.format;
        double rate = format.sampleRate;
        AVAudioChannelCount channels = format.channelCount;
        size_t stride = format.isInterleaved ? channels : 1;

        std::lock_guard<std::mutex> lock(mutex);
        if (!firstChunkLogged) {
            firstChunkLogged = true;
            Log("render first chunk after %.0f ms: %u frames, %.0f Hz, %u ch, %s, on queue %s (main thread: %s)",
                SecondsSinceStart() * 1000, frames, rate, channels,
                format.commonFormat == AVAudioPCMFormatFloat32 ? "float32" :
                format.commonFormat == AVAudioPCMFormatInt16 ? "int16" : "other",
                QueueLabel(), [NSThread isMainThread] ? "yes" : "no");
        }
        chunks++;
        if (sampleRate <= 0) {
            sampleRate = rate;
        } else if (rate != sampleRate) {
            if (problem.empty()) problem = "sample rate changed within one utterance";
            return false;
        }
        size_t base = samples.size();
        samples.resize(base + frames);
        if (format.commonFormat == AVAudioPCMFormatFloat32 && pcm.floatChannelData) {
            const float* src = pcm.floatChannelData[0];
            for (AVAudioFrameCount i = 0; i < frames; i++) samples[base + i] = src[i * stride];
        } else if (format.commonFormat == AVAudioPCMFormatInt16 && pcm.int16ChannelData) {
            const int16_t* src = pcm.int16ChannelData[0];
            for (AVAudioFrameCount i = 0; i < frames; i++) samples[base + i] = src[i * stride] / 32768.0f;
        } else {
            samples.resize(base);
            if (problem.empty()) problem = "AVSpeech delivered a buffer in an unsupported sample format";
        }
        return false;
    }

private:
    bool MarkDone() {
        endMarkers++;
        return !done.exchange(true);
    }
};

using RenderPtr = std::shared_ptr<Render>;

struct Retired {
    AVSpeechSynthesizer* synth;
    RenderPtr render;
};

struct Pending {
    std::string text;
    std::string voice; // AVSpeech identifier, or empty for AVSpeech's default
    float rate;
};

struct VoiceInfo {
    std::string id;
    std::string name;
    std::string language;
};

// ---- the stream ----

struct Stream : std::enable_shared_from_this<Stream> {
    std::mutex mutex;
    dispatch_queue_t queue = nullptr;    // serial: finishes renders, starts the next, runs the stall timer
    dispatch_source_t stallTimer = nullptr;

    AVAudioEngine* engine = nil;
    AVAudioPlayerNode* player = nil;
    AVAudioFormat* format = nil;         // float32 mono at formatRate: the player's connection to the mixer
    double formatRate = 0;
    std::vector<float> gap;              // kGapSeconds of silence at formatRate

    AVSpeechSynthesizer* synth = nil;    // renders now, or nil until the next render needs one; never speaks aloud
    RenderPtr inFlight;
    std::vector<Retired> retired;
    std::deque<Pending> pending;
    bool broken = false;

    AVSpeechSynthesisVoice* voice = nil; // resolved for resolvedVoiceId
    std::string resolvedVoiceId;

    std::vector<VoiceInfo> voices;
    std::string currentVoiceId;          // empty: AVSpeech's default
    float rate = AVSpeechUtteranceDefaultSpeechRate;
    float volume = 0.8f;                 // default 80 percent; game music sits under speech

    int linesEnqueued = 0;
    int linesScheduled = 0;

    // Buffers scheduled on the player and not yet played back, for
    // cai_speech_is_speaking. Shared with the completion blocks, which may
    // outlive the stream; the generation lets a stop discard the blocks of
    // buffers it threw away.
    struct PlayState { std::atomic<int> outstanding{0}; std::atomic<uint64_t> generation{0}; };
    std::shared_ptr<PlayState> play = std::make_shared<PlayState>();

    // AVSpeech delivers its buffer callbacks on the main dispatch queue
    // (observed in the game and in speechtest), so the host must drain it.
    // A canary dispatched at init tells the log whether it does.
    std::shared_ptr<std::atomic<bool>> mainQueueDrained = std::make_shared<std::atomic<bool>>(false); // shared: the canary may outlive the stream
    bool mainQueueWarned = false;
    const Clock::time_point created = Clock::now();
};

std::mutex g_api;                        // held by every public entry point; orders shutdown against the rest
std::shared_ptr<Stream> g_stream;        // set between init and shutdown; guarded by g_api
// Synthesizers whose render was still in flight at shutdown. Their callbacks
// may still run, so they are kept alive for the rest of the process rather
// than freed under a callback: a few hundred bytes each, only at shutdown
// while something was rendering.
std::vector<AVSpeechSynthesizer*> g_abandonedSynths;

// Stop the player and forget its scheduled buffers. Caller holds s.mutex.
void StopPlayer(Stream& s) {
    s.play->generation.fetch_add(1);
    s.play->outstanding.store(0);
    [s.player stop];
}

std::string ToStd(NSString* s) {
    return s ? std::string([s UTF8String] ?: "") : std::string();
}

// ---- playback ----

// Connect the player to the mixer in float32 mono at sampleRate, replacing any earlier connection.
bool Connect(Stream& s, double sampleRate) {
    AVAudioFormat* format = [[AVAudioFormat alloc] initStandardFormatWithSampleRate:sampleRate channels:1];
    if (!format) {
        Warn("AVAudioFormat init returned nil for %.0f Hz", sampleRate);
        return false;
    }
    if (s.format) [s.engine disconnectNodeOutput:s.player];
    s.format = format;
    s.formatRate = sampleRate;
    s.gap.assign((size_t)(kGapSeconds * sampleRate), 0.0f);
    [s.engine connect:s.player to:s.engine.mainMixerNode format:format];
    Log("player connected to mixer at %.0f Hz mono", sampleRate);
    return true;
}

// Copy the samples plus the gap into an AVAudioPCMBuffer and queue it on the
// player node, which plays it after whatever it already holds. Caller holds s.mutex.
void Schedule(Stream& s, const std::vector<float>& trimmed, double sampleRate, const RenderPtr& render) {
    if (trimmed.empty()) return;

    // The engine stops on its own after an output device change; bring it back
    // first. Scheduling on a node whose engine is not running raises an
    // Objective-C exception.
    if (!s.engine.isRunning) {
        // Whatever the player still held was lost with the engine, and its
        // completion handlers may never fire; forget those buffers so the
        // outstanding count does not keep IsSpeaking true.
        StopPlayer(s);
        NSError* error = nil;
        if (![s.engine startAndReturnError:&error]) {
            Warn("AVAudioEngine failed to restart (%s); dropping the line", ToStd(error.localizedDescription).c_str());
            return;
        }
        Log("AVAudioEngine restarted");
    }

    // A voice with another native rate: the player's buffers must match its
    // connection, so reconnect. Whatever the old voice still had queued is dropped.
    if (sampleRate != s.formatRate) {
        StopPlayer(s);
        if (!Connect(s, sampleRate)) return;
    }

    AVAudioFrameCount frames = (AVAudioFrameCount)(trimmed.size() + s.gap.size());
    AVAudioPCMBuffer* buffer = [[AVAudioPCMBuffer alloc] initWithPCMFormat:s.format frameCapacity:frames];
    if (!buffer || !buffer.floatChannelData) {
        Warn("AVAudioPCMBuffer init failed for %u frames", frames);
        return;
    }
    float* channel = buffer.floatChannelData[0];
    memcpy(channel, trimmed.data(), trimmed.size() * sizeof(float));
    memcpy(channel + trimmed.size(), s.gap.data(), s.gap.size() * sizeof(float));
    buffer.frameLength = frames;

    int lineNo = ++s.linesScheduled;
    double durationMs = trimmed.size() / sampleRate * 1000;
    std::string text = render->text;
    std::shared_ptr<Stream::PlayState> play = s.play;
    uint64_t generation = play->generation.load();
    play->outstanding.fetch_add(1);
    @try {
        [s.player scheduleBuffer:buffer
              completionCallbackType:AVAudioPlayerNodeCompletionDataPlayedBack
                   completionHandler:^(AVAudioPlayerNodeCompletionCallbackType type) {
                       if (play->generation.load() == generation) {
                           play->outstanding.fetch_sub(1);
                           Log("played back line %d (\"%s\")", lineNo, text.c_str());
                       } else {
                           Log("discarded line %d (\"%s\")", lineNo, text.c_str());
                       }
                   }];
        [s.player play]; // idempotent while playing; needed after a stop or an engine restart
    } @catch (NSException* ex) {
        if (play->generation.load() == generation) play->outstanding.fetch_sub(1);
        Warn("scheduleBuffer raised %s: %s", ToStd(ex.name).c_str(), ToStd(ex.reason).c_str());
        return;
    }
    Log("scheduled line %d: %.0f ms of speech + %.0f ms gap at %.0f Hz (\"%s\")",
        lineNo, durationMs, kGapSeconds * 1000, sampleRate, text.c_str());
}

// ---- rendering ----

void StartNextLocked(Stream& s);

// The voice object for an identifier, nil for empty or unknown. The result
// is kept for the identifier last asked about, a miss included, so an
// uninstalled voice is looked up and logged once rather than per line.
AVSpeechSynthesisVoice* ResolveVoice(Stream& s, const std::string& identifier) {
    if (identifier == s.resolvedVoiceId) return s.voice;
    s.voice = nil;
    s.resolvedVoiceId = identifier;
    if (identifier.empty()) return nil;
    s.voice = [AVSpeechSynthesisVoice voiceWithIdentifier:[NSString stringWithUTF8String:identifier.c_str()]];
    if (!s.voice) Warn("AVSpeech has no voice %s; its default stands in", identifier.c_str());
    return s.voice;
}

// Hand a synthesizer's last reference to the main queue. The block owns it
// until it has run, so the object is freed on the main thread, after whatever
// AVSpeech still had queued there.
void ReleaseOnMainQueue(AVSpeechSynthesizer* synth) {
    if (!synth) return;
    dispatch_async(dispatch_get_main_queue(), ^{
        @autoreleasepool { (void)synth; }
    });
}

// Release retired synthesizers AVSpeech is finished with. Caller holds s.mutex.
// The first end marker arrives while the framework is still inside the
// write's completion on the main queue, and that completion touches the
// synthesizer after our callback returns; the second empty buffer comes once
// it is done. So a synthesizer is kept until the second marker (or the stall
// timeout, if that marker never comes), and its last reference is dropped by
// a block on the main queue, behind any completion work still queued there,
// never from this thread.
void Reap(Stream& s) {
    for (size_t i = s.retired.size(); i-- > 0;) {
        Retired& r = s.retired[i];
        if (!r.render->SynthesizerReleasable()) continue;
        Log("reaped a retired render (\"%s\", %d chunks, %d end markers)",
            r.render->text.c_str(), r.render->chunks.load(), r.render->endMarkers.load());
        ReleaseOnMainQueue(r.synth);
        s.retired.erase(s.retired.begin() + (long)i);
    }
}

// Abandon the render in flight together with its synthesizer; the next render gets a new one.
void RetireLocked(Stream& s) {
    if (!s.inFlight) return;
    Log("retiring the render in flight (\"%s\")", s.inFlight->text.c_str());
    s.retired.push_back(Retired{ s.synth, s.inFlight });
    s.inFlight = nullptr;
    s.synth = nil;
}

// Drop everything queued, rendering or playing. Caller holds s.mutex. An idle
// stream is left untouched: the key monitor calls this on every key press.
void StopLocked(Stream& s) {
    if (!s.inFlight && s.pending.empty() && s.play->outstanding.load() == 0) return;
    Log("stop: dropping %zu pending line(s)%s", s.pending.size(), s.inFlight ? " and the render in flight" : "");
    s.pending.clear();
    RetireLocked(s);
    StopPlayer(s);
}

// Trim, normalize, and schedule a finished render. Caller holds s.mutex.
void FinishLocked(Stream& s, const RenderPtr& render) {
    s.inFlight = nullptr;
    double seconds = render->SecondsSinceStart();
    Log("render done after %.0f ms: %zu frames in %d chunks at %.0f Hz (\"%s\")%s",
        seconds * 1000, render->samples.size(), render->chunks.load(), render->sampleRate,
        render->text.c_str(), seconds > kSlowRenderSeconds ? " (slow)" : "");
    if (!render->problem.empty()) {
        Warn("rendered speech unusable: %s", render->problem.c_str());
        return;
    }
    if (render->samples.empty() || render->sampleRate <= 0) {
        Warn("render delivered no audio; nothing to play");
        return;
    }
    std::vector<float> trimmed = speech::samples::Trim(render->samples.data(), render->samples.size(), render->sampleRate);
    float gain = speech::samples::Normalize(trimmed, render->sampleRate);
    Log("trimmed %zu -> %zu frames, gain %.2fx", render->samples.size(), trimmed.size(), gain);
    Schedule(s, trimmed, render->sampleRate, render);
}

// Runs on s.queue when a render's end marker arrives.
void OnRenderEnded(Stream& s, const RenderPtr& render) {
    std::lock_guard<std::mutex> lock(s.mutex);
    Reap(s);
    if (s.inFlight == render) FinishLocked(s, render);
    StartNextLocked(s);
}

void StartNextLocked(Stream& s) {
    if (s.inFlight || s.pending.empty() || s.broken) return;
    if (!s.synth) {
        s.synth = [[AVSpeechSynthesizer alloc] init];
        if (!s.synth) {
            Warn("AVSpeechSynthesizer init returned nil; the stream is unusable from here on");
            s.broken = true;
            return;
        }
    }
    Pending next = std::move(s.pending.front());
    s.pending.pop_front();

    NSString* text = [NSString stringWithUTF8String:next.text.c_str()];
    if (!text) {
        Warn("line is not valid UTF-8; dropping it");
        StartNextLocked(s);
        return;
    }
    AVSpeechUtterance* utterance = [AVSpeechUtterance speechUtteranceWithString:text];
    if (!utterance) {
        Warn("AVSpeechUtterance returned nil; dropping the line");
        StartNextLocked(s);
        return;
    }
    if (AVSpeechSynthesisVoice* voice = ResolveVoice(s, next.voice)) utterance.voice = voice;
    utterance.rate = next.rate;

    auto render = std::make_shared<Render>();
    render->text = next.text;
    s.inFlight = render;
    std::weak_ptr<Stream> stream = s.weak_from_this();
    dispatch_queue_t queue = s.queue;
    Log("render start (\"%s\") voice=%s rate=%.2f", next.text.c_str(),
        next.voice.empty() ? "(default)" : next.voice.c_str(), next.rate);
    // The block owns a reference to its Render, so a late callback for a
    // retired render (the second end marker in particular) always finds a
    // live object. It never takes s.mutex: the end-of-render work is handed to
    // the serial queue instead, which also makes a synchronous callback from
    // inside writeUtterance safe.
    [s.synth writeUtterance:utterance toBufferCallback:^(AVAudioBuffer* buffer) {
        @autoreleasepool {
            if (render->Append(buffer)) {
                Log("render end marker after %.0f ms (\"%s\") on queue %s (main thread: %s)",
                    render->SecondsSinceStart() * 1000, render->text.c_str(), QueueLabel(),
                    [NSThread isMainThread] ? "yes" : "no");
                dispatch_async(queue, ^{
                    @autoreleasepool {
                        if (std::shared_ptr<Stream> live = stream.lock()) OnRenderEnded(*live, render);
                    }
                });
            }
        }
    }];
}

// Once a second on s.queue: reap retired renders and abandon a stalled one.
void OnStallTimer(Stream& s) {
    std::lock_guard<std::mutex> lock(s.mutex);
    Reap(s);
    if (!s.mainQueueDrained->load() && !s.mainQueueWarned &&
        std::chrono::duration<double>(Clock::now() - s.created).count() > kMainQueueWarnSeconds) {
        s.mainQueueWarned = true;
        Warn("the main dispatch queue has not drained since init; AVSpeech render callbacks arrive there and will not run until it does");
    }
    if (s.inFlight && s.inFlight->SecondsSinceProgress() > kStallTimeoutSeconds) {
        Warn("a render stalled for %.0f s (\"%s\"); abandoning it", kStallTimeoutSeconds, s.inFlight->text.c_str());
        RetireLocked(s);
        StartNextLocked(s);
    }
}

// ---- voices and Spoken Content settings ----

void ReadVoices(Stream& s) {
    s.voices.clear();
    for (AVSpeechSynthesisVoice* v in [AVSpeechSynthesisVoice speechVoices]) {
        std::string id = ToStd(v.identifier);
        std::string name = ToStd(v.name);
        if (id.empty() || name.empty()) continue;
        s.voices.push_back(VoiceInfo{ id, name, ToStd(v.language) });
    }
}

// The Spoken Content voice from System Settings: NSSpeechSynthesizer.defaultVoice
// carries an identifier AVSpeech shares. Empty if none or AVSpeech lacks it.
// defaultVoice is deprecated since macOS 14, but AVSpeech offers nothing that
// reports the voice chosen in System Settings, and the class method still
// answers off the main thread; do not "fix" this into a behavior change.
std::string SpokenContentVoice() {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    std::string identifier = ToStd([NSSpeechSynthesizer defaultVoice]);
#pragma clang diagnostic pop
    if (identifier.empty()) {
        Warn("macOS reports no default speech voice");
        return {};
    }
    AVSpeechSynthesisVoice* voice = [AVSpeechSynthesisVoice voiceWithIdentifier:[NSString stringWithUTF8String:identifier.c_str()]];
    if (!voice) {
        Warn("AVSpeech has no voice for the Spoken Content identifier %s", identifier.c_str());
        return {};
    }
    Log("Spoken Content voice: %s (%s, %s)", identifier.c_str(), ToStd(voice.name).c_str(), ToStd(voice.language).c_str());
    return identifier;
}

// The Spoken Content rate for a voice, on AVSpeech's [0, 1] scale, or -1 when
// the preference is absent. It lives in the com.apple.Accessibility domain,
// readable through NSUserDefaults without any permission, under the key
// SpokenContentDefaultVoiceSelectionsByLanguage: an array alternating a
// language code with a dictionary holding voiceId and rate.
float SpokenContentRate(const std::string& identifier) {
    NSUserDefaults* defaults = [[NSUserDefaults alloc] initWithSuiteName:@"com.apple.Accessibility"];
    id selections = [defaults objectForKey:@"SpokenContentDefaultVoiceSelectionsByLanguage"];
    if (![selections isKindOfClass:[NSArray class]]) {
        Log("macOS has no Spoken Content voice selections; keeping the default rate");
        return -1.0f;
    }
    for (id entry in (NSArray*)selections) {
        if (![entry isKindOfClass:[NSDictionary class]]) continue;
        NSDictionary* dict = (NSDictionary*)entry;
        if (ToStd([dict[@"voiceId"] description]) != identifier) continue;
        id rateValue = dict[@"rate"];
        float rate = -1.0f;
        if ([rateValue isKindOfClass:[NSNumber class]]) {
            rate = [(NSNumber*)rateValue floatValue];
        } else if ([rateValue isKindOfClass:[NSString class]]) {
            // Stored as text ("0.9"), always with a period regardless of locale.
            char* end = nullptr;
            double parsed = strtod([(NSString*)rateValue UTF8String], &end);
            if (end && *end == '\0') rate = (float)parsed;
        }
        if (rate < 0.0f || rate > 1.0f) {
            Warn("Spoken Content rate for %s is unreadable: \"%s\"", identifier.c_str(), ToStd([rateValue description]).c_str());
            return -1.0f;
        }
        Log("Spoken Content rate for %s: %.2f", identifier.c_str(), rate);
        return rate;
    }
    Log("no Spoken Content rate stored for %s; keeping the default rate", identifier.c_str());
    return -1.0f;
}

void PickDefaults(Stream& s) {
    s.currentVoiceId = SpokenContentVoice();
    if (s.currentVoiceId.empty()) {
        NSString* language = [AVSpeechSynthesisVoice currentLanguageCode];
        AVSpeechSynthesisVoice* voice = [AVSpeechSynthesisVoice voiceWithLanguage:language];
        s.currentVoiceId = voice ? ToStd(voice.identifier) : std::string();
        Log("falling back to the AVSpeech voice for %s: %s", ToStd(language).c_str(),
            s.currentVoiceId.empty() ? "(none; AVSpeech default)" : s.currentVoiceId.c_str());
    }
    float rate = s.currentVoiceId.empty() ? -1.0f : SpokenContentRate(s.currentVoiceId);
    s.rate = rate >= 0.0f ? rate : AVSpeechUtteranceDefaultSpeechRate;
}

} // namespace

// ---- C API ----

extern "C" {

void cai_speech_set_log(cai_speech_log_fn fn) {
    g_logFn.store(fn);
}

bool cai_speech_init(void) {
    std::lock_guard<std::mutex> api(g_api);
    if (g_stream) return true;
    @autoreleasepool {
        std::shared_ptr<Stream> s = std::make_shared<Stream>();
        s->engine = [[AVAudioEngine alloc] init];
        s->player = [[AVAudioPlayerNode alloc] init];
        [s->engine attachNode:s->player];
        if (!Connect(*s, kInitialRate)) return false;
        NSError* error = nil;
        if (![s->engine startAndReturnError:&error]) {
            Warn("AVAudioEngine failed to start: %s", ToStd(error.localizedDescription).c_str());
            return false;
        }
        AVAudioFormat* out = [s->engine.outputNode outputFormatForBus:0];
        Log("AVAudioEngine started; output %.0f Hz, %u ch", out.sampleRate, out.channelCount);
        s->player.volume = s->volume;

        s->queue = dispatch_queue_create("cai.speech", DISPATCH_QUEUE_SERIAL);
        s->stallTimer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, s->queue);
        dispatch_source_set_timer(s->stallTimer, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC),
                                  NSEC_PER_SEC, NSEC_PER_SEC / 4);
        std::weak_ptr<Stream> stream = s;
        dispatch_source_set_event_handler(s->stallTimer, ^{
            @autoreleasepool {
                if (std::shared_ptr<Stream> live = stream.lock()) OnStallTimer(*live);
            }
        });
        dispatch_resume(s->stallTimer);

        std::shared_ptr<std::atomic<bool>> drained = s->mainQueueDrained;
        dispatch_async(dispatch_get_main_queue(), ^{
            drained->store(true);
            Log("main dispatch queue drained (canary ran%s)", [NSThread isMainThread] ? " on the main thread" : "");
        });

        ReadVoices(*s);
        PickDefaults(*s);
        s->synth = [[AVSpeechSynthesizer alloc] init];
        Log("speech initialized: %zu voices, voice %s, rate %.2f", s->voices.size(),
            s->currentVoiceId.empty() ? "(AVSpeech default)" : s->currentVoiceId.c_str(), s->rate);
        g_stream = s;
        return true;
    }
}

void cai_speech_shutdown(void) {
    std::lock_guard<std::mutex> api(g_api);
    std::shared_ptr<Stream> s = std::move(g_stream);
    if (!s) return;
    @autoreleasepool {
        {
            std::lock_guard<std::mutex> lock(s->mutex);
            s->pending.clear();
            RetireLocked(*s);
            StopPlayer(*s);
            [s->engine stop];
            Reap(*s);
            for (Retired& r : s->retired) g_abandonedSynths.push_back(r.synth);
            if (!s->retired.empty()) Log("%zu render(s) still in flight at shutdown; left to finish on their own", s->retired.size());
            s->retired.clear();
            s->broken = true;
        }
        dispatch_source_cancel(s->stallTimer);
        // Drain the serial queue; a block that locked the stream after this
        // keeps it alive until it returns and finds it broken.
        dispatch_sync(s->queue, ^{});
        Log("speech shut down");
    }
}

void cai_speech_say(const char* utf8, bool interrupt) {
    if (!utf8) return;
    std::lock_guard<std::mutex> api(g_api);
    Stream* s = g_stream.get();
    if (!s) return;
    @autoreleasepool {
        std::string text(utf8);
        bool blank = true;
        for (unsigned char c : text) {
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n') { blank = false; break; }
        }
        std::lock_guard<std::mutex> lock(s->mutex);
        if (interrupt) {
            Log("interrupt: dropping %zu pending line(s)%s", s->pending.size(), s->inFlight ? " and the render in flight" : "");
            s->pending.clear();
            RetireLocked(*s);
            StopPlayer(*s);
        }
        if (blank || s->broken) return;
        int lineNo = ++s->linesEnqueued;
        Log("enqueue line %d (\"%s\")%s", lineNo, text.c_str(), interrupt ? " [interrupt]" : "");
        s->pending.push_back(Pending{ std::move(text), s->currentVoiceId, s->rate });
        StartNextLocked(*s);
    }
}

void cai_speech_stop(void) {
    std::lock_guard<std::mutex> api(g_api);
    Stream* s = g_stream.get();
    if (!s) return;
    @autoreleasepool {
        std::lock_guard<std::mutex> lock(s->mutex);
        StopLocked(*s);
    }
}

bool cai_speech_try_stop(void) {
    std::unique_lock<std::mutex> api(g_api, std::try_to_lock);
    if (!api) return false;
    Stream* s = g_stream.get();
    if (!s) return true;
    @autoreleasepool {
        std::unique_lock<std::mutex> lock(s->mutex, std::try_to_lock);
        if (!lock) return false;
        StopLocked(*s);
    }
    return true;
}

bool cai_speech_is_speaking(void) {
    std::lock_guard<std::mutex> api(g_api);
    Stream* s = g_stream.get();
    if (!s) return false;
    std::lock_guard<std::mutex> lock(s->mutex);
    return s->inFlight != nullptr || !s->pending.empty() || s->play->outstanding.load() > 0;
}

void cai_speech_set_rate(float unit0to1) {
    std::lock_guard<std::mutex> api(g_api);
    Stream* s = g_stream.get();
    if (!s) return;
    float rate = unit0to1 < 0.0f ? 0.0f : unit0to1 > 1.0f ? 1.0f : unit0to1;
    std::lock_guard<std::mutex> lock(s->mutex);
    s->rate = rate;
}

void cai_speech_set_volume(float unit0to1) {
    std::lock_guard<std::mutex> api(g_api);
    Stream* s = g_stream.get();
    if (!s) return;
    float volume = unit0to1 < 0.0f ? 0.0f : unit0to1 > 1.0f ? 1.0f : unit0to1;
    @autoreleasepool {
        std::lock_guard<std::mutex> lock(s->mutex);
        s->volume = volume;
        s->player.volume = volume;
    }
}

bool cai_speech_set_voice(const char* identifier) {
    std::lock_guard<std::mutex> api(g_api);
    Stream* s = g_stream.get();
    if (!s) return false;
    @autoreleasepool {
        std::string id = identifier ? identifier : "";
        if (!id.empty() && ![AVSpeechSynthesisVoice voiceWithIdentifier:[NSString stringWithUTF8String:id.c_str()]]) {
            Warn("set_voice: AVSpeech has no voice %s", id.c_str());
            return false;
        }
        std::lock_guard<std::mutex> lock(s->mutex);
        s->currentVoiceId = id;
        return true;
    }
}

int cai_speech_voice_count(void) {
    std::lock_guard<std::mutex> api(g_api);
    Stream* s = g_stream.get();
    return s ? (int)s->voices.size() : 0;
}

const char* cai_speech_voice_id(int i) {
    std::lock_guard<std::mutex> api(g_api);
    Stream* s = g_stream.get();
    if (!s || i < 0 || i >= (int)s->voices.size()) return nullptr;
    return s->voices[(size_t)i].id.c_str();
}

const char* cai_speech_voice_name(int i) {
    std::lock_guard<std::mutex> api(g_api);
    Stream* s = g_stream.get();
    if (!s || i < 0 || i >= (int)s->voices.size()) return nullptr;
    return s->voices[(size_t)i].name.c_str();
}
const char* cai_speech_voice_language(int i) {
    std::lock_guard<std::mutex> api(g_api);
    Stream* s = g_stream.get();
    if (!s || i < 0 || i >= (int)s->voices.size()) return nullptr;
    return s->voices[(size_t)i].language.c_str();
}

float cai_speech_get_rate(void) {
    std::lock_guard<std::mutex> api(g_api);
    Stream* s = g_stream.get();
    if (!s) return AVSpeechUtteranceDefaultSpeechRate;
    std::lock_guard<std::mutex> lock(s->mutex);
    return s->rate;
}

void cai_speech_get_voice(char* buffer, size_t size) {
    if (!buffer || size == 0) return;
    buffer[0] = '\0';
    std::lock_guard<std::mutex> api(g_api);
    Stream* s = g_stream.get();
    if (!s) return;
    std::lock_guard<std::mutex> lock(s->mutex);
    snprintf(buffer, size, "%s", s->currentVoiceId.c_str());
}

} // extern "C"
