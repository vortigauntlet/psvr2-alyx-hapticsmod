// Adaptive trigger state machine.
//
// The trigger is treated as *persistent physical state*, not as an effect you
// fire. A weapon installs a base profile that stays until the weapon changes;
// momentary events (a shot, a reload click, a gravity-glove catch) push a
// time-limited overlay on top and the base is restored automatically when the
// overlay expires. That is what stops a reload click from permanently wiping
// the shotgun's trigger feel, which the previous single-slot "pending restore"
// design did whenever two transients overlapped.
//
// All builders clamp to the ranges documented in pad_trigger_effect.h. Those
// ranges are not advisory: e.g. Weapon mode requires startPosition in 2..7 and
// endPosition > startPosition, and Slope requires strengths in 1..8. Passing
// a zero strength to Slope, or startPosition 0 to Weapon, is rejected by the
// toolkit with INVALID_PARAMETER and the effect silently never happens.

#pragma once

#include "capi.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace psvr2 {

using Clock = std::chrono::steady_clock;

namespace trig {
TriggerCommand Off();
TriggerCommand Feedback(int position, int strength);
TriggerCommand Weapon(int start, int end, int strength);
TriggerCommand Vibration(int position, int amplitude, int frequencyHz);
TriggerCommand MultiFeedback(const std::array<int, 10>& strengths);
TriggerCommand Slope(int start, int end, int startStrength, int endStrength);
TriggerCommand MultiVibration(int frequencyHz, const std::array<int, 10>& amplitudes);
const char* ModeName(TriggerMode m);
} // namespace trig

class TriggerManager {
public:
    TriggerManager(Capi& capi, bool debug) : capi_(capi), debug_(debug) {}

    void SetEnabled(bool enabled);
    void SetMaster(float master);

    // Persistent state. Survives every transient overlay.
    void SetBase(Controller c, const TriggerCommand& cmd, const std::string& label);
    void ClearBase(Controller c);

    // Transient overlay. Higher priority wins; equal priority means newest wins.
    void PushOverlay(Controller c, const TriggerCommand& cmd, int durationMs,
                     int priority, const std::string& label);

    // Updates the overlay carrying `label` in place, or pushes it if absent.
    //
    // For a state the game reports as a repeating heartbeat rather than as
    // start/stop events - "you are still holding something heavy" - this is the
    // right primitive. Clearing and re-pushing would make the trigger toggle
    // back to the base profile and away again on every heartbeat, which is
    // felt. Refreshing an existing overlay's expiry is silent.
    //
    // It also fails safe: if the heartbeat stops for any reason, including the
    // game script dying, the overlay simply expires. Nothing can leave the
    // trigger loaded forever.
    void RefreshOverlay(Controller c, const TriggerCommand& cmd, int durationMs,
                        int priority, const std::string& label);

    // Cancels every live overlay carrying `label`.
    //
    // Some physical states are held rather than momentary - a gravity glove
    // stays locked on until you pull or let go - and the honest way to model
    // that is an overlay that persists until the game says it ended, not a
    // guessed duration that expires while the state is still true. Those
    // overlays still carry a generous timeout as a backstop so a missed "stop"
    // event cannot strand the trigger.
    void ClearOverlays(Controller c, const std::string& label);

    // Expires overlays and re-applies whatever should now be in effect.
    // Called from the main loop so no CAPI call can outlive shutdown.
    void Tick();

    void Reset();

    uint64_t commandsSent() const { return sent_; }
    uint64_t commandErrors() const { return errors_; }

private:
    struct Overlay {
        TriggerCommand cmd;
        Clock::time_point expiry;
        int priority = 0;
        uint64_t seq = 0;
        std::string label;
    };

    struct Side {
        TriggerCommand base = trig::Off();
        std::string baseLabel = "off";
        std::vector<Overlay> overlays;
        TriggerCommand lastSent = trig::Off();
        bool everSent = false;
    };

    Side& SideFor(Controller c) { return c == Controller::Left ? left_ : right_; }
    void ApplyLocked(Controller c);
    void SendLocked(Controller c, const TriggerCommand& cmd, const std::string& label);

    Capi& capi_;
    bool debug_ = false;
    std::mutex mu_;
    bool enabled_ = true;
    float master_ = 1.0f;
    uint64_t seq_ = 0;
    Side left_;
    Side right_;
    uint64_t sent_ = 0;
    uint64_t errors_ = 0;
};

} // namespace psvr2
