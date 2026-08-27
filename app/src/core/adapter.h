// The interface a game integration implements.
//
// Everything the middleware does outside of an adapter - loading the toolkit,
// synthesising PCM, driving the triggers, recording, replaying, analysing - is
// written against this and knows nothing about which game is running.
//
// An adapter owns exactly two things: the translation from its engine's
// vocabulary into tactile intent, and the tactile signatures that intent maps
// to. It does not own the mixer, the trigger manager or the CAPI, and it must
// not call the toolkit directly.

#pragma once

#include "core/capi.h"

#include <string>
#include <vector>

namespace psvr2 {

// The adaptive trigger's recoil ladder, exposed for measurement.
//
// The waveform layer has had a perceptual collision check since it was found
// that seven effects were sharing one perceptual cell. The trigger layer never
// did - and drifted into exactly the same state, with the pistol and SMG within
// 1.5x of each other on drive, rate AND duration simultaneously. Reporting it
// alongside the waveforms is what stops that recurring, so every adapter that
// has weapons is expected to fill this in.
struct RecoilSpec {
    std::string weapon;
    int drive;     // 0..8, capped below the motor's stall point
    int rateHz;    // only ~12-40 Hz reads as kickback
    int revealMs;  // how long the kick outlives the break: the whole sensation
    int restPeak;  // strongest resistance in the RESTING profile, 0..8
};

class IGameAdapter {
public:
    virtual ~IGameAdapter() = default;

    // Short lowercase identity, e.g. "alyx". Used for config keys and messages.
    virtual const char* id() const = 0;
    // Human-readable name of the game.
    virtual const char* gameName() const = 0;

    // The single entry point. Every event source - live transport, recording
    // replay, synthetic test - funnels through here, so a synthetic event
    // exercises exactly the code path a real one does. That property is what
    // makes the whole no-install development mode meaningful rather than a
    // parallel implementation that happens to look similar.
    virtual void Handle(const std::string& event, const std::string& param) = 0;

    // Re-installs persistent trigger state. Called at startup and whenever the
    // game side announces it has (re)loaded.
    virtual void RefreshWeaponState() = 0;

    // --- diagnostics shared by --test and --analyze ----------------------

    // Which hands actually received a waveform since the last reset.
    // bit0 = left, bit1 = right.
    //
    // The self-test used to announce the hand it was ASKED to play on, which is
    // not the same thing: bilateral effects ignore the request and go to both.
    // Announcing "[right]" and then buzzing both hands makes correct behaviour
    // look like a bug.
    virtual void ResetEmitTrace() = 0;
    virtual uint8_t emitTrace() const = 0;

    // Clears per-hand rate limits and re-seeds per-instance variation, so a
    // test suite firing signatures back to back is neither throttled nor
    // rendered with drifting jitter.
    virtual void ResetForTest() = 0;

    // --- self test --------------------------------------------------------
    virtual std::vector<std::string> SelfTestNames() const = 0;
    virtual bool RunSelfTest(const std::string& name, const std::string& side) = 0;

    // Which perceptual family a self-test case belongs to, or nullptr to leave
    // it out of the collision report.
    //
    // Group by CO-OCCURRENCE: two effects need telling apart only if you meet
    // them in the same moment. Confusing the pistol with the shotgun is a real
    // failure; confusing the pistol with a wooden crate is not, and a report
    // that cries wolf stops being read.
    virtual const char* AnalyzeFamily(const std::string& testName) const = 0;

    // Empty for a game with no firearms.
    virtual std::vector<RecoilSpec> RecoilLadder() const = 0;

    // --- game-side plumbing ----------------------------------------------

    // Printed while waiting for the game, so each integration explains its own
    // connection requirements rather than main.cpp hard-coding one game's.
    virtual std::vector<std::string> ConnectionHelp() const = 0;
};

} // namespace psvr2
