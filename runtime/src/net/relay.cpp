#include "net/relay.h"

#include <chrono>
#include <map>
#include <random>

namespace net {
namespace {

using Clock = std::chrono::steady_clock;

int ms_since(Clock::time_point t) {
    return int(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count());
}

constexpr size_t MAX_MEMBERS = 1024; // on the whole relay
// Limits, so one client can't spoil a relay open to anyone. Generous for real players: local
// wireless sends ~10 beacons a second plus game data, and a household shares one address
constexpr size_t MAX_PER_ADDRESS = 8;              // players from one IP address
constexpr double FRAMES_PER_S = 300, FRAME_BURST = 600; // frames one player may send
constexpr double ASKS_PER_S = 20, ASK_BURST = 40;  // hellos and room lists from one IP address

// a token bucket: `rate` a second, up to `burst` at once
struct Bucket {
    double tokens = -1;
    Clock::time_point last = Clock::now();
    bool take(double rate, double burst) {
        auto now = Clock::now();
        if (tokens < 0) tokens = burst;
        tokens = std::min(burst, tokens + rate * std::chrono::duration<double>(now - last).count());
        last = now;
        if (tokens < 1) return false;
        tokens -= 1;
        return true;
    }
};

// names and rooms as shown in logs and lists: no control characters
std::string printable(std::string s) {
    std::erase_if(s, [](char c) { return uint8_t(c) < 0x20 || c == 0x7F; });
    return s;
}

// SipHash-2-4 of an address with the relay's secret key: the token that address is given
uint64_t siphash(const uint64_t key[2], const uint8_t *data, size_t size) {
    uint64_t v0 = 0x736f6d6570736575ull ^ key[0], v1 = 0x646f72616e646f6dull ^ key[1];
    uint64_t v2 = 0x6c7967656e657261ull ^ key[0], v3 = 0x7465646279746573ull ^ key[1];
    auto rotl = [](uint64_t x, int b) { return x << b | x >> (64 - b); };
    auto round = [&] {
        v0 += v1, v1 = rotl(v1, 13), v1 ^= v0, v0 = rotl(v0, 32);
        v2 += v3, v3 = rotl(v3, 16), v3 ^= v2;
        v0 += v3, v3 = rotl(v3, 21), v3 ^= v0;
        v2 += v1, v1 = rotl(v1, 17), v1 ^= v2, v2 = rotl(v2, 32);
    };
    auto block = [&](uint64_t m) {
        v3 ^= m;
        round(), round();
        v0 ^= m;
    };
    size_t whole = size / 8 * 8;
    for (size_t i = 0; i < whole; i += 8) {
        uint64_t m;
        std::memcpy(&m, data + i, 8);
        block(m);
    }
    uint64_t last = uint64_t(size) << 56;
    for (size_t i = whole; i < size; i++) last |= uint64_t(data[i]) << (8 * (i - whole));
    block(last);
    v2 ^= 0xff;
    round(), round(), round(), round();
    return v0 ^ v1 ^ v2 ^ v3;
}

struct Member {
    sockaddr_in addr;
    std::string name;
    uint64_t program_id;
    Clock::time_point seen;
    Bucket frames;
};
struct Room {
    std::string password; // set by whoever opened it
    std::vector<Member> members;
};

struct Relay {
    socket_t s;
    void (*log)(const std::string &);
    uint64_t key[2];
    std::map<std::string, Room> rooms;
    size_t member_count = 0;
    std::map<uint32_t, Bucket> asks; // by IP address; forgotten when idle

    size_t from_address(uint32_t ip) const {
        size_t n = 0;
        for (auto &[name, room] : rooms)
            for (const Member &m : room.members) n += m.addr.sin_addr.s_addr == ip;
        return n;
    }

    uint64_t token_for(const sockaddr_in &a) const {
        uint8_t id[6];
        std::memcpy(id, &a.sin_addr.s_addr, 4);
        std::memcpy(id + 4, &a.sin_port, 2);
        return siphash(key, id, sizeof id) | 1; // never 0, which means "no token yet"
    }

    std::string who(const Member &m, const std::string &room) const {
        return "\"" + m.name + "\" (" + addr_text(m.addr) + ") " + (room.empty() ? "the unnamed room" : "room \"" + room + "\"");
    }

    // the room `addr` is in, and its place there
    std::pair<Room *, size_t> find(const sockaddr_in &addr, std::string *room_name = nullptr) {
        for (auto &[name, room] : rooms)
            for (size_t i = 0; i < room.members.size(); i++)
                if (same_addr(room.members[i].addr, addr)) {
                    if (room_name) *room_name = name;
                    return {&room, i};
                }
        return {nullptr, 0};
    }

    // takes member `i` out of `room` (the caller drops the room if that empties it)
    void remove(const std::string &name, Room &room, size_t i, const char *why) {
        if (log) log(who(room.members[i], name) + " " + why);
        room.members.erase(room.members.begin() + std::ptrdiff_t(i));
        member_count--;
    }

    void forget_quiet() {
        for (auto it = rooms.begin(); it != rooms.end();) {
            for (size_t i = it->second.members.size(); i-- > 0;)
                if (ms_since(it->second.members[i].seen) > FORGET_MS) remove(it->first, it->second, i, "left");
            it = it->second.members.empty() ? rooms.erase(it) : std::next(it);
        }
        std::erase_if(asks, [](auto &kv) { return ms_since(kv.second.last) > FORGET_MS; });
    }

    // whoever said hello: joins (or moves to) their room once their token checks out
    JoinStatus hello(const sockaddr_in &from, uint64_t token, const std::string &name, const std::string &room_name,
                     const std::string &password, uint64_t program_id) {
        if (token != token_for(from)) return CHECKING;
        std::string current;
        auto [room, i] = find(from, &current);
        if (room && current != room_name) { // moved to another room
            remove(current, *room, i, "left for another room");
            if (room->members.empty()) rooms.erase(current);
            room = nullptr;
        }
        if (room) {
            Member &m = room->members[i];
            m.seen = Clock::now(), m.name = name, m.program_id = program_id;
            return JOINED;
        }
        auto it = rooms.find(room_name);
        if (it != rooms.end() && it->second.password != password) return WRONG_PASSWORD;
        if (it != rooms.end() && it->second.members.size() >= ROOM_SIZE) return ROOM_FULL;
        if (member_count >= MAX_MEMBERS || from_address(from.sin_addr.s_addr) >= MAX_PER_ADDRESS) return RELAY_FULL;
        Room &r = rooms[room_name];
        if (r.members.empty()) r.password = password;
        r.members.push_back({from, name, program_id, Clock::now(), {}});
        member_count++;
        if (log) log(who(r.members.back(), room_name) + " joined (" + std::to_string(r.members.size()) + " there)");
        return JOINED;
    }

    void reply(const Writer &w, size_t request_size, const sockaddr_in &to) {
        // never more than we were sent (see relay.h)
        send_to(s, w.b.data(), std::min(w.b.size(), request_size), to);
    }

    void handle(const uint8_t *buf, size_t n, const sockaddr_in &from) {
        int kind = relay_kind(buf, n);
        if ((kind == HELLO || kind == LIST) && !asks[from.sin_addr.s_addr].take(ASKS_PER_S, ASK_BURST)) return;
        if (kind == HELLO) {
            Reader r(buf, n);
            uint64_t token = r.u64();
            std::string name = printable(r.text()), room_name = printable(r.text()), password = r.text();
            uint64_t program_id = r.u64();
            if (!r.ok) return;
            JoinStatus status = hello(from, token, name, room_name, password, program_id);
            Writer w(WELCOME);
            w.u64(token_for(from));
            w.u8(status);
            auto [room, i] = find(from);
            std::vector<Member> none;
            const std::vector<Member> &members = status == JOINED && room ? room->members : none;
            w.u8(uint8_t(members.size()));
            for (const Member &m : members) w.text(m.name), w.u64(m.program_id);
            reply(w, n, from);
        } else if (kind == FRAME) {
            if (n > MAX_FRAME + 5) return;
            auto [room, i] = find(from);
            if (!room) return; // not joined (yet)
            Member &sender = room->members[i];
            sender.seen = Clock::now();
            if (!sender.frames.take(FRAMES_PER_S, FRAME_BURST)) return; // sending too much: dropped
            for (const Member &m : room->members)
                if (!same_addr(m.addr, from)) send_to(s, buf, n, m.addr);
        } else if (kind == LIST) {
            Writer w(ROOMS);
            uint8_t count = 0;
            size_t count_at = w.b.size();
            w.u8(0);
            for (auto &[name, room] : rooms) {
                Writer entry;
                entry.text(name);
                entry.u8(room.password.empty() ? 0 : 1);
                entry.u8(uint8_t(room.members.size()));
                for (const Member &m : room.members) entry.text(m.name), entry.u64(m.program_id);
                if (w.b.size() + entry.b.size() > n || count == 255) break; // what fits in the answer
                w.put(entry.b.data(), entry.b.size());
                count++;
            }
            w.b[count_at] = count;
            reply(w, n, from);
        }
    }
};

} // namespace

void relay_serve(socket_t s, void (*log)(const std::string &line)) {
    Relay relay{s, log, {}, {}, 0, {}};
    std::random_device rd;
    relay.key[0] = uint64_t(rd()) << 32 | rd(), relay.key[1] = uint64_t(rd()) << 32 | rd();
    uint8_t buf[2048];
    auto last_sweep = Clock::now();
    for (;;) {
        sockaddr_in from{};
        int n = recv_from(s, buf, sizeof buf, from);
        if (ms_since(last_sweep) >= 500) relay.forget_quiet(), last_sweep = Clock::now();
        if (n > 0) relay.handle(buf, size_t(n), from);
    }
}

bool relay_list(const std::string &address, std::vector<RoomInfo> &rooms, std::string &error, int ms) {
    rooms.clear();
    sockaddr_in to{};
    if (!sockets_start() || !resolve(address, RELAY_PORT, to)) {
        error = "Can't find " + address + ".";
        return false;
    }
    socket_t s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == NO_SOCKET) {
        error = "Can't open a network socket.";
        return false;
    }
    set_timeout(s, 250);
    Writer w(LIST);
    w.pad(REQUEST_SIZE);
    auto start = Clock::now(), sent = start - std::chrono::seconds(1);
    bool answered = false;
    while (!answered && ms_since(start) < ms) {
        if (ms_since(sent) >= 300) send_to(s, w.b.data(), w.b.size(), to), sent = Clock::now(); // UDP: ask again
        uint8_t buf[2048];
        sockaddr_in from{};
        int n = recv_from(s, buf, sizeof buf, from);
        if (n <= 0 || !same_addr(from, to) || relay_kind(buf, size_t(n)) != ROOMS) continue;
        Reader r(buf, size_t(n));
        for (int count = r.u8(); count > 0 && r.ok; count--) {
            RoomInfo room;
            room.name = r.text();
            room.password = r.u8() & 1;
            for (int members = r.u8(); members > 0 && r.ok; members--) {
                RoomMember m;
                m.name = r.text();
                m.program_id = r.u64();
                room.members.push_back(std::move(m));
            }
            if (r.ok) rooms.push_back(std::move(room));
        }
        answered = true;
    }
    close_socket(s);
    if (!answered) error = "No answer from " + address + ".";
    return answered;
}

} // namespace net
