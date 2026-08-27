// Impact synthesis, shared by every game adapter.
//
// An impact is the one signature every game produces and the one that carries
// the most information: what you hit, how hard, how heavy it was and whether
// it was tumbling. Building it in one place is what stops the second game
// re-deriving a worse version of it.
//
// This is a pure function of the physical description - it renders voices and
// returns them. Routing, gain, hand localisation and the trigger jolt stay with
// the adapter, because those are policy and this is physics.

#pragma once

#include "core/haptics.h"
#include "materials.h"

#include <vector>

namespace psvr2 {

// The physical description of a collision, as an adapter can best determine it.
struct ImpactSpec {
    Material material = Material::Unknown;
    // Normalised against a "solid hit" reference rather than a physical unit,
    // because no two engines agree on impulse units. 0.05 .. 1.0.
    float energy = 0.5f;
    // Kilograms. Only used to decide whether the OTHER hand feels it through
    // the body, so a bad value cannot silently reshape the signature.
    float mass = 1.0f;
    // Angular velocity at contact, deg/s. A tumbling object scrapes and rolls
    // rather than simply striking.
    float spin = 0.0f;
    // The adapter's own estimate of whether this collision really happened.
    //
    // Held-object impacts are inferred from a velocity differential in both
    // games, not read from a collision callback. Rather than picking a
    // threshold and pretending everything above it is certain, confidence
    // scales the level: a marginal reading lands as a light knock, a confident
    // one at full weight. The floor is high enough that a real-but-doubtful hit
    // is still clearly felt.
    float confidence = 1.0f;
};

// Renders the impact itself, for the hand that is holding the object.
std::vector<Voice> BuildImpact(const ImpactSpec& spec);

// The sympathetic thump the OTHER hand feels through the body when something
// genuinely heavy lands. Returns empty when the impact does not earn one.
std::vector<Voice> BuildSympatheticImpact(const ImpactSpec& spec);

// Trigger strength for the brief wall that sells the shock through the finger.
// Returned rather than applied so the adapter owns every CAPI call.
int ImpactTriggerStrength(float energy);
int ImpactTriggerPosition(float energy);

} // namespace psvr2
