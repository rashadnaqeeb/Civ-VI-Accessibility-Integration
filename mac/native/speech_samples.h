// Sample-level work on rendered speech: trimming the silence AVSpeech leaves
// around a line, and bringing every line to one loudness with a look-ahead
// peak limiter. Plain C++ with no framework calls and no state between
// calls; speech_samples_test.cpp exercises it on the host. Port of
// SpeechSamples.cs from the OniAccess mod.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace speech::samples {

// Silence kept on each side of the trimmed speech, so a consonant is never clipped.
constexpr double kKeepEdgeSeconds = 0.005;
// Amplitude at or below which a sample counts as silence.
constexpr float kSilenceThreshold = 0.004f;
// The loudness every line is brought to, as RMS amplitude: -10 dBFS. The
// voices render at -17 to -24 dBFS RMS, each at its own level.
constexpr float kTargetRms = 0.316f;
// Peak amplitude the limiter holds the output under.
constexpr float kCeiling = 0.95f;
// The most gain normalization applies, so a near-silent line is not lifted into noise.
constexpr float kMaxGain = 8.0f;
// How far ahead the limiter looks, which is also how long its gain takes to fall before a peak.
constexpr double kLookaheadSeconds = 0.002;
// How long the limiter's gain takes to recover after a peak.
constexpr double kReleaseSeconds = 0.05;

// The speech between the first and last samples above the silence threshold,
// plus kKeepEdgeSeconds either side where the input has it. Empty when nothing
// rises above the threshold.
inline std::vector<float> Trim(const float* samples, size_t count, double sampleRate) {
    size_t start = 0;
    while (start < count && std::fabs(samples[start]) <= kSilenceThreshold) start++;
    if (start == count) return {};
    size_t end = count - 1;
    while (end > start && std::fabs(samples[end]) <= kSilenceThreshold) end--;
    size_t keep = (size_t)std::llround(kKeepEdgeSeconds * sampleRate);
    start = start > keep ? start - keep : 0;
    end = std::min(count - 1, end + keep);
    return std::vector<float>(samples + start, samples + end + 1);
}

// Multiply samples by gain in place, with a look-ahead peak limiter holding
// the result under kCeiling. The limiter's gain ramps down over the look-ahead
// window ahead of a peak, so it is in place by the time the peak arrives, and
// recovers linearly over the release time. Only the peaks are touched; the
// waveform between them is scaled, not clipped.
inline void Limit(std::vector<float>& samples, float gain, double sampleRate) {
    const size_t n = samples.size();
    if (n == 0) return;
    const size_t look = std::max<size_t>(1, (size_t)std::llround(kLookaheadSeconds * sampleRate));
    const float attackStep = 1.0f / (float)look;
    const float releaseStep = (float)(1.0 / std::max(1.0, kReleaseSeconds * sampleRate));

    // Two allocations per line, negligible next to the render that produced it.
    std::vector<float> required(n);
    std::vector<size_t> queue(n);

    // The gain each sample needs on its own to stay under the ceiling.
    for (size_t i = 0; i < n; i++) {
        float peak = std::fabs(samples[i]) * gain;
        required[i] = peak > kCeiling ? kCeiling / peak : 1.0f;
    }

    // Sliding minimum of required over [i, i + look): a monotonic queue of indices.
    size_t head = 0, tail = 0, next = 0;
    float envelope = 1.0f;
    for (size_t i = 0; i < n; i++) {
        size_t windowEnd = std::min(n, i + look);
        while (next < windowEnd) {
            while (tail > head && required[queue[tail - 1]] >= required[next]) tail--;
            queue[tail++] = next++;
        }
        while (queue[head] < i) head++;
        float target = required[queue[head]];
        if (target < envelope)
            envelope = std::max(target, envelope - attackStep);
        else
            envelope = std::min(target, envelope + releaseStep);
        samples[i] = samples[i] * gain * envelope;
    }
}

// Bring samples to kTargetRms in place, through Limit so the peaks stay
// under the ceiling. Silence is left alone. Returns the gain applied.
inline float Normalize(std::vector<float>& samples, double sampleRate) {
    if (samples.empty()) return 1.0f;
    double sumSquares = 0;
    for (float s : samples) sumSquares += (double)s * s;
    float rms = (float)std::sqrt(sumSquares / (double)samples.size());
    if (rms <= 0.0f) return 1.0f;
    float gain = std::min(kMaxGain, kTargetRms / rms);
    Limit(samples, gain, sampleRate);
    return gain;
}

} // namespace speech::samples
