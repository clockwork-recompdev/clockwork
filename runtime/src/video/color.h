#pragma once
// Pixel formats and tiling shared by the rasterizer, display transfers and the LCD.
#include <cstdint>

namespace pica {

struct Rgba {
    uint8_t r, g, b, a;
};

// Framebuffer formats (color buffer, display transfer, LCD). Texture formats number differently.
enum FbFormat : uint32_t { FB_RGBA8 = 0, FB_RGB8 = 1, FB_RGB565 = 2, FB_RGB5A1 = 3, FB_RGBA4 = 4 };

inline uint32_t fb_bpp(uint32_t format) { return format == FB_RGBA8 ? 4 : format == FB_RGB8 ? 3 : 2; }

inline uint8_t expand4(uint32_t v) { return uint8_t(v * 17); }
inline uint8_t expand5(uint32_t v) { return uint8_t((v << 3) | (v >> 2)); }
inline uint8_t expand6(uint32_t v) { return uint8_t((v << 2) | (v >> 4)); }

inline uint16_t load16(const uint8_t *p) { return uint16_t(p[0] | (p[1] << 8)); }
inline void store16(uint8_t *p, uint32_t v) {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
}

// multi-byte formats are little-endian words, so RGBA8 is A, B, G, R in memory
inline Rgba fb_decode(uint32_t format, const uint8_t *p) {
    switch (format) {
    case FB_RGBA8: return {p[3], p[2], p[1], p[0]};
    case FB_RGB8: return {p[2], p[1], p[0], 255};
    case FB_RGB565: {
        uint32_t v = load16(p);
        return {expand5(v >> 11), expand6((v >> 5) & 0x3F), expand5(v & 0x1F), 255};
    }
    case FB_RGB5A1: {
        uint32_t v = load16(p);
        return {expand5(v >> 11), expand5((v >> 6) & 0x1F), expand5((v >> 1) & 0x1F), uint8_t(v & 1 ? 255 : 0)};
    }
    default: {
        uint32_t v = load16(p);
        return {expand4(v >> 12), expand4((v >> 8) & 0xF), expand4((v >> 4) & 0xF), expand4(v & 0xF)};
    }
    }
}

inline void fb_encode(uint32_t format, Rgba c, uint8_t *p) {
    switch (format) {
    case FB_RGBA8:
        p[0] = c.a, p[1] = c.b, p[2] = c.g, p[3] = c.r;
        break;
    case FB_RGB8:
        p[0] = c.b, p[1] = c.g, p[2] = c.r;
        break;
    case FB_RGB565: store16(p, ((c.r >> 3) << 11) | ((c.g >> 2) << 5) | (c.b >> 3)); break;
    case FB_RGB5A1: store16(p, ((c.r >> 3) << 11) | ((c.g >> 3) << 6) | ((c.b >> 3) << 1) | (c.a >> 7)); break;
    default: store16(p, ((c.r >> 4) << 12) | ((c.g >> 4) << 8) | ((c.b >> 4) << 4) | (c.a >> 4));
    }
}

// GPU surfaces are 8x8 tiles stored row by row; pixels inside a tile are in Morton (Z) order
inline uint32_t morton8(uint32_t x, uint32_t y) {
    return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
}
// pixel index of (x, y) in a tiled surface `width` pixels wide
inline uint32_t tiled_index(uint32_t x, uint32_t y, uint32_t width) {
    return (y & ~7u) * width + (x & ~7u) * 8 + morton8(x, y);
}

} // namespace pica
