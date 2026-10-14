#pragma once
// The relay: carries local wireless frames among the players in a room. The same code is the hub
// a game runs on this PC for two windows (net/air.cpp) and the standalone server, clockwork-relay
// (relay_main.cpp), that players anywhere reach over the internet. See docs/PLAN.md, Rooms.
// The server has its own repository, clockwork-relay, with these files (relay.h, relay.cpp,
// socket.h, relay_main.cpp) and the scripts to run it: keep the two copies the same.
//
// Every message is UDP: "CWAR", a kind byte, then its fields (little-endian; strings are a length
// byte and up to MAX_TEXT bytes).
//   HELLO   token u64, name, room, password, program id u64; padded to REQUEST_SIZE. Sent every
//           HELLO_MS: who we are and which room we're in. A member is forgotten FORGET_MS after
//           its last one.
//   WELCOME token u64, status u8, count u8, then each member of the room: name, program id u64.
//           The token proves the sender receives at its address (so nobody can sign someone
//           else's address up for frames): the relay hands it out, and joins whoever says hello
//           with it.
//   FRAME   the frame, handed to everyone else in the sender's room
//   LIST    padded to REQUEST_SIZE
//   ROOMS   count u8, then each room: name, flags u8 (1: password), count u8, then each member:
//           name, program id u64
// The relay never answers with more bytes than it was sent, so nobody can turn it on a third
// party's address to multiply their traffic.

#include "net/socket.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace net {

inline constexpr uint16_t RELAY_PORT = 47830;
inline constexpr uint32_t RELAY_MAGIC = 0x52415743; // "CWAR"
enum RelayKind : uint8_t { HELLO = 0, WELCOME = 1, FRAME = 2, LIST = 3, ROOMS = 4 };
// WELCOME status
enum JoinStatus : uint8_t { JOINED = 0, CHECKING = 1, ROOM_FULL = 2, WRONG_PASSWORD = 3, RELAY_FULL = 4 };
inline constexpr size_t MAX_TEXT = 32, REQUEST_SIZE = 1200, MAX_FRAME = 2000;
inline constexpr size_t ROOM_SIZE = 16; // nwm::UDS networks have up to 16 nodes
inline constexpr int HELLO_MS = 1000, FORGET_MS = 5000;

struct RoomMember {
    std::string name;
    uint64_t program_id = 0;
};
struct RoomInfo {
    std::string name;
    bool password = false;
    std::vector<RoomMember> members;
};

// Writing and reading messages
struct Writer {
    std::vector<uint8_t> b;
    Writer() = default; // just fields, for building a message in pieces
    explicit Writer(RelayKind kind) {
        put(&RELAY_MAGIC, 4);
        b.push_back(kind);
    }
    void put(const void *p, size_t n) { b.insert(b.end(), static_cast<const uint8_t *>(p), static_cast<const uint8_t *>(p) + n); }
    void u8(uint8_t v) { b.push_back(v); }
    void u64(uint64_t v) { put(&v, 8); }
    void text(const std::string &s) {
        size_t n = std::min(s.size(), MAX_TEXT);
        b.push_back(uint8_t(n));
        put(s.data(), n);
    }
    void pad(size_t size) {
        if (b.size() < size) b.resize(size, 0);
    }
};
struct Reader {
    const uint8_t *p, *end;
    bool ok = true;
    Reader(const uint8_t *data, size_t size) : p(data + 5), end(data + size) {
        if (size < 5) p = end, ok = false;
    }
    bool take(void *out, size_t n) {
        if (size_t(end - p) < n) return ok = false;
        std::memcpy(out, p, n);
        p += n;
        return true;
    }
    uint8_t u8() {
        uint8_t v = 0;
        take(&v, 1);
        return v;
    }
    uint64_t u64() {
        uint64_t v = 0;
        take(&v, 8);
        return v;
    }
    std::string text() {
        uint8_t n = u8();
        if (n > MAX_TEXT || size_t(end - p) < n) return ok = false, std::string();
        std::string s(reinterpret_cast<const char *>(p), n);
        p += n;
        return s;
    }
};
// a message's kind, or -1 if it isn't one of ours
inline int relay_kind(const uint8_t *data, size_t size) {
    uint32_t magic;
    if (size < 5) return -1;
    std::memcpy(&magic, data, 4);
    return magic == RELAY_MAGIC ? data[4] : -1;
}

// Runs a relay on bound socket `s` until the process ends (it should wake every second or so: see
// set_timeout). `log` gets a line for each join and leave.
void relay_serve(socket_t s, void (*log)(const std::string &line));

// Asks the relay at `address` ("host[:port]") which rooms have players, waiting up to `ms`;
// false with `error` set if it doesn't answer
bool relay_list(const std::string &address, std::vector<RoomInfo> &rooms, std::string &error, int ms = 1000);

} // namespace net
