#include "events.h"

#include <cstdio>
#include <sstream>

namespace psvr2 {
namespace {

// One row per event, so the name table cannot drift out of step with the enum.
// The static_assert below is what enforces that.
struct Row { Ev e; const char* name; };
const Row kRows[] = {
    {Ev::Unknown,           "UNKNOWN"},
    {Ev::Menu,              "MENU"},
    {Ev::PrimaryHand,       "PRIMARY_HAND"},
    {Ev::WeaponChange,      "WEAPON"},
    {Ev::TwoHandStart,      "TWO_HAND_START"},
    {Ev::TwoHandEnd,        "TWO_HAND_END"},
    {Ev::WeaponFire,        "FIRE"},
    {Ev::WeaponDryFire,     "DRY_FIRE"},
    {Ev::WeaponChargeStart, "CHARGE_START"},
    {Ev::WeaponChargeFire,  "CHARGE_FIRE"},
    {Ev::ReloadStart,       "RELOAD_START"},
    {Ev::MagazineInsert,    "MAG_INSERT"},
    {Ev::MagazineEject,     "MAG_EJECT"},
    {Ev::Slide,             "SLIDE"},
    {Ev::Bolt,              "BOLT"},
    {Ev::Pump,              "PUMP"},
    {Ev::ShellInsert,       "SHELL_INSERT"},
    {Ev::Draw,              "DRAW"},
    {Ev::Grab,              "GRAB"},
    {Ev::Release,           "RELEASE"},
    {Ev::Pickup,            "PICKUP"},
    {Ev::Throw,             "THROW"},
    {Ev::HoldTick,          "HOLD"},
    {Ev::Impact,            "IMPACT"},
    {Ev::Explosion,         "EXPLOSION"},
    {Ev::Damage,            "DAMAGE"},
    {Ev::TeleLock,          "TELE_LOCK"},
    {Ev::TeleLockEnd,       "TELE_LOCK_END"},
    {Ev::TelePull,          "TELE_PULL"},
    {Ev::TeleCatch,         "TELE_CATCH"},
    {Ev::TeleCatchMass,     "TELE_CATCH_MASS"},
    {Ev::TeleLaunch,        "TELE_LAUNCH"},
    {Ev::TelePunt,          "TELE_PUNT"},
    {Ev::MeleeSwing,        "MELEE_SWING"},
    {Ev::MeleeImpact,       "MELEE_IMPACT"},
    {Ev::WorldButton,       "WORLD_BUTTON"},
    {Ev::WorldLever,        "WORLD_LEVER"},
    {Ev::WorldDoor,         "WORLD_DOOR"},
    {Ev::HealthApply,       "HEALTH_APPLY"},
    {Ev::HealthStation,     "HEALTH_STATION"},
};

// A new enumerator without a name here is a compile error, not a silent
// "UNKNOWN" at runtime. That failure mode has already cost this project a
// subsystem that went quiet without saying a word.
static_assert(sizeof(kRows) / sizeof(kRows[0]) == static_cast<size_t>(Ev::Count),
              "events.cpp name table is out of step with enum Ev");

// Appends "key=value" only when the value is known, so a recorded line carries
// what the engine actually knew and nothing more.
void AppendF(std::string& out, const char* key, float v) {
    if (v < 0.0f) return;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s%s=%.4g", out.empty() ? "" : ",", key,
                  static_cast<double>(v));
    out += buf;
}

void AppendI(std::string& out, const char* key, int v) {
    if (v < 0) return;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s%s=%d", out.empty() ? "" : ",", key, v);
    out += buf;
}

void AppendS(std::string& out, const char* key, const std::string& v) {
    if (v.empty()) return;
    if (!out.empty()) out += ',';
    out += key;
    out += '=';
    out += v;
}

} // namespace

const char* EvName(Ev e) {
    for (const auto& r : kRows) {
        if (r.e == e) return r.name;
    }
    return "UNKNOWN";
}

Ev ParseEv(const std::string& s) {
    for (const auto& r : kRows) {
        if (s == r.name) return r.e;
    }
    return Ev::Unknown;
}

const char* HandName(Hand h) {
    switch (h) {
        case Hand::Left: return "left";
        case Hand::Right: return "right";
        case Hand::Both: return "both";
        case Hand::Support: return "support";
        case Hand::Primary:
        default: return "primary";
    }
}

Hand ParseHand(const std::string& s) {
    if (s == "left") return Hand::Left;
    if (s == "right") return Hand::Right;
    if (s == "both") return Hand::Both;
    if (s == "support") return Hand::Support;
    return Hand::Primary;
}

std::vector<std::string> SplitFields(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    std::istringstream ss(s);
    while (std::getline(ss, cur, sep)) out.push_back(cur);
    return out;
}

float FieldFloat(const std::vector<std::string>& f, size_t i, float fallback) {
    if (i >= f.size()) return fallback;
    try { return std::stof(f[i]); } catch (...) { return fallback; }
}

int FieldInt(const std::vector<std::string>& f, size_t i, int fallback) {
    if (i >= f.size()) return fallback;
    try { return std::stoi(f[i]); } catch (...) { return fallback; }
}

std::string FieldStr(const std::vector<std::string>& f, size_t i) {
    return i < f.size() ? f[i] : std::string{};
}

std::string Encode(const HapticEvent& e) {
    std::string p;
    if (e.hand != Hand::Primary) AppendS(p, "hand", HandName(e.hand));
    if (e.material != Material::Unknown) AppendS(p, "mat", MaterialName(e.material));
    AppendS(p, "weapon", e.weapon);
    AppendS(p, "sub", e.subtype);
    // intensity and confidence default to 1.0 and are omitted at that value,
    // which keeps the common line short without losing anything.
    if (e.intensity != 1.0f) AppendF(p, "amp", e.intensity);
    AppendF(p, "energy", e.energy);
    AppendF(p, "mass", e.mass);
    AppendF(p, "speed", e.speed);
    AppendF(p, "spin", e.spin);
    if (e.confidence != 1.0f) AppendF(p, "conf", e.confidence);
    AppendI(p, "count", e.count);

    std::string out = EvName(e.type);
    if (!p.empty()) { out += ':'; out += p; }
    return out;
}

bool Decode(const std::string& event, const std::string& param, HapticEvent& out) {
    out = HapticEvent{};
    out.type = ParseEv(event);
    if (out.type == Ev::Unknown) return false;

    for (const auto& field : SplitFields(param, ',')) {
        const auto eq = field.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = field.substr(0, eq);
        const std::string v = field.substr(eq + 1);
        auto num = [&v](float fallback) {
            try { return std::stof(v); } catch (...) { return fallback; }
        };
        if (k == "hand") out.hand = ParseHand(v);
        else if (k == "mat") out.material = ParseMaterial(v);
        else if (k == "weapon") out.weapon = v;
        else if (k == "sub") out.subtype = v;
        else if (k == "amp") out.intensity = num(1.0f);
        else if (k == "energy") out.energy = num(-1.0f);
        else if (k == "mass") out.mass = num(-1.0f);
        else if (k == "speed") out.speed = num(-1.0f);
        else if (k == "spin") out.spin = num(-1.0f);
        else if (k == "conf") out.confidence = num(1.0f);
        else if (k == "count") { try { out.count = std::stoi(v); } catch (...) {} }
        // Anything else is ignored on purpose: a newer game side must be able
        // to add a field without breaking an older middleware.
    }
    return true;
}

} // namespace psvr2
