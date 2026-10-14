// y2r:u: the hardware YUV -> RGB converter, used for video frames (the game decodes its
// movies in software and converts each frame here). Conversions run synchronously when
// started; the coefficient math follows the hardware's fixed point (3dbrew, Citra).
#include "service.h"

#include "log.h"
#include "memory.h"
#include "video/color.h"
#include "video/gpu/gpu.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace hle {
namespace {

enum InputFormat : uint8_t { YUV422_8, YUV420_8, YUV422_16, YUV420_16, YUYV422 };
enum OutputFormat : uint8_t { RGBA8, RGB8, RGB5A1, RGB565 };

constexpr int16_t STANDARD_COEFFICIENTS[4][8] = {
    {0x100, 0x166, 0xB6, 0x58, 0x1C5, -0x166F, 0x10EE, -0x1C5B}, // ITU-R BT.601
    {0x100, 0x193, 0x77, 0x2F, 0x1DB, -0x1933, 0xA7C, -0x1D51},  // ITU-R BT.709
    {0x12A, 0x198, 0xD0, 0x64, 0x204, -0x1BDE, 0x10F2, -0x229B}, // BT.601, TV range
    {0x12A, 0x1CA, 0x88, 0x36, 0x21C, -0x1F04, 0x99C, -0x2421},  // BT.709, TV range
};

// a DMA-style buffer: `unit` bytes are transferred, then `gap` bytes skipped
struct Buffer {
    uint32_t addr = 0, size = 0, unit = 0, gap = 0;

    template <typename F> void each_run(uint32_t total, F f) const {
        uint32_t at = addr, done = 0;
        uint32_t step = unit ? unit : total;
        while (done < total) {
            uint32_t n = std::min(step, total - done);
            for (uint32_t page = mem::page_down(at); page < at + n; page += mem::PAGE)
                if (!mem::is_committed(page)) return;
            f(mem::ptr(at), at, done, n);
            done += n;
            at += n + gap;
        }
    }
    std::vector<uint8_t> read(uint32_t total) const {
        std::vector<uint8_t> v(total);
        each_run(total, [&](uint8_t *p, uint32_t, uint32_t off, uint32_t n) { std::memcpy(v.data() + off, p, n); });
        return v;
    }
    // the GPU renderer is told, so it doesn't keep showing its older copy of that memory (a movie's
    // frames decoded into a texture or framebuffer stayed black on the GPU renderer)
    void write(const std::vector<uint8_t> &v) const {
        each_run(uint32_t(v.size()), [&](uint8_t *p, uint32_t guest, uint32_t off, uint32_t n) {
            std::memcpy(p, v.data() + off, n);
            pica::gpu::invalidate(guest, n);
        });
    }
};

struct Y2r : Service {
    std::shared_ptr<kernel::Event> transfer_end =
        std::make_shared<kernel::Event>(kernel::ResetType::OneShot, "Y2R transfer end");
    uint8_t input_format = 0, output_format = 0, rotation = 0, block_alignment = 0;
    uint16_t line_width = 0, lines = 0, alpha = 0xFF;
    int16_t coefficients[8];
    Buffer y, u, v, yuyv, out;
    bool warned_rotation = false;

    Y2r() : Service("y2r:u") { std::memcpy(coefficients, STANDARD_COEFFICIENTS[0], sizeof coefficients); }

    Buffer buffer_args(Request &req) { return {req.arg(0), req.arg(1), req.arg(2) & 0xFFFF, req.arg(3) & 0xFFFF}; }

    void convert() {
        uint32_t w = line_width, h = lines;
        if (!w || !h) return;
        if (rotation && !warned_rotation) {
            LOG("y2r: rotation %u not implemented; output unrotated", rotation);
            warned_rotation = true;
        }
        bool wide = input_format == YUV422_16 || input_format == YUV420_16;
        bool half_v = input_format == YUV420_8 || input_format == YUV420_16;
        uint32_t cw = w / 2, ch = half_v ? h / 2 : h, sample = wide ? 2 : 1;
        std::vector<uint8_t> yb, ub, vb;
        if (input_format == YUYV422) {
            yb = yuyv.read(w * h * 2);
        } else {
            yb = y.read(w * h * sample);
            ub = u.read(cw * ch * sample);
            vb = v.read(cw * ch * sample);
        }
        auto at = [&](const std::vector<uint8_t> &b, uint32_t i) { return int(b[i * sample + (wide ? 1 : 0)]); };

        uint32_t bpp = output_format == RGBA8 ? 4 : output_format == RGB8 ? 3 : 2;
        static constexpr uint32_t FB[4] = {pica::FB_RGBA8, pica::FB_RGB8, pica::FB_RGB5A1, pica::FB_RGB565};
        std::vector<uint8_t> dst(size_t(w) * h * bpp);
        const int16_t *c = coefficients;
        for (uint32_t row = 0; row < h; row++) {
            for (uint32_t x = 0; x < w; x++) {
                int Y, U, V;
                if (input_format == YUYV422) {
                    const uint8_t *p = &yb[(size_t(row) * w + (x & ~1u)) * 2];
                    Y = p[(x & 1) * 2], U = p[1], V = p[3];
                } else {
                    uint32_t ci = (half_v ? row / 2 : row) * cw + x / 2;
                    Y = at(yb, row * w + x), U = at(ub, ci), V = at(vb, ci);
                }
                int cy = c[0] * Y;
                int r = ((cy + c[1] * V) >> 3) + c[5] + 0x18;
                int g = ((cy - c[2] * V - c[3] * U) >> 3) + c[6] + 0x18;
                int b = ((cy + c[4] * U) >> 3) + c[7] + 0x18;
                pica::Rgba px = {uint8_t(std::clamp(r >> 5, 0, 255)), uint8_t(std::clamp(g >> 5, 0, 255)),
                                 uint8_t(std::clamp(b >> 5, 0, 255)), uint8_t(alpha)};
                uint32_t index = block_alignment ? pica::tiled_index(x, row, w) : row * w + x;
                pica::fb_encode(FB[output_format & 3], px, &dst[size_t(index) * bpp]);
            }
        }
        out.write(dst);
    }

    void handle(Request &req) override {
        switch (req.id()) {
        case 0x0001: input_format = uint8_t(req.arg(0)); req.reply(1, 0); break;
        case 0x0003: output_format = uint8_t(req.arg(0)); req.reply(1, 0); break;
        case 0x0005: rotation = uint8_t(req.arg(0)); req.reply(1, 0); break;
        case 0x0007: block_alignment = uint8_t(req.arg(0)); req.reply(1, 0); break;
        case 0x0010: y = buffer_args(req); req.reply(1, 0); break;
        case 0x0011: u = buffer_args(req); req.reply(1, 0); break;
        case 0x0012: v = buffer_args(req); req.reply(1, 0); break;
        case 0x0013: yuyv = buffer_args(req); req.reply(1, 0); break;
        case 0x0018: out = buffer_args(req); req.reply(1, 0); break;
        case 0x001A: line_width = uint16_t(req.arg(0)); req.reply(1, 0); break;
        case 0x001C: lines = uint16_t(req.arg(0)); req.reply(1, 0); break;
        case 0x001E: // SetCoefficient(8 x s16)
            for (int i = 0; i < 8; i++) coefficients[i] = int16_t(req.arg(i / 2) >> (16 * (i & 1)));
            req.reply(1, 0);
            break;
        case 0x0020: // SetStandardCoefficient(index)
            if (req.arg(0) < 4) std::memcpy(coefficients, STANDARD_COEFFICIENTS[req.arg(0)], sizeof coefficients);
            req.reply(1, 0);
            break;
        case 0x0022: alpha = uint16_t(req.arg(0)); req.reply(1, 0); break;
        case 0x0029: { // SetPackageParameter(formats, rotation, alignment, size, coefficient, alpha)
            uint32_t a = req.arg(0), b = req.arg(1), d = req.arg(2);
            input_format = uint8_t(a), output_format = uint8_t(a >> 8), rotation = uint8_t(a >> 16);
            block_alignment = uint8_t(a >> 24);
            line_width = uint16_t(b), lines = uint16_t(b >> 16);
            if ((d & 0xFF) < 4) std::memcpy(coefficients, STANDARD_COEFFICIENTS[d & 0xFF], sizeof coefficients);
            alpha = uint16_t(d >> 16);
            req.reply(1, 0);
            break;
        }
        // dithering, interrupt enable, stop, driver init/finalize
        case 0x0009: case 0x000B: case 0x000D: case 0x0024: case 0x0027: case 0x002B: case 0x002C:
            req.reply(1, 0);
            break;
        case 0x000F: { // GetTransferEndEvent
            auto r = req.reply(1, 2);
            r.push_handles({kernel::add_handle(transfer_end)});
            break;
        }
        case 0x0026: // StartConversion: done synchronously
            convert();
            transfer_end->signal();
            req.reply(1, 0);
            break;
        // queries: sending/receiving finished, busy (no), ping
        case 0x0014: case 0x0015: case 0x0016: case 0x0017: case 0x0019: case 0x0028: case 0x002A: {
            auto r = req.reply(2, 0);
            r.push(req.id() == 0x0028 ? 0 : 1);
            break;
        }
        default:
            unimplemented(*this, req);
        }
    }
};

} // namespace

void register_y2r() { register_service({"y2r:u"}, std::make_shared<Y2r>()); }

} // namespace hle
