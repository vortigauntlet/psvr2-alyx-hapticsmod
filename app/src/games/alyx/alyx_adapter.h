// Half-Life: Alyx integration.
//
// Semantic event -> tactile signature. This is the Alyx-specific layer:
// everything it calls (capi, haptics, triggers, materials, impact) is
// game-agnostic, and everything above it (the transport) only produces event
// names.
//
// The game side is a VScript addon that emits one tagged console line per
// semantic event; see games/alyx/alyx_install.h for why that install needs no
// injection and touches no Valve file.

#pragma once

#include "core/adapter.h"
#include "core/config.h"
#include "core/haptics.h"
#include "core/impact.h"
#include "core/materials.h"
#include "core/profiles.h"
#include "core/triggers.h"
#include "core/variation.h"

#include <string>
#include <vector>

namespace psvr2 {

class HmdChannel;

class AlyxAdapter final : public IGameAdapter {
public:
    // `hmd` is optional and may stay null: the headset channel is off by
    // default, and every use of it is guarded. A null here must behave
    // identically to the build that had no headset support at all.
    AlyxAdapter(Mixer& mixer, TriggerManager& triggers, const Config& cfg,
                const Profiles& profiles, HmdChannel* hmd = nullptr)
        : mixer_(mixer), triggers_(triggers), cfg_(cfg), profiles_(profiles),
          hmd_(hmd) {
        primary_ = cfg.handedness == "left" ? Controller::Left : Controller::Right;
    }

    const char* id() const override { return "alyx"; }
    const char* gameName() const override { return "Half-Life: Alyx"; }

    void Handle(const std::string& event, const std::string& param) override;

    // Re-installs the persistent trigger state for the current weapon. Used at
    // startup and after the game reloads its script.
    void RefreshWeaponState() override;

    void ResetEmitTrace() override { emitTrace_ = 0; }
    uint8_t emitTrace() const override { return emitTrace_; }

    // Clears the per-hand impact rate limit and re-seeds variation.
    //
    // The limiter is a real-time guard against a dragged object reporting an
    // impact every tick. The self-test fires signatures back to back as fast as
    // it can render them, so without this the second and third impact cases
    // would be silently swallowed and --analyze would report zeros for effects
    // that are actually fine. A test harness that can be defeated by its own
    // speed is worse than no test harness.
    void ResetForTest() override;

    std::vector<std::string> SelfTestNames() const override;
    bool RunSelfTest(const std::string& name, const std::string& side) override;
    const char* AnalyzeFamily(const std::string& testName) const override;
    std::vector<RecoilSpec> RecoilLadder() const override;
    std::vector<std::string> ConnectionHelp() const override;

    const std::string& weapon() const { return weapon_; }
    Controller primary() const { return primary_; }

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

    Variation vary_;
    uint8_t emitTrace_ = 0;

    Mixer& mixer_;
    TriggerManager& triggers_;
    const Config& cfg_;
    HmdChannel* hmd_ = nullptr;
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
    // notch. Tracked here so the base is only rewritten when the state actually
    // flips - rewriting a persistent trigger profile on every shot is what made
    // every weapon feel alike in an earlier revision.
    Clock::time_point lastShotgunFire_{};
    bool shotgunRapid_ = false;
};

} // namespace psvr2
