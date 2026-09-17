#include "games/hl2vr/hl2vr_adapter.h"

#include "core/events.h"
#include "core/hmd.h"
#include "games/hl2vr/hl2vr_bhaptics.h"
#include "games/hl2vr/hl2vr_surfaces.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <map>

namespace psvr2 {
namespace {

// Label for the persistent "the gravity gun has hold of something" trigger
// load. Refreshed by a heartbeat from the plugin and cleared the instant the
// object leaves the beam.
constexpr const char* kGravLoad = "grav-load";

// The heartbeat arrives every 250 ms. The overlay outlives it by enough to
// bridge a dropped datagram or a frame hitch, but not so long that letting go
// leaves the trigger loaded for a noticeable time.
constexpr int kGravLoadMs = 900;

// ---------------------------------------------------------------------------
// Persistent weapon trigger mechanics.
//
// Half-Life 2 has SEVEN firing weapons where Half-Life: Alyx had three, so the
// separation problem is genuinely harder here and cannot be solved by nudging
// numbers. It is solved the way the Alyx build eventually solved its own: by
// using different trigger MODES, which are differences in kind, and only then
// by laddering strength within a mode.
//
//   crossbow   MultiFeedback, nearly immovable    a drawn bow held at tension
//   shotgun    MultiFeedback, heavy and rising    a long pull that never snaps
//   magnum     Weapon, late and hard              one deliberate heavy break
//   pistol     Weapon, early and light            a crisp break
//   ar2        Slope, rising                      a weapon that spools up
//   smg        Feedback, flat and light           held down for seconds
//   rpg        Feedback, mid                      a plain trigger on a tube
//   grenade    Slope, gentle rising               an arm winding up
//   gravgun    Feedback, almost nothing           the load comes from the OBJECT
//   crowbar    Feedback, deep                     a grip, not a trigger
// ---------------------------------------------------------------------------
TriggerCommand WeaponBase(const std::string& w) {
    if (w == hl2::kCrossbow) {
        // The heaviest resting profile in the game, and the only one that is
        // meant to feel like it does not want to move at all. Holding a drawn
        // crossbow is holding stored energy, which is a completely different
        // sensation from holding a firearm, and it is the one weapon where a
        // near-immovable trigger is physically honest rather than annoying.
        //
        // It is also the weapon fired least often, which is what makes that
        // affordable - the fatigue warning that caps the carry load at 5 of 8
        // is about resistance HELD for minutes, not about a heavy pull taken
        // a few times a minute.
        return trig::MultiFeedback({0, 5, 6, 7, 8, 8, 8, 8, 8, 0});
    }
    if (w == hl2::kShotgun) {
        // Progressive and heavy, and deliberately NOT a clean break: a shotgun
        // is a long pull that loads all the way through. Against the magnum's
        // crisp snap the contrast is a difference in kind rather than degree.
        return trig::MultiFeedback({0, 4, 5, 6, 7, 7, 7, 7, 7, 0});
    }
    if (w == hl2::kMagnum) {
        // WEAPON mode: resists through a band and then GIVES WAY. The .357 is
        // the most deliberate weapon in the game - six rounds, a long refire -
        // so its break sits LATE in the travel and hits hardest.
        return trig::Weapon(3, 7, 7);
    }
    if (w == hl2::kPistol) {
        // The same mechanism as the magnum, one notch of everything lighter and
        // breaking EARLIER. Two weapons in the same mode have to differ by more
        // than strength or they are the same trigger; start position and band
        // width are doing that work here.
        return trig::Weapon(2, 5, 4);
    }
    if (w == hl2::kAr2) {
        // The pulse rifle is not mechanical. SLOPE rises continuously under the
        // finger, which is the closest this hardware gets to something spooling
        // up rather than tripping a sear.
        return trig::Slope(0, 9, 2, 5);
    }
    if (w == hl2::kSmg) {
        // Light single-stage. This is the one weapon genuinely held bottomed
        // out for seconds at a time, so it must stay comfortable - the fatigue
        // warning that governs the carry load applies here too.
        return trig::Feedback(3, 5);
    }
    if (w == hl2::kRpg) {
        return trig::Feedback(5, 5);
    }
    if (w == hl2::kGrenade) {
        // Continuously increasing tension as the arm winds up.
        return trig::Slope(0, 9, 1, 4);
    }
    if (w == hl2::kGravGun) {
        // Almost nothing at rest, and that is the design.
        //
        // The gravity gun's trigger is not a trigger, it is a GRIP: what it
        // should communicate is what is in the beam, and that arrives as an
        // overlay scaled by the object's mass. A heavy resting profile would
        // sit underneath that and flatten the whole range - the difference
        // between an empty beam and a held filing cabinet is the entire
        // sensation this weapon has to offer.
        return trig::Feedback(2, 2);
    }
    if (w == hl2::kCrowbar) {
        // No trigger mechanism to model - a crowbar is a grip, so the
        // resistance sits deep in the travel and never breaks.
        return trig::Feedback(6, 4);
    }
    if (w == hl2::kBugbait) {
        return trig::Feedback(4, 2);
    }
    return trig::Off();
}

// The supercharged gravity gun, from Nova Prospekt onward.
//
// A real state change - the weapon picks up things it previously could not and
// the game makes a point of it - so it earns a different resting feel rather
// than the same one slightly louder. It goes from almost nothing to a live,
// loaded grip that is never quite at rest.
TriggerCommand MegaCannonBase() {
    return trig::Feedback(3, 5);
}

// ---------------------------------------------------------------------------
// Recoil, in the two stages the Alyx recoil bench established.
//
// Stage one is the BREAK: the resistance vanishing, not more of it. In the game
// a weapon's resting profile is already heavy, so an overlay that adds more
// resistance is heavy-to-differently-heavy, and a small delta is what a finger
// reads as nothing. Dropping the trigger to slack for a few tens of
// milliseconds before the kick arrives is both the larger change AND what a
// trigger physically does.
//
// Stage two is the KICK, and it is the layer that carries the sensation. The
// bench proved it by elimination: the same resistance load with and without the
// vibration was the difference between "nothing" and "the best one".
//
// The measured trigger response that governs both:
//   10-30 Hz reads as genuine kickback; past 60 Hz it is only vibration
//   strength 6-7 pushes back BETTER than 8 once the trigger is at its stop
//   Feedback/Weapon/Slope resist; the vibration modes do not, they shake
// ---------------------------------------------------------------------------

// How long the kick must OUTLAST its break.
//
// The break sits at a higher priority, so while it is alive it masks the kick
// underneath. Only the remainder is ever felt, so that remainder is the number
// to design. A shipped build once paired a 260 ms load with a 300 ms kick - a
// 40 ms reveal, under two thirds of one cycle - and was reported as having no
// kickback at all. The layers were right and the arithmetic was wrong.
constexpr int kMinRevealMs = 55;

// Recoil vibration never runs at full drive on a weapon held bottomed out: the
// motor stalls against the stop and its force goes into the controller body
// instead of the finger. Only the SMG is fired that way in Half-Life 2.
constexpr int kMaxSustainedDrive = 7;
bool IsSustainedFire(const std::string& w) { return w == hl2::kSmg; }

// Stage one: how long the trigger goes slack for.
int BreakMs(const std::string& w) {
    if (w == hl2::kShotgun)  return 50;
    if (w == hl2::kMagnum)   return 55;
    if (w == hl2::kPistol)   return 30;
    if (w == hl2::kSmg)      return 20;   // per round, so automatic fire stutters
    if (w == hl2::kAr2)      return 25;
    if (w == hl2::kCrossbow) return 40;
    if (w == hl2::kRpg)      return 60;
    if (w == hl2::kGrenade)  return 40;
    return 0;
}

// Stage two: the kick.
//
// THE LADDER. Seven weapons is far more than the rate axis alone can hold
// (12-40 Hz is barely 3.3x end to end, about four slots at the 1.5x
// discrimination ratio), so every pair is separated on rate OR on reveal
// duration and the report checks that it stayed that way.
//
//   weapon    rate   reveal   drive   reads as
//   magnum    12 Hz   150 ms     8     one enormous slow shove
//   crossbow  14 Hz    60 ms     5     stored tension letting go, and gone
//   shotgun   16 Hz   260 ms     8     a heavy slam that settles
//   rpg       20 Hz   420 ms     7     a long departure that keeps going
//   pistol    26 Hz    60 ms     7     a crisp single snap
//   ar2       34 Hz   140 ms     6     an electrical thrum, not a kick
//   smg       38 Hz   300 ms     6     fast chatter that chains into a rattle
//
// The two 60 ms entries are the pair to watch: they clear on rate (1.86x) and
// on nothing else, which is exactly why the crossbow sits at the bottom of the
// band and the pistol near the top.
TriggerCommand FireOverlay(const std::string& w, int& ms) {
    if (w == hl2::kMagnum) {
        // 12 Hz is 83 ms per cycle, so 150 ms is 1.8 cycles: one huge shove and
        // most of a second. The .357 is the only weapon in the game allowed to
        // feel like it nearly takes the hand with it.
        ms = BreakMs(w) + 150;
        return trig::Vibration(0, 8, 12);
    }
    if (w == hl2::kCrossbow) {
        // 0.84 of one cycle at 14 Hz: a single pulse cut before it completes.
        // That truncation is what makes it a release rather than a wobble - the
        // string going forward is the event, the string coming back is not.
        ms = BreakMs(w) + 60;
        return trig::Vibration(0, 5, 14);
    }
    if (w == hl2::kShotgun) {
        ms = BreakMs(w) + 260;
        return trig::Vibration(0, 8, 16);
    }
    if (w == hl2::kRpg) {
        // The longest of the lot, because a rocket leaving a tube is not an
        // impulse - it is a shove that continues for as long as the motor is
        // still in the room.
        ms = BreakMs(w) + 420;
        return trig::Vibration(0, 7, 20);
    }
    if (w == hl2::kPistol) {
        // 26 Hz is 38 ms per cycle, so 60 ms is 1.55 cycles: a snap and a stub
        // of a return. Short, tight, and the weapon fired most often.
        ms = BreakMs(w) + 60;
        return trig::Vibration(0, 7, 26);
    }
    if (w == hl2::kAr2) {
        // MULTI-POSITION vibration, not the single-position kind.
        //
        // The pulse rifle has no moving parts to kick. What it has is energy
        // leaving it, and a profile that builds with how far the trigger is
        // held reads as a discharge rather than as a mechanism cycling. It is
        // the only weapon here that gets a rising amplitude map, which is a
        // difference in kind from the other six.
        ms = BreakMs(w) + 140;
        return trig::MultiVibration(34, {3, 4, 5, 5, 6, 6, 6, 6, 6, 6});
    }
    if (w == hl2::kSmg) {
        // Automatic fire at roughly 13 rounds/sec, so these chain continuously
        // and the burst becomes one running rattle rather than separate taps -
        // which is this weapon's real identity, more than any single number.
        // Capped at 6 because this is the weapon held bottomed out, where the
        // last notches of drive stop pushing at all.
        ms = BreakMs(w) + 300;
        return trig::MultiVibration(38, {4, 4, 5, 5, 6, 6, 6, 6, 6, 6});
    }
    // The grenade has NO trigger recoil, deliberately. Throwing something is
    // not a discharge - nothing recoils, so there is no impulse for the trigger
    // to represent. Its PCM release effect carries the whole event.
    ms = 0;
    return trig::Off();
}

// Which built-in tactile profile a weapon's shot uses.
const std::map<std::string, std::string>& FireProfiles() {
    static const std::map<std::string, std::string> kMap = {
        {hl2::kPistol,   "HL2_PISTOL_FIRE"},
        {hl2::kMagnum,   "HL2_MAGNUM_FIRE"},
        {hl2::kSmg,      "HL2_SMG_FIRE"},
        {hl2::kAr2,      "HL2_AR2_FIRE"},
        {hl2::kShotgun,  "HL2_SHOTGUN_FIRE"},
        {hl2::kCrossbow, "HL2_CROSSBOW_FIRE"},
        {hl2::kRpg,      "HL2_RPG_FIRE"},
        {hl2::kGrenade,  "HL2_GRENADE_FIRE"},
    };
    return kMap;
}

} // namespace

std::string MapWeaponToken(const std::string& classname) {
    static const std::map<std::string, std::string> kMap = {
        {"weapon_crowbar",    hl2::kCrowbar},
        {"weapon_stunstick",  hl2::kCrowbar},
        {"weapon_pistol",     hl2::kPistol},
        {"weapon_357",        hl2::kMagnum},
        {"weapon_smg1",       hl2::kSmg},
        {"weapon_ar2",        hl2::kAr2},
        {"weapon_shotgun",    hl2::kShotgun},
        {"weapon_crossbow",   hl2::kCrossbow},
        {"weapon_frag",       hl2::kGrenade},
        {"weapon_slam",       hl2::kGrenade},
        {"weapon_rpg",        hl2::kRpg},
        {"weapon_physcannon", hl2::kGravGun},
        {"weapon_bugbait",    hl2::kBugbait},
    };
    if (classname.empty()) return hl2::kHands;
    const auto it = kMap.find(classname);
    return it == kMap.end() ? std::string{} : it->second;
}

Controller Hl2vrAdapter::SideFromParam(const std::string& s) const {
    if (s == "left") return Controller::Left;
    if (s == "right") return Controller::Right;
    // Half-Life 2 VR holds one weapon, in the primary hand. Where the plugin
    // cannot say, that is the honest default rather than a guess at the other.
    return primary_;
}

Material Hl2vrAdapter::MaterialFrom(const std::string& prop,
                                    const std::string& code) const {
    return MaterialFromSource(prop, code.empty() ? '\0' : code[0]);
}

void Hl2vrAdapter::Emit(Controller c, std::vector<Voice> voices,
                        const std::string& event) {
    // Nothing is felt from behind a pause menu.
    if (menuOpen_) return;
    if (voices.empty()) return;
    const float g = std::clamp(cfg_.GainFor(event) * cfg_.GainForWeapon(weapon_),
                               0.0f, 3.0f);
    if (g <= 0.0f) return;
    if (g != 1.0f) for (auto& v : voices) v.amp *= g;
    vary_.Apply(voices);
    // Recorded after every early return above, so the trace reflects what was
    // actually submitted rather than what was intended.
    if (c == Controller::Left || c == Controller::Both) emitTrace_ |= 1;
    if (c == Controller::Right || c == Controller::Both) emitTrace_ |= 2;
    mixer_.Submit(c, std::move(voices));
}

void Hl2vrAdapter::SetWeapon(const std::string& token) {
    if (token == weapon_) return;
    weapon_ = token;
    RefreshWeaponState();
    if (cfg_.debug) std::cout << "[Weapon] " << weapon_ << "\n";
}

void Hl2vrAdapter::RefreshWeaponState() {
    triggers_.SetBase(primary_,
                      (weapon_ == hl2::kGravGun && megaCannon_) ? MegaCannonBase()
                                                                : WeaponBase(weapon_),
                      weapon_);

    // The support hand, when it is on the weapon.
    //
    // An earlier revision of this file asserted that "Half-Life 2 has no
    // two-handed weapon grip". That was simply wrong, and it is worth recording
    // how: the claim was reasoned from FLAT Half-Life 2, where it is true, and
    // never checked against Half-Life 2 VR, where the official manual says
    // "almost all weapons can be held with both hands" and the Steam page
    // advertises two-handed weapons as a headline feature.
    //
    // Reasoning about a VR mod from the game it is a mod OF is exactly the
    // mistake this project keeps warning about in other layers.
    //
    // The brace is firmer for weapons the manual says EXPECT two hands - the
    // SMG and the pulse rifle - than for those where it is optional or purely
    // cosmetic, because the amount of weapon you are actually supporting
    // differs.
    if (twoHand_) {
        const bool expectsTwo = (weapon_ == hl2::kSmg || weapon_ == hl2::kAr2 ||
                                 weapon_ == hl2::kShotgun);
        triggers_.SetBase(Other(), trig::Feedback(3, expectsTwo ? 3 : 2), "brace");
    } else {
        triggers_.SetBase(Other(), trig::Off(), "free");
    }
}

void Hl2vrAdapter::ResetForTest() {
    lastImpact_[0] = Clock::time_point{};
    lastImpact_[1] = Clock::time_point{};
    swingArmedUntil_ = Clock::time_point{};
    heldMass_ = -1.0f;
    heldMaterial_ = Material::Unknown;
    // Cleared directly rather than by sending HL2_MEGA:0, because that event
    // legitimately plays a 620 ms swell when the state changes - and a reset
    // that emits leaks into the NEXT case's measurement. carry-grab measured
    // 670 ms of somebody else's waveform before this was found.
    megaCannon_ = false;
    // The shotgun forces this true because the game requires two hands to pump
    // it, so without clearing it here the brace case measured nothing: the
    // shotgun test had already braced, and TWO_HAND only emits on a CHANGE.
    // Found by --verify, not by feel.
    twoHand_ = false;
    vary_.Reseed();
}

void Hl2vrAdapter::Fire(const std::string& w, int roundsLeft, bool secondary) {
    int loadMs = BreakMs(w);
    int ms = 0;
    TriggerCommand kick = FireOverlay(w, ms);

    // Bracing genuinely reduces recoil in Half-Life 2 VR - the manual is
    // explicit that the pistol gains "slightly reduced recoil" two-handed and
    // that the SMG and pulse rifle are "reduced significantly". That is a real
    // gameplay difference the player can feel in their aim, so the trigger has
    // to reflect it or the haptics are contradicting the game.
    //
    // Applied to DRIVE only, not to rate or duration: the weapon still kicks
    // with its own character, it simply kicks less hard. Scaling the length
    // would change which weapon it reads as.
    if (twoHand_) {
        const bool expectsTwo = (w == hl2::kSmg || w == hl2::kAr2);
        const float scale = expectsTwo ? 0.62f : 0.82f;
        auto damp = [scale](uint8_t v) {
            return static_cast<uint8_t>(std::max(1, static_cast<int>(
                std::lround(static_cast<float>(v) * scale))));
        };
        if (kick.mode == kTriggerVibration) {
            kick.data.vibration.amplitude = damp(kick.data.vibration.amplitude);
        } else if (kick.mode == kTriggerMultiPositionVibration) {
            for (auto& a : kick.data.multiVibration.amplitude) a = damp(a);
        }
    }

    // Clamp the kick's drive, whatever the table asked for. Full drive
    // measurably pushes back WORSE than 6-7 once the trigger is bottomed out,
    // and enforcing it here rather than trusting each entry means a future
    // "make it stronger" edit cannot silently make it weaker in the hand.
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
    // trusting the two tables to stay in step.
    const int reveal = ms - loadMs;
    if (loadMs > 0 && ms > 0 && reveal < kMinRevealMs) {
        if (cfg_.debug) {
            std::cout << "[Trigger] " << w << " reveal is only " << reveal
                      << " ms; clamping the break so the kick can be felt\n";
        }
        loadMs = std::max(0, ms - kMinRevealMs);
    }

    if (loadMs > 0) {
        triggers_.PushOverlay(primary_, trig::Off(), loadMs, 7, w + "-break");
    }
    if (ms > 0) {
        triggers_.PushOverlay(primary_, kick, ms, 6, w + "-fire");
    }

    const auto it = FireProfiles().find(w);
    std::vector<Voice> v =
        profiles_.Build(it == FireProfiles().end() ? "HL2_PISTOL_FIRE" : it->second);

    // The shotgun's secondary is BOTH barrels at once. Sizing it as "the same
    // shot, louder" would be a lie the limiter would flatten anyway; what
    // actually differs is that two charges leave together, so it goes lower and
    // longer rather than harder.
    if (secondary && w == hl2::kShotgun) {
        for (auto& voice : v) {
            voice.f0 *= 0.82f;
            voice.f1 *= 0.82f;
            voice.length = static_cast<int>(voice.length * 1.25f);
            voice.decayTau *= 1.25f;
        }
        triggers_.PushOverlay(primary_, trig::Vibration(0, 8, 14), 340, 6,
                              "SHOTGUN-double");
    }

    // A new shot replaces the previous shot's tail instead of summing with it.
    // Without this, rapid fire accumulates energy until the limiter pulls the
    // whole mix down, and the first casualty is the sharp transient that makes
    // a shot read as a shot rather than as mush.
    for (auto& voice : v) voice.group = kGroupWeaponFire;
    Emit(primary_, std::move(v), "FIRE");

    // The support hand feels the FRAME, not the action: duller and quieter.
    //
    // Only when the off-hand is actually on the weapon, which in this game is
    // a deliberate act the player performs rather than a permanent state.
    if (twoHand_) {
        std::vector<Voice> support;
        const bool heavy = (w == hl2::kShotgun || w == hl2::kRpg ||
                            w == hl2::kMagnum);
        support.push_back(Body(heavy ? 100.0f : 170.0f,
                               heavy ? 60.0f : 140.0f,
                               heavy ? 0.42f : 0.26f,
                               heavy ? 220.0f : 110.0f,
                               heavy ? 130.0f : 55.0f));
        Emit(Other(), std::move(support), "FIRE");
    }

    // Running dry.
    //
    // Guerrilla use "the absence of adaptive tension to communicate when you're
    // out of ammo", and that is exactly right: the cue is the trigger going
    // slack, plus a low dead clack placed late enough to be heard as separate
    // information rather than as part of the shot.
    if (roundsLeft == 0) {
        std::vector<Voice> dry;
        auto tick = Transient(300, 0.40f, 10, 6);
        tick.delay = kSampleRate * 335 / 1000;
        dry.push_back(tick);
        auto hollow = Body(195, 150, 0.37f, 130, 70);
        hollow.delay = kSampleRate * 337 / 1000;
        dry.push_back(hollow);
        Emit(primary_, std::move(dry), "FIRE");
        triggers_.PushOverlay(primary_, trig::Feedback(1, 2), 320, 5, "empty");
    }
}

void Hl2vrAdapter::Impact(Material m, float energy, Controller side, float mass,
                          float spin, float confidence) {
    const ImpactSpec spec{m, energy, mass, spin, confidence};

    std::vector<Voice> sympathetic = BuildSympatheticImpact(spec);
    if (!sympathetic.empty()) {
        const Controller opposite =
            (side == Controller::Left) ? Controller::Right : Controller::Left;
        Emit(opposite, std::move(sympathetic), "PHYS_IMPACT");
    }
    Emit(side, BuildImpact(spec), "PHYS_IMPACT");

    const float trust = 0.55f + 0.45f * std::clamp(confidence, 0.0f, 1.0f);
    const float e = std::clamp(energy, 0.05f, 1.0f) * trust;
    triggers_.PushOverlay(side, trig::Feedback(ImpactTriggerPosition(e),
                                               ImpactTriggerStrength(e)),
                          55, 4, "impact");
}

std::vector<RecoilSpec> Hl2vrAdapter::RecoilLadder() const {
    std::vector<RecoilSpec> out;
    for (const char* w : {hl2::kPistol, hl2::kMagnum, hl2::kSmg, hl2::kAr2,
                          hl2::kShotgun, hl2::kCrossbow, hl2::kRpg}) {
        int kickMs = 0;
        const TriggerCommand kick = FireOverlay(w, kickMs);
        const int breakMs = BreakMs(w);
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
        const TriggerCommand base = WeaponBase(w);
        int rest = 0;
        switch (base.mode) {
            case kTriggerFeedback: rest = base.data.feedback.strength; break;
            case kTriggerWeapon:   rest = base.data.weapon.strength;   break;
            case kTriggerSlopeFeedback:
                rest = std::max(base.data.slope.startStrength,
                                base.data.slope.endStrength);
                break;
            case kTriggerMultiPositionFeedback:
                for (uint8_t v : base.data.multiFeedback.strength) {
                    rest = std::max(rest, static_cast<int>(v));
                }
                break;
            default: break;
        }
        out.push_back({w, IsSustainedFire(w) ? std::min(drive, kMaxSustainedDrive)
                                             : drive,
                       rate, kickMs - breakMs, rest});
    }
    return out;
}

std::vector<std::string> Hl2vrAdapter::ConnectionHelp() const {
    return {
        "bhaptics (ws)   : 127.0.0.1:15881   <- needs NOTHING installed",
        "                  Half-Life 2 VR has bHaptics support built in and",
        "                  announces its own events here. Turn ON bHaptics in",
        "                  the game's options, and make sure the bHaptics",
        "                  Player is NOT running - it owns this port.",
        "plugin (udp)    : 127.0.0.1:29001   <- richer: mass, material, spin",
        "                  --install writes the .vdf; the DLL is a separate",
        "                  build, see src/games/hl2vr/plugin/README.md",
        "or console.log  : -condebug         (slowest fallback)",
    };
}

void Hl2vrAdapter::Handle(const std::string& event, const std::string& param) {
    const auto parts = SplitFields(param, ',');
    auto arg = [&](size_t i) { return FieldStr(parts, i); };
    auto num = [&](size_t i, float fallback) { return FieldFloat(parts, i, fallback); };

    // ---- the game's own bHaptics stream -----------------------------------
    //
    // Half-Life 2 VR ships with bHaptics support built in, which means it is
    // already broadcasting its own semantic event vocabulary to a local
    // WebSocket. When the bHaptics Player is not running - and it will not be,
    // on a PSVR2 machine - we can be the thing that answers, and get the
    // developers' own classification instead of inferring one.
    //
    // See core/bhaptics_listener.h for the transport and
    // games/hl2vr/hl2vr_bhaptics.h for why the key mapping is deliberately
    // conservative.
    if (event == "BH_APP") {
        std::cout << "[bHaptics] " << param << " connected and is announcing "
                     "its haptic events\n";
        return;
    }
    if (event == "BH_REGISTER") {
        // The game listing a pattern it owns. Nothing plays; this is the
        // vocabulary arriving. An unmapped key is remembered so it can be
        // reported as a list rather than scrolling past as noise.
        const BhapticsMapping m = MapBhapticsKey(param);
        if (m.event.empty()) {
            if (std::find(unmappedBhaptics_.begin(), unmappedBhaptics_.end(), param) ==
                unmappedBhaptics_.end()) {
                unmappedBhaptics_.push_back(param);
            }
        }
        if (cfg_.debug) {
            std::cout << "[bHaptics] register " << param << " -> "
                      << (m.event.empty() ? "(unmapped)" : m.event) << "  ["
                      << (m.rule ? m.rule : "?") << "]\n";
        }
        return;
    }
    if (event == "BH_SUBMIT") {
        // The game FIRING one of its patterns. This is the actual event.
        const BhapticsMapping m = MapBhapticsKey(param);
        if (m.event.empty()) {
            if (cfg_.debug) {
                std::cout << "[bHaptics] ignored " << param << " ("
                          << (m.rule ? m.rule : "no rule matched") << ")\n";
            }
            return;
        }
        if (cfg_.debug) {
            std::cout << "[bHaptics] " << param << " -> " << m.event << "\n";
        }
        Handle(m.event, m.params);
        return;
    }

    // ---- session and state ----------------------------------------------
    if (event == "MENU") {
        // A weapon's trigger resistance is a physical claim about something the
        // player is holding right now, so leaving it loaded while they sit in a
        // menu is just an unexplained stiff trigger. Release it, and restore
        // the exact same state on the way out.
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
    if (event == "HL2_WEAPON") {
        // The plugin sends the classname; unrecognised ones are reported rather
        // than silently mapped, so a modded or HL2VR-specific weapon gets
        // classified against real data instead of guessed at.
        const std::string token = MapWeaponToken(arg(0));
        if (token.empty()) {
            if (cfg_.debug) {
                std::cout << "[Weapon] unmapped classname: " << arg(0) << "\n";
            }
            SetWeapon(hl2::kHands);
            return;
        }
        SetWeapon(token);
        return;
    }
    if (event == "HL2_MEGA") {
        // The supercharged gravity gun. Read by the plugin from the
        // physcannon_mega_enabled convar, which is how the game itself decides.
        const bool on = (param == "1");
        if (on == megaCannon_) return;
        megaCannon_ = on;
        RefreshWeaponState();
        // The moment it changes is worth feeling: the weapon comes alive in the
        // hand. A slow swell rather than a hit, because nothing struck you.
        std::vector<Voice> v;
        // The longest and brightest thing the weapon ever does, deliberately:
        // it is the moment the gravity gun stops being a tool and becomes a
        // weapon, and it should not be confusable with any ordinary launch.
        auto charge = Tone(180, 400, 0.52f, 760, 180);
        charge.amDepth = 0.42f;
        charge.amFreq = 11.0f;
        charge.fmDepth = 16.0f;
        charge.fmFreq = 27.0f;
        v.push_back(charge);
        Emit(primary_, std::move(v), "HL2_MEGA");
        return;
    }

    // ---- weapon ----------------------------------------------------------
    if (event == "HL2_FIRE") {
        const std::string token = arg(0).empty() ? weapon_ : arg(0);
        if (token != weapon_ && !token.empty()) SetWeapon(token);
        const int roundsLeft = FieldInt(parts, 1, -1);
        const bool secondary = arg(2) == "2";
        // A shot may carry the brace state directly. The shotgun REQUIRES two
        // hands in this game - it cannot be pumped otherwise - so a shotgun
        // shot is braced whether or not anything said so.
        if (arg(3) == "2H") twoHand_ = true;
        else if (arg(3) == "1H") twoHand_ = false;
        if (weapon_ == hl2::kShotgun) twoHand_ = true;
        Fire(weapon_, roundsLeft, secondary);
        return;
    }
    if (event == "HL2_TWO_HAND") {
        // The off-hand coming onto, or leaving, the weapon.
        const bool on = (param == "1" || arg(0) == "1");
        if (on == twoHand_) return;
        twoHand_ = on;
        RefreshWeaponState();
        std::vector<Voice> v;
        if (twoHand_) {
            // The hand arrives and the weapon STEADIES. The settle underneath
            // is the point: what bracing changes is that the thing stops
            // moving, so the cue resolves downward into stillness rather than
            // ending on an edge.
            v.push_back(Transient(320, 0.26f, 9, 6));
            v.push_back(Body(230, 178, 0.46f, 145, 88));
        } else {
            // Letting go. Lighter, shorter, and RISING as the hand leaves.
            v.push_back(Body(215, 275, 0.30f, 95, 55));
        }
        Emit(Controller::Both, std::move(v), "HL2_TWO_HAND");
        return;
    }
    if (event == "HL2_DRYFIRE") {
        // The trigger broke on nothing. A bare mechanical click with no body
        // under it at all - the absence of the shot IS the information.
        std::vector<Voice> v;
        v.push_back(Transient(430, 0.34f, 9, 5));
        v.push_back(Body(210, 170, 0.26f, 70, 34));
        Emit(primary_, std::move(v), "HL2_DRYFIRE");
        triggers_.PushOverlay(primary_, trig::Feedback(1, 2), 260, 5, "empty");
        return;
    }
    if (event == "HL2_RELOAD") {
        // ONE event, rendered as the multi-stage mechanism the weapon actually
        // has.
        //
        // This is deliberate and it is a limitation made honest. Half-Life 2
        // refills a magazine in a single step: m_iClip1 goes from whatever it
        // was to full, and there is no observable moment at which the magazine
        // left, or the slide travelled. Emitting MAG_EJECT and SLIDE as if the
        // plugin had seen them would be inventing events.
        //
        // What is NOT invented is the shape of the mechanism: a pistol really
        // does seat a magazine and then rack a slide, in that order, and the
        // two-part rhythm is what makes a reload read as a reload. So the
        // sequence lives inside one waveform with real timing, which is exactly
        // how the Alyx build renders a slide being racked.
        //
        // The shotgun is the exception and gets per-shell events, because there
        // the clip genuinely increments one at a time and each increment is a
        // real observation.
        const std::string w = arg(0).empty() ? weapon_ : arg(0);
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
        // THE GRID.
        //
        // Eight reload mechanisms is more than any single axis can hold, and the
        // first attempt proved it: every one of them landed between 320 and
        // 360 ms at 125-210 Hz, and the collision report flagged twelve pairs.
        // They had been built as the mechanisms they physically are, which made
        // them different CODE and left them the same EFFECT.
        //
        // So they are placed on the two axes a hand actually reads, at roughly
        // 1.6x steps, before any of the character goes back on top:
        //
        //   mechanism    dur   pitch   what it is
        //   smg          145    390    a box mag and a bolt: fast and bright
        //   shell        235    260    one shell pressed into a tube
        //   pistol       235    165    a mag seating, then a slide slamming
        //   ar2          360    390    an energy cell: a hum with no metal in it
        //   pump         360    105    the heaviest thing done to a weapon
        //   magnum       550    260    a cylinder out, rounds in, a late snap
        //   rpg          550    105    a rocket sliding down a tube
        //   crossbow     820    165    drawing a bow: the longest by far
        //
        // Structure is what makes them feel like different mechanisms; this
        // spacing is what makes them register as different events at all. Both
        // are needed, and the first version had only the former.
        std::vector<Voice> v;
        if (w == hl2::kSmg) {
            // The shortest and brightest: a small box magazine and a bolt, both
            // light pieces of metal moving fast.
            stage(v, 470, 0.36f, 400, 350, 0.62f, 90, 45, 0);
            stage(v, 500, 0.44f, 430, 380, 0.58f, 70, 32, 75);
        } else if (w == hl2::kAr2) {
            // An energy cell, not a magazine. No metal-on-metal at all: it
            // seats with a hum that rises as the weapon takes the charge, which
            // is the only reload in the game with no hard stop in it.
            auto seat = Tone(300, 430, 0.50f, 355, 90);
            seat.amDepth = 0.34f;
            seat.amFreq = 19.0f;
            v.push_back(seat);
        } else if (w == hl2::kMagnum) {
            // A revolver: the cylinder swings out, six rounds drop in together,
            // and it snaps shut. The snap is the loudest thing and it lands
            // late, which is what makes it read as a revolver rather than as a
            // magazine.
            // The GAP is the signature. A revolver is not a fast reload and
            // pretending otherwise loses the one thing that identifies it: the
            // cylinder swings out, there is a real pause while rounds go in,
            // and only then does it snap shut, hard and late.
            stage(v, 330, 0.30f, 280, 235, 0.48f, 150, 85, 0);
            // Six rounds dropping into chambers, filling the gap.
            //
            // Added because the gap measured as SILENCE and dragged the whole
            // effect's dominant pitch down to 135 Hz - onto the rocket reload,
            // which is genuinely that low. The fix is also the more accurate
            // description: a revolver being loaded is not quiet in the middle.
            for (int i = 0; i < 3; ++i) {
                auto drop = Transient(320, 0.20f, 8, 5);
                drop.delay = kSampleRate * (190 + 68 * i) / 1000;
                v.push_back(drop);
                auto seat = Body(300, 260, 0.26f, 55, 28);
                seat.delay = kSampleRate * (190 + 68 * i) / 1000;
                v.push_back(seat);
            }
            stage(v, 500, 0.56f, 265, 215, 0.78f, 140, 70, 410);
        } else if (w == hl2::kCrossbow) {
            // Not a reload - a DRAW. Stored energy going in, so it rises and
            // then locks. Nothing else in the game does that.
            // The LONGEST thing in either game, and it should be: drawing a
            // crossbow is work that takes real time, and that length is most of
            // what tells you the weapon is dangerous again.
            auto draw = Tone(110, 210, 0.46f, 680, 150);
            draw.amDepth = 0.30f;
            draw.amFreq = 8.5f;
            draw.fmDepth = 12.0f;
            draw.fmFreq = 17.0f;
            v.push_back(draw);
            auto lock = Transient(480, 0.50f, 11, 7);
            lock.delay = kSampleRate * 700 / 1000;
            v.push_back(lock);
            auto seat = Body(180, 140, 0.62f, 120, 60);
            seat.delay = kSampleRate * 702 / 1000;
            v.push_back(seat);
        } else if (w == hl2::kRpg) {
            // A rocket sliding down a tube and stopping: one long push against
            // friction, and by a distance the heaviest thing loaded by hand.
            // The lowest of the eight, and long with it: a rocket is the
            // heaviest thing loaded by hand here and it goes in slowly.
            stage(v, 240, 0.26f, 118, 92, 0.74f, 440, 255, 0);
            auto drag = Texture(180, 1.2f, 0.18f, 380, 230);
            drag.delay = kSampleRate * 25 / 1000;
            v.push_back(drag);
        } else {
            // Pistol and anything unclassified: seat the magazine, then rack
            // the slide. The slide moves more metal, so it goes LOWER and
            // longer, and the 60 ms gap is the rhythm that identifies it.
            stage(v, 430, 0.40f, 200, 165, 0.66f, 95, 50, 0);
            stage(v, 500, 0.52f, 170, 130, 0.80f, 135, 78, 100);
        }
        triggers_.PushOverlay(primary_, trig::Vibration(8, 6, 190), 90, 7, "click");
        Emit(primary_, std::move(v), "RELOAD");
        return;
    }
    // =====================================================================
    // MANUAL RELOADING
    //
    // Half-Life 2 VR's default reload is a sequence of PHYSICAL ACTIONS, not
    // one button press. The official manual sets them out per weapon, and they
    // are separate hand movements separated by however long the player takes:
    //
    //   eject the magazine (primary hand button; it falls, and you may CATCH
    //   it with Grip) -> reach over your shoulder with the OFF hand for a
    //   fresh one -> insert it, in a way that differs per weapon -> and for a
    //   weapon run completely dry, chamber a round.
    //
    // An earlier revision rendered all of that as ONE event with a multi-stage
    // waveform, on the reasoning that "Half-Life 2 refills a magazine in a
    // single step". True of flat Half-Life 2; false of Half-Life 2 VR, which is
    // the game this adapter is for.
    //
    // WHICH HAND MATTERS HERE and is the main reason these are separate events.
    // The off-hand does the inserting and the chambering while the primary hand
    // holds the weapon, so an insert is felt in BOTH hands - one pushing, one
    // resisting - and a magazine retrieved from the shoulder is felt only in
    // the off-hand. Sending all of it to the primary hand, as the single-event
    // version did, is wrong about the most basic fact of the gesture.
    //
    // HL2_RELOAD is kept, and now means QUICK RELOAD - the game's alternative
    // one-shot mode, which really is a single event.
    // =====================================================================

    if (event == "HL2_MAG_EJECT") {
        // A latch releases and the magazine DROPS AWAY. The whole character is
        // that something leaves the hand: a small click, then nothing.
        std::vector<Voice> v;
        v.push_back(Transient(430, 0.34f, 8, 5));
        v.push_back(Body(280, 230, 0.44f, 95, 48));
        Emit(primary_, std::move(v), "RELOAD");
        triggers_.PushOverlay(primary_, trig::Vibration(9, 5, 210), 60, 7, "click");
        return;
    }
    if (event == "HL2_MAG_CATCH") {
        // Catching the falling magazine. A small solid arrival in whichever
        // hand grabbed it - and it is genuinely a catch, so it lands and stops.
        std::vector<Voice> v;
        v.push_back(Transient(300, 0.26f, 8, 5));
        v.push_back(Body(180, 148, 0.50f, 155, 90));
        Emit(SideFromParam(arg(0)), std::move(v), "RELOAD");
        return;
    }
    if (event == "HL2_MAG_RETRIEVE") {
        // Reaching over your shoulder and pulling a fresh magazine free.
        //
        // OFF hand, and rising: you cannot see it, the hand closes on something
        // and then it comes away. That upward direction is what separates it
        // from every other reload step, all of which end in something seating.
        std::vector<Voice> v;
        v.push_back(Transient(360, 0.30f, 9, 6));
        v.push_back(Body(250, 340, 0.52f, 230, 130));
        Emit(SideFromParam(arg(0).empty() ? std::string() : arg(0)), std::move(v),
             "RELOAD");
        return;
    }
    if (event == "HL2_MAG_INSERT") {
        // The magazine going home, per weapon, exactly as the manual describes.
        //
        // Emitted to BOTH hands: the off-hand pushes and the primary hand takes
        // the push. That is the physical truth of the gesture and it is also
        // what makes an insert unmistakable against everything else in the
        // sequence, all of which are one-handed.
        const std::string w = arg(0).empty() ? weapon_ : arg(0);
        std::vector<Voice> v;
        if (w == hl2::kSmg) {
            // A short box magazine: light, fast, high.
            v.push_back(Transient(470, 0.36f, 9, 5));
            v.push_back(Body(330, 285, 0.62f, 145, 80));
        } else if (w == hl2::kAr2) {
            // "Snap the space-magazine onto the space-magazine well." Not metal
            // on metal at all - it mates and energises, so it has no hard stop
            // and instead rises into a hum.
            auto snap = Tone(300, 430, 0.54f, 195, 60);
            snap.amDepth = 0.38f;
            snap.amFreq = 26.0f;
            v.push_back(snap);
        } else if (w == hl2::kMagnum) {
            // A speedloader dropping six rounds in together: several small
            // impacts at once rather than one seat.
            for (int i = 0; i < 3; ++i) {
                auto drop = Body(215, 180, 0.34f, 70, 38);
                drop.delay = kSampleRate * (i * 26) / 1000;
                v.push_back(drop);
            }
        } else {
            // Pistol and anything unclassified: into the well, and it stops
            // dead. The hard stop is the signature.
            v.push_back(Transient(430, 0.40f, 9, 5));
            v.push_back(Body(215, 172, 0.70f, 225, 130));
        }
        Emit(Controller::Both, std::move(v), "RELOAD");
        triggers_.PushOverlay(primary_, trig::Vibration(8, 6, 190), 80, 7, "click");
        return;
    }
    if (event == "HL2_CHAMBER") {
        // Chambering a round on a weapon run completely dry.
        //
        // The manual: the pistol's SLIDE is grabbed and pulled back, the SMG's
        // CHARGING HANDLE is pulled back. Both are the off-hand pulling against
        // a spring and then the spring winning - so both are two-stage, and the
        // second stage is the harder one.
        const std::string w = arg(0).empty() ? weapon_ : arg(0);
        std::vector<Voice> v;
        auto stage = [](std::vector<Voice>& out, float tickHz, float tickAmp,
                        float f0, float f1, float amp, float ms, float decay,
                        float delayMs) {
            const int d = static_cast<int>(kSampleRate * delayMs / 1000.0f);
            auto tick = Transient(tickHz, tickAmp, 9, 5);
            tick.delay = d;
            out.push_back(tick);
            auto body = Body(f0, f1, amp, ms, decay);
            body.delay = d;
            out.push_back(body);
        };
        if (w == hl2::kSmg) {
            // A charging handle is a small part on a long spring: lighter than
            // a slide, and the return is quicker.
            stage(v, 480, 0.34f, 300, 255, 0.50f, 70, 34, 0);
            stage(v, 500, 0.46f, 205, 165, 0.68f, 155, 88, 130);
        } else {
            // A pistol slide moves real metal and slams into battery harder
            // than it was pulled. LOW and long, with a wide gap - the two-part
            // rhythm is what identifies a slide.
            stage(v, 470, 0.36f, 250, 210, 0.44f, 65, 32, 0);
            stage(v, 500, 0.54f, 155, 120, 0.82f, 205, 120, 175);
        }
        Emit(Controller::Both, std::move(v), "RELOAD");
        triggers_.PushOverlay(primary_, trig::Vibration(9, 7, 175), 100, 7, "click");
        return;
    }
    if (event == "HL2_PUMP_BACK" || event == "HL2_PUMP_FWD") {
        // The shotgun pump, as TWO separate actions.
        //
        // The manual is explicit: "pull in your off-hand towards your primary
        // hand, then move it back. If both parts of the pump aren't complete,
        // the weapon won't fire. (You will hear a sound for each part.)"
        //
        // So this is not one mechanism with an internal rhythm - it is two
        // things the player does, however far apart they choose. Rendering it
        // as a single two-stage waveform, as an earlier revision did, would
        // fire the second half before the player had performed it.
        //
        // Both are OFF-hand: that hand is doing the pumping.
        std::vector<Voice> v;
        if (event == "HL2_PUMP_BACK") {
            // Drawing it back against the action: friction, then a stop.
            v.push_back(Transient(360, 0.34f, 9, 5));
            v.push_back(Body(160, 128, 0.56f, 175, 100));
            auto drag = Texture(200, 1.2f, 0.16f, 140, 90);
            drag.delay = kSampleRate * 12 / 1000;
            v.push_back(drag);
            triggers_.PushOverlay(primary_, trig::Vibration(6, 5, 160), 90, 7, "click");
        } else {
            // Slamming it forward into lockup. The heaviest, lowest thing the
            // hand does to a weapon in this game.
            v.push_back(Transient(400, 0.52f, 9, 5));
            v.push_back(Body(118, 88, 0.88f, 265, 150));
            triggers_.PushOverlay(primary_, trig::Vibration(8, 7, 130), 120, 7, "click");
        }
        Emit(Controller::Both, std::move(v), "RELOAD");
        return;
    }
    if (event == "HL2_CYLINDER") {
        // The revolver. "Tilt the gun back to empty the previous clip... Flick
        // your hand to close the chamber."
        //
        // Both are PRIMARY hand - this is the one reload in the game the weapon
        // hand performs on its own - and the flick is the moment: a wrist
        // action that ends in a hard metallic snap.
        const bool opening = (arg(0) != "close");
        std::vector<Voice> v;
        if (opening) {
            // The cylinder swings out and the cases fall away. Loose, tumbling,
            // no hard stop at the end of it.
            v.push_back(Transient(330, 0.28f, 9, 6));
            v.push_back(Body(260, 205, 0.46f, 260, 160));
            auto cases = Texture(300, 1.1f, 0.18f, 200, 140);
            cases.delay = kSampleRate * 60 / 1000;
            v.push_back(cases);
        } else {
            // The flick. Short, bright, and it STOPS - the whole character is
            // how abruptly it ends.
            v.push_back(Transient(500, 0.56f, 9, 5));
            v.push_back(Body(400, 330, 0.76f, 95, 40));
        }
        Emit(primary_, std::move(v), "RELOAD");
        triggers_.PushOverlay(primary_, trig::Vibration(7, opening ? 5 : 7, 200),
                              opening ? 90 : 60, 7, "click");
        return;
    }
    if (event == "HL2_BOLT_NOCK") {
        // The crossbow. "Nock a bolt to the far end of the rail (it will
        // automatically draw itself back)."
        //
        // Two beats and the second is not the player's doing: you seat the
        // bolt, and then the weapon takes over and hauls the string back by
        // itself. That handover is the signature, and nothing else in the game
        // does it.
        std::vector<Voice> v;
        v.push_back(Transient(470, 0.36f, 9, 5));
        v.push_back(Body(330, 280, 0.44f, 90, 48));
        auto draw = Tone(120, 230, 0.50f, 470, 150);
        draw.delay = kSampleRate * 110 / 1000;
        draw.amDepth = 0.32f;
        draw.amFreq = 9.0f;
        draw.fmDepth = 13.0f;
        draw.fmFreq = 17.0f;
        v.push_back(draw);
        auto lock = Transient(430, 0.44f, 10, 6);
        lock.delay = kSampleRate * 580 / 1000;
        v.push_back(lock);
        Emit(Controller::Both, std::move(v), "RELOAD");
        triggers_.PushOverlay(primary_, trig::Slope(0, 9, 2, 7), 560, 6, "draw");
        return;
    }
    if (event == "HL2_ROCKET_LOAD") {
        // The RPG. "Slide the tail end of the rocket into the front end of the
        // launcher." One long push against friction, and the heaviest thing
        // loaded by hand in the game.
        std::vector<Voice> v;
        v.push_back(Transient(240, 0.24f, 10, 6));
        v.push_back(Body(120, 95, 0.74f, 430, 250));
        auto drag = Texture(180, 1.2f, 0.20f, 380, 240);
        drag.delay = kSampleRate * 25 / 1000;
        v.push_back(drag);
        Emit(Controller::Both, std::move(v), "RELOAD");
        triggers_.PushOverlay(primary_, trig::Feedback(5, 4), 420, 5, "load");
        return;
    }

    // ---- grenades, as the two-stage gesture the game actually uses --------
    if (event == "HL2_GRENADE_ARM") {
        // "Press and hold the Trigger of your primary hand to arm the grenade."
        //
        // Tension that builds and then WAITS. Nothing resolves here; the throw
        // does that. The trigger loads progressively under the finger, which is
        // the closest this hardware gets to a spoon being held down.
        auto arm = Tone(150, 260, 0.34f, 420, 170);
        arm.amDepth = 0.30f;
        arm.amFreq = 7.0f;
        std::vector<Voice> v{arm};
        triggers_.PushOverlay(primary_, trig::Slope(0, 9, 2, 7), 3000, 5, "grenade-arm");
        Emit(primary_, std::move(v), "HL2_GRENADE_ARM");
        return;
    }
    if (event == "HL2_GRENADE_THROW") {
        // The release at the end of a physical throwing motion. The tension
        // ends, and the object is gone.
        triggers_.ClearOverlays(primary_, "grenade-arm");
        std::vector<Voice> v = profiles_.Build("HL2_GRENADE_FIRE");
        Emit(primary_, std::move(v), "FIRE");
        return;
    }

    // ---- ladders ----------------------------------------------------------
    if (event == "HL2_LADDER") {
        // "Grab the ladder by squeezing your Grip button(s) to climb."
        //
        // A hand closing on a rung, and deliberately quiet: you do this many
        // times in a row and it must not accumulate into a drone.
        std::vector<Voice> v;
        v.push_back(Transient(340, 0.26f, 9, 6));
        v.push_back(Body(205, 168, 0.40f, 105, 62));
        Emit(SideFromParam(arg(0)), std::move(v), "HL2_LADDER");
        return;
    }

    if (event == "HL2_SHELL") {
        // One shell pressed into the tube. Genuinely observed: the shotgun
        // increments its clip one at a time.
        //
        // The friction texture is what a shell actually is - brass dragging on
        // steel for the length of the push, rather than a clean seat.
        std::vector<Voice> v;
        auto tick = Transient(320, 0.26f, 9, 5);
        v.push_back(tick);
        v.push_back(Body(290, 235, 0.74f, 225, 130));
        auto drag = Texture(300, 1.3f, 0.14f, 170, 105);
        drag.delay = kSampleRate * 18 / 1000;
        v.push_back(drag);
        triggers_.PushOverlay(primary_, trig::Vibration(7, 6, 195), 70, 7, "click");
        // Both hands: the off-hand presses the shell in, the primary hand holds
        // the weapon steady against it.
        Emit(Controller::Both, std::move(v), "RELOAD");
        return;
    }
    if (event == "HL2_PUMP") {
        // The pump. The largest mechanical event in the game: slide back, then
        // a hard slam into lockup, with a wide gap because that gap IS the
        // gesture. The slam drops into the low lobe - a pump assembly reaching
        // lockup is the heaviest thing your hand does to a weapon here.
        std::vector<Voice> v;
        auto t1 = Transient(360, 0.40f, 9, 5);
        v.push_back(t1);
        v.push_back(Body(170, 135, 0.52f, 80, 40));
        auto t2 = Transient(400, 0.56f, 9, 5);
        t2.delay = kSampleRate * 120 / 1000;
        v.push_back(t2);
        // The pump keeps the low band and gives up LENGTH instead.
        //
        // It sat on the rocket reload at 103 Hz against 100 Hz, and the first
        // attempt to fix that raised its pitch - which cleared the rocket and
        // landed straight on the pistol reload at 162 Hz instead. The low band
        // is crowded and the mid band is crowded; what was actually free was
        // the short end.
        //
        // Which is the more accurate answer anyway. A pump is a FAST violent
        // cycle - back and forward in a third of a second - where loading a
        // rocket is slow, heavy work. Making the pump the shorter of the two
        // is not a compromise for the sake of a number; it is what the two
        // actions are.
        auto slam = Body(115, 82, 0.88f, 180, 105);
        slam.delay = kSampleRate * 120 / 1000;
        v.push_back(slam);
        triggers_.PushOverlay(primary_, trig::Vibration(8, 7, 150), 110, 7, "click");
        Emit(primary_, std::move(v), "RELOAD");
        return;
    }
    if (event == "HL2_CHARGE_START") {
        // The AR2's alternate fire winds up before it releases: the weapon sets
        // a delayed shot half a second out. That gap is real and observable, so
        // it gets the rising tension it deserves and TELE/fire resolves it.
        auto spin = Tone(170, 300, 0.36f, 420, 150);
        spin.amDepth = 0.45f;
        spin.amFreq = 24.0f;
        std::vector<Voice> v{spin};
        triggers_.PushOverlay(primary_, trig::Slope(0, 9, 3, 8), 460, 6, "ar2-charge");
        Emit(primary_, std::move(v), "HL2_CHARGE_START");
        return;
    }
    if (event == "HL2_CHARGE_FIRE") {
        // The ball leaves. A hard departure with a long electrical tail, and
        // deliberately not shaped like a bullet: nothing recoils, something
        // heavy and alive simply goes.
        std::vector<Voice> v;
        v.push_back(Transient(420, 0.52f, 12, 7));
        auto release = Body(320, 120, 0.82f, 260, 150);
        release.amDepth = 0.35f;
        release.amFreq = 30.0f;
        v.push_back(release);
        triggers_.PushOverlay(primary_, trig::Off(), 40, 7, "AR2-break");
        triggers_.PushOverlay(primary_, trig::Vibration(0, 7, 22), 260, 6, "AR2-alt");
        Emit(primary_, std::move(v), "HL2_CHARGE_FIRE");
        return;
    }

    // ---- melee -----------------------------------------------------------
    if (event == "HL2_MELEE_SWING") {
        // NOTHING PLAYS HERE, AND THAT IS THE POINT.
        //
        // Swinging a crowbar through empty air is the single most common way a
        // melee weapon is made to feel wrong: your hand feels the weight of the
        // bar, not a vibration, and a buzz on every swing turns the most-used
        // tool in the game into the noise floor everything else competes with.
        //
        // So a swing only ARMS the window in which an impact will be believed,
        // and loads the trigger as the grip tightens. If nothing is struck,
        // nothing is felt - which is exactly what happens in reality.
        swingArmedUntil_ = Clock::now() + std::chrono::milliseconds(400);
        swingWeapon_ = arg(0).empty() ? weapon_ : arg(0);
        triggers_.PushOverlay(primary_, trig::Feedback(5, 5), 180, 4, "swing");
        return;
    }
    if (event == "HL2_MELEE_HIT") {
        // weapon,surfaceprop,charcode,energy
        const Material m = MaterialFrom(arg(1), arg(2));
        const float e = std::clamp(num(3, 0.7f), 0.05f, 1.0f);
        // A hit reported outside the swing window is not trusted at full
        // weight: the plugin infers hits from a re-traced surface, and a stale
        // one is exactly the case where asserting certainty would be wrong.
        const bool armed = Clock::now() < swingArmedUntil_;
        swingArmedUntil_ = Clock::time_point{};

        // Melee is the one place where the SAME material must feel heavier than
        // it does as a dropped prop: you are driving a steel bar into it with
        // your whole arm. Energy carries that, and the impact builder's own
        // length scaling does the rest.
        Impact(m, e, primary_, 3.0f, 0.0f, armed ? 1.0f : 0.6f);
        return;
    }

    // ---- gravity gun -----------------------------------------------------
    if (event == "HL2_GRAV_GRAB") {
        // mass,surfaceprop,charcode
        const float mass = std::clamp(num(0, -1.0f), -1.0f, 400.0f);
        heldMass_ = mass;
        heldMaterial_ = MaterialFrom(arg(1), arg(2));
        // Half-Life 2's props run far heavier than Alyx's, so the useful
        // resolution is spread wider - a filing cabinet and a soda can are two
        // orders of magnitude apart and both are routine.
        const float heft = mass < 0.0f ? 0.35f
                         : std::clamp(std::sqrt(mass / 60.0f), 0.05f, 1.0f);

        // The object SNAPS into the beam. Bright, immediate, and then the
        // weight arrives underneath it - the same split the Alyx catch uses,
        // for the same reason: capture and mass in one band fight each other
        // and the limiter flattens a heavy grab back towards a light one.
        std::vector<Voice> v;
        v.push_back(Transient(430, 0.40f, 10, 6));
        auto lock = Body(330, 275, 0.54f, 105, 52);
        v.push_back(lock);
        const float f0 = 300.0f - 215.0f * heft;
        auto load = Body(f0, f0 * 0.60f, 0.16f + 0.30f * heft,
                         180.0f + 320.0f * heft, 95.0f + 165.0f * heft);
        load.delay = kSampleRate * 26 / 1000;
        // Something heavy in the beam does not sit still - it sags and hunts.
        load.amDepth = 0.22f + 0.24f * heft;
        load.amFreq = 4.5f + 4.0f * heft;
        v.push_back(load);
        // A trace of the material, so you can feel WHAT is in the beam and not
        // just how heavy it is. Deliberately faint: it identifies, it does not
        // announce.
        if (heldMaterial_ != Material::Unknown) {
            const MaterialRecipe r = RecipeFor(heldMaterial_);
            auto colour = Body(r.toneF0, r.toneF1, 0.10f + 0.08f * heft, 85, 42);
            colour.amDepth = r.amDepth;
            colour.amFreq = r.amFreq;
            colour.delay = kSampleRate * 12 / 1000;
            v.push_back(colour);
        }
        Emit(primary_, std::move(v), "HL2_GRAV_GRAB");

        // And the trigger takes the load, proportional to what is now in the
        // beam. Capped at 5 of 8: this is held for as long as the player
        // carries something, which is precisely the case Guerrilla's finger
        // fatigue warning is about. Weight is carried by being CONSTANT.
        const int s = std::clamp(static_cast<int>(std::lround(1 + heft * 4)), 1, 5);
        triggers_.RefreshOverlay(primary_, trig::Feedback(4, s), kGravLoadMs, 3,
                                 kGravLoad);
        if (cfg_.debug) {
            std::cout << "[GravGun] grab " << MaterialName(heldMaterial_)
                      << " mass=" << mass << " heft=" << heft << "\n";
        }
        return;
    }
    if (event == "HL2_GRAV_HOLD") {
        // mass,handSpeed,spin - a 4 Hz heartbeat while something is in the beam.
        //
        // Two channels doing two different jobs: the TRIGGER holds a static
        // load proportional to mass, the constant fact of the thing being
        // heavy; the PCM adds inertia only while it is actually being MOVED.
        //
        // Standing still holding a crate produces no PCM whatsoever. That gate
        // is the whole reason this is not ambient buzzing, which would raise
        // the noise floor every other effect competes against.
        const float mass = std::clamp(num(0, 0.0f), 0.0f, 400.0f);
        const float handSpeed = num(1, 0.0f);
        const float spin = num(2, 0.0f);
        heldMass_ = mass;
        const float heft = std::clamp(std::sqrt(mass / 80.0f), 0.0f, 1.0f);

        const int strength = std::clamp(static_cast<int>(std::lround(1 + heft * 4)), 1, 5);
        triggers_.RefreshOverlay(primary_, trig::Feedback(4, strength), kGravLoadMs,
                                 3, kGravLoad);

        const float swing = std::clamp((handSpeed - 55.0f) / 300.0f, 0.0f, 1.0f);
        if (swing > 0.0f && heft > 0.25f) {
            const float drive = swing * heft;
            auto wobble = Body(64.0f + 26.0f * heft, 58.0f + 22.0f * heft,
                               0.05f + 0.20f * drive, 240, 150);
            wobble.amDepth = 0.55f;
            wobble.amFreq = 5.0f + 7.0f * swing;
            if (spin > 200.0f) {
                wobble.fmDepth = 9.0f * std::clamp(spin / 800.0f, 0.0f, 1.0f);
                wobble.fmFreq = 17.0f;
            }
            std::vector<Voice> v{wobble};
            Emit(primary_, std::move(v), "PHYS_HOLD");
        }
        return;
    }
    if (event == "HL2_GRAV_LAUNCH") {
        // mass,speed - the held object fired away.
        const float mass = std::clamp(num(0, heldMass_ < 0.0f ? 10.0f : heldMass_),
                                      0.05f, 400.0f);
        const float speed = std::clamp(num(1, 900.0f), 0.0f, 3000.0f);
        // The hand is empty the instant it lets go, so the load goes NOW rather
        // than waiting for the heartbeat to lapse. Launching something heavy
        // should feel like relief, and a trigger that stays loaded for most of
        // a second afterwards is the opposite of that.
        triggers_.ClearOverlays(primary_, kGravLoad);

        const float heft = std::clamp(std::sqrt(mass / 60.0f), 0.05f, 1.0f);
        const float e = std::clamp(speed * (0.4f + 0.6f * heft) / 1400.0f, 0.15f, 1.0f);

        std::vector<Voice> v;
        // A hard bark of energy, then the object GOING - rising, because it is
        // leaving. That rise is what separates a launch from a shot; nothing
        // else the weapon does climbs.
        v.push_back(Transient(400, 0.30f + 0.30f * e, 12, 7));
        auto bark = Body(280, 150, 0.55f + 0.35f * e, 130, 72);
        v.push_back(bark);
        // The departure, and the layer that carries what was launched.
        //
        // Mass moves it on BOTH axes at once - a light object leaves fast and
        // high, a heavy one leaves slowly and low - which is what puts the two
        // ends of the range in different cells instead of one. Sizing this by
        // level alone was what made a soda can and a filing cabinet measure as
        // the same launch.
        auto depart = Body(240.0f - 145.0f * heft, 300.0f - 80.0f * heft,
                           0.22f + 0.26f * e,
                           330 + 220 * heft, 190);
        depart.delay = kSampleRate * 30 / 1000;
        depart.amDepth = 0.20f;
        depart.amFreq = 9.0f;
        v.push_back(depart);
        Emit(primary_, std::move(v), "HL2_GRAV_LAUNCH");

        // The trigger snaps free. The break is the sensation, so it is a real
        // gap rather than more resistance.
        triggers_.PushOverlay(primary_, trig::Off(), 60, 7, "GRAVGUN-break");
        triggers_.PushOverlay(primary_, trig::Vibration(0, 7, 18),
                              60 + static_cast<int>(200 * e), 6, "GRAVGUN-fire");
        heldMass_ = -1.0f;
        heldMaterial_ = Material::Unknown;
        return;
    }
    if (event == "HL2_GRAV_PUNT") {
        // surfaceprop,charcode - a shove at something NOT held.
        //
        // Physically a different event from a launch and it must not feel like
        // one: there is no load to release, so there is no break and no relief.
        // What there is, is a short violent push and whatever it hit answering
        // back through the beam.
        const Material m = MaterialFrom(arg(0), arg(1));
        std::vector<Voice> v;
        v.push_back(Transient(380, 0.46f, 11, 7));
        // BRIGHT, and higher than anything else the weapon does.
        //
        // A punt is not a capture and must not measure like one - it was
        // landing at 157 ms / 191 Hz, inside the discrimination threshold of
        // both the light grab and the light launch. Pitch is the axis it can
        // afford to move on: there is no mass in the beam to represent, so
        // nothing is lost by putting the shove up where nothing else sits.
        auto shove = Body(400, 330, 0.70f, 120, 62);
        v.push_back(shove);
        if (m != Material::Unknown) {
            const MaterialRecipe r = RecipeFor(m);
            auto answer = Body(r.toneF0, r.toneF1, 0.16f, 110, 60);
            answer.amDepth = r.amDepth;
            answer.amFreq = r.amFreq;
            answer.delay = kSampleRate * 34 / 1000;
            v.push_back(answer);
        }
        Emit(primary_, std::move(v), "HL2_GRAV_PUNT");
        triggers_.PushOverlay(primary_, trig::Vibration(0, 6, 24), 130, 6, "punt");
        return;
    }
    if (event == "HL2_GRAV_DROP") {
        // Letting go without firing. The load simply ends, and the only thing
        // felt is the beam releasing - light, brief, and rising as it goes.
        triggers_.ClearOverlays(primary_, kGravLoad);
        std::vector<Voice> v;
        // Held down off the punt, which is now the bright end of this family.
        // Letting go and shoving are opposite intentions and were measuring
        // 101 ms / 247 Hz against 138 ms / 333 Hz - inside the threshold on
        // both axes, so the deliberate act and the passive one felt the same.
        v.push_back(Body(170, 230, 0.28f, 95, 52));
        Emit(primary_, std::move(v), "HL2_GRAV_DROP");
        heldMass_ = -1.0f;
        heldMaterial_ = Material::Unknown;
        return;
    }

    // ---- +USE carrying ----------------------------------------------------
    // The same verb as the gravity gun with none of the machinery, so it shares
    // the load mechanism and gets a quieter, duller shape. Carrying a crate by
    // hand should not feel like carrying it in a tractor beam.
    if (event == "HL2_CARRY_GRAB") {
        const float mass = std::clamp(num(0, 1.0f), 0.05f, 400.0f);
        heldMass_ = mass;
        heldMaterial_ = MaterialFrom(arg(1), arg(2));
        const float heft = std::clamp(std::sqrt(mass / 60.0f), 0.05f, 1.0f);
        std::vector<Voice> v;
        v.push_back(Transient(285, 0.16f * heft + 0.06f, 8, 5));
        v.push_back(Body(172, 138, 0.36f * heft + 0.10f, 118, 70));
        Emit(primary_, std::move(v), "PHYS_PICKUP");
        const int s = std::clamp(static_cast<int>(std::lround(1 + heft * 4)), 1, 5);
        triggers_.RefreshOverlay(primary_, trig::Feedback(4, s), kGravLoadMs, 3,
                                 kGravLoad);
        return;
    }
    if (event == "HL2_CARRY_DROP") {
        triggers_.ClearOverlays(primary_, kGravLoad);
        std::vector<Voice> v;
        // Letting go is not throwing. A drop is the shortest thing the hand
        // does - the load simply stops - where a throw is a deliberate
        // departure with the object's material still on the skin. Those two
        // measured 124 ms and 129 ms before this, which is one event twice.
        v.push_back(Body(230, 290, 0.22f, 70, 38));
        Emit(primary_, std::move(v), "PHYS_THROW");
        heldMass_ = -1.0f;
        return;
    }
    if (event == "HL2_CARRY_THROW") {
        // mass,speed,surfaceprop,charcode
        //
        // The material travels WITH the event rather than being remembered from
        // the grab. An event that only works when the one before it arrived is
        // an event that fails silently, and this one failed in exactly that way:
        // with no preceding grab the entire material layer vanished and a throw
        // measured as a bare 100 ms release.
        const float mass = std::clamp(num(0, 1.0f), 0.05f, 400.0f);
        const float speed = std::clamp(num(1, 400.0f), 0.0f, 2000.0f);
        const Material thrown = arg(2).empty() ? heldMaterial_
                                               : MaterialFrom(arg(2), arg(3));
        triggers_.ClearOverlays(primary_, kGravLoad);
        const float e = std::clamp(speed * mass / 4000.0f, 0.05f, 1.0f);
        std::vector<Voice> v;
        // A release is a departure, not an impact: rising pitch, no hard edge.
        v.push_back(Body(150, 300, 0.16f * e + 0.08f, 95, 52));
        if (thrown != Material::Unknown) {
            const MaterialRecipe r = RecipeFor(thrown);
            // The last thing the hand feels of an object is its surface leaving
            // the skin, so the departure is tinted by what it was.
            // Long enough to be told from both the grab before it and the bare
            // drop it is the opposite of. All three measured inside 100-130 ms
            // before this, which is one gesture rendered three times.
            v.push_back(Body(r.toneF0 * 0.62f, r.toneF0 * 0.95f,
                             0.10f + 0.16f * e, 210, 120));
        }
        Emit(primary_, std::move(v), "PHYS_THROW");
        triggers_.PushOverlay(primary_, trig::Vibration(2, std::clamp(
            static_cast<int>(std::lround(2 + e * 5)), 1, 8), 120), 45, 4, "throw");
        heldMass_ = -1.0f;
        heldMaterial_ = Material::Unknown;
        return;
    }

    // ---- physics impacts --------------------------------------------------
    if (!cfg_.physics && event.rfind("HL2_IMPACT", 0) == 0) return;
    if (event == "HL2_IMPACT") {
        // impulse,mass,surfaceprop,charcode,spin,confidence
        //
        // The rule this project is built on: the hand only feels a collision it
        // is physically connected to. The plugin only reports impacts of an
        // object the player is currently holding or has in the beam - an object
        // that has already left is somebody else's problem no matter how
        // spectacularly it lands.
        const float impulse = num(0, 0.0f);
        if (impulse < cfg_.minImpactImpulse) return;
        const float mass = std::clamp(num(1, 1.0f), 0.05f, 400.0f);
        const Material m = MaterialFrom(arg(2), arg(3));
        const float spin = num(4, 0.0f);
        const float confidence = std::clamp(num(5, 1.0f), 0.0f, 1.0f);

        // Rate limit per hand. A held crate dragged along a wall satisfies the
        // plugin's test every tick; a strike is an event, not a texture.
        const Controller side = primary_;
        const size_t slot = (side == Controller::Left) ? 0 : 1;
        const auto now = Clock::now();
        if (lastImpact_[slot].time_since_epoch().count() != 0 &&
            now - lastImpact_[slot] <
                std::chrono::milliseconds(std::max(0, cfg_.impactCooldownMs))) {
            return;
        }
        lastImpact_[slot] = now;

        // Normalised against a "solid hit" reference rather than a physical
        // unit. Half-Life 2's props are heavier than Alyx's, so the reference
        // is higher: reusing Alyx's would put every crate at full scale and
        // throw away the whole top of the range.
        const float e = std::clamp(impulse / 3200.0f, 0.06f, 1.0f);
        if (cfg_.debug) {
            std::cout << "[Impact] " << MaterialName(m) << " impulse=" << impulse
                      << " mass=" << mass << " spin=" << spin << " e=" << e
                      << " confidence=" << confidence << "\n";
        }
        Impact(m, e, side, mass, spin, confidence);
        return;
    }

    // ---- damage -----------------------------------------------------------
    if (event == "HL2_DAMAGE") {
        // amount,armour,type
        //
        // Sized by the HIT, not by the health bar. Scaling by remaining health
        // would mean identical impacts felt different depending on a number on
        // your HUD, which is a readout rather than a sensation.
        //
        // TYPE is ported from the bHaptics Half-Life integration, which carries
        // DamageFire, DamageSpark, DamageLaser and EnvironmentPoison as separate
        // effects. That split is worth having and this project did not have it:
        // burning, being shocked and being poisoned genuinely do not feel like
        // being shot, and the arms are where all four arrive.
        //
        // What is NOT ported is bHaptics' other split - Combine, MetroPolice,
        // Sniper, Turret, Strider, and the whole Unarmed<enemy> family. Those
        // encode WHO hit you as a direction across the torso. A vest can say
        // that; a pair of controllers cannot, and the hand feels the same jolt
        // either way. Taking them would add twenty effects that all measure the
        // same, which is the exact failure this project keeps finding.
        const float damage = num(0, 0.0f);
        if (damage > 0.0f && damage < cfg_.minDamage) return;
        const float d = std::clamp(damage / 35.0f, 0.30f, 1.0f);
        const std::string type = arg(2);
        const char* profile = "HL2_DAMAGE";
        if (type == "fire")       profile = "HL2_DAMAGE_FIRE";
        else if (type == "shock") profile = "HL2_DAMAGE_SHOCK";
        else if (type == "toxic") profile = "HL2_DAMAGE_TOXIC";
        std::vector<Voice> v = profiles_.Build(profile);
        for (auto& voice : v) voice.amp *= d;
        Emit(Controller::Both, std::move(v), "HURT");
        if (d > 0.55f) {
            // The trigger follows the type too. A shock is a fast bite, a burn
            // is a long low hold, and a bullet is one hard jolt - using the
            // same overlay for all three would flatten the split the waveforms
            // just made.
            if (type == "shock") {
                triggers_.PushOverlay(Controller::Both, trig::Vibration(3, 6, 90),
                                      70, 5, "hurt");
            } else if (type == "fire") {
                triggers_.PushOverlay(Controller::Both, trig::Feedback(4, 3),
                                      520, 4, "hurt");
            } else if (type == "toxic") {
                triggers_.PushOverlay(Controller::Both, trig::Vibration(5, 4, 14),
                                      560, 4, "hurt");
            } else {
                triggers_.PushOverlay(Controller::Both, trig::Vibration(4, 5, 55),
                                      90, 5, "hurt");
            }
        }
        // Head channel, gated harder than the hands: a graze that earns a
        // flinch in the palms does not earn a jolt to the face.
        if (hmd_ != nullptr && damage >= cfg_.hmdMinDamage) {
            hmd_->Play(hmdfx::Hurt(d), 6);
        }
        return;
    }
    if (event == "HL2_SHOCK") {
        // Your HAND on something live - a panel, a broken cable, a stalker beam.
        //
        // bHaptics carries this as ShockOnHandLeft/Right, and it is one of the
        // few entries in their whole list that is unambiguously a hand
        // sensation rather than a torso one, so it ports directly.
        //
        // Same waveform as shock damage, on ONE hand instead of both. That is
        // the entire difference and it is deliberate: the sensation is
        // identical, what changes is where it is, and localisation is
        // information the waveform should not have to duplicate.
        std::vector<Voice> v = profiles_.Build("HL2_DAMAGE_SHOCK");
        Emit(SideFromParam(arg(0)), std::move(v), "HL2_SHOCK");
        triggers_.PushOverlay(SideFromParam(arg(0)), trig::Vibration(2, 6, 95),
                              80, 5, "shock");
        return;
    }
    if (event == "HL2_EXPLOSION") {
        // intensity 0..1. A blast is one of the very few whole-player events
        // that earns a haptic in both hands: it genuinely arrives through the
        // air and through the floor, not through anything you are holding.
        const float e = std::clamp(num(0, 0.8f), 0.1f, 1.0f);
        std::vector<Voice> v = profiles_.Build("HL2_EXPLOSION");
        for (auto& voice : v) voice.amp *= (0.55f + 0.45f * e);
        Emit(Controller::Both, std::move(v), "HL2_EXPLOSION");
        triggers_.PushOverlay(Controller::Both, trig::Vibration(3, 6, 30), 190, 5,
                              "blast");
        if (hmd_ != nullptr && e > 0.5f) hmd_->Play(hmdfx::Hurt(e), 6);
        return;
    }

    // ---- world ------------------------------------------------------------
    if (event == "HL2_BREAK") {
        // breakflags - only ever sent for something the PLAYER broke, because
        // break_breakable carries a userid and the plugin filters on it. A
        // crate collapsing across the room is not a hand sensation.
        const Material m = MaterialFromBreakFlags(FieldInt(parts, 0, 0));
        if (m == Material::Unknown) return;
        // Breaking is not striking: the resistance GIVES. So it is the material
        // at reduced energy with the transient softened, rather than a hit.
        std::vector<Voice> v = BuildImpact({m, 0.55f, 2.0f, 0.0f, 0.85f});
        for (auto& voice : v) {
            if (voice.bus == Bus::Transient) voice.amp *= 0.55f;
        }
        Emit(primary_, std::move(v), "HL2_BREAK");
        return;
    }
    if (event == "HL2_DOOR") {
        // A door under your hand: not an impact, a sustained load. This is the
        // one place a low continuous texture is justified, because pushing a
        // heavy door genuinely IS continuous - the hand feels the mass and the
        // hinge the whole time it is moving.
        if (!cfg_.doors) return;
        const float speed = std::clamp(num(0, 60.0f), 0.0f, 400.0f);
        const float mass = std::clamp(num(1, 60.0f), 1.0f, 400.0f);
        const float heavy = std::clamp(std::sqrt(mass / 120.0f), 0.15f, 1.0f);
        const float rate = std::clamp(speed / 160.0f, 0.0f, 1.0f);
        std::vector<Voice> v;
        auto grind = Body(58.0f + 30.0f * heavy, 54.0f + 26.0f * heavy,
                          0.06f + 0.16f * heavy * (0.4f + 0.6f * rate), 230, 150);
        grind.amDepth = 0.6f;
        grind.amFreq = 7.0f + 16.0f * rate;
        grind.fmDepth = 6.0f * heavy;
        grind.fmFreq = 23.0f;
        v.push_back(grind);
        Emit(primary_, std::move(v), "DOOR_MOVE");
        if (heavy > 0.45f) {
            const int s = std::clamp(static_cast<int>(std::lround(1 + heavy * 3)), 1, 4);
            triggers_.RefreshOverlay(primary_, trig::Feedback(5, s), 420, 2,
                                     "door-load");
        }
        return;
    }
    if (event == "HL2_BUTTON") {
        // A button under the thumb. Small, crisp, and over: the whole character
        // is that it CLICKS and stops. Deliberately one of the quietest things
        // here - you press a lot of buttons.
        std::vector<Voice> v;
        v.push_back(Transient(460, 0.34f, 8, 5));
        v.push_back(Body(280, 235, 0.38f, 72, 34));
        Emit(SideFromParam(arg(0)), std::move(v), "HL2_BUTTON");
        triggers_.PushOverlay(SideFromParam(arg(0)), trig::Feedback(2, 5), 70, 4,
                              "button");
        return;
    }
    if (event == "HL2_LEVER") {
        // A valve or lever turning under the hand: resistance that MOVES, and
        // the only world control that lasts long enough to have a texture.
        std::vector<Voice> v;
        auto turn = Body(150, 125, 0.44f, 300, 190);
        turn.amDepth = 0.55f;
        turn.amFreq = 12.0f;
        turn.fmDepth = 9.0f;
        turn.fmFreq = 21.0f;
        v.push_back(turn);
        v.push_back(Texture(240, 1.0f, 0.16f, 260, 170));
        Emit(SideFromParam(arg(0)), std::move(v), "HL2_LEVER");
        triggers_.RefreshOverlay(SideFromParam(arg(0)), trig::Feedback(4, 4), 420,
                                 2, "lever-load");
        return;
    }

    // ---- health -----------------------------------------------------------
    if (event == "HL2_HEALTHKIT") {
        std::vector<Voice> v;
        v.push_back(Transient(420, 0.42f, 11, 7));
        auto warmth = Tone(150, 230, 0.26f, 380, 40);
        warmth.amDepth = 0.20f;
        warmth.amFreq = 2.6f;
        v.push_back(warmth);
        Emit(primary_, std::move(v), "HEALTH_PEN");
        return;
    }
    if (event == "HL2_CHARGER") {
        // The wall charger: you hold your hand against it and the machine works
        // on you for as long as you stay. A repeating heartbeat, so it is built
        // as a state that refreshes rather than as an event that fires - which
        // is what stops it stacking into a drone.
        // Shorter and higher than the health kit. Both are healing and the
        // game puts both in the same rooms, so they are the pair here most
        // worth keeping apart - and a charger PULSES where a kit spreads.
        auto pulse = Tone(230, 200, 0.30f, 180, 50);
        pulse.amDepth = 0.50f;
        pulse.amFreq = 16.0f;
        std::vector<Voice> v{pulse};
        Emit(primary_, std::move(v), "HL2_CHARGER");
        triggers_.RefreshOverlay(primary_, trig::Feedback(3, 3), 420, 2,
                                 "charger");
        return;
    }
    if (event == "HL2_ITEM") {
        // Ammunition or a weapon coming into the hand. Small and quick: this
        // fires constantly and inflating it would raise the floor every
        // deliberate effect competes against.
        std::vector<Voice> v;
        v.push_back(Transient(400, 0.26f, 8, 5));
        // Longer and lower than the button it shares a corridor with: picking
        // something up is a hand closing, a button is a thumb clicking, and at
        // 76 ms against 106 ms those two measured as one event.
        v.push_back(Body(200, 150, 0.40f, 190, 110));
        Emit(primary_, std::move(v), "HL2_ITEM");
        return;
    }
}

// ---------------------------------------------------------------------------
// Self test
//
// Every case goes through Handle() with exactly the line the plugin would send,
// so a synthetic run exercises the real path rather than a parallel one that
// happens to look similar. This is what makes the whole integration testable
// with Half-Life 2 VR not installed.
// ---------------------------------------------------------------------------

std::vector<std::string> Hl2vrAdapter::SelfTestNames() const {
    return {
        // The seven firing weapons, plus the two that are not discharges.
        "pistol", "magnum", "smg", "ar2", "shotgun", "shotgun-double",
        "crossbow", "rpg", "grenade", "dry-fire", "pistol-empty",
        // Manual reloading, which is Half-Life 2 VR's default and is a
        // SEQUENCE of physical actions rather than one press. Grouped in the
        // collision report by weapon, because the steps of one weapon's reload
        // are performed seconds apart from each other and days apart from
        // another weapon's.
        "mag-eject", "mag-catch", "mag-retrieve",
        "insert-pistol", "insert-smg", "insert-ar2", "insert-magnum",
        "chamber-pistol", "chamber-smg",
        "shell-insert", "pump-back", "pump-fwd",
        "cylinder-open", "cylinder-close",
        "bolt-nock", "rocket-load",
        // Quick Reload, the game's alternative one-shot mode. Really is a
        // single event, so it keeps the single-event signature.
        "quick-reload",
        // Bracing the weapon with the off hand.
        "brace", "unbrace",
        // Grenades: arm, then throw.
        "grenade-arm",
        // Climbing.
        "ladder",
        // AR2 alternate fire, which is the one two-stage shot in the game.
        "ar2-charge", "ar2-ball",
        // The gravity gun, in the order you actually use it. grab-light against
        // grab-heavy is the pair that proves mass is being represented at all;
        // if those ever measure the same, the signature weapon has regressed to
        // a single canned buzz.
        "grav-grab-light", "grav-grab-heavy", "grav-hold-still", "grav-hold-swing",
        "grav-launch-light", "grav-launch-heavy", "grav-punt", "grav-drop",
        "grav-mega",
        // Carrying by hand, which must stay clearly quieter than the beam.
        "carry-grab", "carry-throw", "carry-drop",
        // Melee. melee-swing MUST be silent - it is the proof that swinging
        // through empty air does not vibrate.
        "melee-swing", "melee-metal", "melee-wood", "melee-flesh", "melee-concrete",
        // Held-object impacts, one per material class.
        "impact-metal", "impact-wood", "impact-glass", "impact-concrete",
        "impact-dirt", "impact-flesh", "impact-plastic", "impact-cardboard",
        "impact-rubber", "impact-light", "impact-heavy",
        // Damage, split by what it feels like rather than by what fired it.
        "damage", "damage-light", "damage-fire", "damage-shock", "damage-toxic",
        "shock-hand", "explosion",
        // The world.
        "break-glass", "break-wood", "door-light", "door-heavy", "button", "lever",
        // Health and pickups.
        "healthkit", "charger", "item",
    };
}

bool Hl2vrAdapter::RunSelfTest(const std::string& name, const std::string& side) {
    Hl2vrAdapter& r = *this;
    const std::string s = side.empty() ? "right" : side;
    r.Handle("PRIMARY_HAND", s);
    // Signatures fire back to back here; the real-time impact limiter must not
    // silently eat the ones after the first.
    r.ResetForTest();
    r.ResetEmitTrace();
    // Start every case from empty hands. Weapon identity is persistent state,
    // so without this it LEAKS between cases and a measurement ends up
    // depending on the order of the suite.
    r.Handle("HL2_WEAPON", "");

    auto fire = [&](const char* cls, const char* token, int left, const char* mode) {
        r.Handle("HL2_WEAPON", cls);
        r.Handle("HL2_FIRE", std::string(token) + "," + std::to_string(left) + "," + mode);
    };
    // impulse,mass,surfaceprop,charcode,spin,confidence
    auto impact = [&](const char* impulseMass, const char* prop, const char* code,
                      const char* spin) {
        r.Handle("HL2_IMPACT", std::string(impulseMass) + "," + prop + "," + code +
                               "," + spin + ",1.00");
    };

    if (name == "pistol")   { fire("weapon_pistol",   hl2::kPistol,   9, "1"); return true; }
    if (name == "magnum")   { fire("weapon_357",      hl2::kMagnum,   4, "1"); return true; }
    if (name == "smg")      { fire("weapon_smg1",     hl2::kSmg,     30, "1"); return true; }
    if (name == "ar2")      { fire("weapon_ar2",      hl2::kAr2,     20, "1"); return true; }
    if (name == "shotgun")  { fire("weapon_shotgun",  hl2::kShotgun,  4, "1"); return true; }
    if (name == "crossbow") { fire("weapon_crossbow", hl2::kCrossbow, -1, "1"); return true; }
    if (name == "rpg")      { fire("weapon_rpg",      hl2::kRpg,      2, "1"); return true; }
    if (name == "grenade")  { fire("weapon_frag",     hl2::kGrenade,  3, "1"); return true; }
    if (name == "shotgun-double") { fire("weapon_shotgun", hl2::kShotgun, 3, "2"); return true; }
    // Running dry: the shot, a beat, then a hollow clack and a slack trigger.
    if (name == "pistol-empty") { fire("weapon_pistol", hl2::kPistol, 0, "1"); return true; }
    if (name == "dry-fire") {
        r.Handle("HL2_WEAPON", "weapon_pistol");
        r.Handle("HL2_DRYFIRE", hl2::kPistol);
        return true;
    }

    // The manual reload sequence, step by step, exactly as the game performs it.
    if (name == "mag-eject")     { r.Handle("HL2_WEAPON", "weapon_pistol"); r.Handle("HL2_MAG_EJECT", ""); return true; }
    if (name == "mag-catch")     { r.Handle("HL2_MAG_CATCH", s); return true; }
    if (name == "mag-retrieve")  { r.Handle("HL2_MAG_RETRIEVE", s); return true; }
    if (name == "insert-pistol") { r.Handle("HL2_WEAPON", "weapon_pistol");   r.Handle("HL2_MAG_INSERT", hl2::kPistol); return true; }
    if (name == "insert-smg")    { r.Handle("HL2_WEAPON", "weapon_smg1");     r.Handle("HL2_MAG_INSERT", hl2::kSmg);    return true; }
    if (name == "insert-ar2")    { r.Handle("HL2_WEAPON", "weapon_ar2");      r.Handle("HL2_MAG_INSERT", hl2::kAr2);    return true; }
    if (name == "insert-magnum") { r.Handle("HL2_WEAPON", "weapon_357");      r.Handle("HL2_MAG_INSERT", hl2::kMagnum); return true; }
    if (name == "chamber-pistol"){ r.Handle("HL2_WEAPON", "weapon_pistol");   r.Handle("HL2_CHAMBER", hl2::kPistol);    return true; }
    if (name == "chamber-smg")   { r.Handle("HL2_WEAPON", "weapon_smg1");     r.Handle("HL2_CHAMBER", hl2::kSmg);       return true; }
    if (name == "shell-insert")  { r.Handle("HL2_WEAPON", "weapon_shotgun");  r.Handle("HL2_SHELL", "3");               return true; }
    if (name == "pump-back")     { r.Handle("HL2_WEAPON", "weapon_shotgun");  r.Handle("HL2_PUMP_BACK", "");            return true; }
    if (name == "pump-fwd")      { r.Handle("HL2_WEAPON", "weapon_shotgun");  r.Handle("HL2_PUMP_FWD", "");             return true; }
    if (name == "cylinder-open") { r.Handle("HL2_WEAPON", "weapon_357");      r.Handle("HL2_CYLINDER", "open");         return true; }
    if (name == "cylinder-close"){ r.Handle("HL2_WEAPON", "weapon_357");      r.Handle("HL2_CYLINDER", "close");        return true; }
    if (name == "bolt-nock")     { r.Handle("HL2_WEAPON", "weapon_crossbow"); r.Handle("HL2_BOLT_NOCK", "");            return true; }
    if (name == "rocket-load")   { r.Handle("HL2_WEAPON", "weapon_rpg");      r.Handle("HL2_ROCKET_LOAD", "");          return true; }
    if (name == "quick-reload")  { r.Handle("HL2_WEAPON", "weapon_pistol");   r.Handle("HL2_RELOAD", hl2::kPistol);     return true; }
    if (name == "brace")         { r.Handle("HL2_WEAPON", "weapon_smg1");     r.Handle("HL2_TWO_HAND", "1");            return true; }
    if (name == "unbrace") {
        r.Handle("HL2_WEAPON", "weapon_smg1");
        // The precondition is set DIRECTLY rather than by sending TWO_HAND:1,
        // because that would emit the brace and this case would then measure
        // it - which is exactly what happened: brace and unbrace both reported
        // 154 ms / 195 Hz, i.e. the same waveform twice. TWO_HAND only fires on
        // a change, so the state has to arrive some other way.
        twoHand_ = true;
        r.Handle("HL2_TWO_HAND", "0");
        return true;
    }
    if (name == "grenade-arm")   { r.Handle("HL2_WEAPON", "weapon_frag");     r.Handle("HL2_GRENADE_ARM", "");          return true; }
    if (name == "ladder")        { r.Handle("HL2_LADDER", s); return true; }

    if (name == "ar2-charge") { r.Handle("HL2_WEAPON", "weapon_ar2"); r.Handle("HL2_CHARGE_START", hl2::kAr2); return true; }
    if (name == "ar2-ball")   { r.Handle("HL2_WEAPON", "weapon_ar2"); r.Handle("HL2_CHARGE_FIRE",  hl2::kAr2); return true; }

    if (name.rfind("grav-", 0) == 0) {
        r.Handle("HL2_WEAPON", "weapon_physcannon");
        // A soda can against a filing cabinet - the two ends of what the beam
        // routinely holds in Half-Life 2.
        if (name == "grav-grab-light")   { r.Handle("HL2_GRAV_GRAB", "0.5,popcan,M");      return true; }
        if (name == "grav-grab-heavy")   { r.Handle("HL2_GRAV_GRAB", "120,metal,M");       return true; }
        if (name == "grav-hold-still")   { r.Handle("HL2_GRAV_HOLD", "120,4,0");           return true; }
        if (name == "grav-hold-swing")   { r.Handle("HL2_GRAV_HOLD", "120,280,140");       return true; }
        // Deliberately NOT primed with a grab. In play a launch always follows
        // one, but the grab's own 500 ms tail then sits inside the
        // measurement and every release measures as the capture before it.
        if (name == "grav-launch-light") { r.Handle("HL2_GRAV_LAUNCH", "0.5,1400");        return true; }
        if (name == "grav-launch-heavy") { r.Handle("HL2_GRAV_LAUNCH", "120,900");         return true; }
        if (name == "grav-punt")         { r.Handle("HL2_GRAV_PUNT", "wood_crate,W");      return true; }
        if (name == "grav-drop")         { r.Handle("HL2_GRAV_DROP", "40");                return true; }
        if (name == "grav-mega")         { r.Handle("HL2_MEGA", "1");                      return true; }
        return false;
    }

    if (name == "carry-grab")  { r.Handle("HL2_CARRY_GRAB", "35,wood_crate,W"); return true; }
    // Same reasoning as the gravity-gun releases: measured on their own so the
    // release signature is what is being reported, not the grab before it.
    if (name == "carry-throw") { r.Handle("HL2_CARRY_THROW", "35,420,wood_crate,W"); return true; }
    if (name == "carry-drop")  { r.Handle("HL2_CARRY_DROP", "35"); return true; }

    if (name == "melee-swing") {
        // Must render SILENT. This is the case that proves swinging through
        // empty air produces no waveform at all.
        r.Handle("HL2_WEAPON", "weapon_crowbar");
        r.Handle("HL2_MELEE_SWING", hl2::kCrowbar);
        return true;
    }
    if (name.rfind("melee-", 0) == 0) {
        r.Handle("HL2_WEAPON", "weapon_crowbar");
        r.Handle("HL2_MELEE_SWING", hl2::kCrowbar);
        if (name == "melee-metal")    { r.Handle("HL2_MELEE_HIT", "CROWBAR,metal,M,0.85");      return true; }
        if (name == "melee-wood")     { r.Handle("HL2_MELEE_HIT", "CROWBAR,wood_crate,W,0.75"); return true; }
        if (name == "melee-flesh")    { r.Handle("HL2_MELEE_HIT", "CROWBAR,flesh,F,0.90");      return true; }
        if (name == "melee-concrete") { r.Handle("HL2_MELEE_HIT", "CROWBAR,concrete,C,0.70");   return true; }
        return false;
    }

    if (name == "impact-metal")     { impact("2600,40",  "metal",      "M", "700"); return true; }
    if (name == "impact-wood")      { impact("2100,25",  "wood_crate", "W", "200"); return true; }
    if (name == "impact-glass")     { impact("1800,3",   "glassbottle","Y", "80");  return true; }
    if (name == "impact-concrete")  { impact("3000,80",  "concrete",   "C", "90");  return true; }
    if (name == "impact-dirt")      { impact("2000,30",  "dirt",       "D", "120"); return true; }
    if (name == "impact-flesh")     { impact("2400,60",  "flesh",      "F", "150"); return true; }
    if (name == "impact-plastic")   { impact("1500,8",   "plastic",    "L", "120"); return true; }
    if (name == "impact-cardboard") { impact("1100,2",   "cardboard",  "O", "60");  return true; }
    if (name == "impact-rubber")    { impact("1900,20",  "rubbertire", "L", "500"); return true; }
    // Energy probes: the SAME material at two energies, to show what the
    // scaling does. They are meant to match on material and must not be
    // reported as a collision.
    if (name == "impact-light")     { impact("700,3",    "wood",       "W", "40");  return true; }
    if (name == "impact-heavy")     { impact("6000,150", "wood",       "W", "120"); return true; }

    if (name == "damage")       { r.Handle("HL2_DAMAGE", "35,0");        return true; }
    if (name == "damage-light") { r.Handle("HL2_DAMAGE", "8,0");         return true; }
    if (name == "damage-fire")  { r.Handle("HL2_DAMAGE", "30,0,fire");   return true; }
    if (name == "damage-shock") { r.Handle("HL2_DAMAGE", "30,0,shock");  return true; }
    if (name == "damage-toxic") { r.Handle("HL2_DAMAGE", "30,0,toxic");  return true; }
    if (name == "shock-hand")   { r.Handle("HL2_SHOCK", s);              return true; }
    if (name == "explosion")    { r.Handle("HL2_EXPLOSION", "1.0");      return true; }

    if (name == "break-glass") { r.Handle("HL2_BREAK", "1"); return true; }
    if (name == "break-wood")  { r.Handle("HL2_BREAK", "8"); return true; }
    if (name == "door-light")  { r.Handle("HL2_DOOR", "70,25");   return true; }
    if (name == "door-heavy")  { r.Handle("HL2_DOOR", "120,300"); return true; }
    if (name == "button")      { r.Handle("HL2_BUTTON", s); return true; }
    if (name == "lever")       { r.Handle("HL2_LEVER", s);  return true; }

    if (name == "healthkit") { r.Handle("HL2_HEALTHKIT", ""); return true; }
    if (name == "charger")   { r.Handle("HL2_CHARGER", "1");  return true; }
    if (name == "item")      { r.Handle("HL2_ITEM", "item_ammo_pistol"); return true; }
    return false;
}

const char* Hl2vrAdapter::AnalyzeFamily(const std::string& n) const {
    // Grouped by co-occurrence, as in the Alyx adapter. Half-Life 2 hands the
    // player every weapon at once and lets them switch freely, so the weapon
    // family is the largest in either game and the hardest to keep separated -
    // seven discharges where Alyx had three.
    if (n == "pistol" || n == "magnum" || n == "smg" || n == "ar2" ||
        n == "shotgun" || n == "crossbow" || n == "rpg") {
        return "weapons";
    }
    // The grenade throw belongs with the arming that precedes it, not with the
    // discharges - it is not one.
    if (n == "grenade") return "grenade";
    // shotgun-double and pistol-empty are VARIANTS of entries already in the
    // weapons family. They are meant to resemble the shot they modify, so
    // measuring them against it would flag the intended result as a failure.
    if (n.rfind("ar2-", 0) == 0) return "ar2 alt-fire";
    // Arming and throwing are one gesture in two halves, seconds apart.
    if (n == "grenade-arm") return "grenade";
    // Reload steps are grouped BY WEAPON.
    //
    // The grouping rule is co-occurrence, and manual reloading changed what
    // co-occurs. You eject a pistol magazine, insert a fresh one and rack the
    // slide within a few seconds of each other, so those three must not feel
    // alike. A pistol slide and a shotgun forestock are never part of the same
    // gesture, and forcing them apart would spend real design room solving a
    // problem the hand does not have.
    //
    // An earlier revision lumped every reload into one family, which was
    // correct while each weapon had a single atomic reload and became wrong the
    // moment the sequence was modelled properly.
    if (n == "mag-eject" || n == "mag-catch" || n == "mag-retrieve") {
        // Weapon-independent: these three happen with every reload, so they are
        // the steps you meet most often and most need to tell apart.
        return "magazine handling";
    }
    if (n == "insert-pistol" || n == "chamber-pistol") return "pistol reload";
    if (n == "insert-smg" || n == "chamber-smg") return "smg reload";
    if (n == "shell-insert" || n == "pump-back" || n == "pump-fwd") return "shotgun reload";
    if (n == "cylinder-open" || n == "cylinder-close" || n == "insert-magnum") {
        return "revolver reload";
    }
    // One step each, and never performed alongside one another - but grouped
    // anyway, because a family of one measures nothing and these are exactly
    // the kind of lone effect that drifts.
    if (n == "insert-ar2" || n == "bolt-nock" || n == "rocket-load") {
        return "special reloads";
    }
    // Bringing the support hand on and taking it off again. You do both
    // constantly, so this is the pair most worth separating.
    if (n == "brace" || n == "unbrace") return "bracing";
    if (n.rfind("grav-", 0) == 0) return "gravity gun";
    if (n.rfind("carry-", 0) == 0) return "carrying";
    // melee-swing is excluded on purpose: it is asserted to be SILENT, so it
    // has no duration or pitch to compare and belongs in no family.
    if (n.rfind("melee-", 0) == 0 && n != "melee-swing") return "melee";
    // impact-light and impact-heavy are energy probes on a shared material.
    if (n == "impact-light" || n == "impact-heavy") return nullptr;
    if (n.rfind("impact-", 0) == 0) return "materials";
    if (n.rfind("break-", 0) == 0) return "breaking";
    // door-light and door-heavy are a WEIGHT probe on one shape, the same kind
    // of pair as impact-light and impact-heavy: they are meant to differ by
    // level and by roughness rate, both of which this measure is blind to.
    // Reporting them would be a permanent false alarm.
    if (n.rfind("door-", 0) == 0) return nullptr;
    // The three small world controls, all met in the same corridors.
    if (n == "button" || n == "lever" || n == "item") return "world controls";
    if (n == "healthkit" || n == "charger") return "healing";
    // Every way the game hurts you, in one family: you meet all of them in the
    // same firefight and they must not collapse into one sensation.
    //
    // damage-light is excluded - it is an INTENSITY probe on the same shape as
    // damage, so matching it is the intended result, exactly like
    // impact-light/impact-heavy. shock-hand is excluded too: it is deliberately
    // the same waveform as damage-shock on one hand instead of two, and
    // localisation is not something this measure can see.
    if (n == "damage" || n == "explosion" || n == "damage-fire" ||
        n == "damage-shock" || n == "damage-toxic") {
        return "taking damage";
    }
    return nullptr;
}

} // namespace psvr2
