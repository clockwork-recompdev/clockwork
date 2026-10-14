#pragma once
// HLE services. A guest SendSyncRequest on a session runs the service's handler on the
// command buffer in the calling thread's TLS (TLS + 0x80), with the kernel lock held.

#include "kernel/kernel.h"

#include <memory>
#include <mutex>
#include <string>

namespace hle {

// The IPC command buffer: header, normal words, then translate descriptors.
struct Request {
    uint32_t *cmd;
    Ctx *ctx;
    std::unique_lock<std::mutex> *lock;

    // the request as the game wrote it; replies overwrite `cmd`, so arguments are read from here
    uint32_t in[64];

    uint16_t id() const { return uint16_t(in[0] >> 16); }
    uint32_t header() const { return in[0]; }
    uint32_t normal_count() const { return (in[0] >> 6) & 0x3F; }
    uint32_t arg(int i) const { return in[1 + i]; }

    // Builds a response in place: `normals` words (result code first), then handle/buffer
    // descriptors and their values written with the push functions.
    struct Response {
        uint32_t *cmd;
        int pos;
        void push(uint32_t v) { cmd[pos++] = v; }
        void push64(uint64_t v) {
            push(uint32_t(v));
            push(uint32_t(v >> 32));
        }
        // handles are copied (the service keeps its reference) unless `move`
        void push_handles(std::initializer_list<kernel::Handle> hs, bool move = false) {
            push(((uint32_t(hs.size()) - 1) << 26) | (move ? 0x10 : 0));
            for (auto h : hs) push(h);
        }
    };
    // Copies `size` bytes into the receive buffer the guest registered as static buffer `id`
    // (descriptor pairs at TLS + 0x180) and pushes the descriptor + address.
    void push_static_buffer(Response &r, uint32_t id, const void *data, uint32_t size);

    // normals includes the result code word
    Response reply(uint32_t normals, uint32_t translates, uint32_t result = kernel::RESULT_OK) {
        cmd[0] = (uint32_t(id()) << 16) | (normals << 6) | translates;
        Response r{cmd, 1};
        r.push(result);
        return r;
    }
};

struct Service : std::enable_shared_from_this<Service> {
    std::string name;
    explicit Service(std::string n) : name(std::move(n)) {}
    virtual ~Service() = default;
    virtual void handle(Request &req) = 0;
};

// stops with a message naming the service and command; for commands not implemented yet
[[noreturn]] void unimplemented(const Service &s, const Request &req);

std::shared_ptr<Service> find_port(const std::string &name);
// registers a service factory under one or more names (all names share one instance)
void register_service(std::initializer_list<const char *> names, std::shared_ptr<Service> service);
std::shared_ptr<Service> get_service(const std::string &name);
void handle_request(std::unique_lock<std::mutex> &lk, Ctx *ctx, Service &service);

} // namespace hle
