// dsp::DSP: the audio DSP service: component loading, interrupt events, pipes, and the audio
// frame clock. Each frame the firmware HLE in audio/ mixes the game's voices and outputs them.
#include "service.h"

#include "audio/dsp_hle.h"
#include "log.h"
#include "memory.h"

#include <chrono>
#include <cstring>
#include <deque>
#include <map>

namespace hle {
namespace {

constexpr uint32_t DSP_RAM = 0x1FF00000;
constexpr uint32_t DSP_RAM_SIZE = 0x80000;
constexpr uint64_t FRAME_NS = uint64_t(audio::SAMPLES_PER_FRAME) * 1000000000ull / audio::SAMPLE_RATE;
constexpr uint32_t PIPE_AUDIO = 2;

// DSP word addresses of the shared structures, as the DSP firmware reports them on initialization:
// frame counter, source configurations, source statuses, ADPCM coefficients, DSP configuration,
// DSP status, final samples, intermediate mix, compressor, debug, and five unknown blocks
constexpr uint16_t STRUCT_ADDRESSES[15] = {0xBFFF, 0x9E92, 0x8680, 0xA792, 0x9430, 0x8400, 0x8540, 0x9492,
                                           0x8710, 0x8410, 0xA912, 0xAA12, 0xAAD2, 0xAC52, 0xAC5C};

struct Dsp : Service {
    std::shared_ptr<kernel::Event> semaphore_event =
        std::make_shared<kernel::Event>(kernel::ResetType::OneShot, "DSP semaphore");
    std::map<std::pair<uint32_t, uint32_t>, std::shared_ptr<kernel::Event>> interrupts; // (type, channel)
    std::map<uint32_t, std::deque<uint8_t>> pipes;                                       // DSP -> app data
    bool running = false, clock_started = false;

    Dsp() : Service("dsp::DSP") {}

    void signal(uint32_t type, uint32_t channel) {
        auto it = interrupts.find({type, channel});
        if (it != interrupts.end() && it->second) it->second->signal();
    }

    void start_clock() {
        if (clock_started) return;
        clock_started = true;
        kernel::schedule_periodic(FRAME_NS, [this] {
            if (running) {
                if (log_cfg::dsp) { // audio clock jitter: the longest gap between frames, per second
                    static auto last = std::chrono::steady_clock::now();
                    static double worst = 0;
                    static int n = 0;
                    auto now = std::chrono::steady_clock::now();
                    worst = std::max(worst, std::chrono::duration<double, std::milli>(now - last).count());
                    last = now;
                    if (++n % 200 == 0) LOG_DSP("longest gap between audio frames: %.1f ms", worst), worst = 0;
                }
                audio::tick();
                signal(2, PIPE_AUDIO); // "pipe" interrupt on the audio pipe each frame
            }
        });
    }

    void write_audio_pipe(uint32_t src, uint32_t size) {
        // the first byte is a state change: 0 initialize, 1 shutdown, 2 wakeup, 3 sleep
        uint8_t state = size ? mem::ptr(src)[0] : 0xFF;
        if (state == 0 || state == 2) {
            auto &pipe = pipes[PIPE_AUDIO];
            pipe.clear();
            auto push16 = [&](uint16_t v) {
                pipe.push_back(uint8_t(v));
                pipe.push_back(uint8_t(v >> 8));
            };
            push16(15);
            for (uint16_t a : STRUCT_ADDRESSES) push16(a);
            if (state == 0) audio::reset();
            running = true;
            LOG("dsp: audio %s", state == 0 ? "initialized" : "woken up");
            signal(2, PIPE_AUDIO);
        } else if (state == 1 || state == 3) {
            running = false;
        }
    }

    void handle(Request &req) override {
        switch (req.id()) {
        case 0x0001: { // RecvData(register) -> value; 1 = DSP ready
            auto r = req.reply(2, 0);
            r.push(1);
            break;
        }
        case 0x0002:   // RecvDataIsReady
        case 0x0004: { // SendDataIsEmpty
            auto r = req.reply(2, 0);
            r.push(1);
            break;
        }
        case 0x0003: // SendData
        case 0x0007: // SetSemaphore
        case 0x0012: // UnloadComponent
        case 0x0013: // FlushDataCache
        case 0x0014: // InvalidateDataCache
        case 0x0017: // SetSemaphoreMask
        case 0x0021: // ForceHeadphoneOut
            req.reply(1, 0);
            break;
        case 0x000C: { // ConvertProcessAddressFromDspDram(dsp address)
            auto r = req.reply(2, 0);
            r.push((req.arg(0) << 1) + DSP_RAM + 0x40000);
            break;
        }
        case 0x000D: // WriteProcessPipe(channel, size, buffer)
            if (req.arg(0) == PIPE_AUDIO) write_audio_pipe(req.arg(3), req.arg(1));
            req.reply(1, 0);
            break;
        case 0x000E:   // ReadPipe(channel, peer, size)
        case 0x0010: { // ReadPipeIfPossible(channel, peer, size) -> bytes read, static buffer
            auto &pipe = pipes[req.arg(0)];
            uint32_t want = std::min<uint32_t>(req.arg(2) & 0xFFFF, 0x100);
            uint8_t buf[0x100];
            uint32_t n = std::min<uint32_t>(want, uint32_t(pipe.size()));
            for (uint32_t i = 0; i < n; i++) {
                buf[i] = pipe.front();
                pipe.pop_front();
            }
            bool if_possible = req.id() == 0x0010;
            auto r = req.reply(if_possible ? 2 : 1, 2);
            if (if_possible) r.push(n);
            req.push_static_buffer(r, 0, buf, n);
            break;
        }
        case 0x0011: { // LoadComponent(size, program mask, data mask, buffer) -> loaded
            uint32_t desc = req.arg(3), addr = req.arg(4);
            auto r = req.reply(2, 2);
            r.push(1);
            r.push(desc);
            r.push(addr);
            break;
        }
        case 0x0015: // RegisterInterruptEvents(interrupt, channel, event)
            interrupts[{req.arg(0), req.arg(1)}] = kernel::get<kernel::Event>(req.arg(3));
            start_clock();
            req.reply(1, 0);
            break;
        case 0x0016: { // GetSemaphoreEventHandle
            auto r = req.reply(1, 2);
            r.push_handles({kernel::add_handle(semaphore_event)});
            break;
        }
        case 0x001F: { // GetHeadphoneStatus
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

void register_dsp() {
    mem::commit(DSP_RAM, DSP_RAM_SIZE);
    register_service({"dsp::DSP"}, std::make_shared<Dsp>());
}

} // namespace hle
