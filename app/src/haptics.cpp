#include "haptics.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>

namespace psvr2 {
namespace {

constexpr float kTwoPi = 6.28318530717958647692f;

// ---------------------------------------------------------------------------
// Measured frequency response of the PSVR2 Sense actuator.
//
// Taken with --sweep: fifteen tones from 40 Hz to 800 Hz, all at identical
// amplitude, reported by feel. The result:
//
//     40-60 Hz    fairly strong   (low lobe)
//     80-110 Hz   WEAK            (a real dip)
//     120-180 Hz  very strong
//     180-300 Hz  strongest       (peak)
//     300-500 Hz  fairly strong
//     600 Hz+     not felt at all
//
// Two consequences drive the whole design:
//
//  * The ceiling is ~520 Hz, not Nyquist. An earlier version put glass's
//    transient at 720 Hz, its texture at 950 Hz and its resonance at 620 Hz -
//    every layer that made glass *glass* was above the cutoff, so it collapsed
//    into the same dull thud as everything else. Materials must be separated
//    inside 40-500 Hz, using decay time and layer structure as much as pitch.
//
//  * The response is far from flat, so a designed amplitude did not mean what
//    it said. ResponseGain() compensates, which is why levels here can be read
//    as intent rather than as guesses about the hardware.
// ---------------------------------------------------------------------------

constexpr float kMaxHz = 520.0f;      // measured design ceiling
constexpr float kMaxSafeHz = 1200.0f; // anti-alias ceiling for raw tones

struct ResponsePoint { float hz, strength; };
constexpr ResponsePoint kResponse[] = {
    {  40.0f, 0.70f}, {  60.0f, 0.70f}, {  80.0f, 0.50f}, { 100.0f, 0.52f},
    { 120.0f, 0.90f}, { 150.0f, 0.95f}, { 180.0f, 1.00f}, { 260.0f, 1.00f},
    { 300.0f, 1.00f}, { 350.0f, 0.82f}, { 400.0f, 0.75f}, { 500.0f, 0.68f},
    { 520.0f, 0.60f},
};

// Inverse of the measured curve, so equal amplitudes feel equally strong.
// Clamped so the weak 80-110 Hz dip is lifted without running the limiter.
inline float ResponseGain(float hz) {
    const int n = static_cast<int>(sizeof(kResponse) / sizeof(kResponse[0]));
    float strength = kResponse[n - 1].strength;
    if (hz <= kResponse[0].hz) {
        strength = kResponse[0].strength;
    } else {
        for (int i = 1; i < n; ++i) {
            if (hz <= kResponse[i].hz) {
                const auto& a = kResponse[i - 1];
                const auto& b = kResponse[i];
                const float t = (hz - a.hz) / (b.hz - a.hz);
                strength = a.strength + (b.strength - a.strength) * t;
                break;
            }
        }
    }
    return std::clamp(1.0f / std::max(0.45f, strength), 1.0f, 2.1f);
}

inline int MsToSamples(float ms) {
    return std::max(0, static_cast<int>(ms * kSampleRate / 1000.0f + 0.5f));
}

inline float ClampHz(float hz) { return std::clamp(hz, 8.0f, kMaxHz); }
inline float ClampRawHz(float hz) { return std::clamp(hz, 8.0f, kMaxSafeHz); }

// xorshift32: deterministic, cheap, and does not touch the shared C rand()
// state from the mixer thread.
inline float NextNoise(uint32_t& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return static_cast<float>(static_cast<int32_t>(s)) * (1.0f / 2147483648.0f);
}

} // namespace

float Voice::Next() {
    if (done()) return 0.0f;
    if (age < delay) { ++age; return 0.0f; }

    const int t = age - delay;
    ++age;

    // --- envelope: linear attack, optional hold, exponential decay ---
    float env;
    if (t < attack) {
        env = attack > 0 ? static_cast<float>(t + 1) / static_cast<float>(attack) : 1.0f;
    } else if (static_cast<float>(t - attack) < hold) {
        env = 1.0f;
    } else {
        const float d = static_cast<float>(t - attack) - hold;
        env = std::exp(-d / std::max(1.0f, decayTau));
    }
    // Tremolo. Applied to the envelope so it shapes the whole voice including
    // its noise component.
    if (amDepth > 0.0f && amFreq > 0.0f) {
        env *= (1.0f - amDepth) + amDepth * (0.5f + 0.5f * std::cos(amPhase));
        amPhase += kTwoPi * amFreq / kSampleRate;
        if (amPhase >= kTwoPi) amPhase -= kTwoPi;
    }

    // Fade the last few samples so a truncated tail cannot click.
    const int remaining = (delay + length) - age;
    if (remaining < 8) env *= std::max(0.0f, remaining / 8.0f);

    // --- oscillator: exponential glide f0 -> f1, plus FM roughness ---
    const float x = length > 1 ? static_cast<float>(t) / static_cast<float>(length - 1) : 0.0f;
    float freq = raw ? ClampRawHz(f0 * std::pow(std::max(0.01f, f1 / f0), x))
                     : ClampHz(f0 * std::pow(std::max(0.01f, f1 / f0), x));

    if (fmDepth > 0.0f && fmFreq > 0.0f) {
        freq = ClampHz(freq + fmDepth * std::sin(fmPhase));
        fmPhase += kTwoPi * fmFreq / kSampleRate;
        if (fmPhase >= kTwoPi) fmPhase -= kTwoPi;
    }

    const float tone = std::sin(phase);
    phase += kTwoPi * freq / kSampleRate;
    if (phase >= kTwoPi) phase -= kTwoPi;

    // Compensate per sample rather than per voice, so a sweep that crosses the
    // 80-110 Hz dip (a heavy stone impact settling from 140 Hz down to 55 Hz,
    // for instance) keeps an even perceived weight instead of hollowing out
    // in the middle.
    float sample = raw ? tone : tone * ResponseGain(freq);

    // --- band-passed noise for material texture ---
    if (noiseMix > 0.0f) {
        const float fc = ClampHz(noiseFreq);
        const float f = 2.0f * std::sin(3.14159265f * fc / kSampleRate);
        const float q = 1.0f / std::max(0.5f, noiseQ);
        const float in = NextNoise(rng);
        svfLow += f * svfBand;
        const float high = in - svfLow - q * svfBand;
        svfBand += f * high;
        // Band output, lightly normalised so Q changes do not change level much.
        // Compensated at the filter's centre frequency, since that is where its
        // energy sits.
        const float band = svfBand * std::clamp(noiseQ * 0.9f, 0.5f, 3.0f)
                         * ResponseGain(fc);
        sample = sample * (1.0f - noiseMix) + band * noiseMix;
    }

    return sample * amp * env;
}

// --- layer builders -------------------------------------------------------

// Durations are stated literally in milliseconds by the callers now, so there
// is no blanket multiplier. The decay constants stay a large fraction of the
// stated decay time so the tail survives to the end of the voice rather than
// being truncated to near-silence a third of the way through.
constexpr float kLengthScale = 1.0f;

Voice Transient(float freq, float amp, float ms, float decayMs) {
    Voice v;
    v.bus = Bus::Transient;
    v.priority = 8;
    v.length = MsToSamples(ms * kLengthScale);
    v.attack = std::max(1, MsToSamples(0.7f)); // ~2 samples: near-instant edge
    v.decayTau = std::max(3.0f, static_cast<float>(MsToSamples(decayMs)) * 1.15f);
    v.f0 = freq;
    v.f1 = freq * 0.72f; // slight downward pitch drop reads as "struck"
    v.amp = amp;
    return v;
}

Voice Body(float f0, float f1, float amp, float ms, float decayMs) {
    Voice v;
    v.bus = Bus::Body;
    v.priority = 5;
    v.length = MsToSamples(ms * kLengthScale);
    v.attack = std::max(1, MsToSamples(1.5f));
    v.decayTau = std::max(4.0f, static_cast<float>(MsToSamples(decayMs)) * 1.25f);
    v.f0 = f0;
    v.f1 = f1;
    v.amp = amp;
    return v;
}

Voice Texture(float centreHz, float q, float amp, float ms, float decayMs) {
    Voice v;
    v.bus = Bus::Texture;
    v.priority = 3;
    v.length = MsToSamples(ms * kLengthScale);
    v.attack = std::max(1, MsToSamples(1.0f));
    v.decayTau = std::max(4.0f, static_cast<float>(MsToSamples(decayMs)) * 1.25f);
    v.noiseMix = 1.0f;
    v.noiseFreq = centreHz;
    v.noiseQ = q;
    v.amp = amp;
    return v;
}

Voice RawTone(float hz, float amp, float ms, int attackMs) {
    Voice v = Tone(hz, hz, amp, ms, attackMs);
    v.raw = true;
    return v;
}

Voice Tone(float f0, float f1, float amp, float ms, int attackMs) {
    Voice v;
    v.bus = Bus::Sustain;
    v.priority = 2;
    v.length = MsToSamples(ms);
    v.attack = std::max(1, MsToSamples(static_cast<float>(attackMs)));
    // Sustained voices hold most of their length and decay only at the end.
    v.hold = static_cast<float>(MsToSamples(ms * 0.55f));
    v.decayTau = std::max(8.0f, static_cast<float>(MsToSamples(ms * 0.35f)) * 0.5f);
    v.f0 = f0;
    v.f1 = f1;
    v.amp = amp;
    return v;
}

// --- mixer ----------------------------------------------------------------

void Mixer::SetMaster(float m) {
    std::lock_guard<std::mutex> lk(mu_);
    master_ = std::clamp(m, 0.0f, 2.5f);
}

void Mixer::Submit(Controller c, std::vector<Voice> voices) {
    if (voices.empty()) return;
    std::lock_guard<std::mutex> lk(mu_);

    // Choke first: anything already sounding in an incoming voice's group is
    // faded out rather than left to sum with its own replacement.
    //
    // The fade is eight samples (under 3 ms) - long enough that cutting a
    // voice mid-cycle cannot click, short enough to be inaudible as a gesture.
    // Voice::Next() already ramps the last eight samples down, so moving the
    // end marker is all that is needed.
    auto choke = [](std::vector<Voice>& dst, uint8_t group) {
        if (group == 0) return;
        for (auto& v : dst) {
            if (v.group != group || v.done()) continue;
            const int end = v.age + 8;
            if (end < v.delay + v.length) v.length = end - v.delay;
        }
    };
    for (const auto& v : voices) {
        if (v.group == 0) continue;
        if (c == Controller::Left) choke(left_, v.group);
        else if (c == Controller::Right) choke(right_, v.group);
        else { choke(left_, v.group); choke(right_, v.group); }
    }

    auto add = [this](std::vector<Voice>& dst, const Voice& v) {
        if (dst.size() >= kMaxVoices) {
            // Steal the least important voice: lowest priority, then oldest.
            auto worst = std::min_element(dst.begin(), dst.end(),
                [](const Voice& a, const Voice& b) {
                    if (a.priority != b.priority) return a.priority < b.priority;
                    return a.age > b.age;
                });
            if (worst != dst.end() && worst->priority <= v.priority) *worst = v;
            return;
        }
        dst.push_back(v);
    };

    for (const auto& v : voices) {
        if (c == Controller::Left) add(left_, v);
        else if (c == Controller::Right) add(right_, v);
        else { add(left_, v); add(right_, v); }
    }
}

bool Mixer::RenderChunk(std::vector<Voice>& voices, std::array<uint8_t, kChunkSize>& out,
                        float& limiter) {
    std::array<float, kChunkSize> transient{};
    std::array<float, kChunkSize> body{};
    std::array<float, kChunkSize> texture{};
    std::array<float, kChunkSize> sustain{};

    for (auto& v : voices) {
        auto* dst = &body;
        switch (v.bus) {
            case Bus::Transient: dst = &transient; break;
            case Bus::Body:      dst = &body;      break;
            case Bus::Texture:   dst = &texture;   break;
            case Bus::Sustain:   dst = &sustain;   break;
        }
        for (int i = 0; i < kChunkSize; ++i) {
            if (v.done()) break;
            (*dst)[i] += v.Next();
        }
    }

    voices.erase(std::remove_if(voices.begin(), voices.end(),
                                [](const Voice& v) { return v.done(); }),
                 voices.end());

    // Sidechain: a live transient pushes the sustained and textural layers
    // down so the impact edge stays legible instead of muddying into them.
    float peak = 0.0f;
    for (int i = 0; i < kChunkSize; ++i) peak = std::max(peak, std::fabs(transient[i]));
    const float duck = 1.0f / (1.0f + 2.2f * peak);

    // Gain-riding limiter rather than a fixed tanh drive.
    //
    // A static saturator has to be set for the loudest case, which leaves quiet
    // events tiny, or for the quiet case, which squashes loud ones to ~99% and
    // destroys the transient shape. Either way everything converges on the same
    // perceived level, which is a large part of why effects felt alike. This
    // only pulls gain down when a chunk would actually clip, and lets it back up
    // slowly, so a gentle pickup keeps its small dynamic and a shotgun keeps its
    // edge.
    std::array<float, kChunkSize> mix{};
    float mixPeak = 0.0f;
    for (int i = 0; i < kChunkSize; ++i) {
        mix[i] = (transient[i] + body[i] + texture[i] * duck + sustain[i] * duck) * master_;
        mixPeak = std::max(mixPeak, std::fabs(mix[i]));
    }

    constexpr float kCeiling = 0.97f;
    const float target = mixPeak > kCeiling ? kCeiling / mixPeak : 1.0f;
    // Fast attack so a transient never overshoots, slow release so the level
    // does not pump between chunks.
    const float coeff = (target < limiter) ? 0.45f : 0.015f;
    limiter += (target - limiter) * coeff;
    limiter = std::clamp(limiter, 0.05f, 1.0f);

    // Soft knee, not a blanket saturator.
    //
    // tanh() used to be applied to EVERY sample, which cost 14% of amplitude at
    // 0.8 and 22% at full scale - precisely the peaks that carry an impact -
    // even though the gain-riding limiter above had already guaranteed
    // headroom. It was compressing signals that never needed compressing, and
    // measured on hardware that redundant loss was a large part of why the
    // output felt weak.
    //
    // Below the knee the signal now passes through untouched. Above it a tanh
    // tail still catches intra-chunk overshoot that the per-chunk limiter
    // cannot see, so nothing can clip.
    constexpr float kKnee = 0.75f;
    for (int i = 0; i < kChunkSize; ++i) {
        float y = mix[i] * limiter;
        const float a = std::fabs(y);
        if (a > kKnee) {
            const float over = (a - kKnee) / (1.0f - kKnee);
            y = std::copysign(kKnee + (1.0f - kKnee) * std::tanh(over), y);
        }
        const int s = std::clamp(static_cast<int>(std::lrint(y * 127.0f)), -127, 127);
        out[i] = static_cast<uint8_t>(static_cast<int8_t>(s));
    }
    return mixPeak > kCeiling;
}

Mixer::Analysis Mixer::AnalyzeOffline(Controller c, int maxMs) {
    Analysis a;
    std::vector<Voice>& voices = (c == Controller::Left) ? left_ : right_;
    float& limiter = (c == Controller::Left) ? limitL_ : limitR_;
    limiter = 1.0f;

    const int chunks = std::max(1, maxMs * kSampleRate / (1000 * kChunkSize));
    std::array<uint8_t, kChunkSize> out{};
    std::vector<float> samples;
    samples.reserve(static_cast<size_t>(chunks) * kChunkSize);

    for (int ch = 0; ch < chunks; ++ch) {
        RenderChunk(voices, out, limiter);
        a.limitMin = std::min(a.limitMin, limiter);
        for (int i = 0; i < kChunkSize; ++i) {
            samples.push_back(static_cast<int8_t>(out[i]) / 127.0f);
        }
    }

    double sumSq = 0.0;
    int lastLoud = 0;
    int crossings = 0;
    for (size_t i = 0; i < samples.size(); ++i) {
        const float s = samples[i];
        a.peak = std::max(a.peak, std::fabs(s));
        sumSq += static_cast<double>(s) * s;
        if (std::fabs(s) > 0.02f) lastLoud = static_cast<int>(i);
        if (i > 0 && ((samples[i - 1] < 0.0f) != (s < 0.0f))) ++crossings;
    }
    if (!samples.empty()) a.rms = static_cast<float>(std::sqrt(sumSq / samples.size()));
    a.durationMs = lastLoud * 1000 / kSampleRate;
    // Zero-crossing rate is a good enough dominant-frequency estimate for the
    // near-sine content this synth produces.
    if (lastLoud > 0) {
        a.centroidHz = crossings * 0.5f * kSampleRate / static_cast<float>(lastLoud);
    }

    voices.clear();
    limiter = 1.0f;
    return a;
}

float Mixer::StreamStats::rms() const {
    return samples ? static_cast<float>(std::sqrt(sumSq / static_cast<double>(samples))) : 0.0f;
}

float Mixer::StreamStats::limitedFraction() const {
    return chunks ? static_cast<float>(limitedChunks) / static_cast<float>(chunks) : 0.0f;
}

void Mixer::StepOffline(int chunks, StreamStats& left, StreamStats& right) {
    std::array<uint8_t, kChunkSize> out{};
    for (int ch = 0; ch < chunks; ++ch) {
        for (auto* side : {&left, &right}) {
            const bool isLeft = (side == &left);
            std::vector<Voice>& voices = isLeft ? left_ : right_;
            float& limiter = isLeft ? limitL_ : limitR_;
            const bool over = RenderChunk(voices, out, limiter);
            side->limitMin = std::min(side->limitMin, limiter);
            ++side->chunks;
            if (over) ++side->limitedChunks;
            for (int i = 0; i < kChunkSize; ++i) {
                const float s = static_cast<int8_t>(out[i]) / 127.0f;
                side->peak = std::max(side->peak, std::fabs(s));
                side->sumSq += static_cast<double>(s) * s;
                ++side->samples;
            }
        }
    }
}

void Mixer::Start() {
    running_ = true;
    thread_ = std::thread([this] { Run(); });
}

void Mixer::Stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

void Mixer::Run() {
    std::array<uint8_t, kChunkSize> left{};
    std::array<uint8_t, kChunkSize> right{};
    int consecutiveWaitFailures = 0;

    while (running_) {
        const int waitRc = capi_.WaitForPcm();
        if (!Capi::WaitIsReady(waitRc)) {
            // Driver not ready. Back off rather than spinning, and surface it
            // once rather than every iteration.
            if (++consecutiveWaitFailures == 1 && debug_) {
                std::cout << "[PCM] wait_for_pcm rc=" << waitRc << " ("
                          << ResultName(waitRc) << "), backing off\n";
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        if (consecutiveWaitFailures > 0 && debug_) {
            std::cout << "[PCM] wait_for_pcm recovered after "
                      << consecutiveWaitFailures << " failures\n";
        }
        consecutiveWaitFailures = 0;

        {
            std::lock_guard<std::mutex> lk(mu_);
            RenderChunk(left_, left, limitL_);
            RenderChunk(right_, right, limitR_);
        }

        const int rcL = capi_.WritePcm(Controller::Left, left);
        const int rcR = capi_.WritePcm(Controller::Right, right);
        chunks_ += 2;

        // Only interpret write results on a toolkit build that actually
        // returns them; see Capi::Calibrate.
        if (!capi_.pcm().writeCodesMeaningful) continue;

        // PCM write failures are never swallowed: a silent failure here is
        // indistinguishable from a badly designed effect, which is exactly the
        // ambiguity that made the previous version hard to tune.
        for (auto [rc, name] : {std::pair<int, const char*>{rcL, "LEFT"},
                                std::pair<int, const char*>{rcR, "RIGHT"}}) {
            if (rc != kResultOk) {
                lastRc_ = rc;
                const uint64_t n = ++writeErrors_;
                // Report the first few, then every 500th, so a persistent
                // fault stays visible without flooding the console.
                if (debug_ && (n <= 5 || n % 500 == 0)) {
                    std::cout << "[PCM] controller=" << name << " rc=" << rc
                              << " (" << ResultName(rc) << ") errors=" << n << "\n";
                }
            }
        }
    }
}

} // namespace psvr2
