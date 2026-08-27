// Source 2 network console client (-netconport).
//
// A TCP listener inside the game that streams console output live and accepts
// console commands back. Technique taken from Solla's HalfLifeAlyxEventDetector,
// which is the only other Alyx haptics project that uses it:
// https://github.com/Solla/HalfLifeAlyxEventDetector
//
// Advantages over tailing the log: markedly lower latency (no file buffering or
// flush delay), no unbounded console.log growth, and - uniquely - we can send
// commands *into* the game, which makes live script iteration possible.
//
// Wire format, as observed in that project:
//   receive  packets contain "PRNT" records; the printed line runs from the
//            byte after the preceding NUL up to the newline
//   send     "CMND" + 00 D3 00 00 00, then a length byte (13 + strlen),
//            00 00, the lowercased UTF-8 command, then a terminating 00
//
// Source 1 has no equivalent, which is why Half-Life 2 VR uses a socket opened
// by its own plugin instead - see core/transport.h.

#pragma once

#include "core/transport.h"

#include <cstdint>
#include <string>
#include <vector>

namespace psvr2 {

// Source 2 network console client.
class NetConsole : public Transport {
public:
    explicit NetConsole(uint16_t port) : port_(port) {}
    ~NetConsole() override;

    bool Connect() override;
    bool Connected() const override { return socket_ != ~0ull; }
    std::vector<std::string> Poll() override;
    bool SendCommand(const std::string& command) override;
    const char* name() const override { return "network console"; }
    void WaitForData(int timeoutMs) override;

private:
    void Close();

    uint16_t port_ = 29000;
    uint64_t socket_ = ~0ull; // SOCKET, kept opaque to avoid winsock in the header
    std::string pending_;
    bool wsaStarted_ = false;
};

} // namespace psvr2
