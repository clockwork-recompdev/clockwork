// Texture units: format decoding (all formats are 8x8-tiled, ETC1 in 4x4 blocks), wrapping
// and filtering. Rows are stored bottom to top.
#include "internal.h"
#include "pica.h"

#include "log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>

namespace pica {
namespace {

enum TexFormat : uint32_t {
    T_RGBA8, T_RGB8, T_RGB5A1, T_RGB565, T_RGBA4, T_IA8, T_RG8, T_I8, T_A8, T_IA4, T_I4, T_A4, T_ETC1, T_ETC1A4,
};
constexpr uint32_t BITS[16] = {32, 24, 16, 16, 16, 16, 16, 8, 8, 8, 4, 4, 4, 8, 0, 0};

// per unit: border, dims, params, address registers start here; format registers separately
constexpr uint32_t UNIT_BASE[3] = {TEX0_BORDER, 0x091, 0x099};
constexpr uint32_t UNIT_FORMAT[3] = {TEX0_FORMAT, 0x096, 0x09E};

Rgba etc1(uint64_t block, uint32_t x, uint32_t y) {
    static constexpr int MODIFIERS[8][2] = {{2, 8}, {5, 17}, {9, 29}, {13, 42}, {18, 60}, {24, 80}, {33, 106}, {47, 183}};
    uint32_t texel = 4 * x + y;
    if ((block >> 32) & 1) std::swap(x, y); // flip: sub-blocks are stacked vertically
    bool second = x >= 2;
    int r, g, b;
    if ((block >> 33) & 1) { // differential: 5-bit base, 3-bit signed delta for the second
        auto delta = [&](int shift) { return int(int64_t(block << (61 - shift)) >> 61); };
        r = int((block >> 59) & 31), g = int((block >> 51) & 31), b = int((block >> 43) & 31);
        if (second) r += delta(56), g += delta(48), b += delta(40);
        r = expand5(uint32_t(r) & 31), g = expand5(uint32_t(g) & 31), b = expand5(uint32_t(b) & 31);
    } else {
        int shift = second ? 0 : 4;
        r = expand4((block >> (56 + shift)) & 15), g = expand4((block >> (48 + shift)) & 15),
        b = expand4((block >> (40 + shift)) & 15);
    }
    uint32_t table = second ? (block >> 34) & 7 : (block >> 37) & 7;
    int m = MODIFIERS[table][(block >> texel) & 1];
    if ((block >> (16 + texel)) & 1) m = -m;
    auto clamp = [](int v) { return uint8_t(std::clamp(v, 0, 255)); };
    return {clamp(r + m), clamp(g + m), clamp(b + m), 255};
}

Rgba texel(const Texture &t, uint32_t x, uint32_t y) {
    const uint8_t *d = t.data;
    if (t.format >= T_ETC1) {
        bool alpha = t.format == T_ETC1A4;
        const uint8_t *tile = d + ((y / 8) * (t.width / 8) + x / 8) * (alpha ? 64 : 32);
        const uint8_t *sub = tile + ((x % 8) / 4 + 2 * ((y % 8) / 4)) * (alpha ? 16 : 8);
        x %= 4, y %= 4;
        uint8_t a = 255;
        uint64_t word;
        if (alpha) {
            std::memcpy(&word, sub, 8);
            a = expand4((word >> (4 * (4 * x + y))) & 15);
            sub += 8;
        }
        std::memcpy(&word, sub, 8);
        Rgba c = etc1(word, x, y);
        c.a = a;
        return c;
    }
    uint32_t i = tiled_index(x, y, t.width);
    switch (t.format) {
    case T_RGBA8: return fb_decode(FB_RGBA8, d + i * 4);
    case T_RGB8: return fb_decode(FB_RGB8, d + i * 3);
    case T_RGB5A1: return fb_decode(FB_RGB5A1, d + i * 2);
    case T_RGB565: return fb_decode(FB_RGB565, d + i * 2);
    case T_RGBA4: return fb_decode(FB_RGBA4, d + i * 2);
    case T_IA8: return {d[i * 2 + 1], d[i * 2 + 1], d[i * 2 + 1], d[i * 2]};
    case T_RG8: return {d[i * 2 + 1], d[i * 2], 0, 255};
    case T_I8: return {d[i], d[i], d[i], 255};
    case T_A8: return {0, 0, 0, d[i]};
    case T_IA4: {
        uint8_t v = expand4(d[i] >> 4);
        return {v, v, v, expand4(d[i] & 15)};
    }
    case T_I4: {
        uint8_t v = expand4((i & 1 ? d[i / 2] >> 4 : d[i / 2]) & 15);
        return {v, v, v, 255};
    }
    case T_A4: return {0, 0, 0, expand4((i & 1 ? d[i / 2] >> 4 : d[i / 2]) & 15)};
    default: return {0, 0, 0, 0};
    }
}

// wrap modes 4-7 are variants of clamp-to-edge, clamp-to-border and repeat
int wrap(int c, int size, uint32_t mode) {
    switch (mode & 7) {
    case 2:
    case 6:
    case 7: return ((c % size) + size) % size;
    case 3: {
        int p = ((c % (2 * size)) + 2 * size) % (2 * size);
        return p < size ? p : 2 * size - 1 - p;
    }
    default: return std::clamp(c, 0, size - 1);
    }
}

Rgba fetch(const Texture &t, int x, int y) {
    bool border_s = (t.wrap_s & 3) == 1, border_t = (t.wrap_t & 3) == 1;
    if ((border_s && (x < 0 || x >= int(t.width))) || (border_t && (y < 0 || y >= int(t.height)))) return t.border;
    x = wrap(x, int(t.width), t.wrap_s);
    y = wrap(y, int(t.height), t.wrap_t);
    return texel(t, uint32_t(x), t.height - 1 - uint32_t(y));
}

// YW2_DUMP_TEXTURES=<dir>: writes each distinct texture (address, size, format) once as PPM
// (color) and PGM (alpha), for debugging
void dump_texture(const Texture &t, uint32_t addr) {
    static const char *dir = std::getenv("YW2_DUMP_TEXTURES");
    static std::set<uint64_t> seen;
    if (!dir || !t.data) return;
    uint64_t key = (uint64_t(addr) << 32) ^ (t.width << 20) ^ (t.height << 8) ^ t.format;
    if (!seen.insert(key).second) return;
    std::string base = std::string(dir) + "/" + std::to_string(seen.size()) + "_" + std::to_string(addr) + "_f" +
                       std::to_string(t.format) + "_" + std::to_string(t.width) + "x" + std::to_string(t.height);
    FILE *c = std::fopen((base + ".ppm").c_str(), "wb"), *a = std::fopen((base + "_a.pgm").c_str(), "wb");
    if (!c || !a) return;
    std::fprintf(c, "P6 %u %u 255\n", t.width, t.height);
    std::fprintf(a, "P5 %u %u 255\n", t.width, t.height);
    for (uint32_t y = 0; y < t.height; y++)
        for (uint32_t x = 0; x < t.width; x++) {
            Rgba p = texel(t, x, t.height - 1 - y);
            std::fputc(p.r, c), std::fputc(p.g, c), std::fputc(p.b, c), std::fputc(p.a, a);
        }
    std::fclose(c), std::fclose(a);
}

} // namespace

Texture load_texture(int unit) {
    Texture t;
    if (!((regs[TEX_CONFIG] >> unit) & 1)) return t;
    const uint32_t *r = &regs[UNIT_BASE[unit]];
    uint32_t param = r[2];
    if (unit == 0 && ((param >> 28) & 7) != 0 && ((param >> 28) & 7) != 3) return t; // cube/shadow: TODO
    t.height = r[1] & 0x7FF;
    t.width = (r[1] >> 16) & 0x7FF;
    t.format = regs[UNIT_FORMAT[unit]] & 0xF;
    t.mag_linear = (param >> 1) & 1;
    t.min_linear = (param >> 2) & 1;
    t.wrap_t = (param >> 8) & 7;
    t.wrap_s = (param >> 12) & 7;
    t.border = {uint8_t(r[0]), uint8_t(r[0] >> 8), uint8_t(r[0] >> 16), uint8_t(r[0] >> 24)};
    uint32_t bytes = t.width * t.height * BITS[t.format] / 8;
    if (!t.width || !t.height || !bytes || (t.width | t.height) & 7) return Texture{};
    t.addr = (r[4] & 0x0FFFFFFF) << 3, t.bytes = bytes;
    t.data = phys(t.addr, bytes);
    dump_texture(t, (r[4] & 0x0FFFFFFF) << 3);
    return t;
}

void decode_texture(const Texture &t, uint8_t *rgba) {
    for (uint32_t y = 0; y < t.height; y++)
        for (uint32_t x = 0; x < t.width; x++) {
            Rgba c = texel(t, x, y);
            uint8_t *o = rgba + (size_t(y) * t.width + x) * 4;
            o[0] = c.r, o[1] = c.g, o[2] = c.b, o[3] = c.a;
        }
}

Rgba sample_texture(const Texture &t, float u, float v) {
    if (!t.data) return {0, 0, 0, 0};
    float s = u * float(t.width), tt = v * float(t.height);
    if (!std::isfinite(s) || !std::isfinite(tt)) return {0, 0, 0, 0};
    s = std::clamp(s, -1e6f, 1e6f), tt = std::clamp(tt, -1e6f, 1e6f);
    if (!t.mag_linear) return fetch(t, int(std::floor(s)), int(std::floor(tt)));

    s -= 0.5f, tt -= 0.5f;
    float fs = std::floor(s), ft = std::floor(tt);
    int x = int(fs), y = int(ft);
    // 4-bit filter weights, like the hardware
    int wx = int((s - fs) * 16.0f), wy = int((tt - ft) * 16.0f);
    Rgba c00 = fetch(t, x, y), c10 = fetch(t, x + 1, y), c01 = fetch(t, x, y + 1), c11 = fetch(t, x + 1, y + 1);
    auto mix = [&](uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
        int top = a * (16 - wx) + b * wx, bottom = c * (16 - wx) + d * wx;
        return uint8_t((top * (16 - wy) + bottom * wy + 128) >> 8);
    };
    return {mix(c00.r, c10.r, c01.r, c11.r), mix(c00.g, c10.g, c01.g, c11.g), mix(c00.b, c10.b, c01.b, c11.b),
            mix(c00.a, c10.a, c01.a, c11.a)};
}

} // namespace pica
