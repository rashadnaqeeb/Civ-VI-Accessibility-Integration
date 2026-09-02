// Stand-alone test driver for the CAI macOS speech stream (speech.mm).
//
// Lists the voices, speaks three short lines queued back to back, then a
// longer sentence that is interrupted 0.5 s into its playback, then five
// times a long sentence interrupted the moment its render starts, which
// retires the render and its synthesizer; each retired render must be reaped
// with both end markers before the next round. About 8 s of audio. Prints the
// stream's timestamped log to stderr and waits on the log itself ("played
// back line N") rather than on fixed sleeps, so the timing of each step is
// measured, not assumed.
//
// Options:
//   --runloop   pump the main CFRunLoop while waiting instead of blocking the
//               main thread in usleep; use it to compare whether AVSpeech's
//               buffer callbacks need the main queue drained
//   --silent    volume 0 (a probe run nobody hears)
//   --voice ID  speak with this AVSpeech voice identifier instead of the
//               Spoken Content voice
//   --voice2 ID switch to this voice for "Line three." once the first two
//               lines have played, so the third line renders in another
//               voice; exercises the player reconnect when the two voices
//               have different native sample rates
//   --list-all  print every installed voice (default prints the count and the
//               first ten)
//
// Build: cmake --build --preset default --target speechtest (not part of the
// default build), then run build/speechtest.

#include <CoreFoundation/CoreFoundation.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>

#include "speech.h"

namespace {

bool g_runloop = false;
std::atomic<int> g_playedBack{0};    // highest "played back line N" seen
std::atomic<int> g_scheduled{0};     // highest "scheduled line N" seen
std::atomic<int> g_reaped{0};        // retired renders reaped so far
std::atomic<int> g_reapedEarly{0};   // of those, reaped with fewer than two end markers
const auto g_start = std::chrono::steady_clock::now();

double ElapsedMs() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g_start).count();
}

void OnLog(cai_speech_log_level level, const char* line) {
    fprintf(stderr, "[%9.1f ms] %s%s\n", ElapsedMs(), level >= CAI_SPEECH_LOG_WARNING ? "warning: " : "", line);
    fflush(stderr);
    int n = 0;
    if (const char* p = strstr(line, "played back line ")) {
        if (sscanf(p, "played back line %d", &n) == 1 && n > g_playedBack.load()) g_playedBack.store(n);
    }
    if (const char* p = strstr(line, "scheduled line ")) {
        if (sscanf(p, "scheduled line %d", &n) == 1 && n > g_scheduled.load()) g_scheduled.store(n);
    }
    if (strstr(line, "reaped a retired render")) {
        g_reaped++;
        if (const char* p = strstr(line, " chunks, ")) {
            if (sscanf(p, " chunks, %d end markers", &n) == 1 && n < 2) g_reapedEarly++;
        }
    }
}

// Wait up to timeoutSeconds for cond() to hold, checking every 10 ms. With
// --runloop the main run loop is pumped in the meantime, which also drains
// the main dispatch queue; otherwise the main thread simply sleeps, so
// anything that needs the main queue cannot run.
template <class Cond>
bool WaitFor(double timeoutSeconds, Cond cond) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(timeoutSeconds);
    while (!cond()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        if (g_runloop)
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, false);
        else
            usleep(10000);
    }
    return true;
}

void Sleep(double seconds) {
    WaitFor(seconds, [] { return false; });
}

void Step(const char* what) {
    fprintf(stderr, "[%9.1f ms] test: %s\n", ElapsedMs(), what);
    fflush(stderr);
}

} // namespace

int main(int argc, char** argv) {
    bool silent = false, listAll = false;
    const char* voice = nullptr;
    const char* voice2 = nullptr;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--runloop")) g_runloop = true;
        else if (!strcmp(argv[i], "--silent")) silent = true;
        else if (!strcmp(argv[i], "--list-all")) listAll = true;
        else if (!strcmp(argv[i], "--voice") && i + 1 < argc) voice = argv[++i];
        else if (!strcmp(argv[i], "--voice2") && i + 1 < argc) voice2 = argv[++i];
        else { fprintf(stderr, "usage: speechtest [--runloop] [--silent] [--list-all] [--voice ID] [--voice2 ID]\n"); return 2; }
    }
    fprintf(stderr, "speechtest: main thread %s pumping the run loop while waiting\n", g_runloop ? "IS" : "is NOT");

    cai_speech_set_log(OnLog);
    Step("init");
    if (!cai_speech_init()) {
        fprintf(stderr, "speechtest: cai_speech_init failed\n");
        return 1;
    }

    int count = cai_speech_voice_count();
    printf("%d voices installed\n", count);
    for (int i = 0; i < count && (listAll || i < 10); i++)
        printf("  %s: %s\n", cai_speech_voice_id(i), cai_speech_voice_name(i));
    if (!listAll && count > 10) printf("  ... (%d more; --list-all prints them)\n", count - 10);
    fflush(stdout);

    if (voice) {
        Step(cai_speech_set_voice(voice) ? "voice set" : "voice NOT found; keeping the default");
    }
    if (silent) cai_speech_set_volume(0.0f);

    Step("queueing three lines");
    cai_speech_say("Line one.", false);
    cai_speech_say("Line two.", false);
    if (voice2) {
        // A voice with another native rate reconnects the player and drops
        // whatever it still holds, so let the first two lines finish first.
        if (!WaitFor(20, [] { return g_playedBack.load() >= 2; })) Step("TIMEOUT waiting for the first two lines to play back");
        Step(cai_speech_set_voice(voice2) ? "second voice set for line three" : "second voice NOT found");
    }
    cai_speech_say("Line three.", false);
    if (WaitFor(20, [] { return g_playedBack.load() >= 3; }))
        Step("all three lines played back");
    else
        Step("TIMEOUT waiting for the three lines to play back");

    Sleep(0.3);
    Step("queueing the long sentence");
    cai_speech_say("This is a longer sentence that goes on for a while so that there is something to interrupt in the middle of it.", false);
    if (!WaitFor(10, [] { return g_scheduled.load() >= 4; }))
        Step("TIMEOUT waiting for the long sentence to be scheduled");
    Sleep(0.5);
    Step("interrupting");
    cai_speech_say("Interrupted.", true);
    if (WaitFor(10, [] { return g_playedBack.load() >= 5; }))
        Step("interruption line played back");
    else
        Step("TIMEOUT waiting for the interruption line to play back");

    // Interrupt a render in flight: the retired synthesizer must stay alive
    // until AVSpeech has delivered both end markers on the main queue, and be
    // released there (a release from the stream's queue after the first
    // marker crashed the game inside TextToSpeech).
    for (int round = 1; round <= 5; round++) {
        Sleep(0.2);
        int reapedBefore = g_reaped.load();
        int playedBefore = g_playedBack.load();
        Step("interrupting a render in flight");
        cai_speech_say("This sentence is interrupted before its render has delivered a single buffer of audio.", true);
        cai_speech_say("Cut off.", true);
        if (!WaitFor(10, [reapedBefore, playedBefore] {
                return g_reaped.load() > reapedBefore && g_playedBack.load() > playedBefore;
            }))
            Step("TIMEOUT waiting for the retired render to be reaped and the replacement to play back");
    }
    if (g_reapedEarly.load() > 0)
        fprintf(stderr, "speechtest: FAILED: %d retired render(s) reaped before their second end marker\n", g_reapedEarly.load());
    else
        Step("every retired render was reaped after its second end marker");

    Sleep(0.2);
    Step("shutdown");
    cai_speech_shutdown();
    fprintf(stderr, "speechtest: done in %.0f ms\n", ElapsedMs());
    return g_reapedEarly.load() > 0 ? 1 : 0;
}
