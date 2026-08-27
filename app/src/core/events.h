// The game-independent semantic event model.
//
// This is the contract between a game adapter and the haptic core. An adapter's
// entire job is to turn whatever its engine reports into one of these; nothing
// below this line knows which game is running.
//
// WHAT THIS IS NOT
//
// It is not a superset of every event either game can produce. The set below is
// deliberately the intersection of "things a hand physically feels" with "things
// at least one supported engine can actually report", and it stays small on
// purpose: a vocabulary nobody can populate is not an abstraction, it is a wish
// list. Half-Life: Alyx has no gravity-gun charge; Half-Life 2 has no
// two-handed weapon grip. Each of those lives on ONE side of the model and is
// marked as such, rather than being invented for the other.
//
// WIRE FORMAT
//
// Adapters receive tagged text lines, not structs, because the game side is a
// script (Alyx) or a plugin (HL2VR) in another process. The format is the one
// the Alyx build already uses and recordings already contain:
//
//     [PSVR2H] <EVENT>:<field>,<field>,...
//
// Encode()/Decode() below move between that text and HapticEvent, which is what
// makes a recording from one game replayable through the other's adapter and
// makes the model testable without either game installed.

#pragma once

#include "materials.h"

#include <cstdint>
#include <string>
#include <vector>

namespace psvr2 {

// Which hand an event belongs to.
//
// Primary/Support are RELATIVE - they resolve against the player's handedness at
// routing time. An adapter should prefer them: an engine usually knows "the hand
// holding the weapon", not "the left one", and resolving too early is what makes
// a left-handed player feel everything backwards.
enum class Hand : uint8_t { Primary, Support, Left, Right, Both };

const char* HandName(Hand h);
Hand ParseHand(const std::string& s);

// The semantic event vocabulary.
//
// Grouped by what the hand is doing, because that is the only grouping that
// predicts what a signature should feel like.
enum class Ev : uint16_t {
    Unknown = 0,

    // ---- session and player state -------------------------------------
    Menu,            // the game is paused behind a menu; release everything
    PrimaryHand,     // handedness changed
    WeaponChange,    // a different weapon is now in the primary hand
    TwoHandStart,    // support hand came onto the weapon   (Alyx only today)
    TwoHandEnd,      // support hand left the weapon        (Alyx only today)

    // ---- weapon --------------------------------------------------------
    WeaponFire,      // a round left the weapon
    WeaponDryFire,   // the trigger broke on an empty chamber
    WeaponChargeStart, // a charged shot began winding up   (HL2VR: AR2 alt-fire)
    WeaponChargeFire,  // the charged shot released
    ReloadStart,     // the reload gesture began
    MagazineInsert,  // a magazine seated
    MagazineEject,   // a magazine dropped free
    Slide,           // a pistol slide cycled
    Bolt,            // a bolt or charging handle cycled
    Pump,            // a pump action cycled
    ShellInsert,     // one shell pressed into a tube
    Draw,            // a bolt drawn back under tension     (HL2VR: crossbow)

    // ---- direct interaction --------------------------------------------
    Grab,            // a hand closed on something
    Release,         // a hand opened and let go
    Pickup,          // an object entered the hand
    Throw,           // an object left the hand with intent
    HoldTick,        // heartbeat: still carrying something, with its state

    // ---- collisions and damage ------------------------------------------
    Impact,          // something the hand is connected to struck something
    Explosion,       // a blast the player is inside
    Damage,          // the player was hurt

    // ---- telekinesis ----------------------------------------------------
    // Alyx's gravity gloves and Half-Life 2's gravity gun are the same verb
    // with different tempo, so they share this family rather than each
    // inventing one.
    TeleLock,        // a target was acquired and is held in the beam
    TeleLockEnd,     // the target was let go without being pulled
    TelePull,        // the object is being reeled in
    TeleCatch,       // the capture landed in the hand
    TeleCatchMass,   // what was caught, resolved a tick later
    TeleLaunch,      // the held object was fired away    (HL2VR: gravity gun)
    TelePunt,        // a shove at something not held     (HL2VR: gravity gun)

    // ---- melee ----------------------------------------------------------
    // Deliberately two events. A swing through empty air must NOT vibrate -
    // that is the single most common way a melee weapon is made to feel wrong -
    // so the swing exists only to arm the trigger, and only the impact plays.
    MeleeSwing,
    MeleeImpact,

    // ---- world ----------------------------------------------------------
    WorldButton,     // a button pressed under the hand
    WorldLever,      // a lever or valve turned under the hand
    WorldDoor,       // a door moving under the hand

    // ---- health ---------------------------------------------------------
    HealthApply,     // a health pen or charger applied to the player
    HealthStation,   // a wall station working on the hand

    Count
};

const char* EvName(Ev e);
Ev ParseEv(const std::string& s);

// One semantic event.
//
// Every physical field carries a sentinel for "the engine could not tell me".
// That distinction is load-bearing: a mass of -1 means unknown and must fall
// back to a neutral shape, whereas a mass of 0 would be a claim that the object
// is weightless. Guessing a plausible number instead of admitting ignorance is
// what makes a haptic layer feel arbitrary.
struct HapticEvent {
    Ev type = Ev::Unknown;
    Hand hand = Hand::Primary;
    Material material = Material::Unknown;

    // Weapon token, e.g. "PISTOL", "SMG", "GRAVGUN". Empty means "unchanged".
    std::string weapon;
    // Free-form discriminator within a type, e.g. the melee weapon that hit.
    std::string subtype;

    float intensity  = 1.0f;   // 0..1 designed strength, after any game scaling
    float energy     = -1.0f;  // 0..1 normalised collision energy, <0 unknown
    float mass       = -1.0f;  // kg, <0 unknown
    float speed      = -1.0f;  // engine units/s, <0 unknown
    float spin       = -1.0f;  // deg/s, <0 unknown
    float confidence = 1.0f;   // 0..1, how sure the adapter is this happened
    int   count      = -1;     // rounds left, shells loaded, ..., <0 unknown

    // Milliseconds since the adapter started. Used by recordings; not by
    // synthesis, which must never depend on wall-clock.
    double timestamp = 0.0;

    bool has(float f) const { return f >= 0.0f; }
};

// Text codec.
//
// Encode produces "<EVENT>:<params>" with only the fields that are known, so a
// recording stays readable and hand-editable. Decode is tolerant: unknown
// fields are ignored and a malformed number leaves its sentinel, because one
// bad line in a session recording should not cost every other line.
std::string Encode(const HapticEvent& e);
bool Decode(const std::string& event, const std::string& param, HapticEvent& out);

// Field helpers shared by every adapter's own line parsing.
std::vector<std::string> SplitFields(const std::string& s, char sep = ',');
float FieldFloat(const std::vector<std::string>& f, size_t i, float fallback);
int FieldInt(const std::vector<std::string>& f, size_t i, int fallback);
std::string FieldStr(const std::vector<std::string>& f, size_t i);

} // namespace psvr2
