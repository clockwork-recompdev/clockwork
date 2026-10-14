// Display transfer engine (tiled <-> linear copies with format conversion and downscaling),
// TextureCopy, and reading the LCD framebuffers for presentation.
#include "internal.h"
#include "pica.h"

#include "gpu/gpu.h"
#include "log.h"
#include "memory.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace pica {
namespace {

// host pointer to `size` bytes at a guest virtual or GPU physical address
uint8_t *any_ptr(uint32_t addr, uint32_t size) {
    if (uint8_t *p = phys(addr, size)) return p;
    if (!size) return nullptr;
    for (uint32_t page = mem::page_down(addr); page < addr + size; page += mem::PAGE)
        if (!mem::is_committed(page)) return nullptr;
    return mem::ptr(addr);
}

} // namespace

void convert_transfer(const uint8_t *src, uint32_t in_w, uint32_t in_h, uint8_t *dst, uint32_t out_w, uint32_t out_h,
                      uint32_t flags) {
    bool flip = flags & 1, input_linear = flags & 2, tiled_both = flags & 0x20;
    uint32_t in_fmt = (flags >> 8) & 7, out_fmt = (flags >> 12) & 7;
    uint32_t scale = (flags >> 24) & 3;
    uint32_t hs = scale ? 1 : 0, vs = scale == 2 ? 1 : 0;
    uint32_t w = out_w >> hs, h = out_h >> vs;
    uint32_t ibpp = fb_bpp(in_fmt), obpp = fb_bpp(out_fmt);
    auto src_offset = [&](uint32_t x, uint32_t y) {
        x = std::min(x, in_w - 1), y = std::min(y, in_h - 1);
        return (input_linear && !tiled_both ? x + y * in_w : tiled_index(x, y, in_w)) * ibpp;
    };
    for (uint32_t y = 0; y < h; y++) {
        uint32_t oy = flip ? h - 1 - y : y;
        for (uint32_t x = 0; x < w; x++) {
            uint32_t ix = x << hs, iy = y << vs;
            Rgba c = fb_decode(in_fmt, src + src_offset(ix, iy));
            if (scale) {
                uint32_t r = c.r, g = c.g, b = c.b, a = c.a, n = 1;
                for (uint32_t dy = 0; dy <= vs; dy++)
                    for (uint32_t dx = 0; dx <= hs; dx++) {
                        if (!dx && !dy) continue;
                        Rgba d = fb_decode(in_fmt, src + src_offset(ix + dx, iy + dy));
                        r += d.r, g += d.g, b += d.b, a += d.a, n++;
                    }
                c = {uint8_t(r / n), uint8_t(g / n), uint8_t(b / n), uint8_t(a / n)};
            }
            uint32_t o = (input_linear || tiled_both) ? tiled_index(x, oy, w) : x + oy * w;
            fb_encode(out_fmt, c, dst + o * obpp);
        }
    }
}

void display_transfer(uint32_t in, uint32_t out, uint32_t in_dims, uint32_t out_dims, uint32_t flags) {
    LOG_GPU("display transfer %08x %ux%u -> %08x %ux%u flags %08x", in, in_dims & 0xFFFF, in_dims >> 16, out,
            out_dims & 0xFFFF, out_dims >> 16, flags);
    uint32_t in_w = in_dims & 0xFFFF, in_h = in_dims >> 16;
    uint32_t out_w = out_dims & 0xFFFF, out_h = out_dims >> 16;
    uint32_t in_fmt = (flags >> 8) & 7, out_fmt = (flags >> 12) & 7, scale = (flags >> 24) & 3;
    scene_frame_done(in_h);
    if (flags & 8) {
        LOG("pica: display transfer in raw-copy mode is not supported");
        return;
    }
    if (in_fmt > 4 || out_fmt > 4 || !in_w || !in_h || !out_w || !out_h) return;
    uint32_t w = out_w >> (scale ? 1 : 0), h = out_h >> (scale == 2 ? 1 : 0);
    const uint8_t *src = any_ptr(in, in_w * in_h * fb_bpp(in_fmt));
    uint8_t *dst = any_ptr(out, w * h * fb_bpp(out_fmt));
    if (!src || !dst) {
        LOG("pica: display transfer %08x -> %08x outside mapped memory", in, out);
        return;
    }
    auto t0 = std::chrono::steady_clock::now();
    if (gpu::display_transfer(in, out, in_w, in_h, out_w, out_h, flags)) return;
    gpu::before_cpu_read(in, in_w * in_h * fb_bpp(in_fmt));
    src = any_ptr(in, in_w * in_h * fb_bpp(in_fmt));
    wide_display_transfer(in, out, in_w, in_h, out_w, flags);
    convert_transfer(src, in_w, in_h, dst, out_w, out_h, flags);
    gpu::invalidate(out, w * h * fb_bpp(out_fmt));
    time_transfer += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
}

void texture_copy(uint32_t in, uint32_t out, uint32_t size, uint32_t in_line, uint32_t out_line, uint32_t) {
    LOG_GPU("texture copy %08x -> %08x (%#x bytes)", in, out, size);
    size &= ~15u;
    uint32_t in_width = (in_line & 0xFFFF) * 16, in_gap = (in_line >> 16) * 16;
    uint32_t out_width = (out_line & 0xFFFF) * 16, out_gap = (out_line >> 16) * 16;
    if (!size || !in_width || !out_width) return;
    // total spans including gaps, for the mapping check
    uint64_t in_span = uint64_t(size) / in_width * (in_width + in_gap) + in_width;
    uint64_t out_span = uint64_t(size) / out_width * (out_width + out_gap) + out_width;
    gpu::before_cpu_read(in, uint32_t(std::min<uint64_t>(in_span, 0x10000000)));
    const uint8_t *src = any_ptr(in, uint32_t(std::min<uint64_t>(in_span, 0x10000000)));
    uint8_t *dst = any_ptr(out, uint32_t(std::min<uint64_t>(out_span, 0x10000000)));
    if (!src || !dst) {
        LOG("pica: texture copy %08x -> %08x outside mapped memory", in, out);
        return;
    }
    gpu::invalidate(out, uint32_t(std::min<uint64_t>(out_span, 0x10000000)));
    uint32_t ipos = 0, opos = 0;
    while (size) {
        uint32_t n = std::min({in_width - ipos, out_width - opos, size});
        std::memmove(dst, src, n);
        src += n, dst += n, ipos += n, opos += n, size -= n;
        if (ipos == in_width) src += in_gap, ipos = 0;
        if (opos == out_width) dst += out_gap, opos = 0;
    }
}

void read_lcd_host(const uint8_t *fb, uint32_t stride, uint32_t format, int width, int height, uint8_t *out) {
    format &= 7;
    if (format > 4) format = FB_RGB8;
    uint32_t bpp = fb_bpp(format);
    if (stride < uint32_t(height) * bpp) stride = uint32_t(height) * bpp;
    // the LCDs scan out rotated: each memory row is one screen column, bottom pixel first
    for (int x = 0; x < width; x++) {
        const uint8_t *col = fb + size_t(x) * stride;
        for (int y = 0; y < height; y++) {
            Rgba c = fb_decode(format, col + size_t(height - 1 - y) * bpp);
            uint8_t *o = out + (size_t(y) * width + x) * 4;
            o[0] = c.r, o[1] = c.g, o[2] = c.b, o[3] = 255;
        }
    }
}

void read_lcd(uint32_t addr, uint32_t stride, uint32_t format, int width, uint8_t *out) {
    uint32_t bpp = fb_bpp((format & 7) > 4 ? FB_RGB8 : format & 7);
    const uint8_t *fb = any_ptr(addr, std::max(stride, 240 * bpp) * uint32_t(width));
    if (!fb) {
        std::memset(out, 0, size_t(width) * 240 * 4);
        return;
    }
    read_lcd_host(fb, stride, format, width, 240, out);
}

} // namespace pica
