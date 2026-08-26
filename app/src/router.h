// Semantic event -> tactile signature.
//
// This is the Half-Life: Alyx specific layer: everything below it (capi,
// haptics, triggers) is game-agnostic, and everything above it (the log tailer)
// only produces event names.

#pragma once

#include "config.h"
#include "haptics.h"
#include "profiles.h"
#include "triggers.h"

#include <string>
#include <vector>

namespace psvr2 {

// Coarse tactile material classes inferred game-side from model/classname.
enum class Material {
    Unknown, Glass, Metal, Wood, Stone, Rubber, Organic, Cardboard, Plastic
};
Material ParseMaterial(const std::string& s);
const char* MaterialName(Material m);

class Router {
public:
    Router(Mixer& mixer, TriggerManager& triggers, const Config& cfg,
           const Profiles& profiles)
        : mixer_(mixer), triggers_(triggers), cfg_(cfg), profiles_(profiles) {
        primary_ = cfg.handedness == "left" ? Controller::Left : Controller::Right;
    }

    void Handle(const std::string& event, const std::string& param);

    // Re-installs the persistent trigger state for the current weapon. Used at
    // startup and after the game reloads its script.
    void RefreshWeaponState();

    // Which hands actually received a waveform since the last reset.
    //
    // The self-test used to announce the hand it was ASKED to play on, which
    // is not the same thing: bilateral effects like damage ignore the request
    // and go to both. Announcing "[right]" and then buzzing both hands makes
    // correct behaviour look like a bug. This reports what was really emitted.
    void ResetEmitTrace() { emitTrace_ = 0; }
    uint8_t emitTrace() const { return emitTrace_; } // bit0 = left, bit1 = right

    const std::string& weapon() const { return weapon_; }
    Controller primary() const { return primary_; }

    // Clears the per-hand impact rate limit.
    //
    // The limiter is a real-time guard against a dragged object reporting an
    // impact every tick. The self-test fires signatures back to back as fast as
    // it can render them, so without this the second and third impact cases
    // would be silently swallowed and --analyze would report zeros for effects
    // that are actually fine. A test harness that can be defeated by its own
    // speed is worse than no test harness.
    void ResetRateLimits();

private:
    Controller Other() const {
        return primary_ == Controller::Left ? Controller::Right : Controller::Left;
    }
    Controller SideFromParam(const std::string& s) const;
    void SetWeapon(const std::string& weapon);
    void Fire(const std::string& weapon, bool twoHand, int roundsLeft);
    // `confidence` is the game side's own estimate of whether this impact
    // really happened (held-object impacts are inferred, not reported by a
    // collision callback). A doubtful reading is played softer rather than
    // being either suppressed or asserted at full strength.
    void Impact(Material m, float energy, Controller side, float mass, float spin,
                float confidence);

    void Emit(Controller c, std::vector<Voice> voices, const std::string& event);

    // Per-instance variation.
    //
    // bHaptics authors up to five hand-made variants of each event and picks
    // one at random, because replaying a byte-identical waveform makes an
    // effect read as canned. Synthesising means we can do it continuously
    // instead: every instance is jittered slightly in pitch, length and level.
    //
    // The amounts are deliberately far below the ~1.5x ratio needed for two
    // pitches to read as different, so a hit still lands unmistakably as glass
    // or as stone - it just stops sounding like the same recording each time.
    float Jitter(float amount);
    void Vary(std::vector<Voice>& voices);

    uint32_t rng_ = 0x2545F491u;
    uint8_t emitTrace_ = 0;

    Mixer& mixer_;
    TriggerManager& triggers_;
    const Config& cfg_;
    const Profiles& profiles_;
    Controller primary_ = Controller::Right;
    std::string weapon_ = "HANDS";
    bool twoHand_ = false;
    bool menuOpen_ = false;

    // Per-hand impact rate limit. A held object dragged along a wall satisfies
    // the game-side impact test on tick after tick; without this it would read
    // as a continuous buzz instead of as a strike. Indexed by Controller.
    Clock::time_point lastImpact_[2]{};

    // Shotgun shots fired in quick succession ease the resting resistance by a
    // notch. Tracked here so the base is only rewritten when the state
    // actually flips - rewriting a persistent trigger profile on every shot is
    // what made every weapon feel alike in an earlier revision.
    Clock::time_point lastShotgunFire_{};
    bool shotgunRapid_ = false;
};

// The adaptive trigger's recoil ladder, exposed for measurement.
//
// The waveform layer has had a perceptual collision check since it was found
// that seven effects were sharing one perceptual cell. The trigger layer never
// did - and drifted into exactly the same state, with the pistol and SMG
// within 1.5x of each other on drive, rate AND duration simultaneously.
// Reporting it alongside the waveforms is what stops that recurring.
struct RecoilSpec {
    std::string weapon;
    int drive;     // 0..8, capped below the motor's stall point
    int rateHz;    // only ~12-28 Hz reads as kickback
    int revealMs;  // how long the kick outlives the break: the whole sensation
    int restPeak;  // strongest resistance in the RESTING profile, 0..8
};
std::vector<RecoilSpec> RecoilLadder();

// Named self-test cases for --test. Keeping them here means the test exercises
// exactly the same code path a real game event does.
std::vector<std::string> SelfTestNames();
bool RunSelfTest(Router& router, const std::string& name, const std::string& side);

} // namespace psvr2
