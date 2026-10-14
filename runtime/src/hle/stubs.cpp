// Services where the game only needs commands to succeed: each listed command returns
// success plus a fixed number of zero words. Unlisted commands still stop with their id.
#include "service.h"

#include "log.h"
#include "memory.h"

#include <map>
#include <random>

namespace hle {
namespace {

struct Stub : Service {
    std::map<uint16_t, uint32_t> outputs; // command id -> zero words after the result
    Stub(std::string name, std::map<uint16_t, uint32_t> o) : Service(std::move(name)), outputs(std::move(o)) {}

    void handle(Request &req) override {
        auto it = outputs.find(req.id());
        if (it == outputs.end()) unimplemented(*this, req);
        auto r = req.reply(1 + it->second, 0);
        for (uint32_t i = 0; i < it->second; i++) r.push(0);
    }
};

// ssl:C: TLS for online features. There is no network here, so only Initialize and the random
// number generator work; anything that would open a connection fails, and the game treats it
// like being offline.
struct SslC : Service {
    std::mt19937 rng{std::random_device{}()};
    bool warned = false;
    SslC() : Service("ssl:C") {}

    void handle(Request &req) override {
        switch (req.id()) {
        case 0x0001: // Initialize(process)
            req.reply(1, 0);
            break;
        case 0x0011: { // GenerateRandomData(size, mapped buffer)
            uint32_t size = req.arg(0), addr = req.arg(2);
            for (uint32_t i = 0; i < size; i++) mem::ptr(addr)[i] = uint8_t(rng());
            auto r = req.reply(1, 2);
            r.push(req.arg(1));
            r.push(addr);
            break;
        }
        default:
            if (!warned) LOG("ssl:C: command %#x refused (no network)", req.id());
            warned = true;
            req.reply(1, 0, 0xD8A0B801); // a status the game handles as "connection failed"
        }
    }
};

// am:app: installed titles and their DLC. Only the game card's own content exists here, so
// every count is zero and every list comes back empty (buffer descriptors are echoed).
struct AmApp : Service {
    AmApp() : Service("am:app") {}

    void handle(Request &req) override {
        uint32_t normals = req.normal_count(), translates = req.in[0] & 0x3F;
        switch (req.id()) {
        case 0x1001: // GetDLCContentInfoCount
        case 0x1003: // ListDLCContentInfos
        case 0x1005: // GetDLCTitleInfos
        case 0x1006: // GetNumDataTitleTickets
        case 0x1007: // ListDLCTicketInfos
        case 0x1008: // ListDataTitleTicketInfos
        case 0x100C: // GetNumExistingContentInfos
        case 0x100D: // ListExistingContentInfos
        case 0x100E: // GetPatchTitleInfos
        {
            auto r = req.reply(2, translates);
            r.push(0); // no entries
            for (uint32_t i = 0; i < translates; i++) r.push(req.arg(int(normals + i)));
            break;
        }
        case 0x100B: { // IsExternalTitleDatabaseInitialized
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

void register_stubs() {
    register_service({"am:app"}, std::make_shared<AmApp>());
    register_service({"ssl:C"}, std::make_shared<SslC>());

    // ndm:u: network daemon manager (background downloads; the game runs offline)
    register_service({"ndm:u"}, std::make_shared<Stub>("ndm:u", std::map<uint16_t, uint32_t>{
                                                                    {0x0001, 0}, // EnterExclusiveState
                                                                    {0x0002, 0}, // LeaveExclusiveState
                                                                    {0x0006, 0}, // SuspendDaemons
                                                                    {0x0007, 0}, // ResumeDaemons
                                                                    {0x0008, 0}, // SuspendScheduler
                                                                    {0x0009, 0}, // ResumeScheduler
                                                                    {0x0014, 0}, // OverrideDefaultDaemons
                                                                    {0x0015, 0}, // ResetDefaultDaemons
                                                                    {0x0016, 1}, // GetDefaultDaemons
                                                                }));

    // nim:aoc: DLC downloads; offline, so just accept the session setup
    register_service({"nim:aoc"}, std::make_shared<Stub>("nim:aoc", std::map<uint16_t, uint32_t>{
                                                                        {0x0021, 0}, // Initialize
                                                                        {0x0022, 0}, // Finalize
                                                                    }));

    // boss: SpotPass background downloads. Offline: no tasks, no data, opted out.
    register_service({"boss:U", "boss:P"}, std::make_shared<Stub>("boss", std::map<uint16_t, uint32_t>{
        {0x0001, 0}, {0x0002, 0}, {0x0003, 0}, {0x0004, 1}, {0x0005, 0}, {0x0006, 0}, {0x0007, 1},
        {0x0008, 0}, {0x0009, 0}, {0x000A, 1}, {0x000B, 0}, {0x000C, 0}, {0x000D, 0}, {0x000E, 0},
        {0x000F, 0}, {0x0010, 2}, {0x0011, 2}, {0x0012, 2}, {0x0013, 2}, {0x0014, 0}, {0x0015, 0},
        {0x0016, 1}, {0x0017, 0}, {0x0018, 0}, {0x0019, 1}, {0x001A, 1}, {0x001B, 1}, {0x001C, 0},
        {0x001D, 0}, {0x001E, 0}, {0x0020, 3}, {0x0021, 3}, {0x0022, 3}, {0x0023, 1}, {0x0024, 1},
        {0x0025, 0}, {0x0026, 0}, {0x0027, 0}, {0x0028, 2}, {0x0029, 0}, {0x002A, 1}, {0x002B, 0},
        {0x002C, 1}, {0x002D, 2}, {0x002E, 1}, {0x002F, 0}, {0x0030, 2}, {0x0031, 0}, {0x0033, 0},
        {0x0034, 1}, {0x0035, 0}, {0x0036, 0}, {0x0037, 0},
    }));
}

} // namespace hle
