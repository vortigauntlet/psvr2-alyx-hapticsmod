// Half-Life 2 VR integration.
//
// ---------------------------------------------------------------------------
// WHERE THE EVENTS COME FROM, AND WHY THIS IS NOT THE ALYX DESIGN
// ---------------------------------------------------------------------------
//
// Half-Life: Alyx ships a scripting layer, so its game side is a VScript addon
// that classifies gameplay and prints tagged console lines. Half-Life 2 has no
// equivalent: Source SDK 2013 carries VScript headers but no usable VM, and
// HL2VR is not a Mapbase-derived mod, so there is nothing to write a script in.
//
// The HL2VR source is also NOT public. The Source VR Mod Team's FAQ says it
// "can, in principle, be open sourced" but has not been, and the only public
// HL2VR repository (vittorioromeo/HL2VRU) ships binaries and a README with no
// code. Direct source modification is therefore not available, and neither is
// reading HL2VR's own VR classes.
//
// What IS available is the Source engine's documented third-party extension
// point: a SERVER PLUGIN implementing ISERVERPLUGINCALLBACKS003. That is an
// engine feature, not a game feature, so it does not need HL2VR's source; it is
// loaded by a console command or an addons/*.vdf; and it gets the game event
// manager plus the public vphysics and engine-trace interfaces. No injection,
// no pattern scanning, no hardcoded offsets - which is the standing rule on
// this project and also simply the better engineering.
//
// So:
//     HL2VR gameplay
//       -> psvr2_haptics server plugin  (src/games/hl2vr/plugin)
//       -> loopback UDP, one datagram per event
//       -> this adapter
//       -> the same core the Alyx adapter uses
//
// ---------------------------------------------------------------------------
// WHAT THE PLUGIN CAN AND CANNOT SEE
// ---------------------------------------------------------------------------
//
// This matters more here than it did for Alyx, because a great deal of what the
// plugin reports is INFERRED rather than announced. Half-Life 2 fires almost no
// game events: there is no weapon_fire, no melee event, no explosion event.
// Verified against Source SDK 2013, the events a singleplayer HL2 actually
// fires that are of any use here are player_hurt, entity_killed, break_prop,
// break_breakable (which carries a material), physgun_pickup, weapon_equipped,
// ammo_pickup, take_health and take_armor.
//
// Everything else the plugin derives from polled entity and physics state - a
// magazine count falling by one is a shot, an object's velocity differential is
// an impact. That is the same technique the Alyx addon already uses for held
// impacts, and it is why nearly every event below carries a confidence.
//
// The honest boundary is drawn in games/hl2vr/plugin/README.md and in
// docs/VERIFIED.md. Nothing in this file assumes a fact the plugin cannot
// actually establish.

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

// Weapon tokens. The plugin sends a classname; MapWeaponToken turns it into one
// of these, and everything after that is written against the token so a renamed
// or added HL2VR weapon only has to be classified once.
namespace hl2 {
constexpr const char* kHands    = "HANDS";
constexpr const char* kCrowbar  = "CROWBAR";
constexpr const char* kPistol   = "PISTOL";
constexpr const char* kMagnum   = "MAGNUM";
constexpr const char* kSmg      = "SMG";
constexpr const char* kAr2      = "AR2";
constexpr const char* kShotgun  = "SHOTGUN";
constexpr const char* kCrossbow = "CROSSBOW";
constexpr const char* kGrenade  = "GRENADE";
constexpr const char* kRpg      = "RPG";
constexpr const char* kGravGun  = "GRAVGUN";
constexpr const char* kBugbait  = "BUGBAIT";
} // namespace hl2

// Maps a Source weapon classname to a token. Returns "HANDS" for an empty or
// unrecognised classname; the adapter reports unrecognised ones under debug so
// a modded weapon can be classified against real data rather than guessed at.
std::string MapWeaponToken(const std::string& classname);

class Hl2vrAdapter final : public IGameAdapter {
public:
    Hl2vrAdapter(Mixer& mixer, TriggerManager& triggers, const Config& cfg,
                 const Profiles& profiles, HmdChannel* hmd = nullptr)
        : mixer_(mixer), triggers_(triggers), cfg_(cfg), profiles_(profiles),
          hmd_(hmd) {
        primary_ = cfg.handedness == "left" ? Controller::Left : Controller::Right;
    }

    const char* id() const override { return "hl2vr"; }
    const char* gameName() const override { return "Half-Life 2 VR"; }

    void Handle(const std::string& event, const std::string& param) override;
    void RefreshWeaponState() override;

    void ResetEmitTrace() override { emitTrace_ = 0; }
    uint8_t emitTrace() const override { return emitTrace_; }
    void ResetForTest() override;

    std::vector<std::string> SelfTestNames() const override;
    bool RunSelfTest(const std::string& name, const std::string& side) override;
    const char* AnalyzeFamily(const std::string& testName) const override;
    std::vector<RecoilSpec> RecoilLadder() const override;
    std::vector<std::string> ConnectionHelp() const override;

    // bHaptics keys the game announced that no rule could classify.
    //
    // Kept rather than merely logged, because the whole discovery story rests
    // on handing back a list a human can act on. See games/hl2vr/hl2vr_bhaptics.h.
    const std::vector<std::string>& unmappedBhapticsKeys() const {
        return unmappedBhaptics_;
    }

    const std::string& weapon() const { return weapon_; }

private:
    Controller Other() const {
        return primary_ == Controller::Left ? Controller::Right : Controller::Left;
    }
    Controller SideFromParam(const std::string& s) const;
    void SetWeapon(const std::string& token);
    void Fire(const std::string& token, int roundsLeft, bool secondary);
    void Impact(Material m, float energy, Controller side, float mass, float spin,
                float confidence);
    void Emit(Controller c, std::vector<Voice> voices, const std::string& event);

    // Resolves the material for an event that carries both a Source surface
    // property name and a CHAR_TEX_* character code.
    Material MaterialFrom(const std::string& prop, const std::string& code) const;

    Variation vary_;
    uint8_t emitTrace_ = 0;
    std::vector<std::string> unmappedBhaptics_;

    Mixer& mixer_;
    TriggerManager& triggers_;
    const Config& cfg_;
    const Profiles& profiles_;
    HmdChannel* hmd_ = nullptr;
    Controller primary_ = Controller::Right;
    std::string weapon_ = hl2::kHands;
    bool menuOpen_ = false;

    // The supercharged gravity gun from Nova Prospekt onward. A real state
    // change in the game, read by the plugin from the physcannon_mega_enabled
    // convar, and worth representing because the weapon genuinely becomes a
    // different object in the hand.
    bool megaCannon_ = false;

    // What the gravity gun currently has hold of, so a launch can be sized by
    // what is being launched rather than by a fixed impulse.
    float heldMass_ = -1.0f;
    Material heldMaterial_ = Material::Unknown;

    // Per-hand impact rate limit, as in the Alyx adapter: a dragged object
    // satisfies the plugin's impact test tick after tick, and a strike is an
    // event rather than a texture.
    Clock::time_point lastImpact_[2]{};

    // A melee swing arms the impact; it never plays anything by itself.
    // See the MELEE_SWING handler for why that is the single most important
    // rule in this file.
    Clock::time_point swingArmedUntil_{};
    std::string swingWeapon_;
};

} // namespace psvr2
