// err:f: the game's fatal-error reporter. Prints what the game reported and stops.
#include "service.h"

#include "log.h"

namespace hle {
namespace {

struct ErrF : Service {
    ErrF() : Service("err:f") {}

    void handle(Request &req) override {
        if (req.id() != 0x0001) unimplemented(*this, req);
        // ThrowFatalError: error info struct in the command buffer starting at word 1
        const uint32_t *w = req.in + 1;
        uint8_t type = uint8_t(w[0] & 0xFF);
        static const char *types[] = {"generic", "corrupted", "card removed", "exception", "result failure", "logged"};
        const char *name = type < 6 ? types[type] : "unknown";
        if (type == 3) {
            fatal("game reported a fatal %s error: exception type %u at pc=%08x (lr=%08x sp=%08x)", name, w[8] & 0xFF,
                  w[11 + 15], w[11 + 14], w[11 + 13]);
        }
        // generic/result-failure layout: [type, ?, result code, pc, process id, title id lo/hi, ...]
        fatal("game reported a fatal %s error: result %08x at pc=%08x (title %08x%08x)", name, w[2], w[3], w[6], w[5]);
    }
};

} // namespace

std::shared_ptr<Service> make_errf() { return std::make_shared<ErrF>(); }

} // namespace hle
