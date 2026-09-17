#include "core/profiles.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace psvr2 {

const char* const kProfileNames[] = {
    // Half-Life: Alyx
    "PISTOL_FIRE", "SHOTGUN_FIRE", "SMG_FIRE", "GRENADE_FIRE",
    "MELEE_FIRE", "DEFAULT_FIRE",
    "GLOVE_LOCK", "GLOVE_PULL", "GLOVE_CATCH",
    "HURT",
    // Half-Life 2 VR. Prefixed rather than shared, because the two games'
    // weapons are different objects that happen to have similar names: an Alyx
    // pistol and a Half-Life 2 pistol are laid out against completely different
    // sets of neighbours, and forcing them to share a profile would mean tuning
    // one could only ever damage the other.
    "HL2_PISTOL_FIRE", "HL2_MAGNUM_FIRE", "HL2_SMG_FIRE", "HL2_AR2_FIRE",
    "HL2_SHOTGUN_FIRE", "HL2_CROSSBOW_FIRE", "HL2_RPG_FIRE", "HL2_GRENADE_FIRE",
    "HL2_DAMAGE", "HL2_DAMAGE_FIRE", "HL2_DAMAGE_SHOCK", "HL2_DAMAGE_TOXIC",
    "HL2_EXPLOSION",
};
const int kProfileCount = static_cast<int>(sizeof(kProfileNames) / sizeof(kProfileNames[0]));

namespace {

// One record per voice, so the built-ins can be printed back out in the same
// format a user would type. Storing them as Voices directly would lose which
// builder made them and with what arguments.
struct Spec {
    const char* kind;
    float a, b, c, d, e;
    float amDepth, amFreq, fmDepth, fmFreq, delayMs;
};

struct Builtin {
    const char* name;
    const char* note;
    std::vector<Spec> voices;
};

// The built-in table. These are the values the project ships with, and the
// same numbers that were arrived at by measurement with --analyze.
const std::vector<Builtin>& BuiltinTable() {
    static const std::vector<Builtin> table = {
        // ---------------------------------------------------------------------
        // Glide width: widen DOWNWARD ONLY. Never raise the onset.
        //
        // Most bodies here glided by 1.10-1.17x, well under the ~1.5x skin needs
        // to resolve pitch at all - a parameter being paid for and not felt.
        //
        // The first attempt widened them around their MEAN frequency, holding
        // domHz fixed so the weapon pitch ladder could not shift. It measured
        // beautifully: domHz moved at most 1 Hz, collisions stayed clear.
        // On hardware every weapon read THINNER and the build was rejected.
        //
        // The reason is that ResponseGain() cannot do what it claims above
        // ~300 Hz. Widening around the mean throws the ONSET up to 340-510 Hz,
        // and the compensation answers with digital gain - but a voice coil at
        // 450 Hz physically displaces less than at 250 Hz for the same drive.
        // Digital gain cannot buy back force the actuator is not producing, so
        // the effect simply spends more of its life in the weak part of the
        // band. Trimming amplitude to protect the limiter made it worse again.
        //
        // So: hold f0 exactly where it is and pull f1 DOWN into the 120-300 Hz
        // strong band. Each effect now spends MORE of its life in the powerful
        // region than before, not less. Amplitudes are untouched - trimming is
        // what 'thin' means, and there is nothing to pay for here because the
        // lower tail sits where the actuator is strongest.
        //
        // domHz drops as a result (the pistol 267 -> 236) and that is accepted:
        // the collision report is what protects the ladder, and it still reads
        // none. Measured cost across both games: no limiter moved at all, and
        // rms went UP on hurt (+9%) and HL2 smg, unchanged on both pistols.
        //
        // Weapons already gliding wide - both shotguns, the grenade, the
        // gravity pull, the magnum, the RPG - are untouched. So are the
        // crossbow and the shock, whose identity is the ABSENCE of low end: a
        // downward sweep is exactly the wrong move for them.
        // ---------------------------------------------------------------------
        // The transient moved DOWN (470 -> 380 Hz) and UP in level (0.34 ->
        // 0.45), with the body staggered 10 ms behind it.
        //
        // 470 Hz is in the region the hardware feedback established this build
        // cannot deliver: ResponseGain() answers with digital gain, but the coil
        // displaces too little there for that to become force. The pistol's
        // 'crack' was therefore mostly absent from the hand, and the weapon was
        // carried almost entirely by its body.
        //
        // This could not be fixed before the stagger existed. Lowering the
        // transient toward the body's 300 Hz onset made the two sum coherently
        // instead of sitting clear of each other, and the limiter went 1.00 ->
        // 0.94 for no gain. Moving the body 10 ms back removes the collision,
        // and then the accent can be both lower and larger for free:
        // peak actually DROPS 0.92 -> 0.87, rms holds, limiter stays 1.00.
        //
        // Net delivered output of the accent is up roughly 47% (0.45 x 0.78
        // response, against 0.34 x 0.70). No modulation is added, deliberately -
        // a pistol is one clean snap, and the shudder that suits the shotgun
        // would read here as a mechanism rattling.
        {"PISTOL_FIRE", "A single meaty crack. The middle rung of the length ladder.",
         // Weapons are now separated primarily by DURATION, not pitch.
         //
         // They used to sit at 131/176/542 ms and 335/284/94 Hz. The pistol and
         // SMG were 1.34x apart in length and 1.18x in pitch - both below the
         // ~1.5x ratio skin needs before two vibrations read as different at
         // all. They were not similar; perceptually they were the SAME effect,
         // which is exactly what was reported on hardware.
         //
         // The ladder is now roughly 85 / 175 / 540 ms - each step about 2-3x
         // the last, comfortably past the threshold in both directions.
         {{"transient", 380, 0.45f, 10, 6, 0, 0, 0, 0, 0, 0},
          {"body", 300, 195, 0.86f, 175, 78, 0, 0, 0, 0, 10}}},

        // Reported as not feeling UNIQUE, despite clearing the collision report
        // on both axes (3.0x the pistol's duration, 2.7x its pitch). The report
        // was not wrong, it was blind: it measures duration and pitch, and this
        // effect's problem was in neither.
        //
        // It had NO modulation at all and a transient of 0.34 - the same accent
        // amplitude as the pistol. So the heaviest weapon in the game had the
        // attack of a handgun and, per the note on Voice::amDepth, the temporal
        // signature of a solid knock. It was not a shotgun, it was a long low
        // version of everything else.
        //
        // Three changes, and the third is what pays for the other two:
        //
        //   crack   transient 0.34 -> 0.60. A shotgun is a punch and then a
        //           rumble; this had only the rumble.
        //   shudder am 0.28 at 6 Hz, about 3.5 heaves across the effect - the
        //           weapon shaking itself out. Nothing else in Alyx is both
        //           this long and modulated, and skin reads temporal pattern
        //           far better than it reads pitch.
        //   stagger the body starts 16 ms AFTER the transient instead of on top
        //           of it. Both used to strike at t=0 and sum past the ceiling,
        //           so every attempt to enlarge the crack just fed the limiter
        //           and got squashed - raising the transient alone took the
        //           limiter from 0.94 to 0.90 and bought nothing.
        //
        // Separating them by 16 ms gives the crack the peak to itself and the
        // mass arrives behind it, which is also the physically honest order.
        // Result: peak 0.96 -> 0.94, rms 0.352 -> 0.349, limiter 0.94 -> 0.98.
        // The body could go back UP from 0.76 to 0.84 - reversing a cut made
        // when it had to share the attack window - and still limit less than
        // before. That headroom is what the shudder is spent on.
        {"SHOTGUN_FIRE", "Heaviest thing in the game: a long fall into the low lobe.",
         {{"transient", 200, 0.60f, 16, 10, 0, 0, 0, 0, 0, 0},
          // Brought down from 0.89. At that level the shotgun drove its own
          // limiter to 0.74 and the empty-chamber variant to 0.67, and what a
          // limiter squashes first is the sharp transient - so the heaviest
          // weapon in the game was paying for its weight by losing its edge,
          // and being flattened toward everything else in the process.
          //
          // This weapon is still far and away the largest thing in the suite
          // on the measure that matters: it carries roughly 2.5x the rms of
          // the pistol over 3x the duration. Weight comes from pitch and
          // length, not from the last notch of level - the same conclusion the
          // trigger work reached independently when 8/8 turned out to push
          // back less than 7.
          {"body", 160, 45, 0.84f, 520, 300, 0.28f, 6.0f, 0, 0, 16}}},

        // The second body layer is STAGGERED 10 ms off the attack, and that one
        // number is worth more than any frequency here.
        //
        // The SMG was the quietest Alyx weapon (rms 0.102) and simultaneously
        // the most limited (0.90) - a contradiction that means it was never
        // loud, it was COLLIDING. Three layers all struck at t=0, summed past
        // the ceiling, and the limiter took the difference out of the transient,
        // which is the part that makes a shot read as a shot.
        //
        // Moving one supporting layer out of the attack window fixes both ends
        // at once: peak 0.98 -> 0.95, limiter 0.90 -> 0.95, and rms UP 5% to
        // 0.107. Nothing was made louder; energy that was being squashed now
        // survives. Duration and domHz are unchanged.
        //
        // 10 ms is deliberate. The plateau runs from 6 to 28 ms, so this is not
        // a knife-edge, but GLOVE_LOCK proves 32-36 ms reads as two distinct
        // ticks - and an SMG round must stay one event. 10 ms is clear of the
        // collision and nowhere near the point where skin separates them.
        {"SMG_FIRE", "A short hard spit. The SHORTEST thing in the game.",
         // The bottom rung of the length ladder, at roughly half the pistol.
         //
         // An SMG fires many times a second, so its character comes from the
         // RATE of the shots, not from any one of them - each individual round
         // should be over almost before it registers, so a burst reads as a
         // burst rather than as a smear. The choke group stops consecutive
         // shots summing, which is what makes a short one viable.
         //
         // Full amplitude despite the short length: brevity is the signature,
         // weakness is not. Short-and-quiet is what made the old clicks
         // imperceptible; short-and-hard is a crack.
         {{"transient", 450, 0.38f, 8, 5, 0, 0, 0, 0, 0, 0},
          {"body", 330, 215, 0.92f, 85, 34, 0, 0, 0, 0, 0},
          {"body", 180, 118, 0.34f, 60, 26, 0, 0, 0, 0, 10}}},

        {"GRENADE_FIRE", "Rising rather than falling - a throw, not an impact.",
         {{"body", 150, 350, 0.70f, 300, 160, 0.25f, 9.0f, 0, 0, 0}}},

        {"MELEE_FIRE", "Low and heavy with no metallic edge.",
         {{"transient", 190, 0.32f, 16, 10, 0, 0, 0, 0, 0, 0},
          {"body", 135, 85, 1.02f, 340, 175, 0.30f, 5.5f, 0, 0, 0}}},

        {"DEFAULT_FIRE", "Anything without its own profile.",
         {{"transient", 320, 0.36f, 12, 8, 0, 0, 0, 0, 0, 0},
          {"body", 260, 170, 0.90f, 160, 70, 0, 0, 0, 0, 0}}},

        {"GLOVE_LOCK", "Acquisition: a two-part latch - tick, then it catches.",
         // Reported too weak. Every layer sat at 460-470 Hz, the weakest part
         // of the band, so a cue meant to be small and precise came out as
         // barely there. Now the accents stay bright but the body sits at
         // 300 Hz, the measured peak of the actuator's response.
         //
         // The second tick is the real upgrade: skin resolves TIMING far
         // better than pitch, so two ticks 36 ms apart read unmistakably as
         // "click - locked" where one tone read as a faint buzz.
         //
         // Shortened hard, because it was colliding with the CATCH - the two
         // ends of the game's signature interaction measured 141 ms/289 Hz and
         // 171 ms/268 Hz, inside the ~1.5x ratio skin needs on either axis.
         // They were perceptually the same event. A lock is now a brief high
         // tick-tick; a catch is a long falling snap. Nothing else about the
         // gesture had to change.
         //
         // Then overshot the other way: at a 46 ms body it stopped registering
         // at all on hardware. This is the acquisition cue - the moment an
         // object highlights and becomes grabbable - and it fires constantly,
         // so it must stay small. But small is not the same as absent.
         //
         // Roughly doubled and brought down off the rolloff, which buys real
         // force without much length. It still measures about half the catch,
         // so the two remain clearly separate events.
         //
         // Then overshot the other way twice over. At a 46 ms body it stopped
         // registering at all; doubling it put the QUIETEST cue in the game
         // into the middle of the loudness order, above carrying weight and
         // above a rubber impact.
         //
         // This is the acquisition cue - an object highlighting as grabbable -
         // and it fires constantly while you sweep the room. It belongs at the
         // very bottom of the dynamic range: felt, and nothing more.
         //
         // What rescued it from imperceptibility was PITCH, not level. The
         // dead version sat at 372 Hz, up where the actuator barely displaces.
         // Holding it at ~315 Hz with the double tick intact buys enough force
         // to register at roughly half the amplitude, which is how it can be
         // both the faintest thing here and still there.
         // The double tick was never actually double. Spec carries eleven
         // fields and this row was written with ten, so the 32 ms that was
         // meant to be `delayMs` landed in `fmFreq` - an FM rate with zero
         // depth, which does nothing - and the delay defaulted to 0. Both
         // ticks fired on the same sample and summed into one.
         //
         // That is why this cue kept reading as "a faint buzz" through several
         // rounds of retuning: every round adjusted pitch and level, and the
         // separation that was supposed to be carrying the character was not
         // there to adjust. --dump-profiles showed it plainly once looked at -
         // no `delay:` modifier on the second tick.
         {{"transient", 420, 0.19f, 9, 5, 0, 0, 0, 0, 0, 0},
          {"transient", 390, 0.15f, 8, 5, 0, 0, 0, 0, 0, 32},
          {"body", 330, 300, 0.29f, 88, 42, 0, 0, 0, 0, 0}}},

        {"GLOVE_PULL", "A long RISING sweep. Nothing else in the game rises like this.",
         // The sweep is the CHARACTER of the pull and it is the right shape -
         // but it ends at 430 Hz, and the measured response says force dies as
         // the band climbs. So the gesture was arriving exactly backwards: the
         // climax of the pull, the moment the object is nearly in your hand,
         // was landing in the weakest part of the actuator's range.
         //
         // The project's own rule covers this - character in the accent, FORCE
         // at 150-300 Hz - and the sweep alone had nowhere to put the force.
         //
         // So the sweep keeps rising and a second layer swells underneath it,
         // inside the force band, with a long attack so the tension BUILDS
         // rather than announcing itself. The finger feels the line come taut
         // while the palm hears the pitch climb.
         //
         // The sweep gives up level to pay for it: at 0.56 plus a second voice
         // this limited to 0.71, which squashes the very climb it exists for.
         {{"body", 85, 430, 0.44f, 440, 260, 0.28f, 11.0f, 12.0f, 26.0f, 0},
          {"tone", 175, 265, 0.34f, 430, 210, 0, 0, 0, 0, 0}}},

        {"GLOVE_CATCH", "The capture only - bright and short. Weight arrives separately.",
         // A light catch was reported as borderline. The gesture is right - a
         // bright arrival that falls - but it STARTED at 470 Hz, so the first
         // third of the sweep was spent in the band the hardware cannot drive.
         // Beginning the fall at 330 Hz keeps the same shape and lands all of
         // it inside the usable range. A light catch has almost no mass layer
         // under it, so this sweep is nearly all of what it feels like.
         //
         // Amplitude comes DOWN as the pitch does, which is the whole point:
         // in an efficient part of the band you need less drive, not the same.
         // Holding 0.80 while moving to 330 Hz drove a heavy catch to 0.74 on
         // the limiter, squashing the mass layer underneath it.
         // Shortened, because WEIGHT is what should extend a catch.
         //
         // At 190 ms the bare snap measured 171 ms / 268 Hz and a light catch
         // measured 189 ms / 248 Hz - inside the 1.5x ratio on both axes, so
         // skin reads them as the same event. The mass layer was doing nothing
         // at the light end, which is precisely where most of the game lives.
         //
         // The fix is not to inflate a light catch. It is to make the bare
         // snap honest: when no object resolves, nothing arrived in the hand,
         // so the capture should land and STOP. Every resolved catch then adds
         // its settle underneath and is longer by definition - a ladder that
         // starts at the snap instead of one where two rungs sit on top of
         // each other.
         //
         // Shortening it also buys back headroom the mass layer was fighting
         // for: a heavy catch was limiting to 0.68.
         //
         // Cut to 115 ms first, which overshot: it cleared the light catch and
         // landed on top of GLOVE_LOCK instead (116 ms/267 Hz against
         // 88 ms/316 Hz). The lock is pinned short and faint on purpose, so
         // the snap is what has to move. 165 ms sits nearly two lock-lengths
         // clear while still reading as a capture that stops.
         {{"transient", 470, 0.36f, 11, 6, 0, 0, 0, 0, 0, 0},
          {"body", 330, 215, 0.66f, 165, 78, 0, 0, 0, 0, 0}}},

        {"HURT", "Scaled by how hard you were hit; these are the full-strength values.",
         {{"transient", 240, 0.30f, 18, 12, 0, 0, 0, 0, 0, 0},
          {"body", 130, 82, 0.79f, 190, 110, 0, 0, 0, 0, 0},
          {"texture", 300, 1.1f, 0.14f, 70, 50, 0, 0, 0, 0, 0}}},

        // -------------------------------------------------------------------
        // Half-Life 2 VR.
        //
        // SEVEN firing weapons against Alyx's three, which is a much harder
        // placement problem and the reason these are laid out on a grid rather
        // than tuned one at a time. Skin needs roughly a 1.5x ratio in duration
        // OR in pitch before two vibrations read as different things at all, so
        // with every weapon available at once and switchable at will, all
        // twenty-one pairs have to clear that line.
        //
        //   weapon    dur   pitch   what carries it
        //   smg        70    235    the shortest thing in the game
        //   crossbow   95    430    the only shot with NO low-frequency energy
        //   pistol    115    280    the middle rung, and the weapon fired most
        //   ar2       200    330    fast tremolo: electrical, not mechanical
        //   magnum    260    170    a violent crack, not a boom
        //   shotgun   500     90    the long fall into the low lobe
        //   rpg       620    150    the only shot whose pitch RISES
        //
        // The magnum deliberately does NOT go lowest. A .357 is higher pressure
        // and shorter than a shotgun, and putting it down at 120 Hz for 430 ms
        // - which is where "the most powerful handgun" first landed - collided
        // with both the shotgun and the RPG at once. Moving it up and in is
        // both more accurate and what buys the low end its room.
        // -------------------------------------------------------------------

        {"HL2_PISTOL_FIRE", "9mm. The middle rung, and the weapon fired most.",
         {{"transient", 380, 0.45f, 10, 6, 0, 0, 0, 0, 0, 0},
          {"body", 300, 195, 0.86f, 105, 50, 0, 0, 0, 0, 10}}},

        {"HL2_MAGNUM_FIRE", ".357. A violent crack rather than a boom - high, hard and over.",
         {{"transient", 260, 0.44f, 18, 11, 0, 0, 0, 0, 0, 0},
          {"body", 195, 130, 0.92f, 240, 130, 0, 0, 0, 0, 0}}},

        {"HL2_SMG_FIRE", "MP7. The SHORTEST thing in the game: a burst must read as a burst.",
         // An SMG fires 13 times a second, so its character comes from the RATE
         // of the shots and not from any one of them. Each round has to be over
         // almost before it registers or a burst smears into one noise. Full
         // amplitude despite the length: brevity is the signature, weakness is
         // not - short-and-quiet is imperceptible, short-and-hard is a crack.
         {{"transient", 420, 0.30f, 8, 5, 0, 0, 0, 0, 0, 0},
          {"body", 245, 160, 0.88f, 62, 26, 0, 0, 0, 0, 0}}},

        {"HL2_AR2_FIRE", "Pulse rifle. Electrical, not mechanical - carried by fast tremolo.",
         // The one weapon in either game that is not a chemical explosion, and
         // it gets the axis skin reads best to say so. A 38 Hz tremolo is a
         // temporal pattern, which is far more legible than any pitch offset,
         // and nothing else in Half-Life 2 shimmers like this.
         {{"transient", 430, 0.28f, 9, 5, 0, 0, 0, 0, 0, 0},
          {"body", 350, 310, 0.80f, 185, 90, 0.55f, 38.0f, 0, 0, 0}}},

        // Same crack / shudder / stagger rework as the Alyx shotgun above, but
        // held to 0.78 and 0.24 rather than 0.84 and 0.28. This profile has a
        // second consumer Alyx does not have: the double-barrel secondary
        // rescales it to 0.82x frequency and 1.25x length, which drops the body
        // to ~37 Hz where ResponseGain() applies its largest boost. At the Alyx
        // values the single shot measured fine and the DOUBLE regressed to 0.83.
        // Backing off here leaves both better than before: single 0.94 -> 1.00,
        // double 0.85 -> 0.89.
        {"HL2_SHOTGUN_FIRE", "The heaviest discharge: a long fall into the low lobe.",
         // Level held down deliberately. At full amplitude this drove its own
         // limiter hard, and what a limiter takes first is the sharp transient -
         // so the heaviest weapon would pay for its weight by losing its edge.
         // Weight comes from pitch and length, not from the last notch of level.
         {{"transient", 200, 0.60f, 16, 10, 0, 0, 0, 0, 0, 0},
          {"body", 160, 45, 0.78f, 470, 275, 0.24f, 6.0f, 0, 0, 16}}},

        // Transient dropped 500 -> 400 Hz. Same reasoning as the glide rule
        // above, applied to the attack: 500 Hz is deep in the region where the
        // actuator cannot deliver what ResponseGain() promises, so the accent
        // was expensive in headroom and weak in the hand at the same time.
        // This is the second most limited effect in the game and doctrine says
        // what a limiter squashes first is the transient - so the bowstring was
        // paying for its brightness by losing its edge. 0.83 -> 0.87 limiter
        // for 2% rms. The body is deliberately NOT widened: past this point
        // every extra step cost rms and bought no further headroom.
        {"HL2_CROSSBOW_FIRE", "A bowstring, not a gunshot: NO low-frequency energy at all.",
         // Every other weapon here puts its force at 150-300 Hz. This one has
         // nothing below 350, which makes it unmistakable regardless of how the
         // durations end up - a release of stored tension has no explosion
         // behind it, and the absence is the identity.
         {{"transient", 400, 0.44f, 9, 5, 0, 0, 0, 0, 0, 0},
          {"body", 450, 400, 0.72f, 85, 38, 0, 0, 0, 0, 0},
          {"texture", 380, 1.8f, 0.16f, 70, 40, 0, 0, 0, 0, 0}}},

        {"HL2_RPG_FIRE", "The only shot whose pitch RISES - a rocket leaving and still going.",
         // A rocket is not an impulse. The motor keeps pushing after it has
         // left, so this is the one discharge that climbs and sustains instead
         // of decaying, which no ratio can confuse with anything else.
         {{"transient", 240, 0.34f, 14, 9, 0, 0, 0, 0, 0, 0},
          {"body", 130, 175, 0.72f, 580, 320, 0.28f, 7.0f, 0, 0, 0},
          {"texture", 220, 0.9f, 0.24f, 420, 260, 0, 0, 0, 0, 0}}},

        {"HL2_GRENADE_FIRE", "A throw, not a discharge: a departure with no hard edge.",
         // Moved down and lengthened off the pistol, which it was sitting on at
         // 129 ms / 209 Hz against 112 ms / 268 Hz. Throwing and shooting are
         // the two things a hand does most in this game and they must not be
         // confusable; a release is slower and lower than a discharge, so the
         // separation is also the more accurate shape.
         {{"body", 110, 240, 0.34f, 150, 80, 0, 0, 0, 0, 0}}},

        {"HL2_DAMAGE", "Scaled by how hard you were hit; these are the full-strength values.",
         {{"transient", 300, 0.55f, 14, 9, 0, 0, 0, 0, 0, 0},
          {"body", 175, 95, 0.85f, 195, 110, 0, 0, 0, 0, 0},
          {"texture", 260, 0.9f, 0.30f, 140, 95, 0, 0, 0, 0, 12}}},

        {"HL2_DAMAGE_FIRE", "Burning. A sustained scald with no impact in it at all.",
         // Ported from the bHaptics Half-Life integration, which splits damage
         // by SOURCE where this project splits it by what the arms feel.
         // Nothing struck you, so there is no transient - that absence is most
         // of what makes it read as burning rather than as being hit. What it
         // has instead is length and a rough, uneven crackle.
         {{"body", 200, 175, 0.62f, 520, 300, 0.45f, 11.0f, 22.0f, 31.0f, 0},
          {"texture", 300, 0.7f, 0.30f, 480, 300, 0, 0, 0, 0, 0}}},

        // Transient dropped 500 -> 400 Hz, for the reason given on the crossbow.
        // shock-hand shares this profile and was the MOST limited effect in the
        // game at 0.82; both it and damage-shock gain 0.03-0.04 of headroom for
        // 1% rms. Everything here still sits above 380 Hz, so the identity -
        // the sharpest and brightest thing in the game - is untouched.
        {"HL2_DAMAGE_SHOCK", "Electricity. The sharpest, brightest, shortest thing here.",
         // The opposite of fire on every axis, which is how the two stay apart:
         // instantaneous where fire sustains, bright where fire is mid, and
         // carried by a very fast tremolo that nothing else in the game uses.
         {{"transient", 400, 0.60f, 8, 4, 0, 0, 0, 0, 0, 0},
          {"body", 430, 380, 0.78f, 95, 40, 0.65f, 55.0f, 0, 0, 0}}},

        {"HL2_DAMAGE_TOXIC", "Poison. Slow, low and wrong - it arrives after the hit.",
         // Headcrab poison and toxic sludge. The one damage type that is not an
         // impact but a STATE, so it is the longest and lowest of the set and
         // deliberately has a slow sickly wobble rather than an edge.
         {{"body", 110, 92, 0.66f, 620, 380, 0.55f, 3.2f, 9.0f, 5.0f, 0},
          {"texture", 170, 0.8f, 0.22f, 540, 340, 0, 0, 0, 0, 0}}},

        {"HL2_EXPLOSION", "The largest thing that happens to you. Scaled by distance.",
         // One of the very few whole-player events that earns a haptic in both
         // hands: a blast genuinely arrives through the air and the floor, not
         // through anything you are holding.
         // Levels pulled down from 0.95/1.00/0.55, which drove the limiter to
         // 0.69. Being the biggest thing in the game does not mean riding the
         // ceiling: what a limiter takes first is the sharp transient, so a
         // blast asked to be loudest was paying for it by losing the crack that
         // makes it a blast. The hierarchy is kept by DURATION and by being the
         // only bilateral event with a texture bed under it.
         {{"transient", 280, 0.72f, 26, 16, 0, 0, 0, 0, 0, 0},
          {"body", 150, 45, 0.86f, 320, 210, 0, 0, 0, 0, 0},
          {"texture", 240, 0.8f, 0.42f, 240, 170, 0, 0, 0, 0, 0}}},
    };
    return table;
}

Voice FromSpec(const Spec& s) {
    Voice v;
    const std::string kind = s.kind;
    if (kind == "transient")    v = Transient(s.a, s.b, s.c, s.d);
    else if (kind == "texture") v = Texture(s.a, s.b, s.c, s.d, s.e);
    else if (kind == "tone")    v = Tone(s.a, s.b, s.c, s.d, static_cast<int>(s.e));
    else                        v = Body(s.a, s.b, s.c, s.d, s.e);
    v.amDepth = s.amDepth;
    v.amFreq = s.amFreq;
    v.fmDepth = s.fmDepth;
    v.fmFreq = s.fmFreq;
    if (s.delayMs > 0.0f) v.delay = static_cast<int>(kSampleRate * s.delayMs / 1000.0f);
    return v;
}

std::string Trim(std::string s) {
    const auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const auto b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

bool KnownName(const std::string& n) {
    for (int i = 0; i < kProfileCount; ++i) {
        if (n == kProfileNames[i]) return true;
    }
    return false;
}

// "am:0.4,12" -> depth 0.4, freq 12
bool ParsePair(const std::string& body, float& x, float& y) {
    const auto comma = body.find(',');
    if (comma == std::string::npos) return false;
    try {
        x = std::stof(body.substr(0, comma));
        y = std::stof(body.substr(comma + 1));
    } catch (...) { return false; }
    return true;
}

} // namespace

bool Profiles::Load(const std::string& path, std::vector<std::string>& warnings) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;

    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    // Same BOM tolerance as the main config: PowerShell writes one by default
    // and it would otherwise break the very first section header.
    if (content.size() >= 3 && static_cast<unsigned char>(content[0]) == 0xEF &&
        static_cast<unsigned char>(content[1]) == 0xBB &&
        static_cast<unsigned char>(content[2]) == 0xBF) {
        content.erase(0, 3);
    }

    std::istringstream stream(content);
    std::string line, section;
    int lineNo = 0;
    std::map<std::string, std::vector<Voice>> parsed;

    while (std::getline(stream, line)) {
        ++lineNo;
        line = Trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;

        if (line.front() == '[') {
            const auto close = line.find(']');
            if (close == std::string::npos) {
                warnings.push_back("profiles line " + std::to_string(lineNo) +
                                   ": unterminated section header");
                continue;
            }
            section = Trim(line.substr(1, close - 1));
            if (!KnownName(section)) {
                warnings.push_back("profiles line " + std::to_string(lineNo) +
                                   ": unknown effect '" + section + "', ignored");
                section.clear();
            }
            continue;
        }
        if (section.empty()) continue;

        std::istringstream ls(line);
        Spec s{};
        std::string kind;
        ls >> kind;
        s.kind = nullptr;
        static const char* kKinds[] = {"transient", "body", "texture", "tone"};
        for (const char* k : kKinds) {
            if (kind == k) { s.kind = k; break; }
        }
        if (s.kind == nullptr) {
            warnings.push_back("profiles line " + std::to_string(lineNo) +
                               ": unknown voice type '" + kind + "'");
            continue;
        }

        // transient takes four numbers, everything else takes five.
        const int wanted = (kind == "transient") ? 4 : 5;
        float nums[5]{};
        int got = 0;
        while (got < wanted && (ls >> nums[got])) ++got;
        if (got < wanted) {
            warnings.push_back("profiles line " + std::to_string(lineNo) + ": " + kind +
                               " needs " + std::to_string(wanted) + " numbers, got " +
                               std::to_string(got));
            continue;
        }
        if (kind == "transient") { s.a = nums[0]; s.b = nums[1]; s.c = nums[2]; s.d = nums[3]; }
        else { s.a = nums[0]; s.b = nums[1]; s.c = nums[2]; s.d = nums[3]; s.e = nums[4]; }

        std::string mod;
        while (ls >> mod) {
            const auto colon = mod.find(':');
            if (colon == std::string::npos) {
                warnings.push_back("profiles line " + std::to_string(lineNo) +
                                   ": bad modifier '" + mod + "'");
                continue;
            }
            const std::string key = mod.substr(0, colon);
            const std::string val = mod.substr(colon + 1);
            if (key == "am") {
                if (!ParsePair(val, s.amDepth, s.amFreq)) {
                    warnings.push_back("profiles line " + std::to_string(lineNo) +
                                       ": am needs depth,hz");
                }
            } else if (key == "fm") {
                if (!ParsePair(val, s.fmDepth, s.fmFreq)) {
                    warnings.push_back("profiles line " + std::to_string(lineNo) +
                                       ": fm needs depth,hz");
                }
            } else if (key == "delay") {
                try { s.delayMs = std::stof(val); }
                catch (...) {
                    warnings.push_back("profiles line " + std::to_string(lineNo) +
                                       ": delay needs a number");
                }
            } else {
                warnings.push_back("profiles line " + std::to_string(lineNo) +
                                   ": unknown modifier '" + key + "'");
            }
        }
        parsed[section].push_back(FromSpec(s));
    }

    // A section that appeared but produced no usable voice would silently
    // mute that effect, which is a far worse outcome than ignoring the
    // section. Only non-empty overrides are taken.
    for (auto& [name, voices] : parsed) {
        if (voices.empty()) {
            warnings.push_back("profiles: [" + name +
                               "] had no valid voices, keeping the built-in");
            continue;
        }
        overrides_[name] = std::move(voices);
    }
    return true;
}

bool Profiles::Overridden(const std::string& name) const {
    return overrides_.find(name) != overrides_.end();
}

std::vector<Voice> Profiles::Build(const std::string& name) const {
    const auto it = overrides_.find(name);
    if (it != overrides_.end()) return it->second;
    for (const auto& b : BuiltinTable()) {
        if (name == b.name) {
            std::vector<Voice> out;
            out.reserve(b.voices.size());
            for (const auto& s : b.voices) out.push_back(FromSpec(s));
            return out;
        }
    }
    return {};
}

bool Profiles::Dump(const std::string& path) const {
    std::ofstream f(path, std::ios::trunc);
    if (!f) return false;

    f << "# PSVR2 Alyx Haptics - tactile profiles\n"
         "#\n"
         "# These are the values the program currently uses. Edit them, then run\n"
         "#   psvr2_alyx_haptics.exe --test <name>\n"
         "# to feel the change, or --analyze to measure it. No rebuild needed.\n"
         "#\n"
         "#   transient <freq> <amp> <ms> <decayMs>\n"
         "#   body      <f0> <f1> <amp> <ms> <decayMs>\n"
         "#   texture   <centreHz> <q> <amp> <ms> <decayMs>\n"
         "#   tone      <f0> <f1> <amp> <ms> <attackMs>\n"
         "#\n"
         "# Optional trailing modifiers: am:<depth>,<hz>  fm:<depth>,<hz>  delay:<ms>\n"
         "#\n"
         "# Things worth knowing before you turn a dial:\n"
         "#\n"
         "#   * The actuator produces NOTHING above about 520 Hz. A frequency above\n"
         "#     that is not a bright effect, it is a silent one.\n"
         "#   * There is a real dip at 80-110 Hz and a peak at 180-300 Hz.\n"
         "#   * Pitch alone is coarse: two frequencies need roughly a 1.5x ratio\n"
         "#     before they read as different. Length and tremolo (am:) separate\n"
         "#     effects far better than pitch does.\n"
         "#   * Effects shorter than ~100 ms cannot carry a pitch at all; they all\n"
         "#     read as 'a tap'.\n"
         "#   * If --analyze shows the limiter well below 1.0, the effect is being\n"
         "#     squashed and will feel more like everything else, not less.\n"
         "\n";

    for (const auto& b : BuiltinTable()) {
        f << "# " << b.note << "\n[" << b.name << "]\n";
        const auto it = overrides_.find(b.name);
        const bool custom = it != overrides_.end();
        if (custom) {
            // Dump what is actually in force, not the built-in it replaced.
            for (const auto& v : it->second) {
                char buf[256];
                std::snprintf(buf, sizeof(buf), "body %.1f %.1f %.3f %.1f %.1f",
                              v.f0, v.f1, v.amp,
                              v.length * 1000.0f / kSampleRate,
                              v.decayTau * 1000.0f / kSampleRate);
                f << buf;
                if (v.amDepth > 0.0f) {
                    std::snprintf(buf, sizeof(buf), " am:%.2f,%.1f", v.amDepth, v.amFreq);
                    f << buf;
                }
                if (v.fmDepth > 0.0f) {
                    std::snprintf(buf, sizeof(buf), " fm:%.1f,%.1f", v.fmDepth, v.fmFreq);
                    f << buf;
                }
                f << "\n";
            }
        } else {
            for (const auto& s : b.voices) {
                char buf[256];
                if (std::string(s.kind) == "transient") {
                    std::snprintf(buf, sizeof(buf), "transient %.1f %.3f %.1f %.1f",
                                  s.a, s.b, s.c, s.d);
                } else {
                    std::snprintf(buf, sizeof(buf), "%s %.1f %.1f %.3f %.1f %.1f",
                                  s.kind, s.a, s.b, s.c, s.d, s.e);
                }
                f << buf;
                if (s.amDepth > 0.0f) {
                    std::snprintf(buf, sizeof(buf), " am:%.2f,%.1f", s.amDepth, s.amFreq);
                    f << buf;
                }
                if (s.fmDepth > 0.0f) {
                    std::snprintf(buf, sizeof(buf), " fm:%.1f,%.1f", s.fmDepth, s.fmFreq);
                    f << buf;
                }
                if (s.delayMs > 0.0f) {
                    std::snprintf(buf, sizeof(buf), " delay:%.1f", s.delayMs);
                    f << buf;
                }
                f << "\n";
            }
        }
        f << "\n";
    }
    return f.good();
}

} // namespace psvr2
