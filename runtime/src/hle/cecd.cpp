// cecd:u: StreetPass. Reports an idle StreetPass daemon with nothing exchanged.
#include "service.h"

#include "log.h"
#include "memory.h"
#include "platform.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

namespace hle {
namespace {

constexpr uint32_t ERR_NOT_FOUND = 0xC880BBFA;

// StreetPass mailbox files: one host file per (title, CEC path type)
std::filesystem::path box_file(uint32_t title, uint32_t type) {
    char name[32];
    std::snprintf(name, sizeof name, "%08x_%u.bin", title, type);
    auto dir = platform::state_dir() / "cec";
    std::filesystem::create_directories(dir);
    return dir / name;
}

struct Cecd : Service {
    std::shared_ptr<kernel::Event> info = std::make_shared<kernel::Event>(kernel::ResetType::OneShot, "CECD info");
    std::shared_ptr<kernel::Event> change =
        std::make_shared<kernel::Event>(kernel::ResetType::OneShot, "CECD state change");

    Cecd() : Service("cecd") {}

    void handle(Request &req) override {
        switch (req.id()) {
        case 0x000C: // Start(command)
        case 0x000D: // Stop(command)
            req.reply(1, 0);
            break;
        case 0x000E: { // GetCecStateAbbreviated: 1 = idle
            auto r = req.reply(2, 0);
            r.push(1);
            break;
        }
        case 0x000F: { // GetCecInfoEventHandle
            auto r = req.reply(1, 2);
            r.push_handles({kernel::add_handle(info)});
            break;
        }
        case 0x0010: { // GetChangeStateEventHandle
            auto r = req.reply(1, 2);
            r.push_handles({kernel::add_handle(change)});
            break;
        }
        case 0x0011: { // OpenAndWrite(size, title, path type, flags, pid, buffer)
            uint32_t size = req.arg(0), desc = req.arg(6), src = req.arg(7);
            std::ofstream(box_file(req.arg(1), req.arg(2)), std::ios::binary)
                .write(reinterpret_cast<const char *>(mem::ptr(src)), size);
            auto r = req.reply(1, 2);
            r.push(desc);
            r.push(src);
            break;
        }
        case 0x0012: { // OpenAndRead(size, title, path type, flags, pid, buffer) -> bytes read
            uint32_t size = req.arg(0), desc = req.arg(6), dest = req.arg(7);
            std::ifstream f(box_file(req.arg(1), req.arg(2)), std::ios::binary);
            if (!f) {
                req.reply(1, 0, ERR_NOT_FOUND);
                break;
            }
            std::vector<char> data((std::istreambuf_iterator<char>(f)), {});
            uint32_t n = std::min<uint32_t>(size, uint32_t(data.size()));
            std::memcpy(mem::ptr(dest), data.data(), n);
            auto r = req.reply(2, 2);
            r.push(n);
            r.push(desc);
            r.push(dest);
            break;
        }
        case 0x0009: // Delete(title, path type, ...)
            std::filesystem::remove(box_file(req.arg(0), req.arg(1)));
            req.reply(1, 0);
            break;
        default:
            unimplemented(*this, req);
        }
    }
};

} // namespace

void register_cecd() { register_service({"cecd:u", "cecd:s"}, std::make_shared<Cecd>()); }

} // namespace hle
