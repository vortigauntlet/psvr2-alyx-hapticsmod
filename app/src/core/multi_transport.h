// Running several event sources at once.
//
// Half-Life 2 VR has two routes in, and neither supersedes the other:
//
//   the game's own bHaptics stream   knows WHAT happened, by the developers'
//                                    own name for it, and needs nothing built
//   the server plugin                knows the PHYSICS - mass, surface
//                                    material, spin, rounds remaining - which
//                                    a bHaptics submit does not carry
//
// Picking one means throwing away what the other knows. The material system
// needs the plugin's surface data; the damage-type split needs the bHaptics
// stream's naming. So both run, and their lines merge into one stream.
//
// Duplicate suppression is NOT done here, deliberately. If both routes report
// the same shot, that is a real configuration to be seen and fixed rather than
// silently papered over - and the adapter is the only layer that knows whether
// two lines mean the same event. What this class does is strictly mechanical:
// poll everything, concatenate, and report which sources are live.

#pragma once

#include "core/transport.h"

#include <string>
#include <vector>

namespace psvr2 {

class MultiTransport : public Transport {
public:
    // Sources are polled in the order added, so put the lower-latency one
    // first: within a single poll its lines are handled first.
    void Add(Transport* t, const char* label);

    bool Connect() override;
    bool Connected() const override;
    std::vector<std::string> Poll() override;
    const char* name() const override { return name_.c_str(); }
    void WaitForData(int timeoutMs) override;

    // How many sources are currently live.
    int liveCount() const;

private:
    void RefreshName();

    struct Source {
        Transport* transport = nullptr;
        std::string label;
        bool wasConnected = false;
    };
    std::vector<Source> sources_;
    std::string name_ = "none";
    // Set when the live set changes, so the caller can re-announce.
    bool changed_ = false;

public:
    // True once since the last call, when a source connected or dropped.
    bool TakeChanged() {
        const bool was = changed_;
        changed_ = false;
        return was;
    }
};

} // namespace psvr2
