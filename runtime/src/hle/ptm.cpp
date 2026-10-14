// ptm:u: power state and pedometer. Reports a charging, lid-open console with a full battery
// and no steps walked.
#include "service.h"

#include "memory.h"

#include <cstring>

namespace hle {
namespace {

struct Ptm : Service {
    Ptm() : Service("ptm") {}

    void reply_u32(Request &req, uint32_t v) {
        auto r = req.reply(2, 0);
        r.push(v);
    }

    void handle(Request &req) override {
        switch (req.id()) {
        case 0x0001: // RegisterAlarmClient
        case 0x0002: // SetRtcAlarm
        case 0x0004: // CancelRtcAlarm
        case 0x000D: // SetPedometerRecordingMode
            req.reply(1, 0);
            break;
        case 0x0003: { // GetRtcAlarm
            auto r = req.reply(3, 0);
            r.push64(0);
            break;
        }
        case 0x0005: reply_u32(req, 1); break; // GetAdapterState: charger connected
        case 0x0006: reply_u32(req, 1); break; // GetShellState: lid open
        case 0x0007: reply_u32(req, 5); break; // GetBatteryLevel: full
        case 0x0008: reply_u32(req, 1); break; // GetBatteryChargeState: charging
        case 0x0009: reply_u32(req, 0); break; // GetPedometerState: not counting
        case 0x000C: reply_u32(req, 0); break; // GetTotalStepCount
        case 0x000E: reply_u32(req, 0); break; // GetPedometerRecordingMode
        case 0x000B: { // GetStepHistory(hours, start time) -> u16 steps per hour, into a mapped buffer
            uint32_t hours = req.arg(0), desc = req.arg(3), dest = req.arg(4);
            std::memset(mem::ptr(dest), 0, hours * 2);
            auto r = req.reply(1, 2);
            r.push(desc);
            r.push(dest);
            break;
        }
        default:
            unimplemented(*this, req);
        }
    }
};

} // namespace

void register_ptm() { register_service({"ptm:u", "ptm:play", "ptm:gets", "ptm:sysm", "ptm:s"}, std::make_shared<Ptm>()); }

} // namespace hle
