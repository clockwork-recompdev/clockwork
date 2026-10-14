// gsp::Gpu: the GPU service. Games talk to it through shared memory (interrupt queues,
// framebuffer info, a GX command queue) plus a few IPC commands.
//
// This file is the plumbing: rights, register writes, interrupts, VBlank timing, framebuffer
// swaps, and dispatching GX commands to the PICA200 implementation (video/). Each VBlank the
// framebuffers the LCDs would scan out are handed to the frontend window.
#include "service.h"

#include "frontend/frontend.h"
#include "log.h"
#include "memory.h"
#include "video/gpu/gpu.h"
#include "video/pica.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

namespace hle {
namespace {

constexpr uint32_t SHARED_SIZE = 0x1000;
constexpr uint32_t RESULT_FIRST_INIT = 0x2A07; // RegisterInterruptRelayQueue: first registration
constexpr uint64_t VBLANK_NS = 16715000;       // ~59.83 Hz

enum Interrupt : uint8_t { PSC0 = 0, PSC1 = 1, PDC0 = 2, PDC1 = 3, PPF = 4, P3D = 5, DMA = 6 };

// GPU I/O registers live at physical 0x10400000; WriteHWRegs addresses are offsets from 0x1EB00000
constexpr uint32_t REG_BASE = 0x1EB00000;

struct Gsp : Service {
    std::shared_ptr<kernel::SharedMemory> shared = std::make_shared<kernel::SharedMemory>();
    std::array<std::shared_ptr<kernel::Event>, 4> events{};
    uint32_t threads = 0;
    std::map<uint32_t, uint32_t> regs; // written GPU registers, for the renderer
    bool vblank_started = false;
    uint32_t command_lists = 0;

    // what each LCD scans out (top, bottom), from SetBufferSwap or the shared-memory info
    struct Lcd {
        uint32_t addr = 0, stride = 0, format = 0;
    } lcd[2];
    std::vector<uint8_t> pixels = std::vector<uint8_t>(frontend::TOP_WIDTH * frontend::SCREEN_HEIGHT * 4);

    Gsp() : Service("gsp::Gpu") {
        shared->size = SHARED_SIZE;
        shared->name = "GSP";
    }

    uint8_t *shm(uint32_t off) { return mem::ptr(shared->address + off); }

    // queues an interrupt for every registered thread and signals its event
    void raise(Interrupt irq) {
        if (!shared->mapped) return;
        for (uint32_t t = 0; t < threads; t++) {
            uint8_t *q = shm(t * 0x40);
            uint8_t index = q[0], count = q[1];
            if (count >= 0x34) {
                q[2] = 1; // missed
                continue;
            }
            q[0xC + (index + count) % 0x34] = irq;
            q[1] = uint8_t(count + 1);
            if (events[t]) events[t]->signal();
        }
    }

    // FrameBufferInfo: active index, left address, right address, stride, format, shown index
    void set_buffer(uint32_t screen, const uint32_t *info) {
        if (screen > 1) return;
        LOG_GPU("screen %u shows %08x (stride %u, format %u)", screen, info[1], info[3], info[4]);
        lcd[screen] = {info[1], info[3], info[4]};
    }

    // games queue framebuffer swaps in shared memory (0x200 + thread * 0x80, one 0x40-byte
    // block per screen); GSP applies them at VBlank
    void apply_swaps() {
        if (!shared->mapped || !threads) return;
        for (uint32_t screen = 0; screen < 2; screen++) {
            uint8_t *u = shm(0x200 + screen * 0x40);
            if (!u[1]) continue;
            uint32_t info[7];
            std::memcpy(info, u + 4 + (u[0] & 1) * 0x1C, sizeof info);
            set_buffer(screen, info);
            u[1] = 0;
        }
    }

    uint32_t frames_rendered = 0; // display transfers to the top screen, for YW2_FPS
    std::mutex gx_lock;

    void present() {
        static const bool fps = std::getenv("YW2_FPS") != nullptr;
        static int vblanks = 0;
        if (fps && ++vblanks % 300 == 0) { // every ~5 s
            pica::SceneInfo si = pica::scene_info();
            LOG("scene: top %d draws (%d 3D), bottom %d draws, %zu bottom textures; %d GPU readbacks in 5 s", si.top_draws,
                si.top_3d_draws, si.bottom_draws, si.bottom_textures.size(), pica::gpu::take_readback_count());
            LOG("fps: %.1f  (per second: commands %.0f ms [draw setup %.0f, vertices %.0f for %lld], raster %.0f ms, "
                "transfers %.0f ms)",
                frames_rendered / 5.0, pica::time_commands.exchange(0) / 5e6, pica::time_setup.exchange(0) / 5e6,
                pica::time_vertices.exchange(0) / 5e6, (long long)(pica::vertex_count.exchange(0) / 5),
                pica::time_raster.exchange(0) / 5e6, pica::time_transfer.exchange(0) / 5e6);
            frames_rendered = 0;
        }
        static const int widths[2] = {frontend::TOP_WIDTH, frontend::BOTTOM_WIDTH};
        for (int screen = 0; screen < 2; screen++) {
            if (!lcd[screen].addr) continue;
            int gw, gh;
            if (SDL_GPUTexture *frame = pica::gpu::lcd_frame(lcd[screen].addr, &gw, &gh)) {
                frontend::present_gpu(screen, frame, gw, gh);
                continue;
            }
            uint32_t columns, height, stride, format;
            if (const uint8_t *wide = pica::wide_lcd(lcd[screen].addr, &columns, &height, &stride, &format)) {
                pixels.resize(size_t(columns) * height * 4);
                pica::read_lcd_host(wide, stride, format, int(columns), int(height), pixels.data());
                frontend::present(screen, pixels.data(), int(columns), int(height));
                continue;
            }
            pixels.resize(size_t(widths[screen]) * frontend::SCREEN_HEIGHT * 4);
            pica::read_lcd(lcd[screen].addr, lcd[screen].stride, lcd[screen].format, widths[screen], pixels.data());
            frontend::present(screen, pixels.data(), widths[screen], frontend::SCREEN_HEIGHT);
        }
    }

    void start_vblank() {
        if (vblank_started) return;
        vblank_started = true;
        kernel::schedule_periodic(VBLANK_NS, [this] {
            apply_swaps();
            present();
            raise(PDC0);
            raise(PDC1);
        });
    }

    // ---- GX commands from the shared-memory queue ----

    void memory_fill(uint32_t start, uint32_t value, uint32_t end, uint32_t control) {
        if (!start || end <= start) return;
        uint32_t width = (control >> 8) & 3; // 0: 16-bit, 1/3: 24-bit, 2: 32-bit
        uint8_t *p = mem::ptr(vaddr(start)), *e = mem::ptr(vaddr(end));
        if (width == 2) {
            for (; p + 4 <= e; p += 4) std::memcpy(p, &value, 4);
        } else if (width == 0) {
            uint16_t v = uint16_t(value);
            for (; p + 2 <= e; p += 2) std::memcpy(p, &v, 2);
        } else {
            for (; p + 3 <= e; p += 3) {
                p[0] = uint8_t(value);
                p[1] = uint8_t(value >> 8);
                p[2] = uint8_t(value >> 16);
            }
        }
    }

    // GX addresses are physical; map the ones apps use (VRAM, FCRAM linear) back to virtual
    static uint32_t vaddr(uint32_t paddr) {
        if (paddr >= 0x18000000 && paddr < 0x18600000) return paddr - 0x18000000 + mem::VRAM_BASE;
        if (paddr >= 0x20000000 && paddr < 0x28000000) return paddr - 0x20000000 + mem::LINEAR_BASE;
        return paddr; // already virtual
    }

    // GPU work runs with the kernel lock released, as the real GPU runs alongside the CPU:
    // this thread stays the scheduler's current thread (no other guest code runs), but timers
    // (audio frames, VBlank, input sampling) keep their pace while a frame renders
    void run_command(const uint32_t *c, std::unique_lock<std::mutex> &lk) {
        // One thread at a time drives the GPU. Waiting for it never holds the kernel lock: the
        // thread rendering below runs without it and needs it back when done.
        std::unique_lock gx(gx_lock, std::defer_lock);
        if (!gx.try_lock()) {
            lk.unlock();
            gx.lock();
            lk.lock();
        }
        switch (c[0] & 0xFF) {
        case 0x00: // RequestDma(src, dst, size)
            pica::gpu::before_cpu_read(c[1], c[3]);
            std::memmove(mem::ptr(c[2]), mem::ptr(c[1]), c[3]);
            pica::gpu::invalidate(c[2], c[3]);
            raise(DMA);
            break;
        case 0x01: // ProcessCommandList(address, size, flags)
            if (command_lists++ == 0) LOG("gsp: first GPU command list (%08x, %#x bytes)", c[1], c[2]);
            if (static const bool hold = std::getenv("YW2_GPU_HOLD_CPU") != nullptr; hold) {
                // old behavior: this thread stays current while the GPU works
                lk.unlock();
                pica::run_command_list(c[1], c[2]);
                lk.lock();
            } else {
                // the GPU works while other guest threads (e.g. the game's map streaming) run
                kernel::Thread *self = kernel::current_thread();
                kernel::release_cpu();
                lk.unlock();
                pica::run_command_list(c[1], c[2]);
                lk.lock();
                kernel::reacquire_cpu(lk, self);
            }
            raise(P3D);
            break;
        case 0x02: // MemoryFill(start0, value0, end0, start1, value1, end1, control)
            LOG_GPU("memory fill %08x-%08x = %08x, %08x-%08x = %08x", c[1], c[3], c[2], c[4], c[6], c[5]);
            memory_fill(c[1], c[2], c[3], c[7] & 0xFFFF);
            memory_fill(c[4], c[5], c[6], c[7] >> 16);
            pica::wide_memory_filled(c[1], c[3]);
            pica::wide_memory_filled(c[4], c[6]);
            pica::gpu::memory_fill(c[1], c[3], c[2], c[7] & 0xFFFF);
            pica::gpu::memory_fill(c[4], c[6], c[5], c[7] >> 16);
            if (c[1]) raise(PSC0);
            if (c[4]) raise(PSC1);
            break;
        case 0x03: // DisplayTransfer(in, out, in dims, out dims, flags)
            if ((c[3] >> 16) == 400) frames_rendered++;
            pica::display_transfer(c[1], c[2], c[3], c[4], c[5]);
            raise(PPF);
            break;
        case 0x04: // TextureCopy(in, out, size, in line/gap, out line/gap, flags)
            pica::texture_copy(c[1], c[2], c[3], c[4], c[5], c[6]);
            raise(PPF);
            break;
        case 0x05: // FlushCacheRegions: no caches here
            break;
        default:
            LOG("gsp: unknown GX command %#x", c[0] & 0xFF);
        }
    }

    void process_queue(uint32_t thread, std::unique_lock<std::mutex> &lk) {
        uint8_t *q = shm(0x800 + thread * 0x200);
        uint8_t index = q[0], count = q[1];
        for (uint8_t i = 0; i < count; i++) {
            uint32_t c[8];
            std::memcpy(c, q + 0x20 + ((index + i) % 15) * 0x20, sizeof c);
            run_command(c, lk);
        }
        q[0] = uint8_t((index + count) % 15);
        q[1] = 0;
    }

    void handle(Request &req) override {
        switch (req.id()) {
        case 0x0001: { // WriteHWRegs(reg offset, size, data in a static buffer)
            uint32_t base = req.arg(0), size = req.arg(1), src = req.arg(3);
            LOG_GPU("WriteHWRegs %#x (%u bytes): %08x", base, size, mem::read32(src));
            for (uint32_t i = 0; i < size; i += 4) regs[REG_BASE + base + i] = mem::read32(src + i);
            req.reply(1, 0);
            break;
        }
        case 0x0002: { // WriteHWRegsWithMask(reg offset, size, data, mask)
            uint32_t base = req.arg(0), size = req.arg(1), data = req.arg(3), mask = req.arg(5);
            for (uint32_t i = 0; i < size; i += 4) {
                uint32_t &r = regs[REG_BASE + base + i], m = mem::read32(mask + i);
                r = (r & ~m) | (mem::read32(data + i) & m);
            }
            req.reply(1, 0);
            break;
        }
        case 0x0004: { // ReadHWRegs(reg offset, size) -> static buffer
            uint32_t base = req.arg(0), size = std::min<uint32_t>(req.arg(1), 0x80);
            uint32_t data[0x20] = {};
            for (uint32_t i = 0; i < size; i += 4) data[i / 4] = regs[REG_BASE + base + i];
            auto r = req.reply(1, 2);
            req.push_static_buffer(r, 0, data, size);
            break;
        }
        case 0x0005: // SetBufferSwap(screen, framebuffer info)
            set_buffer(req.arg(0), &req.in[2]);
            req.reply(1, 0);
            break;
        case 0x0008: // FlushDataCache(address, size, process)
        case 0x0009: // InvalidateDataCache
        case 0x000A: // RegisterInterruptEvents
        case 0x000B: // SetLcdForceBlack
        case 0x0010: // SetAxiConfigQoSMode
        case 0x0014: // UnregisterInterruptRelayQueue
        case 0x0016: // AcquireRight
        case 0x0017: // ReleaseRight
        case 0x0019: // SaveVramSysArea
        case 0x001A: // RestoreVramSysArea
        case 0x001E: // SetLedForceOff
        case 0x001F: // SetInternalPriorities
            req.reply(1, 0);
            break;
        case 0x000C: // TriggerCmdReqQueue
            for (uint32_t t = 0; t < threads; t++) process_queue(t, *req.lock);
            req.reply(1, 0);
            break;
        case 0x0013: { // RegisterInterruptRelayQueue(flags, event) -> thread index, shared memory
            uint32_t t = threads < 4 ? threads++ : 3;
            events[t] = kernel::get<kernel::Event>(req.arg(2));
            auto r = req.reply(2, 2, RESULT_FIRST_INIT);
            r.push(t);
            r.push_handles({kernel::add_handle(shared)});
            start_vblank();
            break;
        }
        case 0x0018: { // ImportDisplayCaptureInfo -> per screen: left, right, format, stride
            auto r = req.reply(9, 0);
            for (auto &l : lcd) {
                r.push(l.addr);
                r.push(l.addr);
                r.push(l.format);
                r.push(l.stride);
            }
            break;
        }
        default:
            unimplemented(*this, req);
        }
    }
};

} // namespace

void register_gsp() { register_service({"gsp::Gpu"}, std::make_shared<Gsp>()); }

} // namespace hle
