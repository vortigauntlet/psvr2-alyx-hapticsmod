#include "hmd.h"

#include <algorithm>
#include <cmath>

#include "capi.h"

namespace psvr2 {

// ---------------------------------------------------------------------------
// Channel
// ---------------------------------------------------------------------------

bool HmdChannel::Enable(Capi* capi, std::string& why) {
    capi_ = capi;
    enabled_ = false;
    available_ = false;

    if (capi_ == nullptr || !capi_->loaded()) {
        why = "toolkit not loaded";
        return false;
    }
    if (!capi_->hasHmdRumble()) {
        // An older toolkit. This is a legitimate configuration, not a fault,
        // so it gets a plain explanation rather than an error.
        why = "this PSVR2Toolkit build does not export psvr2_toolkit_set_hmd_rumble";
        return false;
    }

    // Probe with 0. That is the "off" value, so the probe cannot itself start
    // the motor - the one call that is guaranteed safe to make blind.
    //
    // The return code is recorded but deliberately NOT used to decide
    // availability. This DLL is already known to return an uninitialised
    // register from write_pcm and 1-instead-of-0 from wait_for_pcm, so a
    // non-zero here is far more likely to be ABI noise than a real failure,
    // and refusing to enable on it would disable the feature for everyone.
    //
    // The honest position: we cannot detect from software whether the headset
    // will actually buzz. A headset that has not been jailbroken simply
    // ignores the value. That is exactly the "safely fall back if they forgot"
    // behaviour we want - nothing breaks, nothing errors, it is just silent.
    const int rc = capi_->SetHmdRumble(0);
    if (rc < 0 && rc >= -4) {
        // A recognised negative result code from the documented set. This is
        // the one case specific enough to treat as a genuine refusal.
        why = std::string("toolkit refused headset rumble: ") + ResultName(rc);
        return false;
    }

    available_ = true;
    enabled_ = true;
    current_ = 0;
    running_ = false;
    return true;
}

void HmdChannel::Write(uint8_t hz) {
    if (!enabled_ || capi_ == nullptr) return;
    if (hz == current_) return;  // the motor is already there
    const int rc = capi_->SetHmdRumble(hz);
    ++writes_;
    current_ = hz;
    if (rc < 0 && rc >= -4) {
        // A hard, recognised failure mid-run. Shut the channel down rather
        // than spraying the log: one message, motor off, never touched again.
        lastError_ = std::string("headset rumble failed (") + ResultName(rc)
                     + ") - channel disabled";
        capi_->SetHmdRumble(0);
        enabled_ = false;
        running_ = false;
        steps_.clear();
    }
}

void HmdChannel::Play(const HmdPattern& pattern, int priority) {
    if (!enabled_ || pattern.steps.empty()) return;
    if (running_ && priority < priority_) return;

    steps_ = pattern.steps;
    index_ = 0;
    priority_ = priority;
    running_ = true;
    stepEndsAt_ = 0;  // Tick() starts step 0 on its next call
    runEndsAt_ = 0;
}

void HmdChannel::Tick(int64_t nowMs) {
    if (!enabled_ || !running_) return;

    // Watchdog. Armed on the first tick of a pattern so it measures real
    // elapsed time rather than trusting the pattern's own arithmetic.
    if (runEndsAt_ == 0) runEndsAt_ = nowMs + kMaxRunMs;
    if (nowMs >= runEndsAt_) {
        Stop();
        return;
    }

    // Advance through however many steps have expired. A zero-length step is
    // legal and simply passes through.
    while (running_ && nowMs >= stepEndsAt_) {
        if (index_ >= steps_.size()) {
            Stop();
            return;
        }
        const HmdStep& s = steps_[index_];
        Write(s.hz);
        stepEndsAt_ = (stepEndsAt_ == 0 ? nowMs : stepEndsAt_) + std::max(0, s.ms);
        ++index_;
    }
}

void HmdChannel::Stop() {
    // Unconditional: this runs on the shutdown path, so it must work even when
    // the channel has already disabled itself.
    if (capi_ != nullptr && available_ && current_ != 0) {
        capi_->SetHmdRumble(0);
        ++writes_;
    }
    current_ = 0;
    running_ = false;
    steps_.clear();
    index_ = 0;
    stepEndsAt_ = 0;
    runEndsAt_ = 0;
    priority_ = 0;
}

// ---------------------------------------------------------------------------
// Patterns
// ---------------------------------------------------------------------------
//
// Reminder while reading these: the numbers are FREQUENCIES, not levels. A
// higher number is not stronger, it is faster. Separation between these four
// therefore has to come from length and rhythm, which is why they look so
// different from each other on the page rather than being one shape with the
// numbers nudged - the exact failure this project has criticised twice, in the
// weapon triggers and again in the SMG reload steps.

namespace hmdfx {

HmdPattern CoverMouth() {
    // A held breath. Low, continuous, with a slow swell rather than an edge -
    // there is no impact here, just contact and stillness. The gentle 18/22 Hz
    // alternation is the breath; it is deliberately too slow to read as a
    // pulse train.
    //
    // Matched to the 562 ms the grip-actuator COVER_MOUTH already runs for, so
    // the two channels describe one event instead of overlapping untidily.
    return {"cover-mouth", {
        {18, 140},
        {22, 150},
        {18, 150},
        {20, 120},
        {0, 0},
    }};
}

HmdPattern Barnacle() {
    // Being hauled upward by something that has you. Accelerating - the rate
    // climbs the whole way through, which is what makes it read as a process
    // being done TO you rather than as a hit. Ends abruptly at the top: you
    // have arrived, and nothing about that is gentle.
    return {"barnacle", {
        {14, 120},
        {20, 120},
        {28, 110},
        {38, 100},
        {50, 90},
        {64, 220},
        {0, 0},
    }};
}

HmdPattern BarnacleRelease() {
    // It dropped you. One blunt low event and done. Distinguished from
    // Barnacle by being a tenth its length and by not moving at all - the
    // clearest possible contrast with the thing that preceded it.
    return {"barnacle-release", {
        {30, 90},
        {0, 0},
    }};
}

HmdPattern Hurt(float severity) {
    // A hit that snaps the head. severity 0..1 buys LENGTH and PULSE COUNT,
    // never level, because level is not an axis this actuator has.
    //
    // A light hit is one short jolt. A heavy one is a jolt plus a ring-down,
    // which reads as "that rattled you" - a categorical difference in pulse
    // count rather than a matter of degree, which is the distinction skin and
    // bone resolve best.
    const float s = std::clamp(severity, 0.0f, 1.0f);
    const int first = 70 + static_cast<int>(std::lround(60.0f * s));

    if (s < 0.55f) {
        return {"hurt", {
            {static_cast<uint8_t>(first), 70},
            {0, 0},
        }};
    }
    return {"hurt-heavy", {
        {static_cast<uint8_t>(first), 110},
        {0, 40},
        {static_cast<uint8_t>(first / 2), 130},
        {0, 0},
    }};
}

}  // namespace hmdfx
}  // namespace psvr2
