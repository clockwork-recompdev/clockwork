// Services for things this PC doesn't have, answered so that a menu reaching them reports the
// feature unavailable instead of the game stopping:
// - ac:u (internet connection): always offline
// - sockets, HTTP, the camera (QR codes), NFC (medals, amiibo), the Circle Pad Pro and head
//   tracking: Initialize succeeds, everything else fails
// Each call is logged, to see when a game reaches one.
#include "service.h"

#include "kernel/kernel.h"
#include "log.h"

#include <algorithm>
#include <string>

namespace hle {
namespace {

constexpr uint32_t ERR_UNAVAILABLE = 0xC8A12402; // a "not available" failure; games treat it as offline

std::string args_of(const Request &req) {
    std::string args;
    for (uint32_t i = 0; i < std::min<uint32_t>(req.normal_count(), 6); i++) {
        char buf[12];
        std::snprintf(buf, sizeof buf, " %08x", req.arg(int(i)));
        args += buf;
    }
    return args;
}

// the first handle the game passed (not its process id), after the normal words
uint32_t passed_handle(const Request &req) {
    uint32_t at = 1 + req.normal_count(), end = at + (req.header() & 0x3F);
    while (at < end && at < 63) {
        uint32_t d = req.in[at];
        if ((d & 0xE) != 0) { // a buffer: descriptor and address
            at += 2;
            continue;
        }
        if (d & 0x20) { // the calling process's id
            at += 2;
            continue;
        }
        return req.in[at + 1];
    }
    return 0;
}

struct Ac : Service {
    Ac() : Service("ac:u") {}

    void handle(Request &req) override {
        switch (req.id()) {
        case 0x0001: { // CreateDefaultConfig -> 0x200-byte config, static buffer
            static const uint8_t config[0x200] = {};
            auto r = req.reply(1, 2);
            req.push_static_buffer(r, 0, config, sizeof config);
            break;
        }
        case 0x0004: // ConnectAsync(process, event, config): it fails, which the game's event reports
            if (auto e = kernel::get<kernel::Event>(passed_handle(req))) e->signal();
            req.reply(1, 0);
            break;
        case 0x0005: // GetConnectResult -> failed
            req.reply(1, 0, ERR_UNAVAILABLE);
            break;
        case 0x000C:   // GetWifiStatus -> not connected
        case 0x003E: { // IsConnected -> no
            auto r = req.reply(2, 0);
            r.push(0);
            break;
        }
        case 0x0040: // SetClientVersion
            req.reply(1, 0);
            break;
        default:
            LOG("ac: command %#06x%s", req.id(), args_of(req).c_str());
            req.reply(1, 0);
        }
    }
};

struct Unavailable : Service {
    using Service::Service;
    void handle(Request &req) override {
        LOG("%s: command %#06x%s (not available here)", name.c_str(), req.id(), args_of(req).c_str());
        if (req.id() == 0x0001) req.reply(1, 0); // Initialize
        else req.reply(1, 0, ERR_UNAVAILABLE);
    }
};

} // namespace

void register_offline() {
    register_service({"ac:u", "ac:i"}, std::make_shared<Ac>());
    for (const char *name : {"soc:U", "http:C", "cam:u", "nfc:u", "ir:USER", "hid:QTM", "gsp::Lcd", "boss:M"})
        register_service({name}, std::make_shared<Unavailable>(name));
}

} // namespace hle
