// PCM synthesis and mixing for the PSVR2 Sense controllers.
//
// Design notes that drive everything below.
//
// The Sense controllers use wideband voice-coil actuators fed by a 3 kHz,
// 32-sample signed-8-bit PCM stream.
//
// The usable band is NOT set by Nyquist. It was measured on real hardware with
// --sweep (see the response table in haptics.cpp): the actuator peaks around
// 180-300 Hz, has a real dip at 80-110 Hz, and produces nothing at all above
// about 520 Hz. Every oscillator and filter centre is clamped to that measured
// ceiling, and ResponseGain() flattens the curve so a designed amplitude means
// what it says.
//
// This matters more than it sounds. An earlier revision placed the "bright"
// layers of glass, metal and the reload clicks between 560 and 950 Hz - all of
// it inaudible to the hardware - so those effects collapsed to their body
// layers and every material felt like the same dull thud.
//
// A single tactile event is therefore built from layered voices rather than one
// oscillator: a short bright transient for the "edge", a mid-band body for
// weight, band-passed noise for material texture, and optionally a resonant
// tail. That layering is what makes glass read as different from wood, rather
// than just louder or quieter.

#pragma once

#include "capi.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace psvr2 {

// Mix buses exist so a sharp transient can duck sustained texture out of the
// way instead of smearing into it.
enum class Bus : uint8_t {
    Transient, // short, high priority, ducks everything else
    Body,      // the weight of an impact
    Texture,   // noise beds, motor grain, scrapes
    Sustain,   // long tones: heartbeat, healing, levitate
};

struct Voice {
    // --- timing, in samples at kSampleRate ---
    int delay = 0;
    int length = 0;
    int age = 0;

    // --- envelope ---
    // Fast linear attack into an exponential decay. Exponential decay is what
    // makes an impact read as an impact; the linear release used previously
    // gives a synthetic "buzz stops" feel.
    int attack = 3;
    float decayTau = 60.0f; // samples; larger = longer tail
    float hold = 0.0f;      // samples held at full level before decay

    // --- oscillator ---
    float f0 = 200.0f; // Hz at voice start
    float f1 = 200.0f; // Hz at voice end (exponential glide)
    float amp = 0.5f;
    float phase = 0.0f;

    // Roughness: a slow FM wobble. Small depths read as mechanical texture
    // (a motor under load); large depths read as grinding.
    float fmDepth = 0.0f; // Hz of deviation
    float fmFreq = 0.0f;  // Hz of the modulator
    float fmPhase = 0.0f;

    // Amplitude modulation (tremolo).
    //
    // Skin resolves TEMPORAL pattern far better than it resolves pitch, so this
    // is the strongest differentiator available on this hardware. A fast
    // shimmer reads as glass tinkling; a slow pulse reads as metal ringing; no
    // modulation at all reads as a solid knock. Two effects at the same pitch
    // and length are still obviously different if one is modulated.
    float amDepth = 0.0f; // 0 = steady, 1 = full depth
    float amFreq = 0.0f;  // Hz
    float amPhase = 0.0f;

    // --- noise ---
    // 0 = pure tone, 1 = pure band-passed noise. Band-passing matters: raw
    // white noise on a voice coil reads as hiss, not as material.
    float noiseMix = 0.0f;
    float noiseFreq = 400.0f;
    float noiseQ = 1.6f;

    // Diagnostic voices bypass both the 520 Hz design clamp and the response
    // compensation, so --sweep measures the hardware rather than measuring our
    // own correction curve. Never set this on a gameplay effect.
    bool raw = false;

    // --- routing ---
    Bus bus = Bus::Body;
    uint8_t priority = 4; // 0..9, higher survives voice stealing

    // Choke group. 0 = never choked.
    //
    // Submitting a voice in a group fades out any voice already playing in the
    // same group. This is how a repeated gunshot stops SUMMING with its own
    // previous ring-out: without it, rapid fire accumulates energy until the
    // limiter pulls the whole mix down, and the first casualty is the sharp
    // transient that makes a shot read as a shot rather than as mush.
    //
    // Your hand does not feel two overlapping resonances from one gun, so this
    // is closer to the physical truth as well as being kinder to the mix.
    uint8_t group = 0;

    // filter/PRNG state
    float svfLow = 0.0f;
    float svfBand = 0.0f;
    uint32_t rng = 0x9E3779B9u;

    bool done() const { return age >= delay + length; }
    float Next();
};

// Choke groups. Only weapon fire needs one today, but the mechanism is
// general: anything whose repeats should replace rather than accumulate.
constexpr uint8_t kGroupWeaponFire = 1;

// Builders for the layers effects are made of. Times are milliseconds and
// frequencies are Hz, so profile code reads as tactile intent rather than
// sample arithmetic.
Voice Transient(float freq, float amp, float ms, float decayMs);
Voice Body(float f0, float f1, float amp, float ms, float decayMs);
Voice Texture(float centreHz, float q, float amp, float ms, float decayMs);
Voice Tone(float f0, float f1, float amp, float ms, int attackMs);
// Uncompensated, unclamped tone for measurement only.
Voice RawTone(float hz, float amp, float ms, int attackMs);

struct Effect {
    Controller controller = Controller::Right;
    std::vector<Voice> voices;
};

class Mixer {
public:
    explicit Mixer(Capi& capi, bool debug) : capi_(capi), debug_(debug) {}

    void SetMaster(float m);
    void Submit(Controller c, std::vector<Voice> voices);

    void Start();
    void Stop();

    // Offline analysis: renders whatever has been submitted, without touching
    // the CAPI or the mixer thread, and reports what the waveform actually
    // looks like. Exists because tactile values were being tuned by guesswork -
    // this makes "is glass actually louder and brighter than stone" a
    // measurement instead of an opinion.
    struct Analysis {
        float peak = 0.0f;      // 0..1 of full scale
        float rms = 0.0f;       // 0..1
        int   durationMs = 0;   // until it falls below 2% of full scale
        float centroidHz = 0.0f;// dominant frequency, via zero-crossing rate
        float limitMin = 1.0f;  // lowest limiter gain reached (1.0 = never engaged)
    };
    Analysis AnalyzeOffline(Controller c, int maxMs);

    // Streaming offline render, for replaying a whole recorded session.
    //
    // Unlike AnalyzeOffline this does NOT clear the voice lists, so state
    // carries across calls. That is the entire point: a self-test fires one
    // effect at a time, but real play overlaps them constantly - reloading
    // while carrying something, taking a hit mid-burst. Overlap is where the
    // limiter actually gets exercised, and it is invisible to any test that
    // plays effects one at a time.
    struct StreamStats {
        float peak = 0.0f;
        float limitMin = 1.0f;
        double sumSq = 0.0;
        uint64_t samples = 0;
        // How OFTEN the limiter was working, not just how far it once dipped.
        //
        // This is the statistic that actually matters over a session. A brief
        // dip when two big events genuinely coincide is correct - that moment
        // should be loud, and the hand cannot feel twice the energy anyway.
        // Limiting that is engaged much of the time is a different thing
        // entirely: it means everything is riding the ceiling, and the dynamic
        // range the whole design depends on has collapsed.
        uint64_t chunks = 0;
        uint64_t limitedChunks = 0; // chunks whose raw mix exceeded the ceiling
        float rms() const;
        float limitedFraction() const;
    };
    void StepOffline(int chunks, StreamStats& left, StreamStats& right);

    // PCM submission accounting, surfaced by the diagnostics line.
    uint64_t chunksWritten() const { return chunks_.load(); }
    uint64_t writeErrors() const { return writeErrors_.load(); }
    int lastWriteResult() const { return lastRc_.load(); }

private:
    void Run();
    // Returns true when the raw mix for this chunk exceeded the ceiling, i.e.
    // the limiter genuinely had to intervene. That is a far better measure of
    // "is this mix too hot" than the limiter's current value, because the
    // limiter releases slowly on purpose - a single dip keeps its value
    // depressed for over a second afterwards, which would make one loud moment
    // look like sustained clipping.
    bool RenderChunk(std::vector<Voice>& voices, std::array<uint8_t, kChunkSize>& out,
                     float& limiter);

    static constexpr size_t kMaxVoices = 28;

    Capi& capi_;
    bool debug_ = false;
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::mutex mu_;
    float master_ = 1.0f;
    std::vector<Voice> left_;
    std::vector<Voice> right_;
    // Per-controller limiter gain, so one hand cannot duck the other.
    float limitL_ = 1.0f;
    float limitR_ = 1.0f;
    std::atomic<uint64_t> chunks_{0};
    std::atomic<uint64_t> writeErrors_{0};
    std::atomic<int> lastRc_{kResultOk};
};

} // namespace psvr2
