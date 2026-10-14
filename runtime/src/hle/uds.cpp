// nwm::UDS: local wireless (download play aside), what every game here uses for battles, trades
// and co-op with players nearby. "Nearby" is everyone on the air (net/air.cpp): other Clockwork
// windows on this PC, or a room on a relay (see docs/PLAN.md, Rooms). The 802.11 side isn't
// emulated: hosts announce their network and node list, we build the beacon frames games parse
// from that, and joining, node lists and data travel as our own small messages. Encryption is
// skipped too, since both ends are us. Every call the game makes is logged ("uds:").
#include "service.h"

#include "kernel/kernel.h"
#include "log.h"
#include "memory.h"
#include "net/air.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace hle {
namespace {

constexpr int SCAN_MS = 1000;      // how long a scan listens, as on hardware (roughly)
constexpr int CONNECT_MS = 5000;   // how long joining waits for the host (asking again meanwhile)
constexpr int BEACON_MS = 100;     // how often a host announces its network, as on hardware
constexpr int HERE_MS = 1000;      // how often a client tells the host it's still there
constexpr int GONE_MS = 5000;      // silence after which a host or client counts as gone
constexpr uint64_t POLL_NS = 2'000'000;
constexpr uint64_t SIGNAL_DELAY_NS = 20'000'000; // status changes a call makes: signaled this long after
constexpr uint16_t BROADCAST = 0xFFFF;
constexpr uint32_t NETWORK_INFO_SIZE = 0x108, NODE_INFO_SIZE = 0x28, MAX_NODES = 16;
constexpr uint32_t ERR_NOT_FOUND = 0xC86113F2;

// what GetConnectionStatus reports (3dbrew: ConnectionStatus)
enum NetworkStatus : uint32_t { NOT_CONNECTED = 3, CONNECTED_AS_HOST = 6, CONNECTED_AS_CLIENT = 9 };
enum ChangeReason : uint32_t { NO_REASON = 0, CONNECTION_ESTABLISHED = 1, CONNECTION_LOST = 4 };

// messages on the air: type, sender's MAC, the network's host MAC (zeros: none), then the body
enum Message : uint8_t {
    BEACON = 1, // host: network info, node infos
    JOIN,       // client to host: node info, passphrase
    JOINED,     // host to client: node id (0: refused)
    NODES,      // host: node bitmask, then MAC + node info of each
    LEAVE,      // client leaving, or the host closing the network
    DATA,       // source node, destination node, data channel, flags, payload
    HERE,       // client to host: still here
};
constexpr size_t HEADER = 13;

using Mac = std::array<uint8_t, 6>;
using NodeInfo = std::array<uint8_t, NODE_INFO_SIZE>; // 3dbrew: NodeInfo; node id at 0x20
using Clock = std::chrono::steady_clock;

int ms_since(Clock::time_point t) {
    return int(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count());
}

// SHA-1, for the hash games check in a beacon's network info tag
std::array<uint8_t, 20> sha1(const uint8_t *data, size_t size) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    std::vector<uint8_t> m(data, data + size);
    m.push_back(0x80);
    while (m.size() % 64 != 56) m.push_back(0);
    for (int i = 7; i >= 0; i--) m.push_back(uint8_t(uint64_t(size) * 8 >> (i * 8)));
    auto rol = [](uint32_t v, int n) { return v << n | v >> (32 - n); };
    for (size_t at = 0; at < m.size(); at += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = uint32_t(m[at + i * 4]) << 24 | uint32_t(m[at + i * 4 + 1]) << 16 | uint32_t(m[at + i * 4 + 2]) << 8 | m[at + i * 4 + 3];
        for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) f = (b & c) | (~b & d), k = 0x5A827999;
            else if (i < 40) f = b ^ c ^ d, k = 0x6ED9EBA1;
            else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
            else f = b ^ c ^ d, k = 0xCA62C1D6;
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d, d = c, c = rol(b, 30), b = a, a = t;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
    }
    std::array<uint8_t, 20> out;
    for (int i = 0; i < 20; i++) out[i] = uint8_t(h[i / 4] >> (24 - (i % 4) * 8));
    return out;
}

void put(std::vector<uint8_t> &v, const void *p, size_t n) {
    auto b = static_cast<const uint8_t *>(p);
    v.insert(v.end(), b, b + n);
}
void put16(std::vector<uint8_t> &v, uint16_t x) { put(v, &x, 2); }
void put32(std::vector<uint8_t> &v, uint32_t x) { put(v, &x, 4); }
uint16_t get16(const uint8_t *p) { return uint16_t(p[0] | p[1] << 8); }

// The beacon frame body (after the 802.11 header) a host on hardware would send: fixed fields,
// the standard tags, then Nintendo's: network info (with its SHA-1) and the node list
std::vector<uint8_t> beacon_body(const std::vector<uint8_t> &info, const std::vector<NodeInfo> &nodes) {
    static const uint8_t OUI[3] = {0x00, 0x1F, 0x32};
    std::vector<uint8_t> b;
    uint64_t timestamp = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count());
    put(b, &timestamp, 8);
    put16(b, 100);    // beacon interval (TU)
    put16(b, 0x0431); // capabilities
    auto tag = [&](uint8_t id, std::vector<uint8_t> data) {
        b.push_back(id);
        b.push_back(uint8_t(data.size()));
        put(b, data.data(), data.size());
    };
    tag(0x00, std::vector<uint8_t>(8, 0));                                // SSID (hidden)
    tag(0x01, {0x82, 0x84, 0x8B, 0x0C, 0x12, 0x96, 0x18, 0x24});         // supported rates
    tag(0x03, {info[6]});                                                 // channel
    tag(0x05, {0, 2, 0, 0, 0, 0});                                        // traffic indication map
    tag(0xDD, {OUI[0], OUI[1], OUI[2], 20, 0x0A, 0x00, 0x00});            // Nintendo: (always there)
    // Nintendo network info: the network info from its OUI on, the SHA-1 over this tag's data
    // (taken with the hash zeroed), application data size and data
    uint8_t app_size = std::min<uint8_t>(info[0x3F], 0xC8);
    std::vector<uint8_t> net(info.begin() + 0xC, info.begin() + 0xC + 0x1F);
    net.resize(0x1F + 20, 0);
    net.push_back(app_size);
    put(net, info.data() + 0x40, app_size);
    auto hash = sha1(net.data(), net.size());
    std::copy(hash.begin(), hash.end(), net.begin() + 0x1F);
    tag(0xDD, net);
    // Nintendo node list (normally encrypted; DecryptBeaconData below reads it as is): eight
    // nodes a tag, each friend code seed, user name and node id
    for (size_t first = 0, part = 0; first < nodes.size() && part < 2; first += 8, part++) {
        std::vector<uint8_t> list = {OUI[0], OUI[1], OUI[2], uint8_t(24 + part)};
        for (size_t i = first; i < std::min(nodes.size(), first + 8); i++) {
            put(list, nodes[i].data(), 0x1C);       // friend code seed, user name
            put(list, nodes[i].data() + 0x20, 2);   // node id
        }
        tag(0xDD, list);
    }
    return b;
}

struct Uds : Service {
    std::shared_ptr<kernel::Event> connection_event =
        std::make_shared<kernel::Event>(kernel::ResetType::OneShot, "UDS connection status");

    struct Packet {
        uint16_t from;
        std::vector<uint8_t> data;
    };
    struct Binding {
        uint8_t channel = 0;
        uint16_t from = BROADCAST; // only this node's packets (BROADCAST: anyone's)
        uint32_t capacity = 0, queued = 0;
        std::deque<Packet> packets;
        std::shared_ptr<kernel::Event> event;
    };
    std::map<uint32_t, Binding> binds; // by bind node id

    struct Node {
        bool here = false;
        Mac mac{};
        NodeInfo info{};
        Clock::time_point seen;
    };
    struct Seen { // a network someone announced
        std::vector<uint8_t> info;
        std::vector<NodeInfo> nodes;
        Clock::time_point when;
    };

    Mac me{};
    NodeInfo my_info{};
    uint32_t status = NOT_CONNECTED, reason = NO_REASON;
    uint16_t node_id = 0;       // ours, once on a network (the host is 1)
    uint16_t changed_nodes = 0; // nodes that joined or left since the game last asked (bitmask)
    std::array<Node, MAX_NODES> nodes{}; // by node id - 1
    Mac host{};                          // the network's host (us, when hosting)
    std::vector<uint8_t> network_info;   // the network we're on (3dbrew: NetworkInfo, 0x108 bytes)
    std::vector<uint8_t> passphrase;
    std::map<Mac, Seen> beacons;
    Clock::time_point last_beacon, last_here, host_seen;
    bool polling = false;
    // joining: the host we asked, and its answer (0: none yet, node id, or -1: refused)
    bool joining = false;
    std::atomic<int> join_answer{0};
    std::atomic<bool> got_nodes{false}; // and its node list, with us in it
    uint32_t session = 0;
    bool join_deferred = false; // a join was put off (for the log) // counts networks joined, so a late "connected" signal can tell it's stale
    // for the log: the commands seen so far, and the last connection status the game read
    std::set<uint32_t> seen_commands;
    std::array<uint32_t, 5> last_status{};
    int sent = 0, received = 0;

    Uds() : Service("nwm::UDS") {}

    // the static buffer the game passed as number `n` (0 = first), after the normals
    bool in_static(const Request &req, int n, uint32_t &addr, uint32_t &size) {
        uint32_t translates = req.header() & 0x3F, at = 1 + req.normal_count(), end = at + translates;
        for (int found = 0; at < end && at + 1 < 64;) {
            uint32_t d = req.in[at];
            if ((d & 0xF) == 0x2) { // static buffer: size << 14 | id << 10 | 2
                if (found++ == n) {
                    size = d >> 14, addr = req.in[at + 1];
                    return true;
                }
                at += 2;
            } else if ((d & 0xE) == 0) {
                at += (d >> 26) + 2; // handles
            } else {
                at += 2;
            }
        }
        return false;
    }

    // the mapped buffer the game passed (descriptor & 0xE == 0xC: written by us), after the normals
    bool out_buffer(const Request &req, uint32_t &addr, uint32_t &size) {
        uint32_t translates = req.header() & 0x3F, at = 1 + req.normal_count();
        for (uint32_t i = 0; i + 1 < translates + 1 && at + 1 < 64; i++, at++) {
            uint32_t d = req.in[at];
            if ((d & 0xE) == 0xC || (d & 0xE) == 0xA) {
                size = d >> 4, addr = req.in[at + 1];
                return true;
            }
            if ((d & 0xE) == 0x0) at += (d >> 26) + 1; // handles
            else at++;                                  // other buffer: one address word
        }
        return false;
    }

    void log_call(const Request &req, const char *what) {
        std::string args;
        for (uint32_t i = 0; i < std::min<uint32_t>(req.normal_count(), 8); i++) {
            char buf[12];
            std::snprintf(buf, sizeof buf, " %08x", req.arg(int(i)));
            args += buf;
        }
        LOG("uds: %s (%#06x)%s", what, req.id(), args.c_str());
    }

    static std::string mac_text(const Mac &m) {
        char buf[18];
        std::snprintf(buf, sizeof buf, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
        return buf;
    }

    uint16_t node_bitmask() const {
        uint16_t mask = 0;
        for (uint32_t i = 0; i < MAX_NODES; i++)
            if (nodes[i].here) mask |= uint16_t(1u << i);
        return mask;
    }
    uint8_t max_nodes() const { return network_info.size() > 0x1D ? network_info[0x1D] : 0; }
    // the network's attribute bits (big-endian in the network info); UpdateNetworkAttribute changes them
    uint16_t attributes() const {
        return network_info.size() > 0x17 ? uint16_t(network_info[0x16] << 8 | network_info[0x17]) : 0;
    }
    // bit 1 keeps new players out: a game sets it until its own side of the network is ready (Pia
    // publishes its session first), and clears it to let them in
    static constexpr uint16_t ATTR_NO_NEW_PLAYERS = 0x0002;
    std::vector<NodeInfo> node_list() const {
        std::vector<NodeInfo> list;
        for (auto &n : nodes)
            if (n.here) list.push_back(n.info);
        return list;
    }

    void set_status(uint32_t s, uint16_t node, uint32_t why) {
        status = s, node_id = node, reason = why;
        connection_event->signal();
    }

    // signals the status change once the current call has returned, as on hardware (and as games
    // expect: one marks itself as connecting or hosting when the call returns, and ignores an
    // "established" that comes before)
    void signal_after_reply() {
        kernel::schedule_timer(SIGNAL_DELAY_NS, [this, now = session] {
            if (session == now && status != NOT_CONNECTED) connection_event->signal();
        });
    }

    void send(Message type, const Mac &to, const std::vector<uint8_t> &body = {}) {
        std::vector<uint8_t> m = {type};
        put(m, me.data(), 6);
        put(m, to.data(), 6);
        put(m, body.data(), body.size());
        net::air_send(m.data(), m.size());
    }

    void send_nodes() {
        std::vector<uint8_t> body;
        put16(body, node_bitmask());
        for (auto &n : nodes)
            if (n.here) put(body, n.mac.data(), 6), put(body, n.info.data(), NODE_INFO_SIZE);
        send(NODES, me, body);
    }

    void send_beacon() {
        std::vector<uint8_t> body = network_info;
        auto list = node_list();
        body.push_back(uint8_t(list.size()));
        for (auto &n : list) put(body, n.data(), NODE_INFO_SIZE);
        send(BEACON, me, body);
        last_beacon = Clock::now();
    }

    // forgets the network (it closed, we left, or it went quiet)
    void leave_network(uint32_t why) {
        session++;
        changed_nodes = node_bitmask();
        nodes = {};
        network_info.clear();
        host = {}, joining = false;
        for (auto &[id, b] : binds) b.packets.clear(), b.queued = 0;
        set_status(NOT_CONNECTED, 0, why);
    }

    void remove_node(uint32_t i) {
        LOG("uds: node %u (%s) left", i + 1, mac_text(nodes[i].mac).c_str());
        nodes[i] = {};
        changed_nodes |= uint16_t(1u << i);
        network_info[0x1C] = uint8_t(node_list().size());
        send_nodes();
        connection_event->signal();
    }

    // a host's answer to someone joining
    void handle_join(const Mac &from, const uint8_t *body, size_t size) {
        if (status != CONNECTED_AS_HOST || size < NODE_INFO_SIZE + 1) return;
        // not letting players in yet: no answer, and the player asks again. Letting one in early
        // left the host's game half set up, and both games waiting on "please wait" for good
        if (attributes() & ATTR_NO_NEW_PLAYERS) {
            if (!join_deferred) LOG("uds: %s asked to join; not letting players in yet", mac_text(from).c_str());
            join_deferred = true;
            return;
        }
        join_deferred = false;
        uint8_t pass_size = body[NODE_INFO_SIZE];
        std::vector<uint8_t> pass(body + NODE_INFO_SIZE + 1, body + std::min<size_t>(size, NODE_INFO_SIZE + 1 + pass_size));
        int id = 0;
        for (uint32_t i = 0; i < MAX_NODES; i++)
            if (nodes[i].here && nodes[i].mac == from) id = int(i + 1); // asked again: same node
        if (!id && pass == passphrase)
            for (uint32_t i = 1; i < max_nodes() && i < MAX_NODES; i++)
                if (!nodes[i].here) {
                    id = int(i + 1);
                    Node &n = nodes[i];
                    n.here = true, n.mac = from, n.seen = Clock::now();
                    std::memcpy(n.info.data(), body, NODE_INFO_SIZE);
                    n.info[0x20] = uint8_t(id), n.info[0x21] = 0;
                    changed_nodes |= uint16_t(1u << i);
                    network_info[0x1C] = uint8_t(node_list().size());
                    LOG("uds: %s joined as node %d", mac_text(from).c_str(), id);
                    reason = CONNECTION_ESTABLISHED;
                    connection_event->signal();
                    break;
                }
        if (!id) LOG("uds: %s couldn't join (%s)", mac_text(from).c_str(), pass == passphrase ? "full" : "wrong passphrase");
        std::vector<uint8_t> answer;
        put16(answer, uint16_t(id));
        send(JOINED, from, answer);
        if (id) send_nodes();
    }

    // the host's node list, on a client
    void handle_nodes(const uint8_t *body, size_t size) {
        if (size < 2) return;
        uint16_t old = node_bitmask();
        std::array<Node, MAX_NODES> now{};
        bool still_in = false;
        for (size_t at = 2; at + 6 + NODE_INFO_SIZE <= size; at += 6 + NODE_INFO_SIZE) {
            Node n;
            n.here = true;
            std::memcpy(n.mac.data(), body + at, 6);
            std::memcpy(n.info.data(), body + at + 6, NODE_INFO_SIZE);
            uint16_t id = get16(n.info.data() + 0x20);
            if (id < 1 || id > MAX_NODES) continue;
            if (n.mac == me) still_in = true;
            now[id - 1] = n;
        }
        if (joining && still_in) got_nodes = true;
        if (status == CONNECTED_AS_CLIENT && !still_in) { // the host let us go
            LOG("uds: the host removed us");
            leave_network(CONNECTION_LOST);
            return;
        }
        nodes = now;
        if (!network_info.empty()) network_info[0x1C] = uint8_t(node_list().size());
        if (uint16_t diff = uint16_t(old ^ node_bitmask())) {
            changed_nodes |= diff;
            if (status != NOT_CONNECTED) connection_event->signal(); // not mid-join: see ConnectToNetwork
        }
    }

    void handle_data(const uint8_t *body, size_t size) {
        if (status == NOT_CONNECTED || size < 6) return;
        uint16_t from = get16(body), to = get16(body + 2);
        uint8_t channel = body[4];
        if (from == node_id || (to != BROADCAST && to != node_id)) return;
        for (auto &[id, b] : binds) {
            if (b.channel != channel || (b.from != BROADCAST && b.from != from)) continue;
            uint32_t n = uint32_t(size - 6);
            if (b.queued + n > b.capacity) continue; // its receive buffer is full: dropped, as on hardware
            b.packets.push_back({from, std::vector<uint8_t>(body + 6, body + size)});
            b.queued += n;
            b.event->signal();
            if (received++ < 3) LOG("uds: packet from node %u to %#x, channel %u, %u bytes", from, to, channel, n);
        }
    }

    void receive(const std::vector<uint8_t> &m) {
        if (m.size() < HEADER) return;
        Mac from, network;
        std::memcpy(from.data(), m.data() + 1, 6);
        std::memcpy(network.data(), m.data() + 7, 6);
        const uint8_t *body = m.data() + HEADER;
        size_t size = m.size() - HEADER;
        bool ours = status != NOT_CONNECTED && network == host; // about the network we're on
        if (status == CONNECTED_AS_HOST)
            for (auto &n : nodes)
                if (n.here && n.mac == from) n.seen = Clock::now();
        if (status == CONNECTED_AS_CLIENT && from == host) host_seen = Clock::now();
        switch (m[0]) {
        case BEACON:
            if (size >= NETWORK_INFO_SIZE + 1) {
                Seen &s = beacons[from];
                s.info.assign(body, body + NETWORK_INFO_SIZE);
                s.nodes.clear();
                for (size_t i = 0; i < body[NETWORK_INFO_SIZE] && NETWORK_INFO_SIZE + 1 + (i + 1) * NODE_INFO_SIZE <= size; i++) {
                    NodeInfo n;
                    std::memcpy(n.data(), body + NETWORK_INFO_SIZE + 1 + i * NODE_INFO_SIZE, NODE_INFO_SIZE);
                    s.nodes.push_back(n);
                }
                s.when = Clock::now();
            }
            break;
        case JOIN:
            if (network == me) handle_join(from, body, size);
            break;
        case JOINED:
            if (network == me && joining && from == host && size >= 2) join_answer = get16(body) ? int(get16(body)) : -1;
            break;
        case NODES:
            if (from == host && (joining || status == CONNECTED_AS_CLIENT)) handle_nodes(body, size);
            break;
        case LEAVE:
            if (status == CONNECTED_AS_CLIENT && from == host) {
                LOG("uds: the host closed the network");
                leave_network(CONNECTION_LOST);
            } else if (status == CONNECTED_AS_HOST && network == me) {
                for (uint32_t i = 1; i < MAX_NODES; i++)
                    if (nodes[i].here && nodes[i].mac == from) remove_node(i);
            }
            break;
        case DATA:
            if (ours) handle_data(body, size);
            break;
        default:
            break;
        }
    }

    // runs every POLL_NS on the kernel's timer thread (kernel lock held)
    void poll() {
        std::vector<uint8_t> m;
        while (net::air_recv(m)) receive(m);
        if (status == CONNECTED_AS_HOST) {
            if (ms_since(last_beacon) >= BEACON_MS) send_beacon();
            for (uint32_t i = 1; i < MAX_NODES; i++)
                if (nodes[i].here && ms_since(nodes[i].seen) > GONE_MS) remove_node(i);
        } else if (status == CONNECTED_AS_CLIENT) {
            if (ms_since(last_here) >= HERE_MS) send(HERE, host), last_here = Clock::now();
            if (ms_since(host_seen) > GONE_MS) {
                LOG("uds: lost the host");
                leave_network(CONNECTION_LOST);
            }
        }
    }

    // lets the game's other threads run while this one waits for `done` (up to `ms`)
    template <typename F> void wait_for(Request &req, int ms, F done) {
        kernel::Thread *self = kernel::current_thread();
        kernel::release_cpu();
        req.lock->unlock();
        auto start = Clock::now();
        while (ms_since(start) < ms && !done()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        req.lock->lock();
        kernel::reacquire_cpu(*req.lock, self);
    }

    void handle(Request &req) override {
        if (seen_commands.insert(req.id()).second) LOG("uds: first call of %#06x", req.id());
        switch (req.id()) {
        case 0x0001:   // Initialize (deprecated)
        case 0x001B: { // InitializeWithVersion(shared memory size, node info, version, shared memory)
            log_call(req, "Initialize");
            std::memcpy(me.data(), mem::ptr(mem::SHARED_PAGE + 0x60), 6);
            for (uint32_t i = 0; i < NODE_INFO_SIZE / 4; i++) {
                uint32_t w = req.arg(1 + int(i));
                std::memcpy(my_info.data() + i * 4, &w, 4);
            }
            if (!polling && net::air_start()) {
                polling = true;
                kernel::schedule_periodic(POLL_NS, [this] { poll(); });
            }
            // the starting state, without signaling the event: a status change after Initialize
            // reads to games as the connection dropping ("connection lost" when hosting)
            status = NOT_CONNECTED, node_id = 0, reason = NO_REASON;
            connection_event->clear();
            auto r = req.reply(1, 2);
            r.push_handles({kernel::add_handle(connection_event)});
            break;
        }
        case 0x0003: // Shutdown
            log_call(req, "Shutdown");
            if (status == CONNECTED_AS_HOST || status == CONNECTED_AS_CLIENT) send(LEAVE, host);
            nodes = {}, network_info.clear(), host = {};
            status = NOT_CONNECTED, node_id = 0;
            binds.clear();
            req.reply(1, 0);
            break;
        case 0x000B: { // GetConnectionStatus -> 0x30 bytes (3dbrew: ConnectionStatus)
            uint16_t mask = status == NOT_CONNECTED ? 0 : node_bitmask(); // none mid-join
            auto r = req.reply(13, 0);
            r.push(status);
            r.push(reason);
            r.push(node_id | uint32_t(changed_nodes) << 16); // this node's id, nodes that changed
            for (uint32_t i = 0; i < MAX_NODES; i += 2) // node ids present, two a word
                r.push((mask >> i & 1 ? i + 1 : 0) | (mask >> (i + 1) & 1 ? i + 2 : 0) << 16);
            uint32_t total = uint32_t(__builtin_popcount(mask));
            r.push(total | uint32_t(max_nodes()) << 8 | uint32_t(mask) << 16); // total, max, bitmask
            std::array<uint32_t, 5> now{status, reason, node_id, changed_nodes, mask};
            if (now != last_status)
                LOG("uds: status read: %u, reason %u, node %u, changed %#x, nodes %#x (%u of %u)", status, reason, node_id,
                    changed_nodes, mask, total, max_nodes());
            last_status = now;
            changed_nodes = 0; // reported
            break;
        }
        case 0x000F: { // RecvBeaconBroadcastData(out size, scan filter, ...) -> the networks around
            log_call(req, "Scan");
            // a real scan listens for a while; answering at once reads to Pia (Blasters) as a
            // failed one. Beacons keep arriving meanwhile
            wait_for(req, SCAN_MS, [] { return false; });
            uint32_t addr = 0, size = 0;
            if (out_buffer(req, addr, size) && size >= 12) {
                uint8_t *out = mem::ptr(addr);
                uint32_t used = 12, entries = 0;
                for (auto &[mac, s] : beacons) {
                    if (ms_since(s.when) > SCAN_MS + 500 || mac == me) continue;
                    auto body = beacon_body(s.info, s.nodes);
                    uint32_t entry = 0x1C + uint32_t(body.size());
                    if (used + entry > size) break;
                    uint8_t header[0x1C] = {}; // 3dbrew: BeaconEntryHeader
                    std::memcpy(header, &entry, 4);
                    header[5] = s.info[6]; // channel
                    std::memcpy(header + 8, mac.data(), 6);
                    uint32_t body_size = uint32_t(body.size()), header_size = 0x1C;
                    std::memcpy(header + 0x14, &body_size, 4);
                    std::memcpy(header + 0x18, &header_size, 4);
                    std::memcpy(out + used, header, 0x1C);
                    std::memcpy(out + used + 0x1C, body.data(), body.size());
                    used += entry, entries++;
                }
                uint32_t reply_header[3] = {size, used, entries}; // max output size, total size, entries
                std::memcpy(out, reply_header, sizeof reply_header);
                if (entries) LOG("uds: found %u network(s)", entries);
            }
            auto r = req.reply(1, 2);
            r.push((size << 4) | 0xC);
            r.push(addr);
            break;
        }
        case 0x001F: { // DecryptBeaconData(network info, node list tags) -> 16 node infos
            uint32_t addr[3] = {}, size[3] = {};
            for (int i = 0; i < 3; i++) in_static(req, i, addr[i], size[i]);
            std::vector<uint8_t> out(MAX_NODES * NODE_INFO_SIZE, 0);
            uint32_t total = addr[0] && size[0] > 0x1C ? mem::ptr(addr[0])[0x1C] : 0;
            for (uint32_t i = 0; i < std::min<uint32_t>(total, MAX_NODES); i++) {
                uint32_t tag = i / 8, at = 4 + (i % 8) * 0x1E;
                if (!addr[1 + tag] || at + 0x1E > size[1 + tag]) break;
                const uint8_t *src = mem::ptr(addr[1 + tag]) + at;
                std::memcpy(out.data() + i * NODE_INFO_SIZE, src, 0x1C);
                std::memcpy(out.data() + i * NODE_INFO_SIZE + 0x20, src + 0x1C, 2);
            }
            auto r = req.reply(1, 2);
            req.push_static_buffer(r, 0, out.data(), uint32_t(out.size()));
            break;
        }
        case 0x0010: { // SetApplicationData(size, static buffer): shown in the beacon
            log_call(req, "SetApplicationData");
            uint32_t addr = 0, size = 0;
            in_static(req, 0, addr, size);
            size = std::min<uint32_t>({req.arg(0), size, 0xC8});
            if (network_info.size() == NETWORK_INFO_SIZE && addr) {
                network_info[0x3F] = uint8_t(size);
                std::memcpy(network_info.data() + 0x40, mem::ptr(addr), size);
            }
            req.reply(1, 0);
            break;
        }
        case 0x0011: { // GetApplicationData(size) -> size, static buffer
            uint32_t size = network_info.size() == NETWORK_INFO_SIZE ? std::min<uint32_t>(network_info[0x3F], req.arg(0)) : 0;
            auto r = req.reply(2, 2);
            r.push(size);
            req.push_static_buffer(r, 0, size ? network_info.data() + 0x40 : nullptr, size);
            break;
        }
        case 0x0012: { // Bind(bind node id, receive buffer size, data channel, network node id) -> event
            log_call(req, "Bind");
            Binding &b = binds[req.arg(0)];
            b = {};
            b.capacity = req.arg(1), b.channel = uint8_t(req.arg(2)), b.from = uint16_t(req.arg(3));
            b.event = std::make_shared<kernel::Event>(kernel::ResetType::OneShot, "UDS bind");
            auto r = req.reply(1, 2);
            r.push_handles({kernel::add_handle(b.event)});
            break;
        }
        case 0x0013: { // Unbind(bind node id)
            log_call(req, "Unbind");
            binds.erase(req.arg(0));
            auto r = req.reply(5, 0);
            r.push(req.arg(0));
            r.push(0), r.push(0), r.push(0);
            break;
        }
        case 0x0014: { // PullPacket(bind node id, max size in words, max size) -> size, source node
            auto it = binds.find(req.arg(0));
            uint32_t room = mem::read32(req.ctx->tls + 0x180) >> 14; // the receive buffer it set up
            auto r = req.reply(3, 2);
            if (it == binds.end() || it->second.packets.empty()) {
                r.push(0), r.push(0);
                req.push_static_buffer(r, 0, nullptr, 0);
                break;
            }
            Packet p = std::move(it->second.packets.front());
            it->second.packets.pop_front();
            it->second.queued -= uint32_t(p.data.size());
            uint32_t n = std::min<uint32_t>(uint32_t(p.data.size()), room);
            r.push(n);
            r.push(p.from);
            req.push_static_buffer(r, 0, p.data.data(), n);
            break;
        }
        case 0x0017: { // SendTo(?, destination node, data channel, size in words, size, flags, data)
            uint32_t addr = 0, size = 0;
            in_static(req, 0, addr, size);
            size = std::min(size, req.arg(4));
            uint16_t to = uint16_t(req.arg(1));
            uint8_t channel = uint8_t(req.arg(2));
            if (sent++ < 3) LOG("uds: SendTo node %#x, channel %u, %u bytes, flags %#x", to, channel, size, req.arg(5) & 0xFF);
            if (status != NOT_CONNECTED && addr && size <= 1500) {
                std::vector<uint8_t> body;
                put16(body, node_id);
                put16(body, to);
                body.push_back(channel);
                body.push_back(uint8_t(req.arg(5)));
                put(body, mem::ptr(addr), size);
                send(DATA, host, body);
            }
            req.reply(1, 0);
            break;
        }
        case 0x001A: { // GetChannel
            auto r = req.reply(2, 0);
            r.push(status == NOT_CONNECTED || network_info.size() < 7 ? 0 : network_info[6]);
            break;
        }
        case 0x001D: { // BeginHostingNetwork(passphrase size, network info, passphrase): we're host, node 1
            log_call(req, "BeginHostingNetwork");
            uint32_t addr = 0, size = 0;
            network_info.assign(NETWORK_INFO_SIZE, 0);
            if (in_static(req, 0, addr, size) && size >= NETWORK_INFO_SIZE)
                std::memcpy(network_info.data(), mem::ptr(addr), NETWORK_INFO_SIZE);
            passphrase.clear();
            if (in_static(req, 1, addr, size)) passphrase.assign(mem::ptr(addr), mem::ptr(addr) + std::min(size, req.arg(0)));
            std::memcpy(network_info.data(), me.data(), 6);
            if (!network_info[6]) network_info[6] = 1; // channel: the game leaves it to the system
            network_info[8] = 1;                       // initialized
            network_info[0x1C] = 1;                    // nodes: just the host
            nodes = {};
            nodes[0].here = true, nodes[0].mac = me, nodes[0].info = my_info;
            nodes[0].info[0x20] = 1, nodes[0].info[0x21] = 0;
            host = me;
            LOG("uds: hosting as %s: channel %u, max %u players, attributes %#x", mac_text(me).c_str(), network_info[6],
                max_nodes(), attributes());
            changed_nodes = 1; // the host is node 1, and just arrived
            // "established", as on hardware: the game sets its network up on it
            status = CONNECTED_AS_HOST, node_id = 1, reason = CONNECTION_ESTABLISHED;
            session++;
            signal_after_reply();
            send_beacon();
            req.reply(1, 0);
            break;
        }
        case 0x0005: { // EjectClient(node id)
            log_call(req, "EjectClient");
            uint16_t id = uint16_t(req.arg(0));
            if (status == CONNECTED_AS_HOST)
                for (uint32_t i = 1; i < MAX_NODES; i++)
                    if (nodes[i].here && (id == BROADCAST || id == i + 1)) remove_node(i);
            req.reply(1, 0);
            break;
        }
        case 0x0007: // UpdateNetworkAttribute(bits, set): sets or clears attribute bits (shown in the
            // beacon). Blasters clears 6 to let players join and sets 2 or 6 to stop them
            log_call(req, "UpdateNetworkAttribute");
            if (network_info.size() == NETWORK_INFO_SIZE) {
                uint16_t attributes = uint16_t(network_info[0x16] << 8 | network_info[0x17]), bits = uint16_t(req.arg(0));
                attributes = req.arg(1) & 0xFF ? attributes | bits : attributes & ~bits;
                network_info[0x16] = uint8_t(attributes >> 8), network_info[0x17] = uint8_t(attributes);
            }
            req.reply(1, 0);
            break;
        case 0x0008: // DestroyNetwork
        case 0x000A: // DisconnectNetwork
            log_call(req, req.id() == 0x0008 ? "DestroyNetwork" : "DisconnectNetwork");
            if (status != NOT_CONNECTED) send(LEAVE, host);
            leave_network(NO_REASON);
            req.reply(1, 0);
            break;
        case 0x000D: { // GetNodeInformation(node id) -> 0x28 bytes
            uint16_t id = uint16_t(req.arg(0));
            NodeInfo info{};
            if (id >= 1 && id <= MAX_NODES && nodes[id - 1].here) info = nodes[id - 1].info;
            else LOG("uds: GetNodeInformation(%u): no such node", id);
            auto r = req.reply(11, 0);
            for (uint32_t i = 0; i < NODE_INFO_SIZE; i += 4) {
                uint32_t w;
                std::memcpy(&w, info.data() + i, 4);
                r.push(w);
            }
            break;
        }
        case 0x001E: { // ConnectToNetwork(connection type, passphrase size, network info, passphrase)
            log_call(req, "ConnectToNetwork");
            uint32_t addr = 0, size = 0;
            std::vector<uint8_t> info(NETWORK_INFO_SIZE, 0);
            if (in_static(req, 0, addr, size) && size >= NETWORK_INFO_SIZE)
                std::memcpy(info.data(), mem::ptr(addr), NETWORK_INFO_SIZE);
            std::vector<uint8_t> pass;
            if (in_static(req, 1, addr, size)) pass.assign(mem::ptr(addr), mem::ptr(addr) + std::min(size, req.arg(1)));
            std::memcpy(host.data(), info.data(), 6);
            LOG("uds: joining %s", mac_text(host).c_str());
            std::vector<uint8_t> ask(my_info.begin(), my_info.end());
            ask.push_back(uint8_t(pass.size()));
            put(ask, pass.data(), pass.size());
            joining = true, join_answer = 0, got_nodes = false;
            nodes = {};
            for (int tries = 0; tries < CONNECT_MS / 500 && join_answer == 0; tries++) {
                send(JOIN, host, ask);
                wait_for(req, 500, [this] { return join_answer != 0; });
            }
            // the host sends its node list right after letting us in: wait for it too, so the game
            // never sees itself connected to a network without nodes (it asks at once)
            if (join_answer > 0) wait_for(req, 1000, [this] { return got_nodes.load(); });
            if (join_answer > 0 && !got_nodes) LOG("uds: joined, but the host's node list didn't come");
            joining = false;
            int answer = join_answer;
            if (answer <= 0) {
                LOG("uds: %s", answer < 0 ? "the host refused (full, or a different passphrase)" : "no answer from the host");
                host = {}, nodes = {};
                req.reply(1, 0, ERR_NOT_FOUND);
                break;
            }
            network_info = info;
            network_info[0x1C] = uint8_t(std::max<size_t>(node_list().size(), 2));
            host_seen = last_here = Clock::now();
            changed_nodes |= node_bitmask();
            LOG("uds: joined as node %d", answer);
            // joining finishes in the background on hardware: "connected" comes after the call
            status = CONNECTED_AS_CLIENT, node_id = uint16_t(answer), reason = CONNECTION_ESTABLISHED;
            session++;
            signal_after_reply();
            req.reply(1, 0);
            break;
        }
        default: // log it, and let the game carry on
            log_call(req, "unknown command");
            req.reply(1, 0);
        }
    }
};

} // namespace

void register_uds() { register_service({"nwm::UDS"}, std::make_shared<Uds>()); }

} // namespace hle
