// srv: the service manager. Hands out sessions to services by name.
#include "service.h"

#include "log.h"

#include <cstring>

namespace hle {
namespace {

struct Srv : Service {
    std::shared_ptr<kernel::Semaphore> notification = std::make_shared<kernel::Semaphore>(0, 64);

    Srv() : Service("srv:") {}

    void handle(Request &req) override {
        switch (req.id()) {
        case 0x0001: // RegisterClient
            req.reply(1, 0);
            break;
        case 0x0002: { // EnableNotification -> semaphore signaled when notifications arrive
            auto r = req.reply(1, 2);
            r.push_handles({kernel::add_handle(notification)});
            break;
        }
        case 0x0005: { // GetServiceHandle(name[8], length, flags)
            char name[9] = {};
            std::memcpy(name, &req.in[1], 8);
            name[std::min<uint32_t>(req.arg(2), 8)] = 0;
            auto service = get_service(name);
            auto r = req.reply(1, 2);
            r.push_handles({kernel::add_handle(std::make_shared<kernel::Session>(service))}, true);
            LOG_IPC("srv: GetServiceHandle(\"%s\")", name);
            break;
        }
        case 0x0009: // Subscribe(notification id)
        case 0x000A: // Unsubscribe
            req.reply(1, 0);
            break;
        case 0x000B: { // ReceiveNotification -> notification id (0: none)
            auto r = req.reply(2, 0);
            r.push(0);
            break;
        }
        default:
            unimplemented(*this, req);
        }
    }
};

} // namespace

std::shared_ptr<Service> make_srv() { return std::make_shared<Srv>(); }

} // namespace hle
