#include "triggers.h"

#include <algorithm>
#include <cmath>
#include <iostream>

namespace psvr2 {
namespace trig {

namespace {
inline uint8_t U8(int v, int lo, int hi) {
    return static_cast<uint8_t>(std::clamp(v, lo, hi));
}
} // namespace

const char* ModeName(TriggerMode m) {
    switch (m) {
        case kTriggerOff: return "OFF";
        case kTriggerFeedback: return "FEEDBACK";
        case kTriggerWeapon: return "WEAPON";
        case kTriggerVibration: return "VIBRATION";
        case kTriggerMultiPositionFeedback: return "MULTI_FEEDBACK";
        case kTriggerSlopeFeedback: return "SLOPE";
        case kTriggerMultiPositionVibration: return "MULTI_VIBRATION";
        default: return "?";
    }
}

TriggerCommand Off() {
    TriggerCommand c{};
    c.mode = kTriggerOff;
    return c;
}

TriggerCommand Feedback(int position, int strength) {
    TriggerCommand c{};
    c.mode = kTriggerFeedback;
    c.data.feedback.position = U8(position, 0, 9);
    c.data.feedback.strength = U8(strength, 0, 8);
    return c;
}

TriggerCommand Weapon(int start, int end, int strength) {
    TriggerCommand c{};
    c.mode = kTriggerWeapon;
    // Documented: startPosition 2..7, endPosition startPosition+1..8.
    const int s = std::clamp(start, 2, 7);
    const int e = std::clamp(end, s + 1, 8);
    c.data.weapon.startPosition = static_cast<uint8_t>(s);
    c.data.weapon.endPosition = static_cast<uint8_t>(e);
    c.data.weapon.strength = U8(strength, 0, 8);
    return c;
}

TriggerCommand Vibration(int position, int amplitude, int frequencyHz) {
    TriggerCommand c{};
    c.mode = kTriggerVibration;
    c.data.vibration.position = U8(position, 0, 9);
    c.data.vibration.amplitude = U8(amplitude, 0, 8);
    c.data.vibration.frequency = U8(frequencyHz, 0, 255);
    return c;
}

TriggerCommand MultiFeedback(const std::array<int, 10>& strengths) {
    TriggerCommand c{};
    c.mode = kTriggerMultiPositionFeedback;
    for (int i = 0; i < 10; ++i) c.data.multiFeedback.strength[i] = U8(strengths[i], 0, 8);
    return c;
}

TriggerCommand Slope(int start, int end, int startStrength, int endStrength) {
    TriggerCommand c{};
    c.mode = kTriggerSlopeFeedback;
    // Documented: startPosition 0..endPosition, endPosition start+1..9,
    // and BOTH strengths 1..8 - zero is invalid, not "off".
    const int s = std::clamp(start, 0, 8);
    const int e = std::clamp(end, s + 1, 9);
    c.data.slope.startPosition = static_cast<uint8_t>(s);
    c.data.slope.endPosition = static_cast<uint8_t>(e);
    c.data.slope.startStrength = U8(startStrength, 1, 8);
    c.data.slope.endStrength = U8(endStrength, 1, 8);
    return c;
}

TriggerCommand MultiVibration(int frequencyHz, const std::array<int, 10>& amplitudes) {
    TriggerCommand c{};
    c.mode = kTriggerMultiPositionVibration;
    c.data.multiVibration.frequency = U8(frequencyHz, 0, 255);
    for (int i = 0; i < 10; ++i) c.data.multiVibration.amplitude[i] = U8(amplitudes[i], 0, 8);
    return c;
}

} // namespace trig

// ---------------------------------------------------------------------------

namespace {

// Scale every strength-like field by the trigger master gain. Done centrally
// so profiles are written in absolute 0..8 terms and stay readable.
TriggerCommand Scaled(const TriggerCommand& in, float g) {
    if (g == 1.0f) return in;
    auto s = [g](uint8_t v, int lo, int hi) {
        return static_cast<uint8_t>(std::clamp(
            static_cast<int>(std::lround(v * g)), lo, hi));
    };
    TriggerCommand c = in;
    switch (c.mode) {
        case kTriggerFeedback:
            c.data.feedback.strength = s(c.data.feedback.strength, 0, 8);
            break;
        case kTriggerWeapon:
            c.data.weapon.strength = s(c.data.weapon.strength, 0, 8);
            break;
        case kTriggerVibration:
            c.data.vibration.amplitude = s(c.data.vibration.amplitude, 0, 8);
            break;
        case kTriggerMultiPositionFeedback:
            for (auto& v : c.data.multiFeedback.strength) v = s(v, 0, 8);
            break;
        case kTriggerSlopeFeedback:
            c.data.slope.startStrength = s(c.data.slope.startStrength, 1, 8);
            c.data.slope.endStrength = s(c.data.slope.endStrength, 1, 8);
            break;
        case kTriggerMultiPositionVibration:
            for (auto& v : c.data.multiVibration.amplitude) v = s(v, 0, 8);
            break;
        default:
            break;
    }
    return c;
}

const char* SideName(Controller c) { return c == Controller::Left ? "LEFT" : "RIGHT"; }

} // namespace

void TriggerManager::SetEnabled(bool enabled) {
    std::lock_guard<std::mutex> lk(mu_);
    enabled_ = enabled;
    if (!enabled) {
        left_.overlays.clear();
        right_.overlays.clear();
        SendLocked(Controller::Left, trig::Off(), "disabled");
        SendLocked(Controller::Right, trig::Off(), "disabled");
    }
}

void TriggerManager::SetMaster(float master) {
    std::lock_guard<std::mutex> lk(mu_);
    master_ = std::clamp(master, 0.0f, 2.0f);
    ApplyLocked(Controller::Left);
    ApplyLocked(Controller::Right);
}

void TriggerManager::SetBase(Controller c, const TriggerCommand& cmd, const std::string& label) {
    std::lock_guard<std::mutex> lk(mu_);
    if (c == Controller::Both) {
        left_.base = cmd;  left_.baseLabel = label;
        right_.base = cmd; right_.baseLabel = label;
        ApplyLocked(Controller::Left);
        ApplyLocked(Controller::Right);
        return;
    }
    Side& s = SideFor(c);
    s.base = cmd;
    s.baseLabel = label;
    ApplyLocked(c);
}

void TriggerManager::ClearBase(Controller c) {
    SetBase(c, trig::Off(), "off");
}

void TriggerManager::PushOverlay(Controller c, const TriggerCommand& cmd, int durationMs,
                                 int priority, const std::string& label) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!enabled_) return;

    auto push = [&](Controller side) {
        Side& s = SideFor(side);
        Overlay o;
        o.cmd = cmd;
        o.expiry = Clock::now() + std::chrono::milliseconds(std::max(10, durationMs));
        o.priority = priority;
        o.seq = ++seq_;
        o.label = label;
        s.overlays.push_back(std::move(o));
        ApplyLocked(side);
    };

    if (c == Controller::Both) { push(Controller::Left); push(Controller::Right); }
    else push(c);
}

void TriggerManager::RefreshOverlay(Controller c, const TriggerCommand& cmd, int durationMs,
                                    int priority, const std::string& label) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!enabled_) return;

    auto refresh = [&](Controller side) {
        Side& s = SideFor(side);
        const auto expiry = Clock::now() + std::chrono::milliseconds(std::max(10, durationMs));
        for (auto& o : s.overlays) {
            if (o.label != label) continue;
            o.cmd = cmd;
            o.expiry = expiry;
            o.priority = priority;
            ApplyLocked(side);
            return;
        }
        Overlay o;
        o.cmd = cmd;
        o.expiry = expiry;
        o.priority = priority;
        o.seq = ++seq_;
        o.label = label;
        s.overlays.push_back(std::move(o));
        ApplyLocked(side);
    };

    if (c == Controller::Both) { refresh(Controller::Left); refresh(Controller::Right); }
    else refresh(c);
}

void TriggerManager::ClearOverlays(Controller c, const std::string& label) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!enabled_) return;

    auto clear = [&](Controller side) {
        Side& s = SideFor(side);
        const size_t before = s.overlays.size();
        s.overlays.erase(
            std::remove_if(s.overlays.begin(), s.overlays.end(),
                           [&](const Overlay& o) { return o.label == label; }),
            s.overlays.end());
        if (s.overlays.size() != before) ApplyLocked(side);
    };

    if (c == Controller::Both) { clear(Controller::Left); clear(Controller::Right); }
    else clear(c);
}

void TriggerManager::Tick() {
    std::lock_guard<std::mutex> lk(mu_);
    if (!enabled_) return;
    const auto now = Clock::now();
    for (Controller c : {Controller::Left, Controller::Right}) {
        Side& s = SideFor(c);
        const size_t before = s.overlays.size();
        s.overlays.erase(
            std::remove_if(s.overlays.begin(), s.overlays.end(),
                           [&](const Overlay& o) { return o.expiry <= now; }),
            s.overlays.end());
        if (s.overlays.size() != before) ApplyLocked(c);
    }
}

void TriggerManager::ApplyLocked(Controller c) {
    if (!enabled_) return;
    Side& s = SideFor(c);

    const Overlay* top = nullptr;
    for (const auto& o : s.overlays) {
        if (top == nullptr || o.priority > top->priority ||
            (o.priority == top->priority && o.seq > top->seq)) {
            top = &o;
        }
    }

    if (top != nullptr) SendLocked(c, top->cmd, top->label);
    else SendLocked(c, s.base, s.baseLabel);
}

void TriggerManager::SendLocked(Controller c, const TriggerCommand& cmd, const std::string& label) {
    // Offline modes (--analyze) construct a manager over a CAPI that was never
    // loaded. Calling through it would report INVALID_PARAMETER for a call that
    // was never actually attempted, which is a false error - and this project's
    // whole diagnostic contract is that reported failures are real ones.
    if (!capi_.loaded()) return;

    Side& s = SideFor(c);
    const TriggerCommand scaled = Scaled(cmd, master_);

    // Skip redundant traffic. Besides halving USB chatter during rapid fire,
    // this is what keeps the debug log readable enough to actually diagnose.
    if (s.everSent && SameCommand(scaled, s.lastSent)) return;

    const int rc = capi_.SetTriggerEffect(c, scaled);
    s.lastSent = scaled;
    s.everSent = true;
    ++sent_;
    if (rc != kResultOk) ++errors_;

    if (debug_ || rc != kResultOk) {
        std::cout << "[Trigger] " << SideName(c)
                  << " mode=" << trig::ModeName(scaled.mode)
                  << " (" << label << ") rc=" << rc
                  << " (" << ResultName(rc) << ")\n";
    }
}

void TriggerManager::Reset() {
    std::lock_guard<std::mutex> lk(mu_);
    left_.overlays.clear();
    right_.overlays.clear();
    left_.base = trig::Off();
    right_.base = trig::Off();
    SendLocked(Controller::Left, trig::Off(), "reset");
    SendLocked(Controller::Right, trig::Off(), "reset");
}

} // namespace psvr2
