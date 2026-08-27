// Getting semantic event lines out of a running game.
//
// Every integration works the same way: the game side prints or sends one
// tagged line per semantic event, and the middleware reads those lines. What
// differs is the pipe, so each transport is a small implementation of the
// interface below and the main loop does not care which one it got.
//
// Latency is not a detail here. Every tactile decision in this project assumes
// the effect lands with the thing that caused it; a gunshot 80 ms late is not a
// gunshot, and no amount of waveform tuning fixes that. That is why WaitForData
// exists rather than a blind sleep, and why a transport that can genuinely
// block on its input is always preferred to one that cannot.
//
//   Half-Life: Alyx   games/alyx/netconsole.h   Source 2 network console
//                     LogTail                   console.log (-condebug)
//   Half-Life 2 VR    UdpListener               the server plugin's socket

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
    // Only a bidirectional transport can do this; the others ignore it.
    virtual bool SendCommand(const std::string& command) { (void)command; return false; }
    virtual const char* name() const = 0;

    // Blocks until data is available, or the timeout expires.
    //
    // On a socket this is a real select(), so an event is picked up the instant
    // it arrives rather than on the next poll tick. That removes the poll
    // interval from the latency budget entirely. The log tailer has nothing to
    // wait on, so it just sleeps - which is one more reason to prefer a socket.
    virtual void WaitForData(int timeoutMs);
};

// console.log tailer. Works for any Source game launched with -condebug.
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

// Loopback UDP receiver.
//
// The Half-Life 2 VR game side is a Source server plugin running inside the
// game process, so unlike a script it can open a socket and hand us events
// directly. That is worth doing: it skips the console entirely, which removes
// both the file-buffering delay of console.log and any dependency on how the
// engine happens to flush its console.
//
// Datagrams, not a stream, and deliberately:
//   * the plugin never blocks on a slow or absent reader, so a middleware that
//     is not running cannot stall the game's frame loop;
//   * there is no connection to establish, so either side can start first and
//     restart freely - which matters because the game will be relaunched far
//     more often than this process is;
//   * one datagram is one event, so a partially-written line is impossible.
//
// Loss is possible in principle and irrelevant in practice on loopback. A lost
// event is a missed haptic, not a corrupted state: every persistent state in
// this project is either refreshed by a heartbeat or expires on its own.
class UdpListener : public Transport {
public:
    explicit UdpListener(uint16_t port) : port_(port) {}
    ~UdpListener() override;

    bool Connect() override;
    bool Connected() const override { return socket_ != ~0ull; }
    std::vector<std::string> Poll() override;
    const char* name() const override { return "plugin (udp)"; }
    void WaitForData(int timeoutMs) override;

    // Datagrams seen since startup, for the diagnostics line.
    uint64_t received() const { return received_; }

private:
    void Close();

    uint16_t port_ = 29001;
    uint64_t socket_ = ~0ull; // SOCKET, kept opaque to avoid winsock here
    bool wsaStarted_ = false;
    uint64_t received_ = 0;
};

} // namespace psvr2
