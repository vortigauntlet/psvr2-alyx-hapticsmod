#include "core/bhaptics_listener.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <sstream>
#include <thread>

#pragma comment(lib, "ws2_32.lib")

namespace psvr2 {
namespace {

constexpr uint64_t kInvalid = ~0ull;

// --- SHA-1 -----------------------------------------------------------------
//
// Needed only for the WebSocket handshake, so it is the whole of the crypto in
// this project. Written out rather than pulled in as a dependency: it is fifty
// lines, it has published test vectors, and adding a library to a build that
// currently has none would cost more than it saves.
//
// This is a handshake, not a security boundary. SHA-1's collision weakness is
// irrelevant here - RFC 6455 uses it purely to prove the peer read the request.
struct Sha1 {
    uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    uint8_t block[64]{};
    size_t blockLen = 0;
    uint64_t totalBits = 0;

    static uint32_t Rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

    void ProcessBlock() {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) |
                   (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
                   (static_cast<uint32_t>(block[i * 4 + 3]));
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = Rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | ((~b) & d);          k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d;                     k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d);   k = 0x8F1BBCDCu; }
            else             { f = b ^ c ^ d;                     k = 0xCA62C1D6u; }
            const uint32_t tmp = Rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = Rol(b, 30); b = a; a = tmp;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }

    void Update(const uint8_t* data, size_t len) {
        totalBits += static_cast<uint64_t>(len) * 8;
        while (len > 0) {
            const size_t take = std::min(len, sizeof(block) - blockLen);
            std::memcpy(block + blockLen, data, take);
            blockLen += take;
            data += take;
            len -= take;
            if (blockLen == sizeof(block)) {
                ProcessBlock();
                blockLen = 0;
            }
        }
    }

    void Final(uint8_t out[20]) {
        const uint64_t bits = totalBits;
        const uint8_t one = 0x80;
        Update(&one, 1);
        const uint8_t zero = 0x00;
        while (blockLen != 56) Update(&zero, 1);
        uint8_t lenBytes[8];
        for (int i = 0; i < 8; ++i) {
            lenBytes[i] = static_cast<uint8_t>((bits >> (56 - i * 8)) & 0xFF);
        }
        // Update() would add these to the bit count; write them directly.
        std::memcpy(block + blockLen, lenBytes, 8);
        ProcessBlock();
        for (int i = 0; i < 5; ++i) {
            out[i * 4]     = static_cast<uint8_t>((h[i] >> 24) & 0xFF);
            out[i * 4 + 1] = static_cast<uint8_t>((h[i] >> 16) & 0xFF);
            out[i * 4 + 2] = static_cast<uint8_t>((h[i] >> 8) & 0xFF);
            out[i * 4 + 3] = static_cast<uint8_t>(h[i] & 0xFF);
        }
    }
};

std::string Base64(const uint8_t* data, size_t len) {
    static const char* kTable =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        const uint32_t a = data[i];
        const uint32_t b = (i + 1 < len) ? data[i + 1] : 0;
        const uint32_t c = (i + 2 < len) ? data[i + 2] : 0;
        const uint32_t triple = (a << 16) | (b << 8) | c;
        out += kTable[(triple >> 18) & 0x3F];
        out += kTable[(triple >> 12) & 0x3F];
        out += (i + 1 < len) ? kTable[(triple >> 6) & 0x3F] : '=';
        out += (i + 2 < len) ? kTable[triple & 0x3F] : '=';
    }
    return out;
}

std::string Trim(const std::string& s) {
    const auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const auto b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Extracts the value of a quoted string field from JSON.
//
// A targeted scan rather than a JSON parser, and deliberately so: the only
// things read out of a bHaptics packet are two string fields, and a parser
// would be several hundred lines of surface for no gain. It handles escaped
// quotes, which is the one case that would otherwise truncate a key.
bool FindStringField(const std::string& json, size_t from, const char* field,
                     std::string& out, size_t& valueEnd) {
    const std::string needle = std::string("\"") + field + "\"";
    const auto k = json.find(needle, from);
    if (k == std::string::npos) return false;
    auto colon = json.find(':', k + needle.size());
    if (colon == std::string::npos) return false;
    auto q1 = json.find('"', colon + 1);
    if (q1 == std::string::npos) return false;
    std::string value;
    size_t i = q1 + 1;
    for (; i < json.size(); ++i) {
        if (json[i] == '\\' && i + 1 < json.size()) {
            value += json[i + 1];
            ++i;
            continue;
        }
        if (json[i] == '"') break;
        value += json[i];
    }
    out = value;
    valueEnd = i;
    return true;
}

} // namespace

std::string WebSocketAcceptKey(const std::string& clientKey) {
    static const char* kGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    const std::string combined = clientKey + kGuid;
    Sha1 sha;
    sha.Update(reinterpret_cast<const uint8_t*>(combined.data()), combined.size());
    uint8_t digest[20];
    sha.Final(digest);
    return Base64(digest, sizeof(digest));
}

BhapticsListener::~BhapticsListener() { CloseAll(); }

void BhapticsListener::CloseClient() {
    if (client_ != kInvalid) {
        closesocket(static_cast<SOCKET>(client_));
        client_ = kInvalid;
    }
    handshaken_ = false;
    rx_.clear();
}

void BhapticsListener::CloseAll() {
    CloseClient();
    if (listener_ != kInvalid) {
        closesocket(static_cast<SOCKET>(listener_));
        listener_ = kInvalid;
    }
    if (wsaStarted_) {
        WSACleanup();
        wsaStarted_ = false;
    }
}

bool BhapticsListener::Connect() {
    if (Connected()) return true;

    if (listener_ == kInvalid) {
        if (!wsaStarted_) {
            WSADATA wsa{};
            if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
            wsaStarted_ = true;
        }
        SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) return false;

        // Loopback only. This port exists to hear one process on this machine.
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port_);
        InetPtonA(AF_INET, "127.0.0.1", &addr.sin_addr);

        if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
            // Almost certainly the bHaptics Player, which owns this port when
            // it is running. Recorded rather than merely failed, because the
            // advice for that case is completely different from "the game has
            // not started yet".
            portInUse_ = (WSAGetLastError() == WSAEADDRINUSE);
            closesocket(s);
            return false;
        }
        if (::listen(s, 4) == SOCKET_ERROR) {
            closesocket(s);
            return false;
        }
        u_long nonBlocking = 1;
        ioctlsocket(s, FIONBIO, &nonBlocking);
        listener_ = static_cast<uint64_t>(s);
        portInUse_ = false;
    }

    return AcceptPending();
}

bool BhapticsListener::AcceptPending() {
    if (listener_ == kInvalid) return false;
    SOCKET c = ::accept(static_cast<SOCKET>(listener_), nullptr, nullptr);
    if (c == INVALID_SOCKET) return false;
    // A second connection replaces the first. Only one game can be running,
    // and a stale half-open socket must not lock out the real one.
    CloseClient();
    u_long nonBlocking = 1;
    ioctlsocket(c, FIONBIO, &nonBlocking);
    client_ = static_cast<uint64_t>(c);
    return true;
}

bool BhapticsListener::Handshake(const std::string& request) {
    // Pull Sec-WebSocket-Key and, for the diagnostics line, app_name from the
    // query string the bHaptics SDKs append.
    std::string key;
    std::istringstream lines(request);
    std::string line;
    while (std::getline(lines, line)) {
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string name = line.substr(0, colon);
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char ch) { return static_cast<char>(::tolower(ch)); });
        if (name == "sec-websocket-key") key = Trim(line.substr(colon + 1));
    }
    if (key.empty()) return false;

    const auto app = request.find("app_name=");
    if (app != std::string::npos) {
        const auto end = request.find_first_of("& \r\n", app + 9);
        appName_ = request.substr(app + 9, end == std::string::npos
                                               ? std::string::npos
                                               : end - (app + 9));
    }

    const std::string response =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " + WebSocketAcceptKey(key) + "\r\n\r\n";
    ::send(static_cast<SOCKET>(client_), response.data(),
           static_cast<int>(response.size()), 0);
    handshaken_ = true;
    return true;
}

void BhapticsListener::DrainFrames(std::vector<std::string>& out) {
    for (;;) {
        if (rx_.size() < 2) return;
        const uint8_t b0 = static_cast<uint8_t>(rx_[0]);
        const uint8_t b1 = static_cast<uint8_t>(rx_[1]);
        const uint8_t opcode = b0 & 0x0F;
        const bool masked = (b1 & 0x80) != 0;
        uint64_t len = b1 & 0x7F;
        size_t pos = 2;

        if (len == 126) {
            if (rx_.size() < pos + 2) return;
            len = (static_cast<uint8_t>(rx_[pos]) << 8) |
                  static_cast<uint8_t>(rx_[pos + 1]);
            pos += 2;
        } else if (len == 127) {
            if (rx_.size() < pos + 8) return;
            len = 0;
            for (int i = 0; i < 8; ++i) {
                len = (len << 8) | static_cast<uint8_t>(rx_[pos + i]);
            }
            pos += 8;
        }

        // A frame far larger than any plausible bHaptics packet means the
        // stream has desynchronised. Dropping the connection is the only safe
        // response; it will be re-accepted.
        if (len > 4u * 1024u * 1024u) {
            CloseClient();
            return;
        }

        uint8_t mask[4]{};
        if (masked) {
            if (rx_.size() < pos + 4) return;
            std::memcpy(mask, rx_.data() + pos, 4);
            pos += 4;
        }
        if (rx_.size() < pos + len) return;

        std::string payload = rx_.substr(pos, static_cast<size_t>(len));
        if (masked) {
            for (size_t i = 0; i < payload.size(); ++i) {
                payload[i] = static_cast<char>(payload[i] ^ mask[i % 4]);
            }
        }
        rx_.erase(0, pos + static_cast<size_t>(len));

        if (opcode == 0x8) {          // close
            CloseClient();
            return;
        }
        if (opcode == 0x9) {          // ping -> pong
            const char pong[2] = {static_cast<char>(0x8A), 0x00};
            ::send(static_cast<SOCKET>(client_), pong, 2, 0);
            continue;
        }
        if (opcode == 0x1 || opcode == 0x0) {  // text, or a continuation
            ParsePacket(payload, out);
        }
        // Binary frames are ignored: bHaptics sends JSON text.
    }
}

void BhapticsListener::ParsePacket(const std::string& json,
                                   std::vector<std::string>& out) {
    // "Register": [ { "Key": "<name>", ... }, ... ]
    //
    // Announced on connect, and this is the whole point of the exercise: it is
    // the game's own list of every haptic event it knows how to produce.
    const auto reg = json.find("\"Register\"");
    if (reg != std::string::npos) {
        size_t at = reg;
        for (;;) {
            std::string key;
            size_t end = 0;
            if (!FindStringField(json, at, "Key", key, end)) break;
            at = end + 1;
            if (key.empty()) continue;
            if (std::find(registered_.begin(), registered_.end(), key) ==
                registered_.end()) {
                registered_.push_back(key);
                out.push_back("[PSVR2H] BH_REGISTER:" + key);
            }
            // Stop at the end of the Register array rather than running on into
            // Submit, whose entries use a lowercase "key".
            const auto submit = json.find("\"Submit\"");
            if (submit != std::string::npos && at > submit) break;
        }
    }

    // "Submit": [ { "type": "key", "key": "<name>", ... }, ... ]
    const auto sub = json.find("\"Submit\"");
    if (sub != std::string::npos) {
        size_t at = sub;
        for (;;) {
            std::string key;
            size_t end = 0;
            if (!FindStringField(json, at, "key", key, end)) break;
            at = end + 1;
            if (key.empty()) continue;
            ++submits_;
            out.push_back("[PSVR2H] BH_SUBMIT:" + key);
        }
    }
}

std::vector<std::string> BhapticsListener::Poll() {
    std::vector<std::string> out;
    if (listener_ == kInvalid) return out;

    if (client_ == kInvalid) {
        if (!AcceptPending()) return out;
    }

    char buf[8192];
    for (;;) {
        const int n = ::recv(static_cast<SOCKET>(client_), buf, sizeof(buf), 0);
        if (n == 0) {
            CloseClient();
            break;
        }
        if (n == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAEWOULDBLOCK) break;
            CloseClient();
            break;
        }
        rx_.append(buf, static_cast<size_t>(n));

        if (!handshaken_) {
            const auto endOfHeaders = rx_.find("\r\n\r\n");
            if (endOfHeaders == std::string::npos) {
                // A header block this large is not a WebSocket handshake.
                if (rx_.size() > 16384) CloseClient();
                continue;
            }
            const std::string request = rx_.substr(0, endOfHeaders);
            rx_.erase(0, endOfHeaders + 4);
            if (!Handshake(request)) {
                CloseClient();
                break;
            }
            out.push_back("[PSVR2H] BH_APP:" +
                          (appName_.empty() ? std::string("unknown") : appName_));
        }
        if (handshaken_) DrainFrames(out);
        if (client_ == kInvalid) break;
    }
    return out;
}

void BhapticsListener::WaitForData(int timeoutMs) {
    if (listener_ == kInvalid) {
        std::this_thread::sleep_for(std::chrono::milliseconds(std::max(0, timeoutMs)));
        return;
    }
    fd_set set;
    FD_ZERO(&set);
    FD_SET(static_cast<SOCKET>(listener_), &set);
    if (client_ != kInvalid) FD_SET(static_cast<SOCKET>(client_), &set);
    timeval tv{};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    ::select(0, &set, nullptr, nullptr, &tv);
}

} // namespace psvr2
