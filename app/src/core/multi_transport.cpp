#include "core/multi_transport.h"

#include <algorithm>
#include <chrono>
#include <thread>

namespace psvr2 {

void MultiTransport::Add(Transport* t, const char* label) {
    if (t == nullptr) return;
    sources_.push_back({t, label == nullptr ? "?" : label, false});
    RefreshName();
}

void MultiTransport::RefreshName() {
    std::string live;
    for (const auto& s : sources_) {
        if (!s.transport->Connected()) continue;
        if (!live.empty()) live += " + ";
        live += s.label;
    }
    name_ = live.empty() ? "none" : live;
}

bool MultiTransport::Connect() {
    bool any = false;
    for (auto& s : sources_) {
        // Every source is offered the chance to connect on every call, not just
        // the first one that fails. The plugin and the bHaptics stream come up
        // at different moments - the socket is there the instant the game
        // starts, the plugin only once a level has loaded - and a design that
        // stopped at the first success would never pick up the second.
        const bool now = s.transport->Connect() || s.transport->Connected();
        if (now != s.wasConnected) {
            s.wasConnected = now;
            changed_ = true;
        }
        any = any || now;
    }
    if (changed_) RefreshName();
    return any;
}

bool MultiTransport::Connected() const {
    return std::any_of(sources_.begin(), sources_.end(), [](const Source& s) {
        return s.transport->Connected();
    });
}

int MultiTransport::liveCount() const {
    return static_cast<int>(std::count_if(
        sources_.begin(), sources_.end(),
        [](const Source& s) { return s.transport->Connected(); }));
}

std::vector<std::string> MultiTransport::Poll() {
    std::vector<std::string> out;
    for (auto& s : sources_) {
        if (!s.transport->Connected()) {
            if (s.wasConnected) {
                s.wasConnected = false;
                changed_ = true;
                RefreshName();
            }
            continue;
        }
        auto lines = s.transport->Poll();
        if (lines.empty()) continue;
        out.insert(out.end(), std::make_move_iterator(lines.begin()),
                   std::make_move_iterator(lines.end()));
    }
    return out;
}

void MultiTransport::WaitForData(int timeoutMs) {
    // With ONE live source, delegate: that source can block on its own socket
    // and the poll interval drops out of the latency budget entirely, which is
    // the whole reason WaitForData exists.
    Transport* only = nullptr;
    int live = 0;
    for (const auto& s : sources_) {
        if (!s.transport->Connected()) continue;
        ++live;
        only = s.transport;
    }
    if (live == 1 && only != nullptr) {
        only->WaitForData(timeoutMs);
        return;
    }

    // With several, a plain sleep is used rather than a combined select().
    //
    // Waiting on one socket would add up to the full timeout to anything
    // arriving on another, which is worse than not waiting at all. A real
    // multi-socket select is possible but would mean this class knowing about
    // winsock, which is exactly what the Transport interface exists to avoid.
    //
    // The cost is bounded and small: the poll interval defaults to 12 ms, so
    // the worst case a merged stream can add is 12 ms - well inside the budget
    // for an effect landing with the thing that caused it.
    std::this_thread::sleep_for(
        std::chrono::milliseconds(std::max(0, std::min(timeoutMs, 12))));
}

} // namespace psvr2
