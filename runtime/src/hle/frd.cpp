// frd:u: the friends service. Multiplayer menus ask it who you are before local play: a profile
// (the console's user name), offline, no friends. Unknown commands are logged ("frd:") and succeed,
// so a menu carries on rather than the game stopping.
#include "service.h"

#include "log.h"
#include "platform.h"
#include "memory.h"

#include <algorithm>
#include <cstring>
#include <string>

namespace hle {
namespace {

struct Frd : Service {
    Frd() : Service("frd:u") {}

    void log_call(const Request &req, const char *what) {
        std::string args;
        for (uint32_t i = 0; i < std::min<uint32_t>(req.normal_count(), 6); i++) {
            char buf[12];
            std::snprintf(buf, sizeof buf, " %08x", req.arg(int(i)));
            args += buf;
        }
        LOG("frd: %s (%#06x)%s", what, req.id(), args.c_str());
    }

    void handle(Request &req) override {
        switch (req.id()) {
        case 0x0032: // SetClientSdkVersion(version, process)
            req.reply(1, 0);
            break;
        case 0x0001: // HasLoggedIn -> no
        case 0x0002: { // IsOnline -> no
            auto r = req.reply(2, 0);
            r.push(0);
            break;
        }
        case 0x0005: { // GetMyFriendKey -> principal id, padding, friend code
            auto r = req.reply(5, 0);
            // a made-up principal id (there's no account), one per console: see platform::console_id
            r.push(uint32_t(platform::console_id() * 0x9E3779B97F4A7C15ull >> 32) | 1);
            r.push(0);
            r.push64(0);
            break;
        }
        case 0x0006: { // GetMyProfile -> region, country, area, language, platform (8 bytes)
            auto r = req.reply(3, 0);
            r.push(0x00310001); // USA, country 49 (United States)
            r.push(0x00000001); // English
            break;
        }
        case 0x0009: { // GetMyScreenName -> 11 UTF-16 units
            auto r = req.reply(7, 0);
            const char *name = "Player";
            uint16_t units[12] = {};
            for (size_t i = 0; name[i] && i < 10; i++) units[i] = uint16_t(name[i]);
            for (int i = 0; i < 6; i++) r.push(uint32_t(units[i * 2]) | uint32_t(units[i * 2 + 1]) << 16);
            break;
        }
        case 0x0011: { // GetFriendKeyList(offset, max) -> count, static buffer: none
            auto r = req.reply(2, 2);
            r.push(0);
            req.push_static_buffer(r, 0, nullptr, 0);
            break;
        }
        default:
            log_call(req, "command");
            req.reply(1, 0);
        }
    }
};

} // namespace

void register_frd() { register_service({"frd:u", "frd:a"}, std::make_shared<Frd>()); }

} // namespace hle
