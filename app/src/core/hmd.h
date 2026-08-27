// Headset rumble channel.
//
// ---------------------------------------------------------------------------
// The rule
// ---------------------------------------------------------------------------
//
// This project's governing rule has always been:
//
//     If the player's HAND would not physically feel it, do not vibrate.
//
// Adding a second actuator does not repeal that rule, it generalises it:
//
//     Vibrate the body part that would actually feel it.
//
// So the headset is not a second channel for hand events at lower volume, and
// it is emphatically not a place to put the cues that were deleted for being
// HUD readouts in haptic costume. It answers exactly one question: would the
// player's HEAD or FACE feel this? In Half-Life: Alyx that is a very short
// list, and a short list is the correct outcome rather than a disappointing
// one. Weapon recoil is a hand event - your skull does not recoil. A crate
// landing in your palm is a hand event. Footsteps are neither.
//
// ---------------------------------------------------------------------------
// What the hardware actually gives us
// ---------------------------------------------------------------------------
//
//     int psvr2_toolkit_set_hmd_rumble(uint8_t rumbleHz)
//
// One byte, and it is a FREQUENCY. Three consequences drive this whole file:
//
//   1. There is no amplitude. Nothing here can be made "softer" or "louder"
//      the way a PCM voice can. Designing by loudness is not available.
//   2. There is no duration. The value is a STATE that persists until it is
//      set again, so every effect is a scheduled sequence of steps ending in
//      an explicit 0, not a fire-and-forget.
//   3. Character therefore comes from pitch, length and RHYTHM alone. That is
//      a genuine constraint, but rhythm was already measured on hardware as a
//      usable axis when catch-light and catch-heavy were separated by tremolo
//      rather than by level - so it is a constraint this project has already
//      shown it can design inside.
//
// ---------------------------------------------------------------------------
// Failing safe
// ---------------------------------------------------------------------------
//
// A stuck-on headset rumble is the worst failure this program could produce:
// it is strapped to the player's face and they cannot see a console to work
// out why. Four independent guards, in order of how much has to go wrong:
//
//   * OFF BY DEFAULT. hmd=true is opt-in.
//   * A WATCHDOG. No sequence may hold the motor on past kMaxRunMs. Even a
//     malformed pattern stops on its own.
//   * PERMANENT DISABLE on a hard error. One clear message, then the channel
//     is never touched again - the hand channel is unaffected.
//   * FORCED OFF at shutdown, including the abnormal paths, via Stop().
//
// The frequencies below are CODE level. They are reasoned, not measured: the
// grip actuator's response curve was established with --sweep and the trigger
// motor's with --trigger-sweep, and the headset has had neither. --hmd-sweep
// exists to fix that, and until someone runs it these numbers should be
// treated as a starting point rather than as findings.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace psvr2 {

class Capi;

// One step of a pattern: hold `hz` for `ms`. hz == 0 is a deliberate silence,
// which is how a rhythm gets its gaps.
struct HmdStep {
    uint8_t hz = 0;
    int ms = 0;
};

struct HmdPattern {
    std::string name;
    std::vector<HmdStep> steps;
};

class HmdChannel {
public:
    // Hard ceiling on a single uninterrupted run, watchdog included. Nothing
    // in Alyx justifies a longer head sensation than this, and the cap means a
    // bad pattern is self-limiting.
    static constexpr int kMaxRunMs = 2500;

    // Enables the channel. Returns false (with `why` filled in) when the
    // toolkit cannot support it - an older DLL without the export. Never
    // throws and never affects the controller path.
    bool Enable(Capi* capi, std::string& why);

    bool enabled() const { return enabled_; }
    bool available() const { return available_; }

    // Starts a pattern, replacing whatever is running. Higher priority wins;
    // equal priority also wins, so the most recent event is the current one.
    void Play(const HmdPattern& pattern, int priority = 0);

    // Drives the state machine. Call often - it is cheap and only touches the
    // hardware when the step actually changes. `nowMs` is a monotonic clock.
    void Tick(int64_t nowMs);

    // Forces the motor off immediately and clears any pattern. Safe to call
    // when disabled, unavailable, or already stopped.
    void Stop();

    // Diagnostics.
    int writes() const { return writes_; }
    const std::string& lastError() const { return lastError_; }

private:
    void Write(uint8_t hz);

    Capi* capi_ = nullptr;
    bool enabled_ = false;
    bool available_ = false;

    std::vector<HmdStep> steps_;
    size_t index_ = 0;
    int64_t stepEndsAt_ = 0;
    int64_t runEndsAt_ = 0;   // watchdog
    int priority_ = 0;
    uint8_t current_ = 0;     // last value actually written
    bool running_ = false;

    int writes_ = 0;
    std::string lastError_;
};

// ---------------------------------------------------------------------------
// The patterns
// ---------------------------------------------------------------------------
//
// Four. Each one is here because a specific part of the player's head or face
// is in contact with something.
namespace hmdfx {

// Your own hand clamped over your own mouth, hiding from Jeff. The single most
// defensible headset effect in the game: it is literally a hand pressed
// against the face. Low and sustained with a slow breath in it - presence,
// never a buzz. Deliberately the quietest thing here in character, because the
// moment it belongs to is a held breath.
HmdPattern CoverMouth();

// A barnacle tongue has you and is hauling you off the floor. Whole-body
// restraint that very much includes the head. Rises, because you are being
// lifted and it does not stop until you are.
HmdPattern Barnacle();

// It let go. Short, blunt, over - the release, not a fall.
HmdPattern BarnacleRelease();

// A hit hard enough to snap the head. `severity` in 0..1 scales LENGTH and
// PULSE COUNT, never level, because level does not exist on this actuator.
// Chip damage never reaches here; the router gates it.
HmdPattern Hurt(float severity);

}  // namespace hmdfx
}  // namespace psvr2
