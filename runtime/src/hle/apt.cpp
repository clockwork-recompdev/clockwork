// APT: applet manager. Handles the application's startup handshake (lock, events, wakeup
// parameter) and system queries. APT:A, APT:U and APT:S share one instance.
#include "service.h"

#include "log.h"

#include <algorithm>

namespace hle {
namespace {

constexpr uint32_t APPID_HOMEMENU = 0x101;
constexpr uint32_t APPID_APPLICATION = 0x300;
constexpr uint32_t SIGNAL_WAKEUP = 1;

struct Parameter {
    uint32_t sender = 0;
    uint32_t signal = 0;
    kernel::Handle handle = 0;
};

struct Apt : Service {
    std::shared_ptr<kernel::Mutex> lock = std::make_shared<kernel::Mutex>();
    std::shared_ptr<kernel::Event> notification =
        std::make_shared<kernel::Event>(kernel::ResetType::OneShot, "APT notification");
    std::shared_ptr<kernel::Event> parameter_event =
        std::make_shared<kernel::Event>(kernel::ResetType::OneShot, "APT parameter");
    bool has_parameter = false;
    Parameter parameter;
    uint32_t cpu_time_limit = 0;

    Apt() : Service("APT") {}

    void send_parameter(Parameter p) {
        parameter = p;
        has_parameter = true;
        parameter_event->signal();
    }

    void handle(Request &req) override {
        switch (req.id()) {
        case 0x0001: { // GetLockHandle(attributes) -> attributes, power state, lock
            auto r = req.reply(3, 2);
            r.push(req.arg(0));
            r.push(0);
            r.push_handles({kernel::add_handle(lock)});
            break;
        }
        case 0x0002: { // Initialize(app id, attributes) -> notification event, parameter event
            auto r = req.reply(1, 3);
            r.push_handles({kernel::add_handle(notification), kernel::add_handle(parameter_event)});
            LOG_IPC("APT Initialize app id %#x", req.arg(0));
            break;
        }
        case 0x0003: // Enable(attributes): the system answers by waking the application
            req.reply(1, 0);
            send_parameter({APPID_HOMEMENU, SIGNAL_WAKEUP, 0});
            break;
        case 0x0009: { // IsRegistered(app id): only the application and the HOME Menu exist here
            uint32_t app = req.arg(0);
            bool registered = app == APPID_HOMEMENU || app == APPID_APPLICATION || app == 0;
            LOG_IPC("APT IsRegistered(%#x) -> %d", app, registered);
            auto r = req.reply(2, 0);
            r.push(registered);
            break;
        }
        case 0x000B: { // InquireNotification(app id) -> signal (0: none)
            auto r = req.reply(2, 0);
            r.push(0);
            break;
        }
        case 0x000D:   // ReceiveParameter(app id, buffer size)
        case 0x000E: { // GlanceParameter: same, but leaves the parameter in place
            if (!has_parameter) {
                req.reply(1, 0, 0xC8A0CFEF); // no parameter waiting
                break;
            }
            auto r = req.reply(4, 4);
            r.push(parameter.sender);
            r.push(parameter.signal);
            r.push(0); // parameter data size
            r.push_handles({parameter.handle}, true);
            req.push_static_buffer(r, 0, nullptr, 0);
            if (req.id() == 0x000D) has_parameter = false;
            LOG_IPC("APT %sParameter -> signal %u from %#x", req.id() == 0x000D ? "Receive" : "Glance",
                    parameter.signal, parameter.sender);
            break;
        }
        case 0x003B: // CancelLibraryApplet(exiting): none running
        case 0x0043: // NotifyToWait(app id)
            req.reply(1, 0);
            break;
        case 0x004B: { // AppletUtility(utility, input size, output size, input) -> result, output
            uint32_t out_size = std::min<uint32_t>(req.arg(2), 0x100);
            uint8_t zeros[0x100] = {};
            auto r = req.reply(2, 2);
            r.push(kernel::RESULT_OK);
            req.push_static_buffer(r, 0, zeros, out_size);
            LOG_IPC("APT AppletUtility %u (in %u, out %u)", req.arg(0), req.arg(1), req.arg(2));
            break;
        }
        case 0x004F: // SetAppCpuTimeLimit(fixed, percent)
            cpu_time_limit = req.arg(1);
            req.reply(1, 0);
            break;
        case 0x0050: { // GetAppCpuTimeLimit
            auto r = req.reply(2, 0);
            r.push(cpu_time_limit);
            break;
        }
        case 0x0055: // SetScreenCapPostPermission(permission)
            req.reply(1, 0);
            break;
        case 0x0101: { // GetTargetPlatform -> 0: old 3DS
            auto r = req.reply(2, 0);
            r.push(0);
            break;
        }
        case 0x0102: { // CheckNew3DS -> 0
            auto r = req.reply(2, 0);
            r.push(0);
            break;
        }
        case 0x003E: // asked when a game opens its e-manual, which isn't available here: carry on
            LOG("apt: command %#06x (args %08x %08x): no e-manual here", req.id(), req.arg(0), req.arg(1));
            req.reply(1, 0);
            break;
        case 0x0103: { // GetApplicationRunningMode -> 0: a regular 3DS (multiplayer menus ask)
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

void register_apt() { register_service({"APT:A", "APT:U", "APT:S"}, std::make_shared<Apt>()); }

} // namespace hle
