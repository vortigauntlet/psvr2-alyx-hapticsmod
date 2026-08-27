// Listening to a game's own bHaptics event stream.
//
// ===========================================================================
// WHY THIS IS THE BEST INTEGRATION POINT WE HAVE FOR HALF-LIFE 2 VR
// ===========================================================================
//
// Half-Life 2: VR Mod ships with bHaptics support BUILT IN - the Steam page
// lists it, and bHaptics' own setup guide for the game is "install Half-Life 2,
// install the VR mod, press play". There is no mod to install, because the
// Source VR Mod Team implemented it themselves.
//
// Both of bHaptics' SDKs - the C# one and the native C++ `haptic-library`,
// whose third-party dependency list is nlohmann/json and **easywsclient** -
// are WEBSOCKET CLIENTS. They connect out to the bHaptics Player at:
//
//     ws://127.0.0.1:15881/v2/feedbacks?app_id=<id>&app_name=<name>
//
// and send JSON:
//
//     { "Register": [ { "Key": "<name>", "Project": {...} }, ... ],
//       "Submit":   [ { "type": "key", "key": "<name>", ... }, ... ] }
//
// `Register` announces every named haptic pattern the game knows about, on
// connect. `Submit` fires one by name.
//
// So a game with built-in bHaptics support is already broadcasting its own
// semantic event vocabulary to a local port, classified by the people who
// wrote the game. If the bHaptics Player is not running - and it will not be,
// on a machine using PSVR2 controllers - that port is free, and we can simply
// be the thing that answers.
//
// What that buys over reading game state:
//
//   * No plugin to build, no SDK, no toolchain, nothing to install.
//   * No inference. "The game says the shotgun fired" instead of "the magazine
//     count went down, so probably a shot".
//   * No injection, no offsets, no hooks. We do not touch the game at all; it
//     connects to us.
//
// What it does NOT buy, and the reason the server plugin still has a purpose:
// a bHaptics submit carries an event NAME and vest motor data. It does not
// carry the mass of what you caught, the surface property of what you hit, or
// how fast it was spinning. The material system needs those, and only the
// plugin can read them. The two are complementary rather than alternatives.
//
// ===========================================================================
// WHAT IS VERIFIED AND WHAT IS NOT
// ===========================================================================
//
// VERIFIED (from published sources, without the game):
//   * the endpoint, port and message shape, from bHapticsLib
//     (HerpDerpinstine/bHapticsLib, MIT) and bhaptics/haptic-library
//   * that Half-Life 2 VR advertises built-in bHaptics support
//
// NOT VERIFIED (needs the game, once):
//   * that Half-Life 2 VR really uses this transport rather than an older
//     in-process API
//   * the actual event key NAMES it registers
//
// The second is why `--bhaptics-scan` exists: it listens, prints every key the
// game registers and fires, and asks for nothing else. One play session turns
// the key list from a guess into a fact. Nothing in this project maps a key it
// has not seen, and every unmapped key is reported rather than dropped.

#pragma once

#include "core/transport.h"

#include <cstdint>
#include <string>
#include <vector>

namespace psvr2 {

// The port and path the bHaptics Player itself listens on.
constexpr uint16_t kBhapticsPort = 15881;

// A minimal WebSocket server that accepts one local client and turns its
// bHaptics traffic into event lines.
//
// Deliberately minimal: it speaks exactly enough of RFC 6455 to accept a
// connection from easywsclient and read masked text frames from it. It is not
// a general WebSocket implementation and should not be used as one - it does
// not do TLS, extensions, or fragmentation across more than the simple case,
// and it never sends anything but the handshake and a pong.
//
// Emits lines in this project's usual tagged form, so the rest of the
// middleware needs no special case:
//
//     BH_APP:<app_name>          once, on connect
//     BH_REGISTER:<key>          one per pattern the game announces
//     BH_SUBMIT:<key>            one per pattern the game fires
//
class BhapticsListener : public Transport {
public:
    explicit BhapticsListener(uint16_t port = kBhapticsPort) : port_(port) {}
    ~BhapticsListener() override;

    bool Connect() override;
    bool Connected() const override { return client_ != ~0ull; }
    std::vector<std::string> Poll() override;
    const char* name() const override { return "bhaptics (ws)"; }
    void WaitForData(int timeoutMs) override;

    // True when the listening socket is open, whether or not a game has
    // connected to it yet. Distinguished from Connected() because "nothing has
    // connected" and "we could not take the port" need different advice: the
    // second almost always means the bHaptics Player is running.
    bool listening() const { return listener_ != ~0ull; }
    bool portInUse() const { return portInUse_; }

    // Every distinct key seen this session, in the order first seen. This is
    // the discovery output - see --bhaptics-scan.
    const std::vector<std::string>& registeredKeys() const { return registered_; }
    const std::string& appName() const { return appName_; }
    uint64_t submits() const { return submits_; }

private:
    void CloseClient();
    void CloseAll();
    bool AcceptPending();
    bool Handshake(const std::string& request);
    // Pulls complete frames out of rx_ and appends any payloads to `out`.
    void DrainFrames(std::vector<std::string>& out);
    void ParsePacket(const std::string& json, std::vector<std::string>& out);

    uint16_t port_ = kBhapticsPort;
    uint64_t listener_ = ~0ull;
    uint64_t client_ = ~0ull;
    bool wsaStarted_ = false;
    bool portInUse_ = false;
    bool handshaken_ = false;
    std::string rx_;
    std::string appName_;
    std::vector<std::string> registered_;
    uint64_t submits_ = 0;
};

// Exposed for testing: the RFC 6455 handshake accept value, which is
// base64(sha1(clientKey + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11")).
//
// Public because it is the one piece of this file with a published test vector,
// and a handshake that is subtly wrong fails as "the game never connects",
// which is indistinguishable from "the game does not use this transport". That
// ambiguity would be expensive, so it is tested instead.
std::string WebSocketAcceptKey(const std::string& clientKey);

} // namespace psvr2
