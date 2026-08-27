#include "games/hl2vr/hl2vr_bhaptics.h"

#include <algorithm>
#include <cctype>

namespace psvr2 {
namespace {

std::string Lower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool Has(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

} // namespace

const std::vector<BhapticsRule>& BhapticsRules() {
    // ORDER IS THE WHOLE DESIGN HERE, and getting it wrong is not a cosmetic
    // problem. First match wins, and the substrings overlap badly:
    //
    //   "PistolFire" and "DamageFire" both contain "fire"
    //   "CrowbarHit" contains "hit"
    //   "DamageExplosion" contains "damage"
    //   "ShotgunReload" contains "shotgun"
    //
    // A naive alphabetical or grouped-by-theme ordering turns fire DAMAGE into
    // a GUNSHOT, which is the precise failure this file exists to prevent: the
    // wrong sensation at the right moment is far harder to notice than silence.
    //
    // So the list runs most-specific to least, and the comments say what each
    // block is protecting against rather than merely what it matches.
    static const std::vector<BhapticsRule> kRules = {
        // --- blasts, before anything containing "damage" -------------------
        // "DamageExplosion" is bHaptics' usual spelling, and the blast is the
        // more specific reading of it.
        {"explosion",   "HL2_EXPLOSION", "a blast"},
        {"explode",     "HL2_EXPLOSION", "a blast, verb form"},

        // --- electricity on the hand, before generic shock -----------------
        {"shockonhand", "HL2_SHOCK",     "electricity on the hand specifically"},

        // --- damage BY TYPE, before the weapon rules that share their words -
        // This block is why the ordering matters: "damagefire" has to be seen
        // before "fire", or burning becomes a gunshot.
        {"damagefire",  "HL2_DAMAGE",    "burning"},
        {"damagespark", "HL2_DAMAGE",    "electrical damage"},
        {"damagelaser", "HL2_DAMAGE",    "energy damage"},
        {"damageshock", "HL2_DAMAGE",    "electrical damage"},
        {"damagepoison","HL2_DAMAGE",    "toxic damage"},

        // --- the gravity gun, before "fire"/"shoot"/"pickup" ----------------
        {"physcannon",  "HL2_GRAV",      "the gravity gun, by its classname"},
        {"gravgun",     "HL2_GRAV",      "the gravity gun, by its common name"},
        {"gravitygun",  "HL2_GRAV",      "the gravity gun, spelled out"},
        {"gravityglove","HL2_GRAV",      "the Alyx spelling, in case it survived"},

        // --- melee, before "hit" --------------------------------------------
        {"crowbar",     "HL2_MELEE_HIT", "melee - the crowbar names itself"},
        {"stunstick",   "HL2_MELEE_HIT", "melee - the stunstick"},

        // --- reload mechanisms, before the weapon names they contain -------
        {"reload",      "HL2_RELOAD",    "any reload"},
        {"clipin",      "HL2_RELOAD",    "a magazine seating"},
        {"clipout",     "HL2_RELOAD",    "a magazine dropping"},
        {"clipinserted","HL2_RELOAD",    "a magazine seating, bHaptics spelling"},
        {"magazine",    "HL2_RELOAD",    "a magazine"},
        {"chamber",     "HL2_RELOAD",    "a round chambering"},
        {"shell",       "HL2_SHELL",     "a shotgun shell"},
        {"pump",        "HL2_PUMP",      "a pump action"},

        // --- firing ---------------------------------------------------------
        {"shotgun",     "HL2_FIRE",      "the shotgun firing"},
        {"smg",         "HL2_FIRE",      "the SMG firing"},
        {"ar2",         "HL2_FIRE",      "the pulse rifle firing"},
        {"pulserifle",  "HL2_FIRE",      "the pulse rifle, spelled out"},
        {"357",         "HL2_FIRE",      "the magnum firing"},
        {"magnum",      "HL2_FIRE",      "the magnum, by name"},
        {"revolver",    "HL2_FIRE",      "the magnum, by type"},
        {"crossbow",    "HL2_FIRE",      "the crossbow firing"},
        {"rpg",         "HL2_FIRE",      "the rocket launcher firing"},
        {"rocket",      "HL2_FIRE",      "the rocket launcher, by projectile"},
        {"pistol",      "HL2_FIRE",      "the pistol firing"},
        {"grenade",     "HL2_FIRE",      "a grenade thrown"},
        {"kickback",    "HL2_FIRE",      "bHaptics splits recoil out; we do too"},
        {"playershoot", "HL2_FIRE",      "bHaptics' generic shot"},
        {"shoot",       "HL2_FIRE",      "a generic shoot event"},

        // --- damage, generically. AFTER the weapons, so "PistolFire" has
        // already been claimed and only real damage reaches here.
        {"damage",      "HL2_DAMAGE",    "taking damage"},
        {"hurt",        "HL2_DAMAGE",    "taking damage, alternate wording"},
        {"bullet",      "HL2_DAMAGE",    "being shot"},
        {"hit",         "HL2_DAMAGE",    "being hit"},

        // --- damage types named on their own, e.g. EnvironmentFire ----------
        {"burn",        "HL2_DAMAGE",    "burning"},
        {"fire",        "HL2_DAMAGE",    "burning - every weapon spelling is gone by now"},
        {"electric",    "HL2_DAMAGE",    "electrical damage"},
        {"shock",       "HL2_DAMAGE",    "electrical damage"},
        {"spark",       "HL2_DAMAGE",    "electrical damage"},
        {"laser",       "HL2_DAMAGE",    "energy damage"},
        {"poison",      "HL2_DAMAGE",    "toxic damage"},
        {"toxic",       "HL2_DAMAGE",    "toxic damage"},
        {"acid",        "HL2_DAMAGE",    "toxic damage"},

        // --- health and pickups ---------------------------------------------
        {"charger",     "HL2_CHARGER",   "a wall charger"},
        {"heal",        "HL2_HEALTHKIT", "healing"},
        {"health",      "HL2_HEALTHKIT", "a health item"},
        {"medkit",      "HL2_HEALTHKIT", "a health item"},
        {"ammo",        "HL2_ITEM",      "picking up ammunition"},
        {"pickup",      "HL2_ITEM",      "picking something up"},
        {"item",        "HL2_ITEM",      "an item"},
        {"weapon",      "HL2_FIRE",      "a generic weapon event, last resort"},
    };
    return kRules;
}

BhapticsMapping MapBhapticsKey(const std::string& key) {
    const std::string k = Lower(key);
    BhapticsMapping out;

    // Effects that belong to the torso, the head or the whole body are
    // deliberately dropped rather than mapped to something hand-shaped.
    //
    // This is the project's governing rule applied to somebody else's event
    // list: a vest can represent a heartbeat on the chest, and a controller
    // cannot represent it at all. bHaptics integrations are full of these
    // because they are designing for a vest, and taking them all would be the
    // fastest possible route to the ambient-buzzing failure this whole project
    // exists to avoid.
    // NOTE: "environment" is deliberately NOT in this list. bHaptics names
    // several real damage effects EnvironmentFire, EnvironmentExplosion and so
    // on, and those are felt in the arms exactly like their Damage* twins.
    // Only the genuinely ambient ones are dropped, and each is named.
    static const char* kNotAHandSensation[] = {
        "heartbeat", "breath", "cough", "footstep", "step", "walk", "run",
        "ambient", "idle", "rain", "wind", "heat", "cold",
        "radiation", "drown", "suffocat", "fall", "land", "jump", "swim",
        "vehicle", "car", "buggy", "airboat", "elevator", "train",
    };
    for (const char* skip : kNotAHandSensation) {
        if (Has(k, skip)) {
            out.rule = "not a hand sensation - deliberately dropped";
            return out;
        }
    }

    for (const auto& rule : BhapticsRules()) {
        if (!Has(k, rule.match)) continue;
        out.rule = rule.why;

        // The gravity gun's key set has to be split further, because the whole
        // family shares one substring and they are completely different
        // sensations - a capture is not a launch.
        if (std::string(rule.event) == "HL2_GRAV") {
            if (Has(k, "catch") || Has(k, "grab") || Has(k, "pickup") ||
                Has(k, "hold") || Has(k, "pull")) {
                out.event = "HL2_GRAV_GRAB";
                // Mass and material are unknown on this transport - a bHaptics
                // submit carries vest motor data, not physics. -1 means the
                // adapter falls back to a neutral heft rather than asserting a
                // weight it was not told.
                out.params = "-1,,";
            } else if (Has(k, "launch") || Has(k, "shoot") || Has(k, "fire") ||
                       Has(k, "throw") || Has(k, "punt")) {
                out.event = "HL2_GRAV_LAUNCH";
                out.params = "-1,-1";
            } else if (Has(k, "drop") || Has(k, "release")) {
                out.event = "HL2_GRAV_DROP";
                out.params = "-1";
            } else {
                // A gravity gun key we cannot place. Reported, not guessed.
                out.event.clear();
                out.rule = "gravity gun, but which action is unclear";
            }
            return out;
        }

        if (std::string(rule.event) == "HL2_MELEE_HIT") {
            // No surface information on this transport, and no way to tell a
            // hit from a swing by name alone. A bHaptics pattern only fires
            // when something HAPPENED, though, so a key existing at all is
            // evidence of contact rather than of a swing through air.
            out.event = "HL2_MELEE_HIT";
            out.params = "CROWBAR,,,0.75";
            return out;
        }

        if (std::string(rule.event) == "HL2_FIRE") {
            // The weapon token, where the key names one. Left empty otherwise,
            // which makes the adapter fire with whatever weapon it last saw -
            // the honest behaviour when the key does not say.
            const char* token = "";
            if (Has(k, "shotgun")) token = "SHOTGUN";
            else if (Has(k, "smg")) token = "SMG";
            else if (Has(k, "ar2") || Has(k, "pulserifle")) token = "AR2";
            else if (Has(k, "357") || Has(k, "magnum") || Has(k, "revolver")) token = "MAGNUM";
            else if (Has(k, "crossbow")) token = "CROSSBOW";
            else if (Has(k, "rpg") || Has(k, "rocket")) token = "RPG";
            else if (Has(k, "grenade")) token = "GRENADE";
            else if (Has(k, "pistol")) token = "PISTOL";
            out.event = "HL2_FIRE";
            // roundsLeft is -1: unknown. That matters - 0 would trigger the
            // running-dry cue on every single shot.
            out.params = std::string(token) + ",-1,1";
            return out;
        }

        if (std::string(rule.event) == "HL2_DAMAGE") {
            // Amount unknown. The adapter's own floor is 0 and its ceiling
            // scaling caps at 35, so a mid value is the least wrong choice and
            // is flagged as a guess in the scan output.
            //
            // The TYPE, though, is real information: this transport carries the
            // game's own name for the effect, and a key called DamageFire is
            // the game telling us it was fire. It is the one place where the
            // bHaptics route knows something the server plugin cannot - HL2's
            // player_hurt event carries no damage-type bits at all.
            const char* type = "";
            if (Has(k, "fire") || Has(k, "burn")) type = "fire";
            else if (Has(k, "shock") || Has(k, "spark") || Has(k, "electric") ||
                     Has(k, "laser")) type = "shock";
            else if (Has(k, "poison") || Has(k, "toxic") || Has(k, "acid")) type = "toxic";
            out.event = "HL2_DAMAGE";
            out.params = std::string("20,0,") + type;
            return out;
        }

        if (std::string(rule.event) == "HL2_SHOCK") {
            // Hand unknown unless the key names one, which bHaptics keys often
            // do - their convention is a Left/Right suffix.
            out.event = "HL2_SHOCK";
            out.params = Has(k, "left") ? "left" : (Has(k, "right") ? "right" : "");
            return out;
        }

        if (std::string(rule.event) == "HL2_EXPLOSION") {
            out.event = "HL2_EXPLOSION";
            out.params = "0.85";
            return out;
        }

        if (std::string(rule.event) == "HL2_RELOAD") {
            out.event = "HL2_RELOAD";
            out.params = "";
            return out;
        }

        out.event = rule.event;
        return out;
    }

    out.rule = "no rule matched";
    return out;
}

} // namespace psvr2
