#include "router.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <map>
#include <sstream>

namespace psvr2 {
namespace {

// Label for the held "locked on" trigger detent. Shared between the lock, stop
// and catch handlers, which is exactly why it is a named constant: a typo in
// one of the three would silently leave the trigger loaded until the backstop
// timeout, and nothing would report it.
constexpr const char* kGloveHold = "glove-hold";

// Label for the persistent "you are holding something heavy" trigger load.
// Refreshed by a heartbeat from the game script and cleared the moment the
// object leaves the hand.
constexpr const char* kGripLoad = "grip-load";

// The heartbeat arrives every 250 ms. The overlay outlives it by enough to
// bridge a dropped line or a frame hitch, but not so long that letting go
// leaves the trigger loaded for a noticeable time.
constexpr int kGripLoadMs = 900;

std::vector<std::string> Split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    std::istringstream ss(s);
    while (std::getline(ss, cur, sep)) out.push_back(cur);
    return out;
}

float ToFloat(const std::string& s, float fallback) {
    try { return std::stof(s); } catch (...) { return fallback; }
}

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
struct MaterialRecipe {
    float onsetHz, onsetAmp, onsetMs;               // accent (0 amp = none)
    float toneF0, toneF1, toneAmp, toneMs, toneDecay; // the character
    float amDepth, amFreq;                          // tremolo (0 = steady)
    int   pulses; float pulseGapMs;                 // repeats (1 = single hit)
    float ringHz, ringAmp, ringMs, ringDecay;       // long tail (0 amp = none)
};

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
        case Material::Unknown:
        default:
            return {/*onset*/ 280, 0.26f, 10,
                    /*tone */ 250, 235, 0.60f, 190, 85,
                    /*am   */ 0, 0,
                    /*pulse*/ 1, 0,
                    /*ring */ 0, 0, 0, 0};
    }
}

// ---------------------------------------------------------------------------
// Persistent weapon trigger mechanics.
//
// Differentiation comes from using genuinely different trigger MODES, not from
// nudging the numbers of one mode. The previous build gave the pistol and the
// shotgun the same WEAPON mode at the same strength with endPosition 6 vs 7,
// which is indistinguishable in the hand.
// ---------------------------------------------------------------------------

// The shotgun eased by one notch, for shots fired in quick succession.
//
// Working a heavy pump twice in a row does not take the same effort the second
// time - the action is already part-cycled and the shooter is already braced.
// Dropping 8 to 7 is small, but it is the difference between a deliberate shot
// and a hurried one.
TriggerCommand ShotgunBaseRapid() {
    return trig::MultiFeedback({0, 3, 4, 5, 6, 6, 6, 6, 6, 0});
}

TriggerCommand WeaponBase(const std::string& w) {
    if (w == "PISTOL") {
        // WEAPON mode, not MultiFeedback.
        //
        // The pistol and the shotgun were both MultiFeedback with a wall in
        // roughly the same place, differing only in numbers - which is the
        // "nudge one mode" failure this file criticises for PCM, committed on
        // the trigger. Mode 2 exists precisely to model a firearm: it resists
        // through a band and then GIVES WAY. That break is a categorically
        // different sensation from a static resistance curve, and it is what
        // makes a pistol read as a pistol rather than as a stiff trigger.
        //
        // Strength 6, the MIDDLE rung. It sat at 8 alongside the shotgun's 8,
        // so the two heaviest weapons had identical resistance and there was
        // no hierarchy to feel at all - only the SMG differed. Resistance now
        // ladders 8 / 6 / 4 across shotgun / pistol / SMG, two notches apart
        // at each step. The band is widened a little so the resistance is
        // present for more of the pull before it breaks.
        return trig::Weapon(2, 6, 4);
    }
    if (w == "SHOTGUN") {
        // Deliberately NOT a clean break. A shotgun is a long, heavy pull that
        // loads progressively and never snaps - so it keeps MultiFeedback, now
        // heavier and rising across nearly the whole travel. Against the
        // pistol's crisp break the contrast is a difference in kind.
        //
        // The HEAVIEST resting profile of the lot, heavy almost from the start
        // rather than ramping gently into it - a shotgun should feel like work
        // through the whole pull, not just at the end. The drop to 0 at full
        // travel is still the break.
        //
        // Note this is the RESTING pull, which is a separate thing from how
        // hard the weapon KICKS. The shotgun rests at 7 and kicks at 8/8 - it
        // is not the stiffest trigger to hold, it is the one that hits back
        // hardest. Those two were conflated for several revisions.
        return trig::MultiFeedback({0, 4, 5, 6, 7, 7, 7, 7, 7, 0});
    }
    if (w == "SMG") {
        // Light single-stage, but not weightless. At position 2 / strength 3
        // there was almost nothing under the finger, so the recoil chatter had
        // nothing to push against and read as a buzz in empty air. Held a
        // little deeper and firmer, it still stays comfortable for sustained
        // automatic fire - which is the reason it is the lightest of the three
        // guns rather than an accident.
        //
        // The LIGHTEST of the three at strength 4. It went to 5 when the
        // recoil chatter had nothing to push against, but that was before the
        // kick layer was understood - the chatter is a vibration and does not
        // need a wall behind it. 4 keeps something under the finger while
        // leaving the SMG clearly the easiest to hold down, which is what it
        // needs to be given it is the one weapon fired continuously.
        return trig::Feedback(3, 7);
    }
    if (w == "GRENADE") {
        // Continuously increasing tension as the arm winds up - but the
        // LIGHTEST resting profile of anything. The ladder runs 4 / 5 / 6 / 8
        // across grenade, pistol, SMG, shotgun - the shotgun cannot rise with
        // the rest because 8 is the top of the scale, so raising the other
        // three compresses the spread toward it rather than shifting it.
        return trig::Slope(0, 9, 1, 4);
    }
    if (w == "TOOL") {
        return trig::Feedback(5, 2);
    }
    if (w == "MELEE") {
        // No trigger mechanism to model - a crowbar is a grip, so the
        // resistance sits deep in the travel and never breaks.
        return trig::Feedback(6, 4);
    }
    return trig::Off();
}

// Momentary recoil applied on top of the persistent profile.
//
// The SMG uses MULTIPLE_POSITION_VIBRATION (mode 6) rather than the single
// Vibration mode: it gives an independent amplitude at each of the ten trigger
// positions, so the chatter builds as the trigger is held deeper instead of
// being one flat buzz. That is the difference between automatic fire feeling
// like a mechanism cycling and feeling like a rumble motor.
// Trigger vibration frequency is the difference between a KICK and a BUZZ.
//
// The trigger motor cannot physically move in and out at a weapon's fire rate,
// so recoil has to be a vibration - but the frequency decides what that
// vibration reads as. Around 30 Hz the finger feels individual mechanical
// thumps; by 100 Hz they have fused into a smooth electrical buzz that does
// not read as a mechanism at all.
//
// Hardware confirmed this by accident. The shotgun was the only weapon low
// enough to feel mechanical (42 Hz) and it was the only one reported as
// distinct; the SMG sat at 130 Hz and was reported as "not moving with it"
// despite firing correctly with rc=0 every shot. Pistol at 95 Hz was in
// between and read as "the same as the shotgun".
//
// Everything now lives in the 28-55 Hz band where the trigger reads as
// machinery, and the weapons separate by RATE and DEPTH rather than by pitch.
// Recoil is a LADDER, and it needs headroom at the top to be one.
//
//   weapon    amp  freq   length   reads as
//   pistol     5    55 Hz    55 ms  a light crisp kickback
//   smg      4-8    30 Hz   110 ms  a running mechanical rattle
//   shotgun    8    24 Hz   190 ms  a heavy slam that settles
//
// The pistol and the shotgun were both sitting at amplitude 8 of 8 - the
// ceiling - so the shotgun had nowhere left to go and could not possibly read
// as "the most". Dropping the pistol to 5 is what buys the shotgun its room.
//
// All three separate on THREE axes at once: strength, length, and rate. Rate
// matters as much as strength here - the trigger motor cannot move in and out
// at a weapon's fire rate, so recoil has to be vibration, and only below about
// 40 Hz does that vibration read as machinery rather than as electrical buzz.
// The shotgun's 24 Hz over 190 ms is roughly four heavy pulses: one shove and
// a settle, rather than a single flat tone.
// ---------------------------------------------------------------------------
// Measured response of the adaptive trigger motor.
//
// Taken with --trigger-sweep on real hardware, one variable at a time, with the
// trigger held half-pressed so there was travel in both directions. This is the
// trigger's equivalent of the grip actuator's frequency table, and until it
// existed every trigger value in this file was reasoned about by analogy.
//
//   FREQUENCY   10-30 Hz reads as genuine KICKBACK against the finger.
//               40-50 Hz starts becoming vibration instead. Past 60 Hz it is
//               only vibration. Best: ~20 Hz.
//
//   STRENGTH    Usable and clearly distinct across the whole 0-8 range. Unlike
//               the grip, it does not plateau - so amplitude is a real design
//               axis here and the recoil ladder can be built on it.
//
//   MODE        Feedback, Weapon and Slope all genuinely RESIST the finger.
//               SLOPE resists hardest of all. Vibration modes do not resist;
//               they shake. So a kick must be built from Slope, and only the
//               settle afterwards should be vibration.
//
//   POSITION    A wall only exists AHEAD of the finger. Feedback from the start
//               and Feedback halfway in feel clearly different, which is why a
//               full-strength "shove" parked at position 1 did nothing while
//               the trigger was held down past it.
//
// Two earlier designs died on these facts: the frequencies were far too high to
// kick, and the shove was Feedback parked behind the finger where there was
// nothing to push against.
// ---------------------------------------------------------------------------

// Stage one of recoil: the resistance LOAD.
//
// Deliberately not called the kick any more, because the recoil bench settled
// which layer does what and it was not this one.
//
// Six candidate designs were played back to back on hardware. The two that
// differed only by the presence of a 16 Hz vibration told the whole story:
// Weapon mode alone (260 ms) registered as nothing, and the same thing with
// the vibration added was the clear winner. So the VIBRATION is the kick, and
// the resistance mode is a load the kick pushes against.
//
// It still matters - a plain hard wall on its own was rated "ok" while Weapon
// mode on its own was rated nothing - so the load stays. It is simply not the
// impulse, and sizing it as though it were is what produced three consecutive
// redesigns that felt like nothing.
//
// Duration is short on purpose: a 90 ms Slope beat the same Slope at 260 ms.
// Long resistance stops reading as an event and starts reading as a trigger
// that has gone stiff. The shotgun is the one exception, at the 260 ms proven
// by the winning candidate.
//
// ms = 0 means no load, the right answer for anything that is not a firearm
// going off in the hand.
TriggerCommand RecoilKick(const std::string& w, int& ms) {
    // THE BREAK - the resistance vanishing, not more of it.
    //
    // The bench that chose this design ran with no resting profile at all, so
    // its winning candidate went from a slack trigger to a loaded one: a large
    // change, and change is what the finger reads. In the game a weapon's
    // resting profile is already heavy, so the same overlay was heavy-to-
    // differently-heavy. Kickback was present but underwhelming, which is
    // exactly what a small delta feels like.
    //
    // Inverting it restores the contrast AND is what a trigger physically
    // does: you pull against the wall, it BREAKS and goes slack, and only then
    // does the weapon jolt. Firing now drops the trigger to nothing for a few
    // tens of milliseconds before the kick arrives underneath.
    //
    // The Weapon-mode load this replaces was never carrying the sensation
    // anyway - on the bench it scored "nothing" on its own, and its only real
    // effect here was masking the kick that follows it.
    if (w == "SHOTGUN") { ms = 50; return trig::Off(); }
    if (w == "PISTOL")  { ms = 35; return trig::Off(); }
    // Per round, so automatic fire stutters the trigger open and shut.
    if (w == "SMG")     { ms = 25; return trig::Off(); }
    if (w == "GRENADE") { ms = 40; return trig::Off(); }
    ms = 0;
    return trig::Off();
}

// Stage two of recoil: THE KICK ITSELF.
//
// This is the layer that actually does the work. The recoil bench proved it by
// elimination - the same resistance load with and without this vibration was
// the difference between "nothing" and "the best one" - and it matches the
// trigger sweep, where 10-30 Hz was reported as genuine kickback and anything
// past 60 Hz as mere vibration.
//
// So the weapon ladder is built HERE, on strength and duration at 16-20 Hz,
// and the resistance load above is scaled to match rather than the other way
// round. Three previous designs had this backwards.
//
// Each of these must outlast its load so it is still running when the load
// expires - that reveal is what makes the pair read as one gesture.
//
// Position 0 throughout, so the effect exists across the whole travel and does
// not vanish depending on how far the trigger happens to be pressed.
// How long the kick must OUTLAST its load.
//
// The load sits at a higher priority, so while it is alive it completely masks
// the kick underneath. Only the remainder - kick duration minus load duration -
// is ever felt. That remainder is the entire sensation, so it is the number
// that has to be designed, not the total.
//
// The winning bench candidate ran a 260 ms load against a 490 ms kick: a 230 ms
// reveal, about four cycles at 16 Hz. The first shipped version paired a 260 ms
// load with a 300 ms kick - a 40 ms reveal, under two thirds of ONE cycle - and
// was reported as the worst of every trigger test, with no kickback at all.
// The layers were right and the arithmetic was wrong.
//
// A check that the kick merely outlasts the load is not enough and was exactly
// the check that passed on the broken build. What matters is that it outlasts
// it by enough cycles to be felt.
constexpr int kMinRevealMs = 140;

// Recoil vibration never runs at full drive.
//
// Measured with the trigger held hard against its stop: strength 6-7 pushes
// back BETTER than 8/8. At maximum drive the motor stalls against the stop and
// its force goes into the controller body instead of the finger - felt as a
// buzz somewhere in the grip rather than a shove.
//
// That is the opposite of the intuition this file ran on for several revisions.
// The shotgun and SMG were pinned at 8/8 precisely because they were meant to
// be the heaviest, which made them the two weapons that stopped pushing when
// the trigger was bottomed - and the SMG is the one weapon players genuinely
// DO hold bottomed out.
//
// Weight therefore comes from RATE and LENGTH, never from the last notch of
// strength. 16 Hz and 24 Hz both measured well; 16 for heavy, 24 for light.
// The stall was measured with the trigger held HARD AGAINST ITS STOP. That is
// how the SMG is fired and nothing else - a shotgun or pistol is pulled and
// released, so its trigger is moving through travel where the motor has room
// to work and full drive is not necessarily wasted.
//
// So the cap applies to sustained fire only. Pulse weapons set their own drive
// and may deliberately use 8.
constexpr int kMaxSustainedDrive = 7;
bool IsSustainedFire(const std::string& w) { return w == "SMG"; }

// The recoil ladder, spread the way the waveform ladder had to be.
//
// Reported as "much better, but they all feel a bit samey again" - and the
// numbers said exactly that. Pistol and SMG sat at 155 ms vs 150 ms reveal,
// 24 Hz vs 20 Hz, drive 6 vs 7: inside the ~1.5x discrimination ratio on ALL
// THREE axes at once, which makes them the same effect to a finger.
//
// The same trap that made the gunshot waveforms feel alike, committed again on
// the trigger, because the collision check built for one layer was never
// applied to the other.
//
// Drive cannot fix it - the stall caps everything at 6-7, a 1.17x span.
//
// Rate turned out to have more room than first measured. 30-40 Hz was later
// reported as still usable, which widens the band from ~12-28 Hz to ~12-40 and
// takes the rate axis from barely 1.5x end to end to nearly 3x.
//
//   pistol   120 ms  20 Hz   a solid snap
//   smg      130 ms  35 Hz   fast chatter, chains into a running brrrt
//   shotgun  320 ms  16 Hz   a slow heavy heave
//
// The pistol briefly sat at 85 ms and drive 6 - the weakest on BOTH axes at
// once - purely to hold it clear of the SMG on duration, and it was reported
// as too weak. With the SMG moved to the top of the rate band that separation
// no longer has to come from length, so the pistol takes back its energy and
// is told apart by rate instead (20 Hz against 35 Hz is 1.75x).
//
// It also gets 20 Hz specifically, which measured as the best-feeling rate on
// this hardware - a reasonable thing to spend on the weapon fired most.
//
// The SMG takes the top of the band deliberately. An automatic weapon's
// trigger character IS chatter rather than individual thumps, its shots chain
// at ~10/sec so the rate is what the burst reads as, and it is the one weapon
// held bottomed out - where the motor cannot produce discrete kicks anyway, so
// leaning into fast oscillation plays to what the hardware can still do there.
//
// Duration steps 85 -> 130 -> 320 (1.53x, 2.46x) and rate now steps 16 -> 24
// -> 35 (1.5x, 1.46x). Every adjacent pair clears the threshold on at least
// one axis, and the extremes clear it on both.
TriggerCommand FireOverlay(const std::string& w, int& ms) {
    // One hard low pulse, and deliberately almost no oscillation.
    //
    // Reveal durations are set from CYCLE COUNT, not from milliseconds.
    //
    // Hardware pinned the boundary precisely: at 1.26 cycles the shotgun was
    // reported as "two bursts", at 0.84 as one. So a single-pulse weapon wants
    // to land just UNDER one complete cycle - that delivers the most energy
    // the pulse can carry without beginning a second one.
    //
    // One cycle is 1000/rate ms, so the target moves with the rate rather than
    // being a fixed number:
    //
    //   shotgun  12 Hz ->  83.3 ms per cycle -> 50 ms = 0.60 cycles
    //   pistol   28 Hz ->  35.7 ms per cycle -> 35 ms = 0.98 cycles
    //
    // The pistol at 0.98 is a COMPLETE cycle - push and full return - which at
    // a lower rate was the "needs to feel tighter" complaint. It should be
    // fine here precisely because of the rate: one cycle at 28 Hz lasts 36 ms,
    // fast enough to fuse into a single crisp tick, where the same shape at
    // 20 Hz took 50 ms and could be felt travelling out and back as a wobble.
    // Worth watching, since it is the one value sitting on that boundary.
    //
    // RATE IS SPENT ON THE CONFUSABLE PAIR. The pistol and shotgun are both
    // single pulses of similar length, so rate is nearly all that separates
    // them; the pistol and SMG cannot be confused at any rate, being a 27 ms
    // pulse against a 267 ms rattle. Pushing the pistol to the top of the band
    // takes pistol/shotgun from 1.6x to 2.8x and costs only pistol/SMG
    // proximity, which nothing depends on.
    //
    // The shotgun sits at 0.60 cycles: past the peak, cut partway through the
    // return. 10 Hz is the measured FLOOR of the kickback band and risked
    // reading vague; 15 Hz would have pushed it to 0.75 and handed back the
    // wobble the truncation exists to remove. 12 Hz also keeps the widest rate
    // gap from the pistol - 2.33x against 1.87x at 15 Hz.
    //   smg      30 Hz -> deliberately many cycles; it is a rattle, not a pulse
    //
    // The SHOTGUN is the one held to a single pulse. Cut at 0.72 of a cycle it
    // ends during the return stroke, and that abrupt stop is what makes it a
    // punch rather than a wobble - the push is the hit; the return is not.
    //
    // The pistol follows the same shape at its own scale. It sat at 0.90
    // cycles - a full push AND most of the return - and was reported as
    // needing to feel tighter. That return stroke is the trigger travelling
    // back out, which reads as a wobble rather than a hit; cutting before it
    // completes is precisely what makes the shotgun punchy. Both weapons now
    // deliver the push and stop.
    //
    // They stay distinct by rate and by how far past the peak each is cut:
    // the shotgun at 0.42 is a bare shove, the pistol at 0.60 carries slightly
    // more of the stroke before it ends.
    //
    // The shotgun sat at 80 ms = 1.12 cycles: over the line, a full pulse plus
    // a stub of a second. That is the same stutter that was disliked at 1.26,
    // just smaller. The pistol sat at 40 ms = 0.84, a pulse cut short before
    // it completed - 46 ms lets it finish without doubling.
    //
    // Full drive: the stall only bites when the trigger is held at its stop,
    // which is how the SMG is fired and not how a shotgun is.
    if (w == "SHOTGUN") { ms = 50 + 50;  return trig::Vibration(0, 8, 12); }
    // Back to the low end of the band and to full usable drive.
    //
    // 22 Hz / 115 ms / drive 6 was reported as way too weak. That change had
    // moved rate, length AND drive at once from a version that felt good, so
    // nothing could be attributed - this restores all three toward it.
    if (w == "PISTOL")  { ms = 35 + 35;  return trig::Vibration(0, 7, 28); }
    if (w == "SMG") {
        // Automatic fire. At ~10 rounds/sec these chain continuously, so the
        // burst becomes one running rattle rather than separate taps - which
        // is this weapon's real identity, more than any single number here.
        // Tops out at 7 because this is the weapon held bottomed out, and 8 is
        // where the motor gives up.
        // 65 ms was cut purely to satisfy a separation threshold and was
        // reported as WAY too short. Length is this weapon's substance;
        // separation from the pistol comes from rate and from the fact that
        // these chain into a continuous rattle, which no ratio can see.
        ms = 25 + 267;
        return trig::MultiVibration(22, {4, 5, 6, 6, 7, 7, 7, 7, 7, 7});
    }
    // The grenade has NO trigger recoil, deliberately.
    //
    // Throwing something is not a discharge - nothing recoils, so there is no
    // impulse for the trigger to represent. It only ever had one because every
    // other weapon did.
    //
    // Raising the separation threshold to 1.75x made that impossible to ignore.
    // The usable rate band is about 14-30 Hz, barely 2.1x end to end, so it
    // holds only two well-separated slots - and with duration carrying the
    // rest, a fourth entry could not be fitted without colliding with one of
    // the three real firearms. The honest fix was to stop pretending a throw
    // is a gunshot. Its PCM release effect is untouched.
    if (w == "MELEE")   { ms = 240;      return trig::Vibration(0, 7, 16); }
    ms = 150;
    return trig::Vibration(0, 6, 20);
}

} // namespace

Material ParseMaterial(const std::string& s) {
    if (s == "glass") return Material::Glass;
    if (s == "metal") return Material::Metal;
    if (s == "wood") return Material::Wood;
    if (s == "stone") return Material::Stone;
    if (s == "rubber") return Material::Rubber;
    if (s == "organic") return Material::Organic;
    if (s == "cardboard") return Material::Cardboard;
    if (s == "plastic") return Material::Plastic;
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
        default: return "unknown";
    }
}

float Router::Jitter(float amount) {
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    const float u = static_cast<float>(rng_ & 0xFFFFFF) / 16777215.0f; // 0..1
    return 1.0f + (u * 2.0f - 1.0f) * amount;
}

void Router::Vary(std::vector<Voice>& voices) {
    for (auto& v : voices) {
        const float pitch = Jitter(0.055f);
        v.f0 *= pitch;
        v.f1 *= pitch;
        v.amp *= Jitter(0.08f);
        const float stretch = Jitter(0.11f);
        v.length = std::max(1, static_cast<int>(v.length * stretch));
        v.decayTau *= stretch;
        if (v.hold > 0.0f) v.hold *= stretch;
    }
}

void Router::ResetRateLimits() {
    lastImpact_[0] = Clock::time_point{};
    lastImpact_[1] = Clock::time_point{};
    // Re-seed the per-instance variation so every self-test case renders the
    // SAME instance every time.
    //
    // Without this the measurements are not reproducible. Vary() moves length
    // by up to +/-11% and pitch by +/-5.5%, drawn from one RNG stream that
    // runs across the whole suite - so inserting a test case shifts the draw
    // for every case after it, and the entire table moves. Adding the two
    // bracing tests changed glove-pull from 463 ms to 408 ms without a single
    // value in its profile being touched.
    //
    // That matters because the collision report compares ratios against a
    // 1.5x threshold. An 11% swing on each of two effects is enough to move a
    // pair across that line in either direction, so pairs would appear and
    // disappear between runs for no reason connected to the design - and any
    // effort spent chasing one of those is spent chasing noise.
    //
    // The variation stays in the signal path, so what is measured is still a
    // real instance of what ships. It is just always the SAME instance.
    rng_ = 0x2545F491u;
}

Controller Router::SideFromParam(const std::string& s) const {
    if (s == "left") return Controller::Left;
    if (s == "right") return Controller::Right;
    return primary_;
}

void Router::Emit(Controller c, std::vector<Voice> voices, const std::string& event) {
    // Nothing is felt from behind a pause menu.
    if (menuOpen_) return;
    const float g = std::clamp(cfg_.GainFor(event) * cfg_.GainForWeapon(weapon_), 0.0f, 3.0f);
    if (g <= 0.0f) return;
    if (g != 1.0f) for (auto& v : voices) v.amp *= g;
    Vary(voices);
    // Recorded after every early return above, so the trace reflects what was
    // actually submitted rather than what was intended.
    if (c == Controller::Left || c == Controller::Both) emitTrace_ |= 1;
    if (c == Controller::Right || c == Controller::Both) emitTrace_ |= 2;
    mixer_.Submit(c, std::move(voices));
}

void Router::SetWeapon(const std::string& w) {
    if (w == weapon_) return;
    weapon_ = w;
    RefreshWeaponState();
    if (cfg_.debug) std::cout << "[Weapon] " << weapon_ << "\n";
}

void Router::RefreshWeaponState() {
    // Any weapon change drops the eased shotgun profile.
    shotgunRapid_ = false;
    triggers_.SetBase(primary_, WeaponBase(weapon_), weapon_);
    // The support hand gets a light brace only when actually gripping the
    // weapon two-handed; otherwise it stays free for the gravity glove.
    if (twoHand_ && (weapon_ == "SHOTGUN" || weapon_ == "SMG" ||
                     weapon_ == "PISTOL" || weapon_ == "MELEE")) {
        triggers_.SetBase(Other(), trig::Feedback(3, 2), "brace");
    } else {
        triggers_.SetBase(Other(), trig::Off(), "free");
    }
}

void Router::Fire(const std::string& w, bool twoHand, int roundsLeft) {
    // Recoil in two stages: a SHOVE, then a settle.
    //
    // Vibration mode drives the trigger's own motor, so it genuinely moves
    // against the finger - but it oscillates, and an oscillation is a rattle
    // rather than a kick. A real kick needs the resistance to CHANGE abruptly:
    // slam a hard wall into a shallow position and the trigger drives back
    // toward it against the finger, one distinct shove.
    //
    // So stage one is that wall, at a higher priority and a short duration.
    // When it expires the settle underneath is revealed - already running,
    // because both were pushed at the same instant. The finger feels a punch
    // and then the weapon shaking itself out, which is the shape of recoil.
    //
    // The shove is what makes the shotgun feel like a shotgun; the settle is
    // what makes automatic fire feel like a mechanism.
    // Shots in quick succession ease the shotgun's resting resistance.
    //
    // The base is only rewritten when the state actually FLIPS. Rewriting a
    // persistent trigger profile on every shot is what made every weapon feel
    // alike in an earlier revision, and it would do so again here.
    if (w == "SHOTGUN") {
        const auto now = Clock::now();
        const bool rapid = (now - lastShotgunFire_) < std::chrono::milliseconds(700);
        lastShotgunFire_ = now;
        if (rapid != shotgunRapid_) {
            shotgunRapid_ = rapid;
            triggers_.SetBase(primary_,
                              rapid ? ShotgunBaseRapid() : WeaponBase("SHOTGUN"),
                              rapid ? "SHOTGUN-rapid" : "SHOTGUN");
        }
    } else if (shotgunRapid_) {
        // Switched away mid-burst; do not leave the eased profile behind.
        shotgunRapid_ = false;
    }

    int loadMs = 0;
    const TriggerCommand load = RecoilKick(w, loadMs);
    int ms = 45;
    TriggerCommand kick = FireOverlay(w, ms);

    // Clamp the kick's drive, whatever a profile asked for.
    //
    // Full drive measurably pushes back WORSE than 6-7 once the trigger is
    // bottomed out, because the motor stalls against the stop. Enforcing it
    // here rather than trusting each entry means a future "make it stronger"
    // edit cannot silently make it weaker in the hand.
    if (IsSustainedFire(w)) {
        if (kick.mode == kTriggerVibration) {
            kick.data.vibration.amplitude =
                std::min<uint8_t>(kick.data.vibration.amplitude,
                                  static_cast<uint8_t>(kMaxSustainedDrive));
        } else if (kick.mode == kTriggerMultiPositionVibration) {
            for (auto& a : kick.data.multiVibration.amplitude) {
                a = std::min<uint8_t>(a, static_cast<uint8_t>(kMaxSustainedDrive));
            }
        }
    }

    // The reveal window is the whole sensation, so guard it here rather than
    // trusting the two tables to stay in step. Shipping a 40 ms reveal once
    // already cost a full test round.
    const int reveal = ms - loadMs;
    if (loadMs > 0 && reveal < kMinRevealMs) {
        if (cfg_.debug) {
            std::cout << "[Trigger] " << w << " reveal is only " << reveal
                      << " ms; clamping the load so the kick can be felt\n";
        }
        loadMs = std::max(0, ms - kMinRevealMs);
    }

    if (loadMs > 0) {
        triggers_.PushOverlay(primary_, load, loadMs, 7, w + "-break");
    }
    triggers_.PushOverlay(primary_, kick, ms, 6, w + "-fire");

    // Each weapon gets a clearly separated pitch and duration. The onset is
    // punctuation only; the sustained tone is what you actually identify.
    //
    // The shapes now come from the profile table, so they can be retuned in a
    // text file and heard immediately rather than needing a rebuild. The
    // built-in values are unchanged - see profiles.cpp.
    static const std::map<std::string, std::string> kFireProfile = {
        {"SHOTGUN", "SHOTGUN_FIRE"}, {"PISTOL", "PISTOL_FIRE"},
        {"SMG", "SMG_FIRE"},         {"GRENADE", "GRENADE_FIRE"},
        {"MELEE", "MELEE_FIRE"},
    };
    const auto it = kFireProfile.find(w);
    std::vector<Voice> v =
        profiles_.Build(it == kFireProfile.end() ? "DEFAULT_FIRE" : it->second);
    // A new shot replaces the previous shot's tail instead of summing with it.
    // Offline replay of a real firefight showed repeated fire driving the
    // limiter to 0.53, and the first thing a limiter takes is the transient -
    // so rapid fire was losing exactly the edge that identifies it.
    for (auto& voice : v) voice.group = kGroupWeaponFire;
    Emit(primary_, std::move(v), "FIRE");

    // Running dry. The count is inferred, so this only ever *adds* a cue to a
    // shot that already happened - it can never suppress one.
    if (roundsLeft == 0) {
        // "That was the last one."
        //
        // This was inaudible, and for two compounding reasons: a 480 Hz tick
        // and a 40 ms body at amplitude 0.20 is barely any energy to begin
        // with, and it fired 55 ms in - underneath the blast's own 520 ms body,
        // which buries it completely.
        //
        // It now waits until the shot has largely decayed and lands as a
        // hollow, dead clack well below the blast: low, late and toneless is
        // what "empty" feels like, and being late is what makes it separable
        // from the shot at all.
        // Timing is the whole problem here. At 150 ms the cue sat inside the
        // blast's body and drove the limiter to 0.72; at 230 ms it was still
        // fighting it, because the transient sidechain ducks texture and
        // sustain but NOT body, so a sustained blast and a new body layer sum
        // at full weight.
        //
        // 310 ms is past the point where the shot has decayed enough to leave
        // room. That is not a compromise - "the shot, then a beat, then a dead
        // clack" is what running dry actually feels like, and the pause is
        // what makes it read as separate information rather than as part of
        // the shot.
        //
        // Trimmed once more when the blast itself came down from 0.89 to 0.76:
        // the cue is judged against the shot it follows, not in isolation, so
        // it can give up the same proportion and read identically while
        // stopping the pair from summing past the ceiling. It was still the
        // one signature in the suite limiting below 0.75.
        std::vector<Voice> dry;
        auto tick = Transient(300, 0.40f, 10, 6);
        tick.delay = kSampleRate * 335 / 1000;
        dry.push_back(tick);
        auto hollow = Body(195, 150, 0.37f, 130, 70);
        hollow.delay = kSampleRate * 337 / 1000;
        dry.push_back(hollow);
        Emit(primary_, std::move(dry), "FIRE");
        // The trigger goes slack behind the shot: nothing left to break.
        triggers_.PushOverlay(primary_, trig::Feedback(1, 2), 320, 5, "empty");
    }

    if (twoHand) {
        // The support hand feels the frame, not the action: duller and quieter.
        std::vector<Voice> s;
        s.push_back(Body(w == "SHOTGUN" ? 100.0f : 170.0f,
                         w == "SHOTGUN" ? 60.0f : 140.0f,
                         w == "SHOTGUN" ? 0.42f : 0.26f,
                         w == "SHOTGUN" ? 220.0f : 110.0f,
                         w == "SHOTGUN" ? 130.0f : 55.0f));
        Emit(Other(), std::move(s), "FIRE");
    }
}

void Router::Impact(Material m, float energy, Controller side, float mass, float spin,
                    float confidence) {
    const MaterialRecipe r = RecipeFor(m);
    // A held-object impact is inferred from a velocity differential, not read
    // from a collision callback, so the game side reports how sure it is.
    // Rather than picking a threshold and pretending everything above it is
    // certain, confidence scales the level: a marginal reading lands as a
    // light knock, a confident one lands at full weight. The floor is high
    // enough that a real-but-doubtful hit is still clearly felt.
    const float trust = 0.55f + 0.45f * std::clamp(confidence, 0.0f, 1.0f);
    const float e = std::clamp(energy, 0.05f, 1.0f) * trust;
    // Energy stretches the effect, but the floor is high: below roughly 100 ms
    // skin cannot resolve pitch and every material collapses back into "a tap".
    const float lenScale = 0.80f + 0.45f * e;
    // Angular velocity (deg/s) at the moment of contact. A tumbling object
    // does not just hit, it scrapes and rolls, so spin lengthens and roughens
    // the texture layer rather than changing the transient.
    const float tumble = std::clamp(spin / 600.0f, 0.0f, 1.0f);

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

    // A genuinely heavy object also loads the opposite hand through the body.
    // (`far` is a legacy MSVC keyword macro, hence the name.)
    if (mass > 5.0f && e > 0.55f) {
        const Controller opposite =
            (side == Controller::Left) ? Controller::Right : Controller::Left;
        std::vector<Voice> sympathetic;
        sympathetic.push_back(Body(70, 55, 0.22f * e, 150, 95));
        Emit(opposite, std::move(sympathetic), "PHYS_IMPACT");
    }
    Emit(side, std::move(v), "PHYS_IMPACT");

    // A brief trigger wall sells the shock through the finger.
    const int strength = std::clamp(static_cast<int>(std::lround(2 + e * 6)), 1, 8);
    triggers_.PushOverlay(side, trig::Feedback(e > 0.7f ? 1 : 3, strength), 55, 4, "impact");
}

void Router::Handle(const std::string& event, const std::string& param) {
    const auto parts = Split(param, ',');
    auto arg = [&](size_t i) -> std::string {
        return i < parts.size() ? parts[i] : std::string{};
    };

    // ---- session / state ----
    if (event == "MENU") {
        // The game pauses behind its menu. A weapon's trigger resistance is a
        // physical claim about something the player is holding right now, so
        // leaving it loaded while they sit in a menu is just an unexplained
        // stiff trigger. Release it, and restore the exact same state on the
        // way out - which the persistent-base design makes free.
        menuOpen_ = (param == "1");
        if (menuOpen_) triggers_.Reset();
        else RefreshWeaponState();
        return;
    }
    if (event == "PRIMARY_HAND") {
        primary_ = (param == "left") ? Controller::Left : Controller::Right;
        RefreshWeaponState();
        return;
    }
    if (event == "WEAPON") {
        SetWeapon(arg(0).empty() ? "HANDS" : arg(0));
        return;
    }
    if (event == "WEAPON_RAW") {
        // Unrecognised weapon token. Reported so profiles can be extended
        // against real data instead of guessed classnames.
        if (cfg_.debug) std::cout << "[Weapon] unmapped token: " << param << "\n";
        return;
    }
    if (event == "TWO_HAND_START" || event == "TWO_HAND_END") {
        twoHand_ = (event == "TWO_HAND_START");
        if (!param.empty()) weapon_ = param;
        RefreshWeaponState();
        // Bringing the support hand onto the foregrip. A real two-handed
        // contact event - both hands are on the same object - and one you
        // perform constantly, since bracing is how any serious shot is taken.
        //
        // It was 55 ms of a single Body voice, the same shape and the same
        // too-short duration as the rest of the world table before it was
        // rebuilt. This one hid longer than the others because it lives up in
        // the weapon-state branch rather than down among the world events.
        std::vector<Voice> v;
        if (twoHand_) {
            // The hand arrives and the weapon STEADIES. The settle underneath
            // is the point: what bracing actually changes is that the thing
            // stops moving, so the cue resolves downward into stillness
            // instead of ending on an edge.
            v.push_back(Transient(320, 0.26f, 9, 6));
            v.push_back(Body(230, 178, 0.46f, 145, 88));
        } else {
            // Letting go. Lighter, shorter, and rising as the hand leaves -
            // the mirror of the arrival, the same way a throw mirrors a catch.
            v.push_back(Body(215, 275, 0.30f, 95, 55));
        }
        Emit(Controller::Both, std::move(v), event);
        return;
    }

    // ---- combat ----
    if (event == "FIRE") {
        const std::string w = arg(0).empty() ? weapon_ : arg(0);
        if (w != weapon_ && w != "NONE") SetWeapon(w);
        const bool two = arg(2) == "2H" || twoHand_;
        // Inferred rounds remaining; -1 when the game side cannot know.
        const int roundsLeft = static_cast<int>(ToFloat(arg(3), -1.0f));
        Fire(weapon_, two, roundsLeft);
        return;
    }
    if (event == "HURT") {
        // Damage is one of the very few whole-player events that earns a
        // haptic: a real hit jolts the arms, not just the torso. But it has to
        // be sized by the HIT, not by the health bar - scaling by remaining
        // health, as this used to, meant identical impacts felt different
        // depending on a number on your wrist. That is a HUD readout, and it
        // is the same objection that removed the kill cue.
        const float damage = ToFloat(arg(1), 0.0f);
        // Chip damage - a headcrab graze, standing in something unpleasant -
        // should not interrupt whatever your hands are actually doing.
        if (damage > 0.0f && damage < cfg_.minDamage) return;
        // Sized against a hit worth flinching at rather than against max health.
        const float d = std::clamp(damage / 35.0f, 0.30f, 1.0f);
        std::vector<Voice> v = profiles_.Build("HURT");
        for (auto& voice : v) voice.amp *= d;
        Emit(Controller::Both, std::move(v), "HURT");
        // Only a genuinely heavy hit gets the trigger jolt as well.
        if (d > 0.55f) {
            triggers_.PushOverlay(Controller::Both, trig::Vibration(4, 5, 55), 90, 5, "hurt");
        }
        if (cfg_.debug) {
            std::cout << "[Hurt] damage=" << damage << " scale=" << d << "\n";
        }
        return;
    }
    // KILL is deliberately absent. See the note in the game script: the event
    // fires for every entity that dies anywhere, and a kill is not something a
    // hand physically feels. It was a HUD cue in haptic clothing.

    // ---- gravity gloves ----
    // These are the signature interaction of the game and get the most
    // deliberately shaped signatures of anything here.
    if (event == "GLOVE_LOCK_START") {
        const auto side = SideFromParam(param);
        // Acquisition: a small bright click plus a wall right at the top of
        // trigger travel, so the lock is felt in the finger as well as the palm.
        triggers_.PushOverlay(side, trig::Feedback(1, 6), 95, 5, "glove-lock");
        // ...and then the lock HOLDS. Being locked on is a state, not an
        // instant: the earlier build let the sensation expire after 95 ms while
        // the glove was still very much locked on, which is what made the
        // glove read as four unrelated buzzes instead of one continuous
        // gesture. This lighter detent sits underneath until the game says the
        // lock ended, with a generous backstop so a missed stop event cannot
        // strand the trigger.
        triggers_.PushOverlay(side, trig::Feedback(2, 3), 8000, 3, kGloveHold);
        std::vector<Voice> v = profiles_.Build("GLOVE_LOCK");
        Emit(side, std::move(v), "GLOVE_LOCK_START");
        return;
    }
    if (event == "GLOVE_LOCK_STOP") {
        const auto side = SideFromParam(param);
        triggers_.ClearOverlays(side, kGloveHold);
        std::vector<Voice> v;
        v.push_back(Body(360, 300, 0.30f, 110, 48));
        Emit(side, std::move(v), "GLOVE_LOCK_STOP");
        return;
    }
    if (event == "GLOVE_PULL") {
        // Now "side,mass,material" when the game could identify the target,
        // still bare "side" when it could not - so the side comes from arg(0)
        // rather than the whole parameter string.
        const auto side = SideFromParam(arg(0));
        const float mass = std::clamp(ToFloat(arg(1), -1.0f), -1.0f, 40.0f);
        // Most props in Alyx are light, so the useful resolution is at the
        // bottom of the range. -1 means the game could not identify the target.
        const float heft = mass < 0.0f ? 0.35f
                         : std::clamp(std::sqrt(mass / 14.0f), 0.05f, 1.0f);

        // Tension scales with what is on the end of it. Reeling in a hazmat
        // crate should load the finger noticeably harder than a resin chip,
        // and that difference is the reason the mass is now read at all.
        const int endStrength = std::clamp(static_cast<int>(std::lround(3 + heft * 5)), 3, 8);
        triggers_.PushOverlay(side, trig::Slope(0, 8, 2, endStrength), 210, 6, "glove-pull");

        // A long RISING sweep. Pitch movement is the most legible gesture this
        // hardware can produce, and nothing else in the game rises like this.
        //
        // Mass moves where the sweep STARTS: something heavy begins its climb
        // far lower and takes longer about it, which reads as more inertia
        // being overcome rather than simply as more amplitude.
        std::vector<Voice> v = profiles_.Build("GLOVE_PULL");
        if (!v.empty() && mass >= 0.0f) {
            for (auto& voice : v) {
                voice.f0 *= (1.20f - 0.55f * heft);
                voice.amp *= (0.80f + 0.45f * heft);
                const float stretch = 0.85f + 0.40f * heft;
                voice.length = std::max(1, static_cast<int>(voice.length * stretch));
                voice.decayTau *= stretch;
            }
        }
        Emit(side, std::move(v), "GLOVE_PULL");
        if (cfg_.debug && mass >= 0.0f) {
            std::cout << "[Glove] pull mass=" << mass << " heft=" << heft << "\n";
        }
        return;
    }
    if (event == "GLOVE_CATCH") {
        const auto side = SideFromParam(param);
        // Arrival resolves the lock, so the held detent ends here too.
        triggers_.ClearOverlays(side, kGloveHold);
        // Arrival: the sharpest event in the game.
        triggers_.PushOverlay(side, trig::Feedback(0, 8), 90, 7, "glove-catch");

        // The mirror of the pull: a hard bright arrival that FALLS fast. The
        // pull rises, the catch drops - the pair is unmistakable.
        //
        // This is the CAPTURE only. What was actually caught is not known yet:
        // the object lands in the hand a tick or two later and arrives as
        // GLOVE_CATCH_MASS, which layers the weight in underneath. Playing the
        // snap immediately keeps the latency that makes a catch feel like a
        // catch, while still letting a crate and a bottle end up different.
        //
        // The snap is deliberately BRIGHT AND SHORT, and no longer sweeps down
        // into the low lobe. It used to run 470 -> 95 Hz over 340 ms, which put
        // it in the same band as the weight layer that follows: the two summed,
        // drove the limiter to 0.52, and the squashing flattened a heavy catch
        // back towards feeling like a light one. Splitting them by band means
        // they layer instead of fight - the capture up top, the mass beneath.
        // A catch with no object resolved still lands as a firm snap; it just
        // has no heft under it, which is honest, because nothing arrived.
        std::vector<Voice> v = profiles_.Build("GLOVE_CATCH");
        Emit(side, std::move(v), "GLOVE_CATCH");
        return;
    }
    if (event == "GLOVE_CATCH_MASS") {
        // The weight of what was just caught, landing inside the snap's own
        // envelope. Without this every catch felt identical no matter what was
        // on the end of it, which for the signature interaction of the game is
        // the single biggest thing that gave it away as a generic rumble mod.
        const float mass = std::clamp(ToFloat(arg(0), 1.0f), 0.05f, 40.0f);
        const auto side = SideFromParam(arg(1));
        const Material m = ParseMaterial(arg(2));
        const float spin = ToFloat(arg(3), 0.0f);

        // Heft curve. Most props in Alyx are under 5 kg, so the useful
        // resolution lives at the bottom of the range rather than spread
        // linearly to 40.
        const float heft = std::clamp(std::sqrt(mass / 12.0f), 0.06f, 1.0f);

        std::vector<Voice> v;
        // Settle: heavier means lower, longer and stronger. A 0.3 kg bottle
        // barely registers; a 20 kg crate drops into the low lobe and sits
        // there. This is the layer that carries mass.
        //
        // Levels here are constrained by something measurable rather than
        // chosen by feel: this lands on top of the catch snap, which already
        // peaks near full scale, and --analyze showed the first attempt
        // driving the limiter to 0.52. That squashes the snap's edge and
        // pushes a heavy catch back towards sounding like everything else -
        // the exact failure this layer exists to prevent. Mass is therefore
        // carried by PITCH and DURATION, which cost no headroom, and only
        // modestly by level. The delay keeps it clear of the snap's attack.
        // Mass now moves the settle across the WHOLE band rather than the
        // bottom third of it. It used to run 210 Hz down to 80: a light object
        // landed at ~186 Hz for 145 ms, directly underneath the snap's own
        // body, so the two merged and a light catch measured the same as a
        // catch with nothing in it at all.
        //
        // Spanning 335 Hz down to 70 puts a light arrival UP where the snap
        // has already fallen past, and a heavy one down in the low lobe. The
        // length ladder widens with it, so the two ends of the range differ in
        // pitch and duration together rather than in level.
        const float f0 = 335.0f - 265.0f * heft;
        auto settle = Body(f0, f0 * 0.55f, 0.10f + 0.23f * heft,
                           205.0f + 295.0f * heft, 105.0f + 155.0f * heft);
        settle.delay = kSampleRate * 22 / 1000;
        // What arrival actually DOES is different at the two ends of the
        // range, and it is a difference of rhythm rather than of level.
        //
        // Something heavy does not stop cleanly - it loads, sags and settles,
        // which is a slow wobble. Something light has no authority to plant
        // itself: it clatters into the palm and is still moving when it gets
        // there, which is a fast shimmer.
        //
        // This is the axis that carries the light end. A 0.4 kg bottle and an
        // unresolved catch sit close in pitch and length no matter how the
        // numbers are arranged - catching something nearly weightless really
        // is almost the bare capture - but tremolo separates them outright,
        // and skin reads temporal pattern far better than it reads pitch.
        if (heft > 0.45f) {
            settle.amDepth = 0.30f * heft;
            settle.amFreq = 5.0f + 4.0f * heft;
        } else {
            settle.amDepth = 0.42f * (1.0f - heft);
            settle.amFreq = 26.0f - 14.0f * heft;
        }
        v.push_back(settle);

        // A short, quiet colour tick at the material's own pitch, so you can
        // feel WHAT you caught and not just how heavy it was. Deliberately
        // faint: it identifies, it does not announce.
        const MaterialRecipe r = RecipeFor(m);
        auto colour = Body(r.toneF0, r.toneF1, 0.11f + 0.09f * heft, 90, 45);
        colour.amDepth = r.amDepth;
        colour.amFreq = r.amFreq;
        colour.delay = kSampleRate * 10 / 1000;
        v.push_back(colour);

        // A caught object still spinning grinds as it beds into the grip.
        if (spin > 150.0f) {
            const float tumble = std::clamp(spin / 700.0f, 0.0f, 1.0f);
            v.push_back(Texture(330, 1.4f, 0.10f * tumble, 70 + 60 * tumble, 45));
        }
        Emit(side, std::move(v), "GLOVE_CATCH_MASS");

        // And the trigger takes the load, proportional to what is now in the
        // hand, so the weight persists past the moment of arrival.
        const int s = std::clamp(static_cast<int>(std::lround(1 + heft * 6)), 1, 7);
        triggers_.PushOverlay(side, trig::Feedback(4, s),
                              220 + static_cast<int>(420 * heft), 3, "catch-heft");
        if (cfg_.debug) {
            std::cout << "[Catch] " << MaterialName(m) << " mass=" << mass
                      << " heft=" << heft << " spin=" << spin << "\n";
        }
        return;
    }

    // ---- physics ----
    if (!cfg_.physics && event.rfind("PHYS_", 0) == 0) return;

    if (event == "PHYS_PICKUP") {
        const float mass = std::clamp(ToFloat(arg(1), 1.0f), 0.05f, 40.0f);
        const auto side = SideFromParam(arg(2));
        const float spin = ToFloat(arg(4), 0.0f);
        const float heft = std::clamp(0.15f + mass / 14.0f, 0.15f, 0.85f);
        std::vector<Voice> v;
        // Closing a hand around an object is the single most frequent
        // hand-contact event in the game, and it measured 55 ms at rms 0.019 -
        // the quietest thing in the whole suite by a wide margin, and squarely
        // in the range the hardware round found delivers no felt energy at all.
        //
        // It has to stay small: it fires constantly, and inflating it would
        // raise the noise floor every deliberate effect competes against. But
        // the same round settled how to make a small thing land - keep the
        // accent for character, and give the body enough TIME at 150-300 Hz to
        // be integrated. Roughly tripling the duration at a modest level costs
        // far less headroom than raising amplitude would, and is what the
        // reload clicks needed for exactly the same reason.
        //
        // Placed LOW and quick, which also keeps it clear of the holster pair
        // it shares a moment with - you pick something up and stow it in one
        // motion, so those three have to be mutually legible. A grip closing
        // on an object is duller and faster than a reach into a holster, so
        // the cell it belongs in is the one it can physically justify.
        v.push_back(Transient(285, 0.16f * heft + 0.06f, 8, 5));
        v.push_back(Body(172, 138, 0.36f * heft + 0.07f, 118, 70));
        // Picking up something already spinning has a faint grinding quality.
        v.push_back(Texture(380, 1.3f,
                            0.14f * heft * (1.0f + std::clamp(spin / 900.0f, 0.0f, 1.0f)),
                            60, 40));
        Emit(side, std::move(v), "PHYS_PICKUP");
        // Heavier objects load the trigger so you can feel what you are holding.
        if (mass > 1.5f) {
            const int s = std::clamp(static_cast<int>(std::lround(1 + mass * 0.8f)), 1, 7);
            triggers_.PushOverlay(side, trig::Feedback(4, s), 260, 3, "heft");
        }
        return;
    }
    if (event == "DOOR_MOVE") {
        if (!cfg_.doors) return;
        // A door under your hand: not an impact, a sustained load.
        //
        // This is the one place a low continuous texture is justified, because
        // pushing a heavy door genuinely IS a continuous sensation - the hand
        // feels the mass and the hinge the whole time it is moving. It stops
        // the instant the door does, because the game only reports a door that
        // is actually turning under a moving hand.
        const auto side = SideFromParam(arg(0));
        const float spin = std::clamp(ToFloat(arg(1), 0.0f), 0.0f, 400.0f);
        const float mass = std::clamp(ToFloat(arg(2), 40.0f), 1.0f, 400.0f);

        const float heavy = std::clamp(std::sqrt(mass / 120.0f), 0.15f, 1.0f);
        const float rate = std::clamp(spin / 160.0f, 0.0f, 1.0f);

        std::vector<Voice> v;
        // Low, rough and quiet: a hinge taking load, not an event.
        auto grind = Body(58.0f + 30.0f * heavy, 54.0f + 26.0f * heavy,
                          0.06f + 0.16f * heavy * (0.4f + 0.6f * rate), 230, 150);
        // The roughness rate follows how fast the door is swinging, so the
        // sensation tracks the movement rather than idling at a fixed buzz.
        grind.amDepth = 0.6f;
        grind.amFreq = 7.0f + 16.0f * rate;
        grind.fmDepth = 6.0f * heavy;
        grind.fmFreq = 23.0f;
        v.push_back(grind);
        Emit(side, std::move(v), "DOOR_MOVE");

        // A heavy door also loads the hand that is pushing it.
        if (heavy > 0.45f) {
            const int s = std::clamp(static_cast<int>(std::lround(1 + heavy * 3)), 1, 4);
            triggers_.RefreshOverlay(side, trig::Feedback(5, s), 420, 2, "door-load");
        }
        return;
    }
    if (event == "PHYS_HOLD") {
        // Weight, for as long as you are actually carrying it.
        //
        // Two channels, doing two different jobs:
        //   the TRIGGER holds a static load proportional to mass - the constant
        //   fact of the thing being heavy;
        //   the PCM adds inertia only while the object is genuinely being
        //   MOVED - the part your hand feels as a swing loads and unloads.
        //
        // Standing still holding a crate produces no PCM whatsoever. That gate
        // is the whole reason this is not just ambient buzzing, which would
        // raise the noise floor every other effect has to compete against.
        const float mass = std::clamp(ToFloat(arg(0), 0.0f), 0.0f, 40.0f);
        const auto side = SideFromParam(arg(1));
        const float handSpeed = ToFloat(arg(3), 0.0f);
        const float spin = ToFloat(arg(4), 0.0f);
        const float heft = std::clamp(std::sqrt(mass / 20.0f), 0.0f, 1.0f);

        // Deliberately capped at 5 of a possible 8, and sitting mid-travel.
        // Guerrilla found on Horizon that trigger values set "relatively high"
        // produced finger fatigue within minutes; this one is held for as long
        // as the player carries something, which is exactly the case that
        // warning applies to. Weight is carried by being CONSTANT, not by
        // being strong.
        const int strength = std::clamp(static_cast<int>(std::lround(1 + heft * 4)), 1, 5);
        triggers_.RefreshOverlay(side, trig::Feedback(4, strength), kGripLoadMs, 3, kGripLoad);

        // Inertia. A heavy thing being swung loads the hand in a slow, low
        // wobble; a light thing moved quickly barely registers. Below the
        // motion gate this emits nothing at all.
        const float swing = std::clamp((handSpeed - 55.0f) / 300.0f, 0.0f, 1.0f);
        if (swing > 0.0f && heft > 0.25f) {
            const float drive = swing * heft;
            auto wobble = Body(64.0f + 26.0f * heft, 58.0f + 22.0f * heft,
                               0.05f + 0.20f * drive, 240, 150);
            // The wobble rate rises with how fast it is being swung, so the
            // sensation tracks the gesture instead of running at a fixed idle.
            wobble.amDepth = 0.55f;
            wobble.amFreq = 5.0f + 7.0f * swing;
            if (spin > 200.0f) {
                // Something heavy also rotating grinds against the grip.
                wobble.fmDepth = 9.0f * std::clamp(spin / 800.0f, 0.0f, 1.0f);
                wobble.fmFreq = 17.0f;
            }
            std::vector<Voice> v{wobble};
            Emit(side, std::move(v), "PHYS_HOLD");
        }
        return;
    }
    if (event == "PHYS_THROW") {
        const float speed = std::clamp(ToFloat(arg(0), 0.0f), 0.0f, 2000.0f);
        const float mass = std::clamp(ToFloat(arg(1), 1.0f), 0.05f, 40.0f);
        const auto side = SideFromParam(arg(2));
        const float spin = ToFloat(arg(4), 0.0f);
        const Material m = ParseMaterial(arg(3));
        // The hand is empty the instant it lets go, so the grip load goes now
        // rather than waiting for the heartbeat to lapse. Releasing a heavy
        // object should feel like relief, and a trigger that stays loaded
        // for most of a second afterwards is the opposite of that.
        triggers_.ClearOverlays(side, kGripLoad);
        const float e = std::clamp(speed * mass / 2000.0f, 0.05f, 1.0f);
        const float whirr = std::clamp(spin / 700.0f, 0.0f, 1.0f);
        std::vector<Voice> v;
        // A release is a departure, not an impact: rising pitch, no hard edge.
        v.push_back(Body(150, 300, 0.16f * e + 0.06f, 90, 50));
        // The last thing the hand feels of an object is its surface leaving the
        // skin, so the departure is tinted by what it was. Material was being
        // parsed here and then thrown away, which meant a glass bottle and a
        // steel pipe left the hand identically.
        const MaterialRecipe r = RecipeFor(m);
        auto air = Body(r.toneF0 * 0.62f, r.toneF0 * 0.95f,
                        0.10f + 0.16f * e, 120 + 60 * whirr, 70);
        if (whirr > 0.15f) {
            // A spinning throw flutters as it leaves the hand.
            air.amDepth = 0.45f * whirr;
            air.amFreq = 14.0f + 20.0f * whirr;
        }
        v.push_back(air);
        Emit(side, std::move(v), "PHYS_THROW");
        triggers_.PushOverlay(side, trig::Vibration(2, std::clamp(
            static_cast<int>(std::lround(2 + e * 5)), 1, 8), 120), 45, 4, "throw");
        return;
    }
    if (event == "PHYS_IMPACT") {
        const float impulse = ToFloat(arg(0), 0.0f);
        if (impulse < cfg_.minImpactImpulse) return;
        const float mass = std::clamp(ToFloat(arg(1), 1.0f), 0.05f, 40.0f);
        const auto side = SideFromParam(arg(2));
        const Material m = ParseMaterial(arg(3));
        const float spin = ToFloat(arg(4), 0.0f);
        // arg(5) is the model name, kept for diagnostics only.
        const bool held = ToFloat(arg(6), 1.0f) != 0.0f;
        const float confidence = std::clamp(ToFloat(arg(7), 1.0f), 0.0f, 1.0f);

        // The rule this project is built on: the hand only feels a collision it
        // is physically connected to. An object that has already left the hand
        // is somebody else's problem no matter how spectacularly it lands.
        //
        // The game side no longer reports those at all, so this is a guard
        // against a stale addon rather than a routine filter - but a stale
        // addon is exactly the case where a silent wrong behaviour is worst.
        if (!held) {
            if (cfg_.debug) {
                std::cout << "[Impact] ignored: object is no longer held "
                             "(update the game addon)\n";
            }
            return;
        }

        // Rate limit per hand. Dragging a held crate along a wall satisfies the
        // game-side test every tick; a strike is an event, not a texture.
        const size_t slot = (side == Controller::Left) ? 0 : 1;
        const auto now = Clock::now();
        if (lastImpact_[slot].time_since_epoch().count() != 0 &&
            now - lastImpact_[slot] <
                std::chrono::milliseconds(std::max(0, cfg_.impactCooldownMs))) {
            return;
        }
        lastImpact_[slot] = now;

        // Normalised against a "solid hit" reference rather than a physical unit.
        const float e = std::clamp(impulse / 1400.0f, 0.06f, 1.0f);
        if (cfg_.debug) {
            std::cout << "[Impact] " << MaterialName(m) << " impulse=" << impulse
                      << " mass=" << mass << " spin=" << spin << " e=" << e
                      << " confidence=" << confidence << " held=1\n";
        }
        Impact(m, e, side, mass, spin, confidence);
        return;
    }

    // ---- weapon manipulation ----
    // Reload work is mechanical: short, bright, high-frequency clicks near the
    // end of trigger travel. They ride on top of the weapon profile and the
    // weapon profile returns by itself when the overlay expires.
    // A mechanical action is not a click.
    //
    // The previous shared shape - an 11 ms tick plus 34 ms of body - measured
    // at rms 0.03 over 30 ms, roughly a TENTH of the energy of an impact that
    // reads clearly, and all ten reload events used it with only the pitch
    // changed. On hardware they were reported as too weak and too alike, which
    // is exactly what those numbers predict.
    //
    // Two things were wrong. Peak amplitude was fine (0.53-0.59); what skin
    // integrates is level over TIME, and 30 ms delivers almost none. And the
    // energy sat at 400-480 Hz, where the actuator barely displaces at all.
    //
    // So a mechanism is now built as what it physically is: metal contacts
    // metal - a bright tick that carries character but almost no force - and
    // then MASS MOVES AND STOPS, a low body in the 130-260 Hz band that
    // carries the force. Two-stage actions get two of them, because a slide
    // racking or a pump cycling really is two events, and skin resolves that
    // timing far better than it resolves pitch.
    auto stage = [](std::vector<Voice>& v, float tickHz, float tickAmp,
                    float f0, float f1, float amp, float ms, float decay,
                    float delayMs) {
        const int d = static_cast<int>(kSampleRate * delayMs / 1000.0f);
        auto tick = Transient(tickHz, tickAmp, 9, 5);
        tick.delay = d;
        v.push_back(tick);
        auto body = Body(f0, f1, amp, ms, decay);
        body.delay = d;
        v.push_back(body);
    };
    auto mech = [&](std::vector<Voice> v, int triggerPos, int triggerAmp, int freq, int ms) {
        triggers_.PushOverlay(primary_, trig::Vibration(triggerPos, triggerAmp, freq), ms, 7, "click");
        Emit(primary_, std::move(v), "RELOAD");
    };

    if (event == "PISTOL_CLIP") {
        // A magazine seating home: one firm push that stops dead.
        std::vector<Voice> v;
        stage(v, 430, 0.40f, 215, 170, 0.70f, 115, 60, 0);
        mech(std::move(v), 8, 6, 200, 60);
        return;
    }
    if (event == "PISTOL_CHAMBER") {
        // A slide: drawn back, then released to snap forward harder than it
        // was pulled. Two stages, the second the stronger.
        //
        // You rack the slide seconds after seating the magazine, so these two
        // are the tightest co-occurring pair on the pistol - and they measured
        // 158 ms/215 Hz against 118 ms/195 Hz, which is the same event twice.
        //
        // A slide slamming into battery moves considerably more metal than a
        // magazine seating does, so it goes LOWER and longer. The stage gap
        // widens to 58 ms as well: the two-part rhythm is the thing that makes
        // a slide read as a slide, and skin resolves that far better than it
        // resolves the third of an octave these two used to differ by.
        std::vector<Voice> v;
        stage(v, 470, 0.38f, 250, 210, 0.44f, 60, 30, 0);
        stage(v, 500, 0.52f, 168, 132, 0.80f, 195, 112, 58);
        mech(std::move(v), 9, 7, 175, 90);
        return;
    }
    if (event == "SHOTGUN_SHELL") {
        // Pressing a shell into the tube: the softest tick in the set, and the
        // LONGEST of the single-stage mechanisms. It measured 124 ms against
        // the pistol clip's 118 ms, which is no separation at all - a push and
        // a seat felt identical. Length is what tells them apart now: a shell
        // is pressed home over time, a magazine seats and stops.
        //
        // Raised out of the pump's band. At 150 Hz this measured 135 Hz
        // against the pump's 154 Hz - a 1.14x ratio, well inside the ~1.5x
        // skin needs, so the two biggest shotgun mechanisms were perceptually
        // one event. The separation is now physical rather than arbitrary: a
        // single brass shell sliding into a tube moves a fraction of the mass
        // a whole pump assembly does, so it belongs higher and lighter.
        //
        // The friction texture is what a shell actually is - brass dragging on
        // steel for the length of the push, rather than a clean seat.
        std::vector<Voice> v;
        stage(v, 300, 0.26f, 236, 188, 0.74f, 225, 130, 0);
        auto drag = Texture(250, 1.3f, 0.14f, 170, 105);
        drag.delay = kSampleRate * 18 / 1000;
        v.push_back(drag);
        mech(std::move(v), 7, 6, 195, 70);
        return;
    }
    if (event == "SHOTGUN_LOADED") {
        // The pump. The largest mechanical event in the game, and now built to
        // measure like it: the lowest and longest mechanism in the set.
        //
        // Slide back, then a hard slam into lockup. The gap between the two
        // stages widens to 105 ms because that gap IS the gesture - skin
        // resolves timing far better than pitch, and the pump is the one
        // mechanism where the two-part rhythm is unmistakable in the hand.
        //
        // The slam drops into the low lobe: a pump assembly slamming into
        // lockup is the heaviest thing your hand does to a weapon in this
        // game, and it should sit below every other click by a clear margin.
        std::vector<Voice> v;
        stage(v, 360, 0.40f, 200, 160, 0.52f, 75, 38, 0);
        stage(v, 400, 0.56f, 138, 96, 0.88f, 215, 120, 105);
        mech(std::move(v), 8, 7, 150, 110);
        return;
    }

    // The rapidfire family.
    //
    // These six all used ONE shape with only the pitch moved - the exact
    // "nudge one mode's numbers" failure this project criticises in weapon
    // triggers, sitting unnoticed in the reload layer because no self-test
    // ever measured them. Reloading the SMG is a five-step sequence performed
    // in sequence, so five steps that differ only by a third of an octave are
    // five steps that feel like the same click five times.
    //
    // Each is now built as the mechanism it physically is. The differences are
    // in COUNT and TIMING first - one event or two, and how far apart - then
    // in direction of pitch, then in texture. All three of those are things
    // skin reads better than it reads a pitch offset.
    auto smallMech = [&](std::vector<Voice> v, int tp, int ta, int tf, int tms) {
        mech(std::move(v), tp, ta, tf, tms);
    };
    // Laying five small mechanisms out so a hand can actually tell them apart
    // is a placement problem, not a tuning one. Skin needs roughly 1.5x on
    // duration OR on pitch, which over the usable range gives about four
    // rungs on each axis - so five effects fit only if they are placed on the
    // GRID deliberately rather than clustered and nudged.
    //
    // First attempt differentiated them by structure alone (tick counts,
    // texture, direction of glide) and left the numbers where they were. All
    // five landed in one cell - 101-167 ms, 160-250 Hz - and --analyze flagged
    // eight collisions among them. Structure is what makes them feel like
    // different mechanisms; separation on the two coarse axes is what makes
    // them register as different events at all. Both are needed.
    //
    //   step      dur    pitch   what it is
    //   close      95    340     a snap: shortest and highest
    //   cycle     150    150     a detent turning: lowest
    //   mag       150    330     lands among others: light, no hard stop
    //   insert    235    225     a push against friction that stops dead
    //   open      375    215     a cover swinging away: longest, and rising
    if (event == "RAPID_CYCLE") {
        // Rotating the capsule carousel one position: a detent that turns,
        // catches and stops. The LOW one - it moves the most metal.
        std::vector<Voice> v;
        stage(v, 330, 0.30f, 165, 140, 0.62f, 90, 46, 0);
        auto latch = Transient(300, 0.26f, 8, 5);
        latch.delay = kSampleRate * 55 / 1000;
        v.push_back(latch);
        auto seat = Body(150, 128, 0.48f, 85, 44);
        seat.delay = kSampleRate * 55 / 1000;
        v.push_back(seat);
        smallMech(std::move(v), 6, 5, 165, 95);
        return;
    }
    if (event == "RAPID_OPEN") {
        // The casing swinging OPEN: a latch releases and the cover falls away
        // from the hand. The LONGEST of the five, and the only one that
        // rises - a thing leaving is the opposite gesture to a thing seating,
        // and the swing genuinely takes the most time of any step.
        std::vector<Voice> v;
        stage(v, 330, 0.28f, 160, 285, 0.54f, 430, 250, 0);
        smallMech(std::move(v), 6, 5, 175, 120);
        return;
    }
    if (event == "RAPID_CLOSE") {
        // Snapping it shut. A single decisive slam with no preamble: the
        // SHORTEST and the brightest, which is what a snap is.
        std::vector<Voice> v;
        stage(v, 470, 0.44f, 350, 300, 0.80f, 78, 34, 0);
        smallMech(std::move(v), 7, 6, 230, 60);
        return;
    }
    if (event == "RAPID_INSERT") {
        // A capsule into the chamber: a push against friction that stops
        // dead. Mid-length, with drag along the whole of it.
        std::vector<Voice> v;
        stage(v, 400, 0.26f, 240, 205, 0.56f, 235, 140, 0);
        auto drag = Texture(300, 1.5f, 0.13f, 190, 120);
        drag.delay = kSampleRate * 14 / 1000;
        v.push_back(drag);
        smallMech(std::move(v), 8, 5, 215, 110);
        return;
    }
    if (event == "RAPID_MAG") {
        // Into the magazine rather than the chamber: the same push, but it
        // lands among capsules already there. High and light, with no hard
        // stop under it - the one step of the five that does not end in metal
        // meeting metal.
        std::vector<Voice> v;
        stage(v, 420, 0.24f, 340, 295, 0.50f, 145, 82, 0);
        smallMech(std::move(v), 7, 5, 225, 80);
        return;
    }
    if (event == "SHOTGUN_AUTOLOAD_ADD") {
        // A different weapon entirely: a shell going into the autoloader.
        // Kept lowest of all of them, and quiet - it is a background top-up,
        // not something you performed.
        std::vector<Voice> v;
        stage(v, 280, 0.24f, 155, 125, 0.50f, 105, 62, 0);
        smallMech(std::move(v), 7, 5, 145, 70);
        return;
    }

    if (event == "RAPID_EXPLODE") {
        std::vector<Voice> v;
        v.push_back(Transient(300, 0.90f, 24, 15));
        v.push_back(Body(160, 60, 1.00f, 190, 140));
        v.push_back(Texture(280, 0.9f, 0.55f, 120, 90));
        Emit(Controller::Both, std::move(v), "RAPID_EXPLODE");
        return;
    }
    if (event == "RAPID_UPGRADE" || event == "SHOTGUN_GL" || event == "SHOTGUN_AUTOLOADER") {
        std::vector<Voice> v;
        v.push_back(Body(200, 340, 0.30f, 80, 50));
        Emit(primary_, std::move(v), event);
        return;
    }

    // ---- items, health, world ----
    // ---- world and campaign moments ----
    //
    // These were sixteen single-voice effects: one Body(), one glide, no
    // accent, no texture, no modulation, 42-170 ms each. That construction is
    // the same "one shape, change the pitch" failure corrected in the weapon
    // triggers and again in the rapidfire family - and it was applied here to
    // some of the most memorable things in the campaign.
    //
    // Worse, most of them sat at 42-66 ms. The hardware round already settled
    // what that means: peak amplitude is not what skin integrates, LEVEL OVER
    // TIME is, and a sub-70 ms event delivers almost none of it. Those are the
    // same numbers that made the reload clicks read as nothing before they
    // were rebuilt. Every one of these was quietly in that state.
    //
    // So each is now built from the layers the effect physically has, under
    // the rule the hardware taught: character goes in the ACCENT at any
    // frequency, FORCE goes at 150-300 Hz, and anything meant to be felt needs
    // enough duration to be integrated.
    //
    // The small ones stay small. Being at the bottom of the dynamic range is
    // correct for picking up a resin chip; being imperceptible is not.
    {
        // A two-part handling cue: a contact accent, then the body that
        // carries the force. `rise` inverts the body's glide, which is what
        // separates taking something out from putting it away - direction is
        // legible in a way that a 40 Hz pitch offset is not.
        auto handling = [&](float tickHz, float tickAmp, float f0, float amp,
                            float ms, bool rise, Controller c) {
            std::vector<Voice> v;
            v.push_back(Transient(tickHz, tickAmp, 8, 5));
            v.push_back(Body(f0, rise ? f0 * 1.32f : f0 * 0.76f, amp, ms, ms * 0.58f));
            Emit(c, std::move(v), event);
        };
        const Controller side = SideFromParam(param);
        if (event == "ITEM_PICKUP") { handling(400, 0.30f, 250, 0.42f, 105, true,  primary_); return; }
        if (event == "ITEM_RELEASE"){ handling(330, 0.24f, 215, 0.38f,  95, false, primary_); return; }
        if (event == "ITEM_STORE")  { handling(380, 0.28f, 235, 0.44f, 110, false, side); return; }
        if (event == "ITEM_REMOVE") { handling(420, 0.30f, 225, 0.44f, 110, true,  side); return; }
        // Reaching over your shoulder: you cannot see it, so the hand wants a
        // firmer, lower confirmation than a wrist holster needs.
        //
        // These two are the same gesture in opposite directions and you
        // perform both constantly, which makes them the pair in this whole set
        // most worth separating. Direction of glide alone does not do it -
        // they measured 129 ms apiece, 170 Hz against 217 Hz.
        //
        // What separates them is what the hand is actually doing. Putting
        // something away is a push that ENDS in a seat, so it takes time and
        // stops. Pulling something out is a yank: it is over the moment the
        // object clears, and there is nothing at the end of it.
        if (event == "BACKPACK_STORE")    { handling(300, 0.30f, 175, 0.54f, 185, false, side); return; }
        if (event == "BACKPACK_RETRIEVE") { handling(360, 0.34f, 228, 0.50f,  98, true,  side); return; }
        if (event == "WEAPON_OFFHAND")    { handling(320, 0.34f, 205, 0.50f, 125, false, primary_); return; }
        if (event == "HEALTH_STORAGE")    { handling(360, 0.24f, 240, 0.34f,  95, false, primary_); return; }
    }

    if (event == "LADDER") {
        // A hand closing around a rung. One solid contact, and deliberately
        // quiet: you do this many times in a row and it must not accumulate
        // into a drone.
        std::vector<Voice> v;
        v.push_back(Transient(340, 0.26f, 9, 6));
        v.push_back(Body(205, 168, 0.40f, 105, 62));
        Emit(SideFromParam(param), std::move(v), "LADDER");
        return;
    }
    if (event == "MANTLE") {
        // Hauling your own body weight over a ledge. Both hands take it, and
        // it is a STRAIN rather than a knock: the grip bites, then the load
        // comes on and rides for as long as the pull lasts.
        std::vector<Voice> v;
        v.push_back(Transient(300, 0.34f, 11, 7));
        auto strain = Body(190, 145, 0.56f, 230, 145);
        strain.amDepth = 0.26f;
        strain.amFreq = 8.0f;
        v.push_back(strain);
        Emit(Controller::Both, std::move(v), "MANTLE");
        return;
    }
    if (event == "COVER_MOUTH") {
        // Jeff. Your hand is physically pressed over your own mouth, and this
        // is one of the longest sustained hand-contact states in the game -
        // held for as long as you need to stay silent.
        //
        // It was 44 ms of a single tone: the shortest effect in the table for
        // the moment the game most wants you to feel your own hand.
        //
        // Palm against face is not an impact. There is no hard edge to it at
        // all, so it gets no transient - the one effect here that deliberately
        // has none. What it has is presence: a soft low contact that arrives
        // over ~90 ms and then breathes, slowly, underneath everything.
        //
        // Deliberately quiet. This plays while you are holding your breath in
        // the dark, and it should sit just at the edge of noticing.
        std::vector<Voice> v;
        auto press = Tone(165, 140, 0.30f, 520, 90);
        press.amDepth = 0.22f;
        press.amFreq = 3.4f;
        v.push_back(press);
        auto skin = Texture(230, 0.8f, 0.09f, 300, 210);
        skin.delay = kSampleRate * 40 / 1000;
        v.push_back(skin);
        Emit(Controller::Both, std::move(v), "COVER_MOUTH");
        return;
    }
    if (event == "LEVITATE") {
        // Two-handed levitate: something too heavy for one hand coming up off
        // the floor. A slow swell in BOTH hands, rising, with the roughness of
        // a load that does not want to move.
        std::vector<Voice> v;
        auto lift = Tone(120, 245, 0.46f, 480, 200);
        lift.amDepth = 0.30f;
        lift.amFreq = 6.5f;
        lift.fmDepth = 10.0f;
        lift.fmFreq = 19.0f;
        v.push_back(lift);
        Emit(Controller::Both, std::move(v), "LEVITATE");
        return;
    }
    if (event == "COMBINE_TANK") {
        // Heaving a Combine barrier aside. The heaviest sustained thing the
        // player pushes, and it should grind rather than click: low, rough,
        // long, and in both hands because you are leaning your whole body in.
        std::vector<Voice> v;
        auto heave = Body(96, 74, 0.62f, 420, 280);
        heave.amDepth = 0.44f;
        heave.amFreq = 9.0f;
        heave.fmDepth = 14.0f;
        heave.fmFreq = 21.0f;
        v.push_back(heave);
        v.push_back(Texture(150, 0.9f, 0.20f, 340, 230));
        Emit(Controller::Both, std::move(v), "COMBINE_TANK");
        return;
    }
    if (event == "TRIPMINE_START") {
        // Reaching into the beam and taking hold of the hack. Rising tension,
        // no resolution yet - the answer comes on TRIPMINE_HACKED.
        std::vector<Voice> v;
        auto tension = Tone(180, 300, 0.34f, 300, 150);
        tension.amDepth = 0.35f;
        tension.amFreq = 14.0f;
        v.push_back(tension);
        Emit(SideFromParam(param), std::move(v), "TRIPMINE_START");
        return;
    }
    if (event == "TRIPMINE_HACKED") {
        // It gives. A hard release: the tension that was climbing lets go all
        // at once, which is the payoff the rising phase was setting up.
        std::vector<Voice> v;
        v.push_back(Transient(420, 0.48f, 12, 7));
        v.push_back(Body(280, 165, 0.74f, 175, 95));
        Emit(SideFromParam(param), std::move(v), "TRIPMINE_HACKED");
        return;
    }
    if (event == "BARNACLE_RELEASE") {
        // Cut loose and dropping. The grip lets go, then nothing - so this is
        // a release shape: a sharp departure with no settle under it.
        std::vector<Voice> v;
        v.push_back(Transient(330, 0.40f, 10, 6));
        v.push_back(Body(240, 130, 0.58f, 150, 88));
        Emit(Controller::Both, std::move(v), "BARNACLE_RELEASE");
        return;
    }

    if (event == "BARNACLE") {
        // The tongue takes hold and starts hauling you up. Both hands, because
        // you grab at it with both - and it is the one effect in the game that
        // should feel unpleasant.
        //
        // Three layers doing three jobs: the wet slap of contact, a coiling
        // grip that tightens, and the steady haul upward underneath. The haul
        // RISES, which is what makes it read as being lifted rather than as
        // being hit.
        std::vector<Voice> v;
        v.push_back(Transient(260, 0.34f, 14, 9));
        auto coil = Texture(210, 0.9f, 0.44f, 340, 240);
        coil.amDepth = 0.40f;
        coil.amFreq = 5.5f;
        v.push_back(coil);
        auto haul = Body(115, 205, 0.52f, 400, 270);
        haul.fmDepth = 18.0f;
        haul.fmFreq = 7.0f;
        haul.amDepth = 0.28f;
        haul.amFreq = 3.8f;
        haul.delay = kSampleRate * 30 / 1000;
        v.push_back(haul);
        Emit(Controller::Both, std::move(v), "BARNACLE");
        return;
    }
    if (event == "HEALTH_PEN") {
        const auto side = SideFromParam(param);
        std::vector<Voice> v;
        v.push_back(Transient(420, 0.55f, 12, 7));       // the needle
        // The warmth after. Given a slow pulse so it reads as something
        // spreading rather than as a flat tone being held - the two are very
        // different in the hand and cost nothing to tell apart.
        auto warmth = Tone(150, 230, 0.26f, 420, 40);
        warmth.amDepth = 0.20f;
        warmth.amFreq = 2.6f;
        v.push_back(warmth);
        Emit(side, std::move(v), "HEALTH_PEN");
        return;
    }
    if (event == "HEALTH_STATION") {
        // The wall station: you push your hand INTO it and the machine works
        // on you. That is a three-part sequence and it was one flat 700 ms
        // tone - the longest effect in the build, and the least shaped.
        //
        // Now: the mechanism engaging around the hand, then the injection
        // itself, then the warmth spreading out from it. The middle stage is
        // the only sharp thing in it, and it lands late on purpose - the wait
        // is what makes the station feel like a machine deciding rather than
        // a button being pressed.
        std::vector<Voice> v;
        auto engage = Body(210, 175, 0.34f, 190, 120);
        engage.amDepth = 0.38f;
        engage.amFreq = 17.0f;
        v.push_back(engage);
        auto needle = Transient(400, 0.46f, 12, 7);
        needle.delay = kSampleRate * 230 / 1000;
        v.push_back(needle);
        auto warmth = Tone(145, 235, 0.30f, 520, 70);
        warmth.amDepth = 0.22f;
        warmth.amFreq = 2.4f;
        warmth.delay = kSampleRate * 245 / 1000;
        v.push_back(warmth);
        Emit(SideFromParam(param), std::move(v), "HEALTH_STATION");
        return;
    }
}

// ---------------------------------------------------------------------------
// Self test
// ---------------------------------------------------------------------------

std::vector<RecoilSpec> RecoilLadder() {
    std::vector<RecoilSpec> out;
    for (const char* w : {"PISTOL", "SMG", "SHOTGUN"}) {
        int breakMs = 0, kickMs = 0;
        RecoilKick(w, breakMs);
        const TriggerCommand kick = FireOverlay(w, kickMs);
        int drive = 0;
        int rate = 0;
        if (kick.mode == kTriggerVibration) {
            drive = kick.data.vibration.amplitude;
            rate = kick.data.vibration.frequency;
        } else if (kick.mode == kTriggerMultiPositionVibration) {
            rate = kick.data.multiVibration.frequency;
            for (uint8_t a : kick.data.multiVibration.amplitude) {
                drive = std::max(drive, static_cast<int>(a));
            }
        }
        // Peak resistance of the resting profile, so the two independent
        // ladders - how hard the trigger is to PULL, and how hard it KICKS -
        // can be read side by side. They were tuned blind of each other until
        // the pistol and shotgun were found sitting at identical resistance.
        const TriggerCommand base = WeaponBase(w);
        int rest = 0;
        switch (base.mode) {
            case kTriggerFeedback: rest = base.data.feedback.strength; break;
            case kTriggerWeapon:   rest = base.data.weapon.strength;   break;
            case kTriggerSlopeFeedback:
                rest = std::max(base.data.slope.startStrength, base.data.slope.endStrength);
                break;
            case kTriggerMultiPositionFeedback:
                for (uint8_t v : base.data.multiFeedback.strength) {
                    rest = std::max(rest, static_cast<int>(v));
                }
                break;
            default: break;
        }
        out.push_back({w, IsSustainedFire(w) ? std::min(drive, kMaxSustainedDrive) : drive,
                       rate, kickMs - breakMs, rest});
    }
    return out;
}

std::vector<std::string> SelfTestNames() {
    return {
        // Weapons
        "pistol", "shotgun", "smg", "grenade", "shotgun-empty",
        // Weapon manipulation.
        //
        // The rapidfire family is here because it was NOT, and that is how six
        // events came to share one waveform with only the pitch moved. An
        // effect no test names cannot be measured, and anything that cannot be
        // measured drifts - this project has now found the same failure three
        // times in three layers, each time in the part that had no test.
        "reload", "chamber", "shell-insert", "shotgun-pump",
        "smg-cycle", "smg-open", "smg-close", "smg-insert", "smg-mag",
        "brace", "unbrace",
        // Gravity gloves, as the continuous sequence they are meant to be.
        // catch-light and catch-heavy exist specifically so the mass layer can
        // be compared: if those two ever measure the same, the showcase
        // interaction has regressed to a single canned buzz.
        "glove-lock", "glove-pull", "glove-catch",
        "catch-light", "catch-heavy",
        // Held-object impacts, one per material. This is also Alyx's melee.
        "impact-light", "impact-heavy", "impact-glass", "impact-metal",
        "impact-wood", "impact-stone", "impact-rubber", "impact-plastic",
        "impact-cardboard", "impact-organic",
        // Carrying weight. hold-still must be SILENT - it is the proof that
        // the grip load does not degenerate into ambient buzzing.
        "hold-still", "hold-swing",
        // Doors: a light cupboard against a heavy blast door.
        "door-light", "door-heavy",
        // Everything else that earns a haptic
        "hurt", "hurt-light", "pickup", "throw", "health-pen",
        // Campaign moments. Every one of these was a single Body() voice of
        // 42-170 ms, and none of them had a test, so nothing ever reported
        // that some of the game's most memorable hand-contact beats were
        // sitting below the threshold of being felt at all.
        "health-station", "mantle", "ladder", "cover-mouth",
        "levitate", "combine-tank", "barnacle", "barnacle-release",
        "tripmine-start", "tripmine-hacked",
        "store", "retrieve",
    };
}

bool RunSelfTest(Router& r, const std::string& name, const std::string& side) {
    // Every test takes the hand it should play on, so the suite alternates and
    // localisation is actually exercised. Previously almost every case was
    // hard-coded to the right hand, which made the whole thing feel one-sided.
    const std::string s = side.empty() ? "right" : side;
    r.Handle("PRIMARY_HAND", s);
    // Signatures fire back to back here; the real-time impact limiter must not
    // silently eat the ones after the first.
    r.ResetRateLimits();
    r.ResetEmitTrace();
    // Start every case from empty hands.
    //
    // Weapon identity is persistent state, so without this it LEAKS between
    // cases: running the shotgun test left weapon_ = SHOTGUN, and every
    // signature after it then picked up weapon.SHOTGUN's config gain. A
    // gravity-glove catch measured 0.71 on the limiter purely because a
    // shotgun test happened to run earlier in the list. A measurement that
    // depends on the order of the suite is not a measurement.
    // Weapon cases set their own identity immediately below.
    r.Handle("WEAPON", "HANDS");

    // Held-object impacts. Trailing fields are model,held,confidence - held=1
    // because a hand only feels what it is still holding, and confidence=1
    // because these are authored cases, not inferred ones.
    auto impact = [&](const char* impulseMass, const char* material,
                      const char* spin, const char* model) {
        r.Handle("PHYS_IMPACT", std::string(impulseMass) + "," + s + "," + material +
                                "," + spin + "," + model + ",1,1.00");
    };

    if (name == "pistol")        { r.Handle("WEAPON", "PISTOL");  r.Handle("FIRE", "PISTOL," + s + ",1H,-1"); return true; }
    if (name == "shotgun")       { r.Handle("WEAPON", "SHOTGUN"); r.Handle("FIRE", "SHOTGUN," + s + ",2H,2"); return true; }
    if (name == "smg")           { r.Handle("WEAPON", "SMG");     r.Handle("FIRE", "SMG," + s + ",1H,-1"); return true; }
    if (name == "grenade")       { r.Handle("WEAPON", "GRENADE"); r.Handle("FIRE", "GRENADE," + s + ",1H,-1"); return true; }
    if (name == "shotgun-empty") { r.Handle("WEAPON", "SHOTGUN"); r.Handle("FIRE", "SHOTGUN," + s + ",1H,0"); return true; }

    if (name == "reload")        { r.Handle("PISTOL_CLIP", ""); return true; }
    if (name == "chamber")       { r.Handle("PISTOL_CHAMBER", ""); return true; }
    if (name == "shell-insert")  { r.Handle("SHOTGUN_SHELL", "2"); return true; }
    if (name == "shotgun-pump")  { r.Handle("SHOTGUN_LOADED", "4"); return true; }
    // The SMG reload sequence, in the order you actually perform it.
    if (name == "smg-cycle")     { r.Handle("RAPID_CYCLE", ""); return true; }
    if (name == "smg-open")      { r.Handle("RAPID_OPEN", ""); return true; }
    if (name == "smg-close")     { r.Handle("RAPID_CLOSE", ""); return true; }
    if (name == "smg-insert")    { r.Handle("RAPID_INSERT", ""); return true; }
    if (name == "smg-mag")       { r.Handle("RAPID_MAG", ""); return true; }
    if (name == "brace")         { r.Handle("TWO_HAND_START", "SHOTGUN"); return true; }
    if (name == "unbrace")       { r.Handle("TWO_HAND_END", "SHOTGUN"); return true; }

    if (name == "glove-lock")    { r.Handle("GLOVE_LOCK_START", s); return true; }
    if (name == "glove-pull")    { r.Handle("GLOVE_PULL", s); return true; }
    if (name == "glove-catch")   { r.Handle("GLOVE_CATCH", s); return true; }
    // The catch snap plus the weight that follows it, which is how a real
    // catch arrives. A 0.4 kg bottle against a 22 kg crate.
    if (name == "catch-light") {
        r.Handle("GLOVE_CATCH", s);
        r.Handle("GLOVE_CATCH_MASS", "0.40," + s + ",glass,60,bottle");
        return true;
    }
    if (name == "catch-heavy") {
        r.Handle("GLOVE_CATCH", s);
        r.Handle("GLOVE_CATCH_MASS", "22.0," + s + ",metal,320,crate");
        return true;
    }

    if (name == "impact-light")     { impact("260,0.8",  "wood",      "40",  "light");  return true; }
    if (name == "impact-heavy")     { impact("2600,12",  "organic",   "120", "heavy");  return true; }
    if (name == "impact-glass")     { impact("900,1.2",  "glass",     "80",  "bottle"); return true; }
    if (name == "impact-metal")     { impact("1200,4",   "metal",     "700", "barrel"); return true; }
    if (name == "impact-wood")      { impact("900,3",    "wood",      "200", "crate");  return true; }
    if (name == "impact-stone")     { impact("1600,8",   "stone",     "90",  "block");  return true; }
    if (name == "impact-rubber")    { impact("900,2",    "rubber",    "500", "tire");   return true; }
    if (name == "impact-plastic")   { impact("700,1.0",  "plastic",   "120", "bucket"); return true; }
    if (name == "impact-cardboard") { impact("500,0.6",  "cardboard", "60",  "box");    return true; }
    if (name == "impact-organic")   { impact("1100,5",   "organic",   "150", "crab");   return true; }

    // mass, side, material, handSpeed, spin. hold-still is a 15 kg crate held
    // motionless: it must produce NO waveform at all, only trigger load.
    if (name == "door-light")    { r.Handle("DOOR_MOVE", s + ",70,25"); return true; }
    if (name == "door-heavy")    { r.Handle("DOOR_MOVE", s + ",120,220"); return true; }
    if (name == "hold-still")    { r.Handle("PHYS_HOLD", "15.0," + s + ",wood,5,0"); return true; }
    if (name == "hold-swing")    { r.Handle("PHYS_HOLD", "15.0," + s + ",wood,280,140"); return true; }

    // health, damage, damagebits. Two cases, because the whole point of the
    // rework is that these must no longer feel the same.
    if (name == "hurt")          { r.Handle("HURT", "65,35,0"); return true; }
    if (name == "hurt-light")    { r.Handle("HURT", "92,8,0"); return true; }
    if (name == "pickup")        { r.Handle("PHYS_PICKUP", "1.2,6.0," + s + ",metal,0,can"); return true; }
    if (name == "throw")         { r.Handle("PHYS_THROW", "700,2.0," + s + ",metal,650,can"); return true; }
    if (name == "health-pen")    { r.Handle("HEALTH_PEN", s); return true; }

    // Campaign moments.
    if (name == "health-station")  { r.Handle("HEALTH_STATION", s); return true; }
    if (name == "mantle")          { r.Handle("MANTLE", ""); return true; }
    if (name == "ladder")          { r.Handle("LADDER", s); return true; }
    if (name == "cover-mouth")     { r.Handle("COVER_MOUTH", ""); return true; }
    if (name == "levitate")        { r.Handle("LEVITATE", ""); return true; }
    if (name == "combine-tank")    { r.Handle("COMBINE_TANK", ""); return true; }
    if (name == "barnacle")        { r.Handle("BARNACLE", ""); return true; }
    if (name == "barnacle-release"){ r.Handle("BARNACLE_RELEASE", ""); return true; }
    if (name == "tripmine-start")  { r.Handle("TRIPMINE_START", s); return true; }
    if (name == "tripmine-hacked") { r.Handle("TRIPMINE_HACKED", s); return true; }
    if (name == "store")           { r.Handle("BACKPACK_STORE", s); return true; }
    if (name == "retrieve")        { r.Handle("BACKPACK_RETRIEVE", s); return true; }
    return false;
}

} // namespace psvr2
