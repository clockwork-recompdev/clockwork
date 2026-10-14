#pragma once
// The few socket calls the air and the relay need, the same on Windows and elsewhere.

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include <cstring>
#include <string>

namespace net {

#if defined(_WIN32)
using socket_t = SOCKET;
inline constexpr socket_t NO_SOCKET = INVALID_SOCKET;
inline void close_socket(socket_t s) { closesocket(s); }
#else
using socket_t = int;
inline constexpr socket_t NO_SOCKET = -1;
inline void close_socket(socket_t s) { close(s); }
#endif

// once per process, before the first socket (Windows needs it)
inline bool sockets_start() {
#if defined(_WIN32)
    static bool ok = [] {
        WSADATA wsa;
        return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    }();
    return ok;
#else
    return true;
#endif
}

// recv calls on `s` give up after `ms` (returning an error)
inline void set_timeout(socket_t s, int ms) {
#if defined(_WIN32)
    DWORD tv = DWORD(ms);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&tv), sizeof tv);
#else
    timeval tv{ms / 1000, (ms % 1000) * 1000};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#endif
}

inline int send_to(socket_t s, const void *data, size_t size, const sockaddr_in &to) {
    return int(sendto(s, static_cast<const char *>(data), int(size), 0, reinterpret_cast<const sockaddr *>(&to), sizeof to));
}

inline int recv_from(socket_t s, void *buf, size_t size, sockaddr_in &from) {
    socklen_t len = sizeof from;
    return int(recvfrom(s, static_cast<char *>(buf), int(size), 0, reinterpret_cast<sockaddr *>(&from), &len));
}

inline bool same_addr(const sockaddr_in &a, const sockaddr_in &b) {
    return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}

// "host" or "host:port" (IPv4 or a name) -> an address; false if it can't be found
inline bool resolve(const std::string &text, uint16_t default_port, sockaddr_in &out) {
    std::string host = text, port = std::to_string(default_port);
    if (auto colon = host.rfind(':'); colon != std::string::npos) port = host.substr(colon + 1), host.resize(colon);
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET, hints.ai_socktype = SOCK_DGRAM;
    if (host.empty() || getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || !res) return false;
    std::memcpy(&out, res->ai_addr, sizeof out);
    freeaddrinfo(res);
    return true;
}

inline std::string addr_text(const sockaddr_in &a) {
    char ip[INET_ADDRSTRLEN] = "?";
    inet_ntop(AF_INET, &a.sin_addr, ip, sizeof ip);
    return std::string(ip) + ":" + std::to_string(ntohs(a.sin_port));
}

} // namespace net
