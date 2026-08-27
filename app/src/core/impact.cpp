#include "impact.h"

#include <algorithm>
#include <cmath>

namespace psvr2 {

std::vector<Voice> BuildImpact(const ImpactSpec& spec) {
    const MaterialRecipe r = RecipeFor(spec.material);
    const float trust = 0.55f + 0.45f * std::clamp(spec.confidence, 0.0f, 1.0f);
    const float e = std::clamp(spec.energy, 0.05f, 1.0f) * trust;
    // Energy stretches the effect, but the floor is high: below roughly 100 ms
    // skin cannot resolve pitch and every material collapses back into "a tap".
    const float lenScale = 0.80f + 0.45f * e;
    // A tumbling object does not just hit, it scrapes and rolls, so spin
    // lengthens and roughens the tone rather than changing the transient.
    const float tumble = std::clamp(spec.spin / 600.0f, 0.0f, 1.0f);

    std::vector<Voice> v;
    const int pulses = std::max(1, r.pulses);

    for (int p = 0; p < pulses; ++p) {
        const int delaySamples =
            static_cast<int>(kSampleRate * (r.pulseGapMs * p) / 1000.0f);
        // Repeats decay so a burst reads as one gesture, not three events.
        const float pulseAmp = e * std::pow(0.72f, static_cast<float>(p));

        // Onset: punctuation only. Deliberately quiet - when this dominated,
        // every material just felt like "a tap".
        if (r.onsetAmp > 0.0f) {
            auto onset = Transient(r.onsetHz, r.onsetAmp * pulseAmp,
                                   r.onsetMs, r.onsetMs * 0.6f);
            onset.delay = delaySamples;
            v.push_back(onset);
        }

        // The character. Long enough for skin to resolve the pitch, and
        // modulated where the material calls for it.
        auto tone = Body(r.toneF0, r.toneF1, r.toneAmp * pulseAmp,
                         r.toneMs * lenScale, r.toneDecay * lenScale);
        tone.delay = delaySamples;
        tone.amDepth = r.amDepth;
        tone.amFreq = r.amFreq;
        // Spin roughens the tone directly rather than adding a noise layer.
        if (tumble > 0.2f) {
            tone.fmDepth = 25.0f * tumble;
            tone.fmFreq = 7.0f + 11.0f * tumble;
        }
        v.push_back(tone);
    }

    // Long resonance - only metal and glass have one, and it is a large part of
    // what tells them apart from everything else.
    if (r.ringAmp > 0.0f) {
        auto ring = Body(r.ringHz, r.ringHz * 0.97f, r.ringAmp * e,
                         r.ringMs * lenScale, r.ringDecay * lenScale);
        ring.delay = kSampleRate * 6 / 1000;
        ring.amDepth = r.amDepth * 0.6f;
        ring.amFreq = r.amFreq;
        v.push_back(ring);
    }

    // Grain. Zero for every material except loose ground, whose whole identity
    // it is - see the Dirt recipe. Spin widens it, because dragging through
    // gravel is rougher than dropping into it.
    if (r.grainAmp > 0.0f) {
        auto grain = Texture(r.grainHz, r.grainQ, r.grainAmp * e,
                             r.grainMs * lenScale, r.grainMs * 0.6f * lenScale);
        if (tumble > 0.2f) {
            grain.amDepth = 0.35f * tumble;
            grain.amFreq = 18.0f + 22.0f * tumble;
        }
        v.push_back(grain);
    }

    return v;
}

std::vector<Voice> BuildSympatheticImpact(const ImpactSpec& spec) {
    const float trust = 0.55f + 0.45f * std::clamp(spec.confidence, 0.0f, 1.0f);
    const float e = std::clamp(spec.energy, 0.05f, 1.0f) * trust;
    std::vector<Voice> v;
    if (spec.mass > 5.0f && e > 0.55f) {
        v.push_back(Body(70, 55, 0.22f * e, 150, 95));
    }
    return v;
}

int ImpactTriggerStrength(float energy) {
    return std::clamp(static_cast<int>(std::lround(2 + energy * 6)), 1, 8);
}

int ImpactTriggerPosition(float energy) {
    return energy > 0.7f ? 1 : 3;
}

} // namespace psvr2
