// Enhanced top-screen rendering: widescreen and higher internal resolution.
//
// Draws to a top-screen render target (400 rows tall, because the 3DS renders rotated) go to
// a host-side "shadow" copy of the target that is `scale` times larger in both directions and
// has `margin` extra rows on each side (16:9). Perspective (3D) triangles are clipped against
// a correspondingly wider frustum, so the extra columns show more of the scene rather than a
// stretched picture. The game's own buffer is kept in sync (downsampled, visible rows only)
// before anything reads it, and display transfers out of the shadow also produce the full-size
// frame for the window.
#include "internal.h"
#include "pica.h"

#include "memory.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <vector>

namespace pica {

int wide_margin = 0;

namespace {

// set from the window thread; the game thread adopts them at the start of a draw, so a draw
// never sees them change halfway
std::atomic<int> requested_margin{0}, requested_scale{1};
int scale = 1;

constexpr uint32_t TOP_ROWS = 400; // a top-screen render target's height (the screen's width)

struct Shadow {
    uint32_t color_addr = 0, depth_addr = 0; // GPU physical addresses of the game's buffers
    uint32_t width = 0, color_format = 0, depth_format = 0; // the game's width (unscaled)
    std::vector<uint8_t> color, depth; // columns() x rows(), tiled like the originals
    int margin = 0, scale = 1;

    uint32_t rows() const { return (TOP_ROWS + 2 * uint32_t(margin)) * uint32_t(scale); }
    uint32_t columns() const { return width * uint32_t(scale); }
} shadow;

struct WideFrame {
    std::vector<uint8_t> pixels; // LCD layout: one row of `column_height` pixels per screen column
    uint32_t columns, column_height, stride, format;
};
std::map<uint32_t, WideFrame> wide_frames; // by LCD framebuffer address

uint32_t to_physical(uint32_t addr) {
    if (addr >= mem::VRAM_BASE && addr < mem::VRAM_BASE + mem::VRAM_SIZE) return addr - mem::VRAM_BASE + 0x18000000;
    if (addr >= mem::LINEAR_BASE && addr < mem::LINEAR_BASE + 0x08000000) return addr - mem::LINEAR_BASE + 0x20000000;
    return addr;
}

uint32_t depth_bpp(uint32_t format) { return format == 0 ? 2 : format == 3 ? 4 : 3; }

// copies the visible rows of the shadow color buffer into the game's buffer, averaging each
// scale x scale block
void sync_to_guest() {
    if (shadow.color.empty()) return;
    uint32_t fmt = shadow.color_format, bpp = fb_bpp(fmt), w = shadow.width, m = uint32_t(shadow.margin);
    uint32_t s = uint32_t(shadow.scale), sw = shadow.columns(), n = s * s;
    uint8_t *guest = phys(shadow.color_addr, w * TOP_ROWS * bpp);
    if (!guest) return;
    for (uint32_t y = 0; y < TOP_ROWS; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint8_t *dst = guest + tiled_index(x, y, w) * bpp;
            if (s == 1) {
                std::memcpy(dst, shadow.color.data() + tiled_index(x, y + m, sw) * bpp, bpp);
                continue;
            }
            uint32_t r = 0, g = 0, b = 0, a = 0;
            for (uint32_t j = 0; j < s; j++)
                for (uint32_t i = 0; i < s; i++) {
                    Rgba c = fb_decode(fmt, shadow.color.data() + tiled_index(x * s + i, (y + m) * s + j, sw) * bpp);
                    r += c.r, g += c.g, b += c.b, a += c.a;
                }
            fb_encode(fmt, {uint8_t(r / n), uint8_t(g / n), uint8_t(b / n), uint8_t(a / n)}, dst);
        }
}

} // namespace

void set_widescreen(bool on) { requested_margin = on ? 14 : 0; } // 428 columns: 16:9 within a pixel
void set_resolution_scale(int s) { requested_scale = s < 1 ? 1 : s > 4 ? 4 : s; }
void requested_enhancement(int *margin, int *s) { *margin = requested_margin, *s = requested_scale; }

namespace {
std::atomic<int> hud_percent{100};
}
void set_hud_scale(float scale) { hud_percent = int(std::clamp(scale, 0.4f, 1.0f) * 100.0f + 0.5f); }
float hud_scale() { return float(hud_percent) / 100.0f; }

bool wide_target(uint32_t color_addr, uint32_t depth_addr, uint32_t width, uint32_t height, uint32_t color_format,
                 uint32_t depth_format, ShadowTarget *out) {
    wide_margin = requested_margin, scale = requested_scale;
    if ((!wide_margin && scale == 1) || height != TOP_ROWS || !color_addr) return false;
    if (shadow.color_addr != color_addr || shadow.width != width || shadow.color_format != color_format ||
        shadow.depth_format != depth_format || shadow.margin != wide_margin || shadow.scale != scale) {
        shadow.color_addr = color_addr, shadow.width = width, shadow.margin = wide_margin, shadow.scale = scale;
        shadow.color_format = color_format, shadow.depth_format = depth_format;
        // tiled surfaces are stored in whole 8-row tiles (428 rows take 432 rows of memory)
        size_t pixels = size_t(shadow.columns()) * ((shadow.rows() + 7) & ~7u);
        shadow.color.assign(pixels * fb_bpp(color_format), 0);
        shadow.depth.assign(pixels * depth_bpp(depth_format), 0);
    }
    if (depth_addr) shadow.depth_addr = depth_addr; // remembered across draws without depth, for clears
    out->color = shadow.color.data();
    out->depth = depth_addr ? shadow.depth.data() : nullptr;
    out->width = shadow.columns(), out->height = shadow.rows();
    out->scale = scale, out->margin = wide_margin;
    return true;
}

void wide_before_texture(uint32_t addr) {
    if (!shadow.color.empty() && addr == shadow.color_addr) sync_to_guest();
}

void wide_memory_filled(uint32_t start, uint32_t end) {
    if (shadow.color.empty()) return;
    start = to_physical(start), end = to_physical(end);
    // repeat the pattern the fill wrote at the start of the game's buffer over the whole shadow
    auto fill = [&](uint32_t addr, std::vector<uint8_t> &buf, uint32_t bpp) {
        if (!addr || start != addr || end <= start) return;
        const uint8_t *p = phys(addr, bpp);
        if (!p) return;
        for (size_t i = 0; i + bpp <= buf.size(); i += bpp) std::memcpy(&buf[i], p, bpp);
    };
    fill(shadow.color_addr, shadow.color, fb_bpp(shadow.color_format));
    fill(shadow.depth_addr, shadow.depth, depth_bpp(shadow.depth_format));
}

void wide_display_transfer(uint32_t in, uint32_t out, uint32_t in_w, uint32_t in_h, uint32_t out_w, uint32_t flags) {
    if (shadow.color.empty() || to_physical(in) != shadow.color_addr || in_h != TOP_ROWS || in_w != shadow.width ||
        (!wide_margin && scale == 1)) {
        wide_frames.erase(out); // this framebuffer now holds an ordinary frame
        return;
    }
    uint32_t rows = shadow.rows(), s = uint32_t(shadow.scale);
    uint32_t out_fmt = (flags >> 12) & 7, downscale = (flags >> 24) & 3;
    uint32_t column_height = (out_w * s) >> (downscale ? 1 : 0); // output pixels per row (screen height)
    WideFrame &f = wide_frames[out];
    f.columns = rows, f.column_height = column_height, f.stride = column_height * fb_bpp(out_fmt), f.format = out_fmt;
    f.pixels.assign(size_t(f.stride) * rows, 0);
    convert_transfer(shadow.color.data(), shadow.columns(), rows, f.pixels.data(), out_w * s, rows, flags);
    sync_to_guest();
}

const uint8_t *wide_lcd(uint32_t addr, uint32_t *columns, uint32_t *column_height, uint32_t *stride,
                        uint32_t *format) {
    if (!wide_margin && scale == 1) return nullptr;
    auto it = wide_frames.find(addr);
    if (it == wide_frames.end()) return nullptr;
    const WideFrame &f = it->second;
    *columns = f.columns, *column_height = f.column_height, *stride = f.stride, *format = f.format;
    return f.pixels.data();
}

} // namespace pica
