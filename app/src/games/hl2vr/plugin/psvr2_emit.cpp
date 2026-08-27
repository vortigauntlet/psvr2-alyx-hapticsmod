#include "psvr2_emit.h"

#include "tier0/dbg.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#endif

#include <cstdio>
#include <cstring>

namespace psvr2h {
namespace {

#ifdef _WIN32
SOCKET g_socket = INVALID_SOCKET;
bool g_wsa = false;
constexpr SOCKET kBad = INVALID_SOCKET;
#else
int g_socket = -1;
constexpr int kBad = -1;
#endif

sockaddr_in g_dest{};
bool g_console = true;

} // namespace

bool EmitInit(unsigned short port) {
#ifdef _WIN32
    if (!g_wsa) {
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
        g_wsa = true;
    }
#endif
    g_socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_socket == kBad) return false;

    // Non-blocking, so a full send buffer drops the datagram instead of
    // stalling the server tick. A dropped haptic is a missed cue; a stalled
    // tick is a stutter in VR, which is far worse than anything this plugin
    // can add.
#ifdef _WIN32
    u_long nb = 1;
    ioctlsocket(g_socket, FIONBIO, &nb);
#else
    fcntl(g_socket, F_SETFL, fcntl(g_socket, F_GETFL, 0) | O_NONBLOCK);
#endif

    g_dest = sockaddr_in{};
    g_dest.sin_family = AF_INET;
    g_dest.sin_port = htons(port);
#ifdef _WIN32
    InetPtonA(AF_INET, "127.0.0.1", &g_dest.sin_addr);
#else
    inet_pton(AF_INET, "127.0.0.1", &g_dest.sin_addr);
#endif
    return true;
}

void EmitShutdown() {
    if (g_socket != kBad) {
#ifdef _WIN32
        closesocket(g_socket);
#else
        close(g_socket);
#endif
        g_socket = kBad;
    }
#ifdef _WIN32
    if (g_wsa) {
        WSACleanup();
        g_wsa = false;
    }
#endif
}

bool EmitHasSocket() { return g_socket != kBad; }

void EmitSetConsole(bool enabled) { g_console = enabled; }

void Emit(const char* event, const char* params) {
    if (event == nullptr) return;

    char line[1024];
    if (params != nullptr && params[0] != '\0') {
        V_snprintf(line, sizeof(line), "[PSVR2H] %s:%s", event, params);
    } else {
        V_snprintf(line, sizeof(line), "[PSVR2H] %s", event);
    }

    if (g_socket != kBad) {
        // One datagram is one line, so there is nothing to frame and no
        // partial line is possible. The result is deliberately ignored: on
        // loopback the only realistic failure is a full buffer, and there is
        // nothing useful to do about it from inside a frame callback.
        ::sendto(g_socket, line, static_cast<int>(strlen(line)), 0,
                 reinterpret_cast<const sockaddr*>(&g_dest), sizeof(g_dest));
    }

    if (g_console) {
        Msg("%s\n", line);
    }
}

void EmitF(const char* event, const char* fmt, ...) {
    char params[768];
    va_list args;
    va_start(args, fmt);
    V_vsnprintf(params, sizeof(params), fmt, args);
    va_end(args);
    Emit(event, params);
}

} // namespace psvr2h
