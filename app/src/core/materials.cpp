#include "materials.h"

namespace psvr2 {

// ---------------------------------------------------------------------------
// Material signatures.
//
// Each material is a small layered recipe rather than a gain. The distinctions
// that actually read on a voice coil are: how bright the initial transient is,
// how fast it decays, and whether there is a resonant tail. Glass is bright and
// gone; metal is bright and rings; stone is dull and heavy; rubber has no
// transient at all. Those differences survive at low amplitude, which a pure
// gain difference does not.
// ---------------------------------------------------------------------------

// Recipe structure, second revision.
//
// The first version layered a bright transient, a body, band-passed noise and a
// resonance, all within a few hundred Hz of each other, over 30-130 ms. On this
// hardware that reads as generic buzz: the layers sum into a broadband mush,
// and 50 ms is far too short for skin to resolve pitch at all, so every
// material felt the same.
//
// The --sweep test settled it: single sustained tones at different frequencies
// were immediately and obviously distinguishable, while the layered effects
// were not. So the vocabulary here is deliberately narrow and tonal:
//
//   onset   a brief accent so the effect has an attack. Optional, and kept
//           quiet - it is punctuation, not the character.
//   tone    the DOMINANT voice. Near-pure, long enough to actually perceive
//           (120-400 ms), and each material sits at a well-separated pitch.
//   ring    an optional second tone with a long decay. Only metal and glass.
//   grain   a trace of noise. Mostly zero - noise is what made this feel samey.

// Pitch alone cannot carry eight materials. Vibrotactile pitch discrimination
// is coarse - roughly a 1.5x ratio is needed before two frequencies read as
// different - so 40-500 Hz yields only about five reliable slots:
//
//     ~55      ~130      ~200      ~310      ~470
//
// (80-110 Hz is the measured dip and is used only as a sweep destination.)
//
// So every material is separated on at least TWO axes, and the extra axes are
// ones skin resolves better than pitch:
//
//   material   pitch      length  movement   modulation      reads as
//   glass      470        200 ms  slight     fast shimmer    tinkling
//   plastic    470        110 ms  none       none            sharp clack
//   metal      310        520 ms  none       slow pulse      ringing
//   wood       200        200 ms  slight     none            solid knock
//   cardboard  150        3 hits  none       none            crumple
//   organic    125        340 ms  none       slow wobble     squish
//   stone      190->50    520 ms  huge fall  none            boom
//   rubber     115        90 ms   none       none            dead thud
//
// Glass and plastic deliberately share a pitch: one shimmers and lasts twice as
// long, the other is a bare clack. That contrast is far more legible than the
// third-of-an-octave gap they used to have.
MaterialRecipe RecipeFor(Material m) {
    switch (m) {
        case Material::Glass:
            return {/*onset*/ 470, 0.38f, 9,
                    /*tone */ 470, 430, 0.92f, 200, 90,
                    /*am   */ 0.55f, 42.0f,
                    /*pulse*/ 1, 0,
                    /*ring */ 460, 0.24f, 120, 60};
        case Material::Plastic:
            // Same pitch as glass, half the length, no shimmer at all.
            return {/*onset*/ 470, 0.33f, 8,
                    /*tone */ 470, 450, 0.64f, 110, 38,
                    /*am   */ 0, 0,
                    /*pulse*/ 1, 0,
                    /*ring */ 0, 0, 0, 0};
        case Material::Metal:
            // Longest sustain of anything, with a slow pulse under it.
            return {/*onset*/ 380, 0.33f, 11,
                    // Was 1.02 with a 0.31 ring under it - over full scale
                    // before the ring was even added, so this limited to 0.72
                    // every time. Metal's identity is its LENGTH and its slow
                    // pulse, both of which survive the level coming down.
                    /*tone */ 310, 305, 0.86f, 520, 300,
                    /*am   */ 0.40f, 7.5f,
                    /*pulse*/ 1, 0,
                    /*ring */ 235, 0.24f, 520, 330};
        case Material::Wood:
            return {/*onset*/ 230, 0.36f, 11,
                    /*tone */ 200, 185, 0.92f, 200, 80,
                    /*am   */ 0, 0,
                    /*pulse*/ 1, 0,
                    /*ring */ 0, 0, 0, 0};
        case Material::Cardboard:
            // Three soft hits. The rhythm is the whole signature.
            //
            // Moved down off wood. At 150 Hz it measured 149 Hz against wood's
            // 192 - a 1.29x ratio, inside what skin can separate - and the
            // rhythm that is supposed to carry it is invisible to any measure
            // of pitch and duration. Rhythm is a real difference and it is why
            // this pair was left alone before; but a pair that relies ENTIRELY
            // on an axis the report cannot see has no safety net if the rhythm
            // ever gets flattened by a later edit.
            //
            // Dropping it to 108 Hz cleared wood and immediately landed on
            // organic instead - four of the eight materials had ended up
            // inside 105-118 Hz, all fighting for one perceptual cell at the
            // bottom of the band. The low end simply has no room left, so
            // cardboard cannot be separated by going lower.
            //
            // It is separated by TIME instead, which is the axis this material
            // already had and was not using properly. A cardboard box does not
            // knock - it crushes progressively, and it is the only material
            // here whose whole identity is a sequence rather than an event.
            // Four hits spread over ~370 ms make it the longest thing in the
            // set apart from metal and stone, and no other material is
            // remotely close to that shape.
            //
            // Level comes up either way: at peak 0.22 / rms 0.032 this was the
            // quietest thing in the entire suite, quiet enough to be a real
            // candidate for not being felt at all.
            return {/*onset*/ 0, 0, 0,
                    /*tone */ 160, 148, 0.52f, 80, 40,
                    /*am   */ 0, 0,
                    /*pulse*/ 4, 108,
                    /*ring */ 0, 0, 0, 0};
        case Material::Organic:
            // Raised off stone, which it was sitting on at 114 Hz against
            // 105 Hz. Flesh is not masonry: it should squish higher and stop
            // sooner, where stone booms low and rings on. The slow wobble is
            // what makes it read as soft rather than merely low.
            return {/*onset*/ 0, 0, 0,
                    /*tone */ 125, 115, 0.74f, 200, 105,
                    /*am   */ 0.50f, 4.5f,
                    /*pulse*/ 1, 0,
                    /*ring */ 0, 0, 0, 0};
        case Material::Stone:
            // The largest pitch movement in the set, and the longest fall.
            return {/*onset*/ 200, 0.36f, 15,
                    /*tone */ 190, 50, 0.89f, 520, 300,
                    /*am   */ 0, 0,
                    /*pulse*/ 1, 0,
                    /*ring */ 0, 0, 0, 0};
        case Material::Rubber:
            // Shortest and lowest. No onset, no movement, no modulation.
            return {/*onset*/ 0, 0, 0,
                    /*tone */ 115, 112, 0.56f, 90, 26,
                    /*am   */ 0, 0,
                    /*pulse*/ 1, 0,
                    /*ring */ 0, 0, 0, 0};
        case Material::Dirt:
            // Half-Life 2 only. Soft granular ground - dirt, sand, gravel.
            //
            // Every other material here is identified by a TONE. This one is
            // identified by GRAIN, which is the last unused axis in the set and
            // the reason a tenth class could be added at all without landing on
            // top of one of the nine already placed.
            //
            // Physically that is what loose ground is: nothing rings, nothing
            // rebounds, the energy goes straight into moving particles. So it
            // gets no onset (there is no hard edge to a scuff), the quietest
            // tone of anything, and its character carried almost entirely by a
            // wide band of noise on top.
            //
            // It first sat low - 145 Hz over 130 ms, where the intuition about
            // "soft ground" puts it - and that turned out to be the one place
            // it could not go. The bottom of the band is where rubber (109 Hz),
            // organic (115) and cardboard (123) already live, and it landed
            // inside the discrimination threshold of two of them at once.
            //
            // The measurement forced a better answer than the intuition had.
            // Loose ground does not thud: gravel and sand are a long granular
            // HISS, and the energy is mostly high and mostly noise. So the
            // quiet low thud stays as a floor and the grain carries the
            // signature, which puts it at ~340 ms and ~300 Hz - a cell nothing
            // else occupies, and a more honest description of what a boot or a
            // crate hitting dirt actually feels like.
            return {/*onset*/ 0, 0, 0,
                    /*tone */ 150, 125, 0.22f, 200, 110,
                    /*am   */ 0, 0,
                    /*pulse*/ 1, 0,
                    /*ring */ 0, 0, 0, 0,
                    /*grain*/ 330, 0.75f, 0.52f, 340};
        case Material::Unknown:
        default:
            return {/*onset*/ 280, 0.26f, 10,
                    /*tone */ 250, 235, 0.60f, 190, 85,
                    /*am   */ 0, 0,
                    /*pulse*/ 1, 0,
                    /*ring */ 0, 0, 0, 0};
    }
}

Material ParseMaterial(const std::string& s) {
    if (s == "glass") return Material::Glass;
    if (s == "metal") return Material::Metal;
    if (s == "wood") return Material::Wood;
    if (s == "stone") return Material::Stone;
    if (s == "rubber") return Material::Rubber;
    if (s == "organic") return Material::Organic;
    if (s == "cardboard") return Material::Cardboard;
    if (s == "plastic") return Material::Plastic;
    if (s == "dirt") return Material::Dirt;
    return Material::Unknown;
}

const char* MaterialName(Material m) {
    switch (m) {
        case Material::Glass: return "glass";
        case Material::Metal: return "metal";
        case Material::Wood: return "wood";
        case Material::Stone: return "stone";
        case Material::Rubber: return "rubber";
        case Material::Organic: return "organic";
        case Material::Cardboard: return "cardboard";
        case Material::Plastic: return "plastic";
        case Material::Dirt: return "dirt";
        default: return "unknown";
    }
}


const std::vector<Material>& AllMaterials() {
    static const std::vector<Material> all = {
        Material::Glass, Material::Metal, Material::Wood, Material::Stone,
        Material::Rubber, Material::Organic, Material::Cardboard,
        Material::Plastic, Material::Dirt, Material::Unknown,
    };
    return all;
}

} // namespace psvr2
