// Host test for speech_samples.h: synthetic signals through Trim and
// Normalize, checking the edges, the gain cap and the limiter ceiling.
// Build: cmake --build --preset default --target samplestest (not part of the
// default build), then run build/samplestest; a non-zero exit is a failure.
#include "speech_samples.h"
#include <cmath>
#include <cstdio>
#include <vector>

namespace {
using namespace speech::samples;

int g_failures = 0;
void Check(bool ok, const char* what) {
    printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_failures++;
}

constexpr double kRate = 22050;
constexpr size_t kKeep = 110;   // kKeepEdgeSeconds at kRate, rounded

// A cosine, so the first sample is at full amplitude and Trim has a clear edge.
std::vector<float> Tone(size_t count, float amplitude) {
    std::vector<float> v(count);
    for (size_t i = 0; i < count; i++) v[i] = amplitude * (float)std::cos(i * 0.05);
    return v;
}
float Peak(const std::vector<float>& v) {
    float peak = 0;
    for (float s : v) peak = std::max(peak, std::fabs(s));
    return peak;
}
}

int main() {
    // Trim: silence on both sides goes, kKeep samples of it stay on each side.
    {
        std::vector<float> line(1000, 0.0f);
        std::vector<float> tone = Tone(500, 0.5f);
        std::copy(tone.begin(), tone.end(), line.begin() + 250);
        std::vector<float> out = Trim(line.data(), line.size(), kRate);
        Check(out.size() == 500 + 2 * kKeep, "trim keeps the tone plus the edge on each side");
        Check(std::fabs(out[kKeep + 1] - tone[1]) < 1e-6f, "trim keeps the samples aligned");
    }
    // Trim: no leading silence to keep, trailing silence shorter than the edge.
    {
        std::vector<float> line = Tone(300, 0.5f);
        line.resize(350, 0.0f);
        std::vector<float> out = Trim(line.data(), line.size(), kRate);
        Check(out.size() == 350, "trim never reads before the start or past the end");
    }
    // Trim: silence only.
    {
        std::vector<float> line(400, 0.001f);
        Check(Trim(line.data(), line.size(), kRate).empty(), "trim of silence is empty");
    }
    // Normalize: a quiet line is lifted, but by kMaxGain at most.
    {
        std::vector<float> quiet = Tone(4000, 0.02f);
        float gain = Normalize(quiet, kRate);
        Check(gain == kMaxGain, "normalize caps the gain at kMaxGain");
        Check(Peak(quiet) <= kCeiling, "normalize keeps the peak under the ceiling");
    }
    // Normalize: a loud line is brought down to the target and limited.
    {
        std::vector<float> loud = Tone(4000, 0.9f);
        float gain = Normalize(loud, kRate);
        Check(gain < 1.0f, "normalize attenuates a line above the target");
        Check(Peak(loud) <= kCeiling + 1e-4f, "limiter holds the peak under the ceiling");
    }
    // Limit: gain that would clip is held at the ceiling. The look-ahead needs
    // its window of lead-in before the first peak; Trim always leaves more
    // than that (kKeepEdgeSeconds exceeds kLookaheadSeconds).
    {
        std::vector<float> line(200, 0.0f);
        std::vector<float> tone = Tone(4000, 0.9f);
        line.insert(line.end(), tone.begin(), tone.end());
        Limit(line, 2.0f, kRate);
        Check(Peak(line) <= kCeiling + 1e-4f, "limit holds the ceiling under excess gain");
        Check(kKeepEdgeSeconds > kLookaheadSeconds, "trim keeps more edge than the limiter looks ahead");
    }
    // Edge cases.
    {
        std::vector<float> empty;
        Check(Normalize(empty, kRate) == 1.0f, "normalize of nothing is a no-op");
        std::vector<float> zeros(100, 0.0f);
        Check(Normalize(zeros, kRate) == 1.0f, "normalize of silence is a no-op");
    }
    printf("%d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
