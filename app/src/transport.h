// Getting console output out of Half-Life: Alyx.
//
// Two transports, tried in order.
//
// 1. Source 2 network console (-netconport). A TCP listener inside the game
//    that streams console output live and accepts console commands back.
//    Technique taken from Solla's HalfLifeAlyxEventDetector, which is the only
//    Alyx haptics project that uses it:
//    https://github.com/Solla/HalfLifeAlyxEventDetector
//
//    Advantages over tailing the log: markedly lower latency (no file buffering
//    or flush delay), no unbounded console.log growth, and - uniquely - we can
//    send commands *into* the game, which makes live script iteration possible.
//
//    Wire format, as observed in that project:
//      receive  packets contain "PRNT" records; the printed line runs from the
//               byte after the preceding NUL up to the newline
//      send     "CMND" + 00 D3 00 00 00, then a length byte (13 + strlen),
//               00 00, the lowercased UTF-8 command, then a terminating 00
//
// 2. console.log tailing (-condebug). Always available, slightly laggier.
//    Kept as the fallback so an existing setup keeps working untouched.

#pragma once

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace psvr2 {

class Transport {
public:
    virtual ~Transport() = default;
    virtual bool Connect() = 0;
    virtual bool Connected() const = 0;
    // Returns whole console lines received since the last call.
    virtual std::vector<std::string> Poll() = 0;
    // Only the network console can do this; the log tailer ignores it.
    virtual bool SendCommand(const std::string& command) { (void)command; return false; }
    virtual const char* name() const = 0;

    // Blocks until data is available, or the timeout expires.
    //
    // On the network console this is a real select() on the socket, so an event
    // is picked up the instant it arrives rather than on the next poll tick.
    // That removes the poll interval from the latency budget entirely. The log
    // tailer has nothing to wait on, so it just sleeps - which is one more
    // reason to prefer -netconport.
    virtual void WaitForData(int timeoutMs);
};

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

// console.log tailer.
class LogTail : public Transport {
public:
    explicit LogTail(std::string path) : path_(std::move(path)) {}

    bool Connect() override;
    bool Connected() const override { return file_.is_open(); }
    std::vector<std::string> Poll() override;
    const char* name() const override { return "console.log"; }

private:
    std::string path_;
    std::ifstream file_;
    std::streampos pos_ = 0;
};

} // namespace psvr2
