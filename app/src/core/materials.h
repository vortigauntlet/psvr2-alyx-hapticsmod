// Tactile material vocabulary, shared by every game adapter.
//
// This is the layer that decides what glass feels like as opposed to metal,
// and it is deliberately game-agnostic: an adapter's only job is to classify
// whatever its engine can tell it into one of these coarse classes. Half-Life:
// Alyx infers the class from a model path because Source 2 does not expose
// surface properties to VScript; Half-Life 2 VR reads the real surface
// property out of vphysics. Both end up here.
//
// The recipes moved out of the Alyx router unchanged. They are the result of
// several hardware rounds and the reasoning behind every number is kept with
// it, because those numbers look arbitrary and are not.

#pragma once

#include "core/haptics.h"

#include <string>
#include <vector>

namespace psvr2 {

// Coarse tactile material classes.
//
// The set is small on purpose. Vibrotactile pitch discrimination is coarse -
// roughly a 1.5x ratio before two frequencies read as different - so the usable
// 40-500 Hz band holds about five reliable pitch slots. Every class here is
// therefore separated on at least TWO axes, and the extra axes (length, rhythm,
// modulation) are ones skin resolves better than pitch. Adding a tenth class
// without finding it a free cell would make two of them feel the same.
enum class Material {
    Unknown, Glass, Metal, Wood, Stone, Rubber, Organic, Cardboard, Plastic,
    // Added for Half-Life 2: soft granular ground - dirt, sand, gravel. Alyx
    // never produces it (its classifier has no such class), so adding it
    // cannot change any Alyx signature.
    Dirt,
};

// Parses the lowercase token used on the wire by every adapter.
Material ParseMaterial(const std::string& s);
const char* MaterialName(Material m);

// Every material class, for test suites and reports.
const std::vector<Material>& AllMaterials();

// The layered recipe a material is built from.
//
//   onset   a brief accent so the effect has an attack. Optional, and kept
//           quiet - it is punctuation, not the character.
//   tone    the DOMINANT voice. Near-pure, long enough to actually perceive
//           (120-400 ms), and each material sits at a well-separated pitch.
//   ring    an optional second tone with a long decay. Only metal and glass.
//   pulses  repeats. The whole signature for cardboard.
struct MaterialRecipe {
    float onsetHz, onsetAmp, onsetMs;                 // accent (0 amp = none)
    float toneF0, toneF1, toneAmp, toneMs, toneDecay; // the character
    float amDepth, amFreq;                            // tremolo (0 = steady)
    int   pulses; float pulseGapMs;                   // repeats (1 = single hit)
    float ringHz, ringAmp, ringMs, ringDecay;         // long tail (0 amp = none)

    // Band-passed noise, for a material whose character is GRAIN rather than
    // pitch. Appended with defaults so the nine recipes written before it are
    // untouched and still render byte-identically; only Dirt sets it.
    float grainHz = 0.0f, grainQ = 0.0f, grainAmp = 0.0f, grainMs = 0.0f;
};

MaterialRecipe RecipeFor(Material m);

} // namespace psvr2
