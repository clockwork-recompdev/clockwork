#include "net/air.h"

#include "dump.h"
#include "frontend/settings.h"
#include "log.h"
#include "net/relay.h"

#include <chrono>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace net {
namespace {

constexpr int LOST_MS = 3000; // no word from the hub: it's gone
constexpr size_t QUEUE_MAX = 256; // frames waiting for the game; older ones are dropped

using Clock = std::chrono::steady_clock;

int ms_since(Clock::time_point t) {
    return int(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count());
}

void log_line(const std::string &line) { LOG("air (local hub): %s", line.c_str()); }

const char *status_text(int status) {
    switch (status) {
    case JOINED: return "joined";
    case ROOM_FULL: return "the room is full";
    case WRONG_PASSWORD: return "wrong room password";
    case RELAY_FULL: return "the relay is full";
    default: return "joining";
    }
}

struct Air {
    socket_t s = NO_SOCKET;
    sockaddr_in hub{};
    bool local = true;    // the hub is on this PC (no relay chosen)
    bool hosting = false; // this process runs the local hub
    std::string name, room, password;
    uint64_t program_id = 0;
    uint64_t token = 0; // from the relay: proves we receive at our address
    int status = -1;    // the last WELCOME's
    std::vector<RoomMember> members;
    Clock::time_point hello_sent{}, welcomed = Clock::now();
    std::mutex lock; // the frames below, between the air's thread and the game
    std::deque<std::vector<uint8_t>> frames;

    // runs the local hub here if nobody else does
    void try_host_hub() {
        if (hosting) return;
        socket_t h = socket(AF_INET, SOCK_DGRAM, 0);
        if (h == NO_SOCKET) return;
        if (bind(h, reinterpret_cast<sockaddr *>(&hub), sizeof hub) != 0) {
            close_socket(h); // someone else is the hub
            return;
        }
        set_timeout(h, 500); // wake now and then to forget members who left
        hosting = true;
        LOG("air: running the local hub (127.0.0.1:%u)", ntohs(hub.sin_port));
        std::thread(relay_serve, h, log_line).detach();
    }

    void hello() {
        Writer w(HELLO);
        w.u64(token);
        w.text(name), w.text(room), w.text(password);
        w.u64(program_id);
        w.pad(REQUEST_SIZE);
        send_to(s, w.b.data(), w.b.size(), hub);
        hello_sent = Clock::now();
    }

    void welcome(const uint8_t *buf, size_t n) {
        Reader r(buf, n);
        uint64_t t = r.u64();
        int st = r.u8();
        std::vector<RoomMember> list;
        for (int count = r.u8(); count > 0 && r.ok; count--) {
            RoomMember m;
            m.name = r.text();
            m.program_id = r.u64();
            list.push_back(std::move(m));
        }
        if (!r.ok) return;
        welcomed = Clock::now();
        bool new_token = t != token;
        token = t;
        if (st != CHECKING && (st != status || list.size() != members.size())) {
            std::string who;
            for (auto &m : list) who += (who.empty() ? "" : ", ") + m.name;
            LOG("air: %s%s%s%s", status_text(st), room.empty() ? "" : (" room \"" + room + "\"").c_str(),
                who.empty() ? "" : ": ", who.c_str());
        }
        status = st, members = std::move(list);
        if (new_token) hello(); // answer with it now rather than at the next hello
    }

    // the air's thread: says hello now and then, and queues the frames that arrive for the game
    void run() {
        for (;;) {
            if (ms_since(hello_sent) >= HELLO_MS) {
                if (local && ms_since(welcomed) > LOST_MS) try_host_hub(); // the hub's process quit
                hello();
            }
            uint8_t buf[2048];
            sockaddr_in from{};
            int n = recv_from(s, buf, sizeof buf, from);
            if (n <= 0 || !same_addr(from, hub)) continue;
            int kind = relay_kind(buf, size_t(n));
            if (kind == WELCOME) {
                welcome(buf, size_t(n));
            } else if (kind == FRAME) {
                welcomed = Clock::now();
                std::lock_guard lk(lock);
                if (frames.size() >= QUEUE_MAX) frames.pop_front();
                frames.emplace_back(buf + 5, buf + n);
            }
        }
    }

    bool start() {
        if (s != NO_SOCKET) return true;
        if (!sockets_start()) return false;
        const frontend::Settings &st = frontend::settings();
        std::string relay = st.relay;
        if (const char *env = std::getenv("CLOCKWORK_RELAY")) relay = env;
        name = st.player_name.empty() ? "Player" : st.player_name;
        room = st.room, password = st.room_password;
        program_id = dump().program_id();
        hub.sin_family = AF_INET;
        hub.sin_port = htons(RELAY_PORT);
        hub.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (!relay.empty()) {
            if (!resolve(relay, RELAY_PORT, hub)) {
                LOG("air: can't find relay %s", relay.c_str());
                return false;
            }
            local = false;
            LOG("air: relay %s (%s)", relay.c_str(), addr_text(hub).c_str());
        }
        s = socket(AF_INET, SOCK_DGRAM, 0);
        if (s == NO_SOCKET) return false;
        set_timeout(s, 100);
        if (local) try_host_hub();
        std::thread([this] { run(); }).detach();
        return true;
    }

    bool recv(std::vector<uint8_t> &frame) {
        std::lock_guard lk(lock);
        if (frames.empty()) return false;
        frame = std::move(frames.front());
        frames.pop_front();
        return true;
    }

    void send(const void *data, size_t size) {
        if (s == NO_SOCKET || size > MAX_FRAME) return;
        uint8_t buf[MAX_FRAME + 5];
        std::memcpy(buf, &RELAY_MAGIC, 4);
        buf[4] = FRAME;
        std::memcpy(buf + 5, data, size);
        send_to(s, buf, size + 5, hub);
    }
};

Air &air() {
    static Air a;
    return a;
}

} // namespace

bool air_start() { return air().start(); }
void air_send(const void *data, size_t size) { air().send(data, size); }
bool air_recv(std::vector<uint8_t> &frame) { return air().recv(frame); }

bool air_wanted() {
    const char *env = std::getenv("CLOCKWORK_RELAY");
    return (env && *env) || !frontend::settings().relay.empty();
}

} // namespace net
