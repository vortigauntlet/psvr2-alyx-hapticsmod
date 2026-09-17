#include "core/transport.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <chrono>
#include <thread>

#pragma comment(lib, "ws2_32.lib")

namespace psvr2 {
namespace {
constexpr uint64_t kInvalid = ~0ull;
} // namespace

// Default: nothing to wait on, so just yield the interval.
void Transport::WaitForData(int timeoutMs) {
    std::this_thread::sleep_for(std::chrono::milliseconds(std::max(0, timeoutMs)));
}

bool LogTail::Connect() {
    // An empty path means the game folder is unknown. Not an error - the
    // socket routes do not need it - so this simply never connects.
    if (path_.empty()) return false;
    if (file_.is_open()) return true;
    file_.open(path_, std::ios::binary);
    if (!file_) return false;
    // Start at the end so a previous session's log is not replayed.
    file_.seekg(0, std::ios::end);
    pos_ = file_.tellg();
    return true;
}

std::vector<std::string> LogTail::Poll() {
    std::vector<std::string> out;
    if (!file_.is_open()) {
        Connect();
        return out;
    }

    file_.clear();
    file_.seekg(0, std::ios::end);
    const auto end = file_.tellg();
    if (end < pos_) pos_ = 0; // game restarted and truncated the log
    if (end == pos_) return out;

    file_.seekg(pos_);
    std::string line;
    while (std::getline(file_, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out.push_back(std::move(line));
    }
    file_.clear();
    file_.seekg(0, std::ios::end);
    pos_ = file_.tellg();
    return out;
}


// --- loopback UDP ----------------------------------------------------------

UdpListener::~UdpListener() { Close(); }

void UdpListener::Close() {
    if (socket_ != kInvalid) {
        closesocket(static_cast<SOCKET>(socket_));
        socket_ = kInvalid;
    }
    if (wsaStarted_) {
        WSACleanup();
        wsaStarted_ = false;
    }
}

bool UdpListener::Connect() {
    if (Connected()) return true;

    if (!wsaStarted_) {
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
        wsaStarted_ = true;
    }

    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return false;

    // Bound to loopback only. This socket exists to hear one process on this
    // machine; there is no reason for it to be reachable from anywhere else,
    // and binding to INADDR_ANY would open a port on every interface for no
    // benefit at all.
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);
    InetPtonA(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        // Almost always "already in use": a second copy of this program is
        // running. Failing quietly is right - the caller retries, and the other
        // copy is the one holding the port.
        closesocket(s);
        return false;
    }

    u_long nonBlocking = 1;
    ioctlsocket(s, FIONBIO, &nonBlocking);
    socket_ = static_cast<uint64_t>(s);
    return true;
}

std::vector<std::string> UdpListener::Poll() {
    std::vector<std::string> out;
    if (!Connected()) return out;

    // One datagram is one line, so there is no reassembly to do and no partial
    // line to carry between calls - which is the whole reason for datagrams.
    char buf[1024];
    for (;;) {
        const int n = ::recv(static_cast<SOCKET>(socket_), buf, sizeof(buf) - 1, 0);
        if (n == SOCKET_ERROR) {
            const int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) break;
            // WSAECONNRESET on a UDP socket means an earlier send of ours was
            // refused. We never send, so this is not ours to act on; drop it
            // and keep reading rather than tearing the socket down.
            if (err == WSAECONNRESET) continue;
            Close();
            break;
        }
        if (n <= 0) break;
        buf[n] = '\0';
        std::string line(buf, static_cast<size_t>(n));
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r' ||
                                 line.back() == '\0')) {
            line.pop_back();
        }
        if (!line.empty()) {
            ++received_;
            out.push_back(std::move(line));
        }
    }
    return out;
}

void UdpListener::WaitForData(int timeoutMs) {
    if (!Connected()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(std::max(0, timeoutMs)));
        return;
    }
    fd_set set;
    FD_ZERO(&set);
    FD_SET(static_cast<SOCKET>(socket_), &set);
    timeval tv{};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    ::select(0, &set, nullptr, nullptr, &tv);
}

} // namespace psvr2
