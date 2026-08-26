#include "transport.h"

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

// --- network console -------------------------------------------------------

NetConsole::~NetConsole() { Close(); }

void NetConsole::Close() {
    if (socket_ != kInvalid) {
        closesocket(static_cast<SOCKET>(socket_));
        socket_ = kInvalid;
    }
    if (wsaStarted_) {
        WSACleanup();
        wsaStarted_ = false;
    }
}

bool NetConsole::Connect() {
    if (Connected()) return true;

    if (!wsaStarted_) {
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
        wsaStarted_ = true;
    }

    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);
    InetPtonA(AF_INET, "127.0.0.1", &addr.sin_addr);

    // The game may not be running yet, so a failed connect is routine and must
    // not block the main loop.
    u_long nonBlocking = 1;
    ioctlsocket(s, FIONBIO, &nonBlocking);

    const int rc = ::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (rc == SOCKET_ERROR) {
        if (WSAGetLastError() != WSAEWOULDBLOCK) { closesocket(s); return false; }
        fd_set writable;
        FD_ZERO(&writable);
        FD_SET(s, &writable);
        timeval tv{0, 200 * 1000}; // 200 ms
        if (select(0, nullptr, &writable, nullptr, &tv) <= 0) { closesocket(s); return false; }
    }

    socket_ = static_cast<uint64_t>(s);
    pending_.clear();
    return true;
}

std::vector<std::string> NetConsole::Poll() {
    std::vector<std::string> out;
    if (!Connected()) return out;

    const SOCKET s = static_cast<SOCKET>(socket_);
    char buf[16 * 1024];

    for (;;) {
        const int n = recv(s, buf, sizeof(buf), 0);
        if (n > 0) {
            pending_.append(buf, buf + n);
            if (pending_.size() > 4u * 1024 * 1024) {
                // Runaway console spam: keep only the tail so we cannot grow
                // without bound while still finishing the current record.
                pending_.erase(0, pending_.size() - 1024 * 1024);
            }
            continue;
        }
        if (n == 0) { Close(); return out; }          // game exited
        if (WSAGetLastError() == WSAEWOULDBLOCK) break; // drained
        Close();
        return out;
    }

    // Each printed line lives inside a "PRNT" record: the text starts after the
    // last NUL before the newline, and ends at the newline.
    size_t search = 0;
    size_t consumed = 0;
    for (;;) {
        const size_t rec = pending_.find("PRNT", search);
        if (rec == std::string::npos) break;
        const size_t nl = pending_.find('\n', rec);
        if (nl == std::string::npos) break; // record still arriving

        const size_t nul = pending_.rfind('\0', nl);
        const size_t begin = (nul == std::string::npos || nul < rec) ? rec + 4 : nul + 1;
        if (begin < nl) {
            std::string line = pending_.substr(begin, nl - begin);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) out.push_back(std::move(line));
        }
        search = nl + 1;
        consumed = search;
    }
    if (consumed > 0) pending_.erase(0, consumed);
    return out;
}

void NetConsole::WaitForData(int timeoutMs) {
    if (!Connected()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(std::max(0, timeoutMs)));
        return;
    }
    const SOCKET s = static_cast<SOCKET>(socket_);
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(s, &readable);
    timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    // Returns as soon as a byte arrives. This is the whole reason the network
    // console is the preferred transport: the poll interval stops being part
    // of the latency budget.
    select(0, &readable, nullptr, nullptr, &tv);
}

bool NetConsole::SendCommand(const std::string& command) {
    if (!Connected()) return false;

    std::string lower = command;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    std::string packet;
    const unsigned char header[] = {0x43, 0x4D, 0x4E, 0x44, 0x00, 0xD3, 0x00, 0x00, 0x00};
    packet.append(reinterpret_cast<const char*>(header), sizeof(header));
    packet.push_back(static_cast<char>(static_cast<unsigned char>(13 + lower.size())));
    packet.push_back('\0');
    packet.push_back('\0');
    packet.append(lower);
    packet.push_back('\0');

    const int n = send(static_cast<SOCKET>(socket_), packet.data(),
                       static_cast<int>(packet.size()), 0);
    return n == static_cast<int>(packet.size());
}

// --- log tail ---------------------------------------------------------------

bool LogTail::Connect() {
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

} // namespace psvr2
