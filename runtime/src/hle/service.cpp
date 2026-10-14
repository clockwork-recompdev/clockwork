#include "service.h"

#include "log.h"
#include "memory.h"

#include <cstring>
#include <functional>
#include <map>

namespace hle {

std::shared_ptr<Service> make_srv();
std::shared_ptr<Service> make_errf();
void register_apt();
void register_stubs();
void register_fs();
void register_cfg();
void register_hid();
void register_ptm();
void register_gsp();
void register_y2r();
void register_dsp();
void register_cecd();
void register_uds();
void register_frd();
void register_offline();

namespace {
// a service the game asked for that has no implementation yet
struct Placeholder : Service {
    using Service::Service;
    void handle(Request &req) override { unimplemented(*this, req); }
};

std::map<std::string, std::shared_ptr<Service>> &instances() {
    static std::map<std::string, std::shared_ptr<Service>> i;
    return i;
}

void register_all() {
    static bool done = false;
    if (done) return;
    done = true;
    register_service({"srv:"}, make_srv());
    register_apt();
    register_stubs();
    register_fs();
    register_cfg();
    register_hid();
    register_ptm();
    register_gsp();
    register_y2r();
    register_dsp();
    register_cecd();
    register_uds();
    register_frd();
    register_offline();
}
} // namespace

void unimplemented(const Service &s, const Request &req) {
    std::string args;
    for (uint32_t i = 0; i < std::min<uint32_t>(req.normal_count(), 6); i++) {
        char buf[16];
        std::snprintf(buf, sizeof buf, " %08x", req.arg(int(i)));
        args += buf;
    }
    fatal("service %s: command %#06x (header %08x) not implemented; args:%s (lr=%08x)", s.name.c_str(), req.id(),
          req.header(), args.c_str(), req.ctx->r[14]);
}

std::shared_ptr<Service> find_port(const std::string &name) {
    if (name == "srv:") return get_service("srv:");
    if (name == "err:f") {
        static auto errf = make_errf();
        return errf;
    }
    return nullptr;
}

void register_service(std::initializer_list<const char *> names, std::shared_ptr<Service> service) {
    for (const char *n : names) instances()[n] = service;
}

std::shared_ptr<Service> get_service(const std::string &name) {
    register_all();
    auto &inst = instances()[name];
    if (!inst) {
        LOG("service %s has no implementation yet; any call to it will stop", name.c_str());
        inst = std::make_shared<Placeholder>(name);
    }
    return inst;
}

void Request::push_static_buffer(Response &r, uint32_t id, const void *data, uint32_t size) {
    uint32_t dest = mem::read32(ctx->tls + 0x180 + id * 8 + 4);
    if (size) std::memcpy(mem::ptr(dest), data, size);
    r.push((size << 14) | (id << 10) | 2);
    r.push(dest);
}

void handle_request(std::unique_lock<std::mutex> &lk, Ctx *ctx, Service &service) {
    Request req{reinterpret_cast<uint32_t *>(mem::ptr(ctx->tls + 0x80)), ctx, &lk, {}};
    std::memcpy(req.in, req.cmd, sizeof req.in);
    LOG_IPC("%s cmd %#06x (header %08x)", service.name.c_str(), req.id(), req.header());
    service.handle(req);
}

} // namespace hle
