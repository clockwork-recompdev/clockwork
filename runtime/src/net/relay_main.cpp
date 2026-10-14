// clockwork-relay: the server players reach to play local wireless together over the internet.
// Each player's Clockwork names this server and a room in the launcher's Rooms tab; everyone in a
// room plays as if their consoles were side by side. It keeps nothing on disk.
//
//   clockwork-relay [--port N]      (UDP; 47830 by default)

#include "net/relay.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace {

void log_line(const std::string &line) {
    char when[32];
    std::time_t now = std::time(nullptr);
    std::strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    std::printf("%s %s\n", when, line.c_str());
    std::fflush(stdout);
}

} // namespace

int main(int argc, char **argv) {
    int port = net::RELAY_PORT;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = std::atoi(argv[++i]);
        } else {
            std::fprintf(stderr, "usage: %s [--port N]   (UDP, %u by default)\n", argv[0], net::RELAY_PORT);
            return 2;
        }
    }
    if (port <= 0 || port > 65535) {
        std::fprintf(stderr, "clockwork-relay: bad port %d\n", port);
        return 2;
    }
    if (!net::sockets_start()) {
        std::fprintf(stderr, "clockwork-relay: sockets don't work here\n");
        return 1;
    }
    net::socket_t s = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(uint16_t(port));
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (s == net::NO_SOCKET || bind(s, reinterpret_cast<sockaddr *>(&addr), sizeof addr) != 0) {
        std::fprintf(stderr, "clockwork-relay: can't use UDP port %d (is another relay running?)\n", port);
        return 1;
    }
    net::set_timeout(s, 1000);
    log_line("clockwork-relay on UDP port " + std::to_string(port));
    net::relay_serve(s, log_line);
}
