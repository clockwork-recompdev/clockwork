// cfg:u: system settings. Answers with a US-region, English 3DS with defaults for the
// config blocks games read; unknown blocks read as zeros (and are logged).
#include "service.h"

#include "log.h"
#include "memory.h"
#include "platform.h"

#include <cstring>
#include <map>
#include <set>
#include <vector>

namespace hle {
namespace {

constexpr uint8_t REGION_USA = 1, LANGUAGE_ENGLISH = 1, COUNTRY_USA = 49;

std::vector<uint8_t> utf16(const char *s, size_t bytes) {
    std::vector<uint8_t> out(bytes);
    for (size_t i = 0; s[i] && i * 2 + 1 < bytes; i++) out[i * 2] = uint8_t(s[i]);
    return out;
}

template <typename T> std::vector<uint8_t> bytes_of(std::initializer_list<T> values) {
    std::vector<uint8_t> out(values.size() * sizeof(T));
    std::memcpy(out.data(), values.begin(), out.size());
    return out;
}

struct Cfg : Service {
    std::map<uint32_t, std::vector<uint8_t>> blocks;
    std::set<uint32_t> logged;

    Cfg() : Service("cfg") {
        blocks[0x00050005] = bytes_of<float>({62.0f, 289.0f, 76.8f, 46.08f, 10.0f, 5.0f, 55.58f, 21.57f}); // stereo camera
        blocks[0x00070001] = {1};                                     // sound output: stereo
        blocks[0x000A0000] = utf16("Player", 0x1C);                   // username
        blocks[0x000A0001] = {3, 25};                                 // birthday: month, day
        blocks[0x000A0002] = {LANGUAGE_ENGLISH};                      // language
        blocks[0x000B0000] = {0, 0, 0, COUNTRY_USA};                  // country info
        blocks[0x000C0000] = std::vector<uint8_t>(0xC0, 0);           // parental controls: none
        blocks[0x000D0000] = bytes_of<uint16_t>({0x7F, 0x7F});        // EULA version accepted
        blocks[0x000F0004] = {0, 0, 0, 0};                            // system model: 3DS
        // console unique id (twice) and its random number: games tell consoles apart by it, and
        // two windows playing together must not look like the same console (see platform.h)
        uint64_t id = platform::console_id();
        blocks[0x00090000] = bytes_of<uint64_t>({id});
        blocks[0x00090001] = bytes_of<uint64_t>({id});
        blocks[0x00090002] = bytes_of<uint32_t>({uint32_t(id >> 48)});
    }

    void handle(Request &req) override {
        switch (req.id()) {
        case 0x0001: { // GetConfigInfoBlk2(size, block id) -> data into a mapped buffer
            uint32_t size = req.arg(0), id = req.arg(1), desc = req.arg(2), dest = req.arg(3);
            auto it = blocks.find(id);
            std::memset(mem::ptr(dest), 0, size);
            if (it != blocks.end()) {
                std::memcpy(mem::ptr(dest), it->second.data(), std::min<size_t>(size, it->second.size()));
            } else if (logged.insert(id).second) {
                LOG("cfg: block %08x (%u bytes) has no default; returning zeros", id, size);
            }
            auto r = req.reply(1, 2);
            r.push(desc);
            r.push(dest);
            break;
        }
        case 0x0002: { // SecureInfoGetRegion
            auto r = req.reply(2, 0);
            r.push(REGION_USA);
            break;
        }
        case 0x0003: { // GenHashConsoleUnique(salt) -> a stable per-console hash
            uint64_t h = platform::console_id() ^ (uint64_t(req.arg(0)) << 20);
            h = (h ^ (h >> 31)) * 0x7FB5D329728EA185ull, h ^= h >> 27; // mixed, so salts look unrelated
            auto r = req.reply(3, 0);
            r.push(uint32_t(h));
            r.push(uint32_t(h >> 32) & 0xFFFFF);
            break;
        }
        case 0x0004: { // GetRegionCanadaUSA
            auto r = req.reply(2, 0);
            r.push(1);
            break;
        }
        case 0x0005: { // GetSystemModel: 0 = 3DS
            auto r = req.reply(2, 0);
            r.push(0);
            break;
        }
        case 0x0006: { // GetModelNintendo2DS: 1 = not a 2DS
            auto r = req.reply(2, 0);
            r.push(1);
            break;
        }
        default:
            unimplemented(*this, req);
        }
    }
};

} // namespace

void register_cfg() { register_service({"cfg:u", "cfg:s", "cfg:i"}, std::make_shared<Cfg>()); }

} // namespace hle
