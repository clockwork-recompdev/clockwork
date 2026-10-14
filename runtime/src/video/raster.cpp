// Software rasterizer: clipping, triangle setup with 4-bit subpixel precision, texture units,
// the six-stage TEV color combiner, alpha/stencil/depth tests and blending, writing tiled
// pixels straight into the render target in guest memory.
//
// Fixed-point details (combiner and blend math in 8-bit integers, depth formats) follow the
// hardware as documented by 3dbrew and Citra. Fragment lighting is in lighting.cpp. Not yet
// implemented: procedural textures, shadow and cube textures, mipmapping, gas.
//
// Speed: all register state is decoded once per draw (begin_draw), and a draw's triangles are
// collected and rasterized at end_draw by a pool of threads. Each thread owns interleaved
// 8-row strips of the render target, so every pixel still sees its triangles in order.
#include "internal.h"
#include "pica.h"

#include "gpu/gpu.h"
#include "log.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace pica {
namespace {

constexpr uint32_t TEV_STAGE[6] = {0x0C0, 0x0C8, 0x0D0, 0x0D8, 0x0F0, 0x0F8};

// exact floor(x / 255) for 0 <= x < 65535
inline int div255(int x) { return (x + 1 + (x >> 8)) >> 8; }

struct TevStage {
    uint8_t csrc[3], cop[3], asrc[3], aop[3];
    uint8_t ccomb, acomb, cscale, ascale;
    bool update_color, update_alpha;
    Rgba constant;
};

// per-draw state, latched from the registers by begin_draw
struct State {
    uint8_t *color = nullptr, *depth = nullptr;
    uint32_t width = 0, height = 0, color_format = 0, depth_format = 0, color_bpp = 0, depth_bpp = 0;
    Texture tex[3];
    bool sample_tex[3] = {};
    bool tex2_uses_tex1 = false, tex0_projected = false;
    TevStage tev[6];
    int tev_count = 0;
    Rgba tev_buffer_init{};
    bool wbuffer = false;
    bool lighting = false;
    bool fog = false, fog_flip = false;
    Rgba fog_color{};
    // output merger
    bool alpha_test = false;
    uint32_t alpha_func = 0, alpha_ref = 0;
    bool depth_test = false, depth_write = false;
    uint32_t depth_func = 0, depth_max = 0;
    bool stencil = false;
    uint32_t stencil_func = 0, stencil_ops = 0;
    uint8_t stencil_ref = 0, stencil_mask = 0, stencil_write = 0;
    bool blend = false;
    uint32_t blend_cfg = 0, logic = 0;
    Rgba blend_const{};
    uint32_t write_mask = 0xF;
    // rasterizer
    uint32_t cull = 0, scissor = 0;
    int sx0 = 0, sy0 = 0, sx1 = 0, sy1 = 0;
    float half_w = 0, half_h = 0, vx = 0, vy = 0, depth_scale = 0, depth_offset = 0;
    bool wide = false;  // drawing into the enhanced shadow of a top-screen target
    float wide_k = 1.0f; // its width over the original (for the wider 3D frustum)
    int scale = 1, margin = 0;
    bool skip = true;
    uint32_t color_addr = 0, depth_addr = 0; // the game's target buffers (for batching)
    std::shared_ptr<const LightingState> light;
    uint32_t fog_lut[128];
} cur; // the draw being set up

uint8_t u8_of(float f) { return uint8_t(std::clamp(f * 255.0f + 0.5f, 0.0f, 255.0f)); }

// ---- TEV combiner ----

struct Rgb {
    int r, g, b;
};

inline Rgb color_operand(Rgba c, uint32_t op) {
    switch (op) {
    case 1: return {255 - c.r, 255 - c.g, 255 - c.b};
    case 2: return {c.a, c.a, c.a};
    case 3: return {255 - c.a, 255 - c.a, 255 - c.a};
    case 4: return {c.r, c.r, c.r};
    case 5: return {255 - c.r, 255 - c.r, 255 - c.r};
    case 8: return {c.g, c.g, c.g};
    case 9: return {255 - c.g, 255 - c.g, 255 - c.g};
    case 12: return {c.b, c.b, c.b};
    case 13: return {255 - c.b, 255 - c.b, 255 - c.b};
    default: return {c.r, c.g, c.b};
    }
}

inline int alpha_operand(Rgba c, uint32_t op) {
    switch (op & 7) {
    case 0: return c.a;
    case 1: return 255 - c.a;
    case 2: return c.r;
    case 3: return 255 - c.r;
    case 4: return c.g;
    case 5: return 255 - c.g;
    case 6: return c.b;
    default: return 255 - c.b;
    }
}

inline int combine(uint32_t op, int a, int b, int c) {
    switch (op) {
    case 0: return a;
    case 1: return div255(a * b);
    case 2: return std::min(a + b, 255);
    case 3: return std::clamp(a + b - 128, 0, 255);
    case 4: return div255(a * c + b * (255 - c));
    case 5: return std::max(a - b, 0);
    case 8: return std::min((a * b + 255 * c) / 255, 255);
    case 9: return div255(std::min(a + b, 255) * c);
    default: return a;
    }
}

inline int dot3(Rgb a, Rgb b) {
    int r = ((a.r * 2 - 255) * (b.r * 2 - 255) + 128) / 256 + ((a.g * 2 - 255) * (b.g * 2 - 255) + 128) / 256 +
            ((a.b * 2 - 255) * (b.b * 2 - 255) + 128) / 256;
    return std::clamp(r, 0, 255);
}

// source ids: 0 primary color, 1-2 fragment lighting, 3-5 textures 0-2, 0xD buffer,
// 0xE constant, 0xF previous; texture 3 (procedural) reads as zero until implemented
Rgba run_tev(const State &st, Rgba primary, Rgba frag_primary, Rgba frag_secondary, const Rgba tex[3]) {
    Rgba src[16] = {};
    src[0x0] = primary;
    src[0x1] = frag_primary, src[0x2] = frag_secondary;
    src[0x3] = tex[0], src[0x4] = tex[1], src[0x5] = tex[2];
    Rgba next_buffer = st.tev_buffer_init;
    src[0xD] = st.tev_buffer_init;
    for (int i = 0; i < st.tev_count; i++) {
        const TevStage &s = st.tev[i];
        src[0xE] = s.constant;
        Rgb c[3];
        int a[3];
        for (int k = 0; k < 3; k++) {
            c[k] = color_operand(src[s.csrc[k]], s.cop[k]);
            a[k] = alpha_operand(src[s.asrc[k]], s.aop[k]);
        }
        Rgba out;
        if (s.ccomb == 6 || s.ccomb == 7) {
            int d = std::min(dot3(c[0], c[1]) << s.cscale, 255);
            out = {uint8_t(d), uint8_t(d), uint8_t(d), 0};
        } else {
            out.r = uint8_t(std::min(combine(s.ccomb, c[0].r, c[1].r, c[2].r) << s.cscale, 255));
            out.g = uint8_t(std::min(combine(s.ccomb, c[0].g, c[1].g, c[2].g) << s.cscale, 255));
            out.b = uint8_t(std::min(combine(s.ccomb, c[0].b, c[1].b, c[2].b) << s.cscale, 255));
        }
        out.a = s.ccomb == 7 ? out.r : uint8_t(std::min(combine(s.acomb, a[0], a[1], a[2]) << s.ascale, 255));
        src[0xF] = out;

        // the buffer source lags one stage behind the update, as on hardware
        src[0xD] = next_buffer;
        if (s.update_color) next_buffer.r = out.r, next_buffer.g = out.g, next_buffer.b = out.b;
        if (s.update_alpha) next_buffer.a = out.a;
    }
    return src[0xF];
}

// a stage that outputs the previous stage unchanged
bool passthrough(const TevStage &s) {
    return s.ccomb == 0 && s.acomb == 0 && s.csrc[0] == 0xF && s.asrc[0] == 0xF && s.cop[0] == 0 && s.aop[0] == 0 &&
           s.cscale == 0 && s.ascale == 0;
}

// ---- output merger ----

inline bool test(uint32_t func, uint32_t a, uint32_t b) {
    switch (func & 7) {
    case 0: return false;
    case 1: return true;
    case 2: return a == b;
    case 3: return a != b;
    case 4: return a < b;
    case 5: return a <= b;
    case 6: return a > b;
    default: return a >= b;
    }
}

inline uint8_t stencil_op(uint32_t op, uint8_t old, uint8_t ref) {
    switch (op & 7) {
    case 0: return old;
    case 1: return 0;
    case 2: return ref;
    case 3: return old == 255 ? 255 : uint8_t(old + 1);
    case 4: return old == 0 ? 0 : uint8_t(old - 1);
    case 5: return uint8_t(~old);
    case 6: return uint8_t(old + 1);
    default: return uint8_t(old - 1);
    }
}

inline int blend_factor(uint32_t f, int channel, Rgba src, Rgba dst, Rgba constant) {
    auto ch = [channel](Rgba c) { return channel == 0 ? c.r : channel == 1 ? c.g : channel == 2 ? c.b : c.a; };
    switch (f & 0xF) {
    case 0: return 0;
    case 1: return 255;
    case 2: return ch(src);
    case 3: return 255 - ch(src);
    case 4: return ch(dst);
    case 5: return 255 - ch(dst);
    case 6: return src.a;
    case 7: return 255 - src.a;
    case 8: return dst.a;
    case 9: return 255 - dst.a;
    case 10: return ch(constant);
    case 11: return 255 - ch(constant);
    case 12: return constant.a;
    case 13: return 255 - constant.a;
    case 14: return channel == 3 ? 255 : std::min<int>(src.a, 255 - dst.a);
    default: return 255;
    }
}

inline uint8_t blend_channel(uint32_t eq, int s, int sf, int d, int df) {
    int r;
    switch (eq & 7) {
    case 1: r = (s * sf - d * df) / 255; break;
    case 2: r = (d * df - s * sf) / 255; break;
    case 3: r = std::min(s, d); break;
    case 4: r = std::max(s, d); break;
    default: r = (s * sf + d * df) / 255;
    }
    return uint8_t(std::clamp(r, 0, 255));
}

Rgba blend(const State &st, Rgba src, Rgba dst) {
    uint32_t cfg = st.blend_cfg;
    const Rgba &k = st.blend_const;
    uint32_t eq_rgb = cfg & 7, eq_a = (cfg >> 8) & 7;
    uint32_t sf_rgb = (cfg >> 16) & 0xF, df_rgb = (cfg >> 20) & 0xF, sf_a = (cfg >> 24) & 0xF, df_a = cfg >> 28;
    return {blend_channel(eq_rgb, src.r, blend_factor(sf_rgb, 0, src, dst, k), dst.r, blend_factor(df_rgb, 0, src, dst, k)),
            blend_channel(eq_rgb, src.g, blend_factor(sf_rgb, 1, src, dst, k), dst.g, blend_factor(df_rgb, 1, src, dst, k)),
            blend_channel(eq_rgb, src.b, blend_factor(sf_rgb, 2, src, dst, k), dst.b, blend_factor(df_rgb, 2, src, dst, k)),
            blend_channel(eq_a, src.a, blend_factor(sf_a, 3, src, dst, k), dst.a, blend_factor(df_a, 3, src, dst, k))};
}

inline uint8_t logic_op(uint32_t op, uint8_t s, uint8_t d) {
    switch (op & 0xF) {
    case 0: return 0;
    case 1: return s & d;
    case 2: return s & ~d;
    case 3: return s;
    case 4: return 255;
    case 5: return ~s;
    case 6: return d;
    case 7: return ~d;
    case 8: return ~(s & d);
    case 9: return s | d;
    case 10: return ~(s | d);
    case 11: return s ^ d;
    case 12: return ~(s ^ d);
    case 13: return ~s & d;
    case 14: return s | ~d;
    default: return ~s | d;
    }
}

// ---- triangle setup and scan conversion ----

struct ScreenVertex {
    float x, y, z, inv_w;
    // perspective-divided attributes: color rgba, tex0 uv, tex1 uv, tex2 uv, tex0 w,
    // normal quaternion xyzw, view xyz
    float attr[18];
};
constexpr int ATTR_COUNT = 18;

ScreenVertex project(const Vertex &v) {
    float inv_w = 1.0f / v.a[POS + 3];
    ScreenVertex s;
    s.x = (v.a[POS] * inv_w + 1.0f) * cur.half_w + cur.vx;
    s.y = (v.a[POS + 1] * inv_w + 1.0f) * cur.half_h + cur.vy;
    s.z = cur.depth_offset + v.a[POS + 2] * inv_w * cur.depth_scale;
    s.inv_w = inv_w;
    static constexpr int SRC[ATTR_COUNT] = {COLOR, COLOR + 1, COLOR + 2, COLOR + 3, TEX0, TEX0 + 1, TEX1,
                                            TEX1 + 1, TEX2, TEX2 + 1, TEX0_W, QUAT, QUAT + 1, QUAT + 2,
                                            QUAT + 3, VIEW, VIEW + 1, VIEW + 2};
    for (int i = 0; i < ATTR_COUNT; i++) s.attr[i] = v.a[SRC[i]] * inv_w;
    return s;
}

void shade_pixel(const State &st, int x, int y, const ScreenVertex &v0, const ScreenVertex &v1, const ScreenVertex &v2, float b0,
                 float b1, float b2) {
    // depth: z/w is linear in screen space; W-buffering multiplies it back by w
    float inv_w = b0 * v0.inv_w + b1 * v1.inv_w + b2 * v2.inv_w;
    float w = 1.0f / inv_w;
    float depth = b0 * v0.z + b1 * v1.z + b2 * v2.z;
    if (st.wbuffer) depth *= w;
    depth = std::clamp(depth, 0.0f, 1.0f);

    auto attr = [&](int i) { return (b0 * v0.attr[i] + b1 * v1.attr[i] + b2 * v2.attr[i]) * w; };
    Rgba primary = {u8_of(attr(0)), u8_of(attr(1)), u8_of(attr(2)), u8_of(attr(3))};
    Rgba tex[3] = {};
    if (st.sample_tex[0]) {
        float u = attr(4), v = attr(5);
        if (st.tex0_projected) {
            float q = attr(10);
            if (q != 0.0f) u /= q, v /= q;
        }
        tex[0] = sample_texture(st.tex[0], u, v);
    }
    if (st.sample_tex[1]) tex[1] = sample_texture(st.tex[1], attr(6), attr(7));
    if (st.sample_tex[2])
        tex[2] = st.tex2_uses_tex1 ? sample_texture(st.tex[2], attr(6), attr(7)) : sample_texture(st.tex[2], attr(8), attr(9));
    Rgba frag_primary{}, frag_secondary{};
    if (st.lighting) {
        float quat[4] = {attr(11), attr(12), attr(13), attr(14)}, view[3] = {attr(15), attr(16), attr(17)};
        Rgba all_tex[4] = {tex[0], tex[1], tex[2], {}};
        fragment_lighting(*st.light, quat, view, all_tex, frag_primary, frag_secondary);
    }
    Rgba color = run_tev(st, primary, frag_primary, frag_secondary, tex);

    if (st.fog) {
        float index = (st.fog_flip ? 1.0f - depth : depth) * 128.0f;
        float i = std::clamp(std::floor(index), 0.0f, 127.0f);
        uint32_t e = st.fog_lut[int(i)];
        float diff = float(int32_t(e << 19) >> 19) / 2047.0f;
        float f = std::clamp(float((e >> 13) & 0x7FF) / 2047.0f + diff * (index - i), 0.0f, 1.0f);
        color.r = uint8_t(f * color.r + (1.0f - f) * st.fog_color.r);
        color.g = uint8_t(f * color.g + (1.0f - f) * st.fog_color.g);
        color.b = uint8_t(f * color.b + (1.0f - f) * st.fog_color.b);
    }

    if (st.alpha_test && !test(st.alpha_func, color.a, st.alpha_ref)) return;

    uint32_t index = tiled_index(uint32_t(x), st.height - 1 - uint32_t(y), st.width);

    // stencil and depth
    if (st.depth) {
        uint8_t *dp = st.depth + index * st.depth_bpp;
        uint32_t stored = st.depth_format == 0 ? load16(dp) : dp[0] | (dp[1] << 8) | (dp[2] << 16);
        uint8_t stencil = st.depth_format == 3 ? dp[3] : 0;
        uint32_t z = uint32_t(depth * float(st.depth_max));
        auto update_stencil = [&](uint32_t op) {
            uint8_t n = stencil_op(op, stencil, st.stencil_ref);
            dp[3] = uint8_t((stencil & ~st.stencil_write) | (n & st.stencil_write));
        };
        if (st.stencil && !test(st.stencil_func, st.stencil_ref & st.stencil_mask, stencil & st.stencil_mask)) {
            update_stencil(st.stencil_ops);
            return;
        }
        if (st.depth_test && !test(st.depth_func, z, stored)) {
            if (st.stencil) update_stencil(st.stencil_ops >> 4);
            return;
        }
        if (st.depth_write) {
            if (st.depth_format == 0) {
                store16(dp, z);
            } else {
                dp[0] = uint8_t(z), dp[1] = uint8_t(z >> 8), dp[2] = uint8_t(z >> 16);
            }
        }
        if (st.stencil) update_stencil(st.stencil_ops >> 8);
    }

    if (!st.color) return;
    uint8_t *cp = st.color + index * st.color_bpp;
    Rgba dst = fb_decode(st.color_format, cp);
    Rgba out;
    if (st.blend) {
        out = blend(st, color, dst);
    } else {
        uint32_t op = st.logic;
        out = op == 3 ? color
                      : Rgba{logic_op(op, color.r, dst.r), logic_op(op, color.g, dst.g), logic_op(op, color.b, dst.b),
                             logic_op(op, color.a, dst.a)};
    }
    uint32_t wm = st.write_mask;
    if (wm != 0xF) {
        if (!(wm & 1)) out.r = dst.r;
        if (!(wm & 2)) out.g = dst.g;
        if (!(wm & 4)) out.b = dst.b;
        if (!(wm & 8)) out.a = dst.a;
    }
    fb_encode(st.color_format, out, cp);
}

// a triangle ready for scan conversion: counter-clockwise, in 4-bit fixed point
struct Triangle {
    ScreenVertex v[3];
    int64_t x[3], y[3];
    int64_t area;
    int minx, maxx, miny, maxy;
};

std::vector<Triangle> triangles; // of the draw being set up

// draws waiting to be rasterized together (one command list's worth): each keeps its state
struct Draw {
    State st;
    std::vector<Triangle> tris;
};
std::vector<Draw> batch;
int64_t batch_pixels = 0;

bool setup(const ScreenVertex &a, const ScreenVertex &b, const ScreenVertex &c, Triangle &t) {
    auto fx = [](float v) { return int64_t(std::lround(std::clamp(v, -32768.0f, 32768.0f) * 16.0f)); };
    t.v[0] = a, t.v[1] = b, t.v[2] = c;
    t.x[0] = fx(a.x), t.y[0] = fx(a.y), t.x[1] = fx(b.x), t.y[1] = fx(b.y), t.x[2] = fx(c.x), t.y[2] = fx(c.y);
    int64_t area = (t.x[1] - t.x[0]) * (t.y[2] - t.y[0]) - (t.y[1] - t.y[0]) * (t.x[2] - t.x[0]);
    // mode 1 keeps clockwise triangles, mode 2 counter-clockwise (y up)
    if ((cur.cull == 1 && area >= 0) || (cur.cull == 2 && area <= 0) || area == 0) return false;
    if (area < 0) { // make the winding counter-clockwise
        std::swap(t.v[1], t.v[2]);
        std::swap(t.x[1], t.x[2]), std::swap(t.y[1], t.y[2]);
        area = -area;
    }
    t.area = area;
    t.minx = std::max(int(std::min({t.x[0], t.x[1], t.x[2]}) >> 4), 0);
    t.miny = std::max(int(std::min({t.y[0], t.y[1], t.y[2]}) >> 4), 0);
    t.maxx = std::min(int((std::max({t.x[0], t.x[1], t.x[2]}) + 15) >> 4), int(cur.width) - 1);
    t.maxy = std::min(int((std::max({t.y[0], t.y[1], t.y[2]}) + 15) >> 4), int(cur.height) - 1);
    if (cur.scissor == 3) {
        t.minx = std::max(t.minx, cur.sx0), t.miny = std::max(t.miny, cur.sy0);
        t.maxx = std::min(t.maxx, cur.sx1), t.maxy = std::min(t.maxy, cur.sy1);
    }
    return t.minx <= t.maxx && t.miny <= t.maxy;
}

// scan-converts rows [y0, y1] of a triangle
void rasterize(const State &st, const Triangle &t, int y0, int y1) {
    y0 = std::max(y0, t.miny), y1 = std::min(y1, t.maxy);
    if (y0 > y1) return;
    // edge i is opposite vertex i; a top-left style tie rule so shared edges are drawn once
    struct Edge {
        int64_t a, b, c, bias; // value(px, py) = a * px + b * py + c
    } e[3];
    for (int i = 0; i < 3; i++) {
        int j = (i + 1) % 3, k = (i + 2) % 3;
        int64_t dx = t.x[k] - t.x[j], dy = t.y[k] - t.y[j];
        e[i] = {-dy, dx, dy * t.x[j] - dx * t.y[j], (dy > 0 || (dy == 0 && dx < 0)) ? 0 : -1};
    }
    float inv_area = 1.0f / float(t.area);
    for (int y = y0; y <= y1; y++) {
        int64_t py = int64_t(y) * 16 + 8, px = int64_t(t.minx) * 16 + 8;
        int64_t w[3];
        for (int i = 0; i < 3; i++) w[i] = e[i].a * px + e[i].b * py + e[i].c;
        for (int x = t.minx; x <= t.maxx; x++, w[0] += e[0].a * 16, w[1] += e[1].a * 16, w[2] += e[2].a * 16) {
            if (w[0] + e[0].bias < 0 || w[1] + e[1].bias < 0 || w[2] + e[2].bias < 0) continue;
            if (st.scissor == 1 && x >= st.sx0 && x <= st.sx1 && y >= st.sy0 && y <= st.sy1) continue;
            shade_pixel(st, x, y, t.v[0], t.v[1], t.v[2], float(w[0]) * inv_area, float(w[1]) * inv_area,
                        float(w[2]) * inv_area);
        }
    }
}

// ---- worker pool ----

constexpr int STRIP = 8; // rows per strip, one memory tile high

struct Pool {
    std::vector<std::thread> workers;
    std::mutex lock;
    std::condition_variable wake, done;
    uint64_t generation = 0;
    int pending = 0;
    int lanes = 1;

    Pool() {
        unsigned hw = std::thread::hardware_concurrency();
        lanes = int(std::clamp(hw ? hw - 1 : 1u, 1u, 8u));
        for (int i = 1; i < lanes; i++) workers.emplace_back([this, i] { work(i); });
        for (auto &w : workers) w.detach();
    }

    // rasterizes the strips of lane `lane`, draw by draw in order
    void run_lane(int lane) const {
        for (const Draw &d : batch)
            for (const Triangle &t : d.tris)
                for (int s = (t.miny / STRIP / lanes) * lanes + lane; s * STRIP <= t.maxy; s += lanes)
                    rasterize(d.st, t, s * STRIP, s * STRIP + STRIP - 1);
    }

    void work(int lane) {
        uint64_t seen = 0;
        for (;;) {
            {
                std::unique_lock lk(lock);
                wake.wait(lk, [&] { return generation != seen; });
                seen = generation;
            }
            run_lane(lane);
            std::lock_guard lk(lock);
            if (--pending == 0) done.notify_one();
        }
    }

    void run() {
        if (lanes > 1) {
            std::lock_guard lk(lock);
            pending = lanes - 1;
            generation++;
            wake.notify_all();
        }
        run_lane(0);
        if (lanes > 1) {
            std::unique_lock lk(lock);
            done.wait(lk, [&] { return pending == 0; });
        }
    }
};

Pool &pool() {
    static Pool p;
    return p;
}

// ---- clipping (homogeneous, before the divide) ----

Vertex lerp(const Vertex &a, const Vertex &b, float t) {
    Vertex r;
    for (int i = 0; i < SEMANTIC_COUNT; i++) r.a[i] = a.a[i] + (b.a[i] - a.a[i]) * t;
    return r;
}

// `ky` widens the y planes (the top screen's horizontal axis) for widescreen 3D
float plane_distance(const Vertex &v, int plane, float ky) {
    const float *p = &v.a[POS];
    switch (plane) {
    case 0: return p[3] - p[0];
    case 1: return p[3] + p[0];
    case 2: return ky * p[3] - p[1];
    case 3: return ky * p[3] + p[1];
    case 4: return -p[2];          // z <= 0
    case 5: return p[3] + p[2];    // z >= -w
    default: return p[3] - 1e-5f;  // w > 0
    }
}

void latch_output_merger() {
    uint32_t a = regs[ALPHA_TEST];
    cur.alpha_test = a & 1, cur.alpha_func = (a >> 4) & 7, cur.alpha_ref = (a >> 8) & 0xFF;
    uint32_t dm = regs[DEPTH_COLOR_MASK];
    bool can_write = regs[DEPTH_STENCIL_WRITE_ALLOW] != 0;
    cur.depth_test = dm & 1, cur.depth_func = (dm >> 4) & 7;
    cur.depth_write = ((dm >> 12) & 1) && can_write;
    cur.write_mask = (dm >> 8) & 0xF;
    cur.depth_max = cur.depth_format == 0 ? 0xFFFF : 0xFFFFFF;
    uint32_t s = regs[STENCIL_TEST];
    cur.stencil = cur.depth_format == 3 && (s & 1);
    cur.stencil_func = (s >> 4) & 7, cur.stencil_ref = uint8_t(s >> 16), cur.stencil_mask = uint8_t(s >> 24);
    cur.stencil_write = can_write ? uint8_t(s >> 8) : 0;
    cur.stencil_ops = regs[STENCIL_OP];
    cur.blend = (regs[COLOR_OP] >> 8) & 1;
    cur.blend_cfg = regs[BLEND_FUNC];
    uint32_t b = regs[BLEND_COLOR];
    cur.blend_const = {uint8_t(b), uint8_t(b >> 8), uint8_t(b >> 16), uint8_t(b >> 24)};
    cur.logic = regs[LOGIC_OP] & 0xF;
}

void latch_rasterizer() {
    auto sext10 = [](uint32_t v) { return int32_t(v << 22) >> 22; };
    cur.cull = regs[CULL_MODE] & 3;
    cur.scissor = regs[SCISSOR_MODE] & 3;
    cur.sx0 = int(regs[SCISSOR_MIN] & 0x3FF), cur.sy0 = int((regs[SCISSOR_MIN] >> 16) & 0x3FF);
    cur.sx1 = int(regs[SCISSOR_MAX] & 0x3FF), cur.sy1 = int((regs[SCISSOR_MAX] >> 16) & 0x3FF);
    cur.half_w = f24(regs[VIEWPORT_WIDTH]), cur.half_h = f24(regs[VIEWPORT_HEIGHT]);
    cur.vx = float(sext10(regs[VIEWPORT_XY] & 0x3FF)), cur.vy = float(sext10((regs[VIEWPORT_XY] >> 16) & 0x3FF));
    cur.depth_scale = f24(regs[DEPTH_SCALE]), cur.depth_offset = f24(regs[DEPTH_OFFSET]);
    cur.wbuffer = !(regs[DEPTHMAP_ENABLE] & 1);
    if (cur.wide) { // scaled up, with the original 400 rows in the middle of the wider target
        float s = float(cur.scale), m = float(cur.margin);
        cur.half_w *= s, cur.half_h *= s;
        cur.vx *= s, cur.vy = (cur.vy + m) * s;
        if (cur.scissor) {
            bool full = cur.sy0 <= 0 && cur.sy1 >= 399;
            cur.sx0 *= cur.scale, cur.sx1 = (cur.sx1 + 1) * cur.scale - 1;
            cur.sy0 = full ? 0 : (cur.sy0 + cur.margin) * cur.scale;
            cur.sy1 = full ? int(cur.height) - 1 : (cur.sy1 + cur.margin + 1) * cur.scale - 1;
        }
    }
}

void latch_tev() {
    uint32_t buffer_config = regs[TEV_BUFFER_CONFIG];
    for (int i = 0; i < 6; i++) {
        const uint32_t *r = &regs[TEV_STAGE[i]];
        TevStage &s = cur.tev[i];
        for (int k = 0; k < 3; k++) {
            s.csrc[k] = uint8_t((r[0] >> (4 * k)) & 0xF), s.asrc[k] = uint8_t((r[0] >> (16 + 4 * k)) & 0xF);
            s.cop[k] = uint8_t((r[1] >> (4 * k)) & 0xF), s.aop[k] = uint8_t((r[1] >> (12 + 4 * k)) & 7);
        }
        s.ccomb = uint8_t(r[2] & 0xF), s.acomb = uint8_t((r[2] >> 16) & 0xF);
        s.constant = {uint8_t(r[3]), uint8_t(r[3] >> 8), uint8_t(r[3] >> 16), uint8_t(r[3] >> 24)};
        s.cscale = uint8_t(r[4] & 3), s.ascale = uint8_t((r[4] >> 16) & 3);
        if (s.cscale == 3) s.cscale = 2;
        if (s.ascale == 3) s.ascale = 2;
        s.update_color = i < 4 && ((buffer_config >> (8 + i)) & 1);
        s.update_alpha = i < 4 && ((buffer_config >> (12 + i)) & 1);
    }
    cur.tev_count = 6;
    while (cur.tev_count > 0 && passthrough(cur.tev[cur.tev_count - 1])) cur.tev_count--;
    uint32_t b = regs[TEV_BUFFER_COLOR];
    cur.tev_buffer_init = {uint8_t(b), uint8_t(b >> 8), uint8_t(b >> 16), uint8_t(b >> 24)};

    // only sample the textures some stage reads (or lighting uses for shadows/bump maps)
    bool used[16] = {};
    for (int i = 0; i < cur.tev_count; i++)
        for (int k = 0; k < 3; k++) used[cur.tev[i].csrc[k]] = used[cur.tev[i].asrc[k]] = true;
    uint32_t c0 = regs[LIGHTING_CONFIG0];
    bool lighting_reads_tex = cur.lighting && ((c0 & 1) || ((c0 >> 28) & 3));
    for (int u = 0; u < 3; u++) cur.sample_tex[u] = (used[3 + u] || lighting_reads_tex) && cur.tex[u].data;
}

} // namespace

void begin_draw() {
    if (gpu::active()) return gpu::begin_draw();
    cur.skip = true;
    uint32_t dim = regs[FB_DIM];
    cur.width = dim & 0x7FF;
    cur.height = ((dim >> 12) & 0x3FF) + 1;
    if (!cur.width || cur.height < 2) return;
    cur.color_format = (regs[COLOR_FORMAT] >> 16) & 7;
    cur.depth_format = regs[DEPTH_FORMAT] & 3;
    cur.color_bpp = fb_bpp(cur.color_format);
    cur.depth_bpp = cur.depth_format == 0 ? 2 : cur.depth_format == 3 ? 4 : 3;
    uint32_t pixels = cur.width * cur.height;
    uint32_t color_addr = (regs[COLOR_ADDR] & 0x0FFFFFFF) << 3, depth_addr = (regs[DEPTH_ADDR] & 0x0FFFFFFF) << 3;
    cur.color = regs[COLOR_WRITE_ALLOW] ? phys(color_addr, pixels * cur.color_bpp) : nullptr;
    bool depth_used = regs[DEPTH_STENCIL_READ_ALLOW] || regs[DEPTH_STENCIL_WRITE_ALLOW];
    cur.depth = depth_used && regs[DEPTH_ADDR] ? phys(depth_addr, pixels * cur.depth_bpp) : nullptr;
    if (!cur.color && !cur.depth) return;
    for (uint32_t base : {uint32_t(TEX0_ADDR), 0x095u, 0x09Du}) {
        // render-to-texture: draws still queued must land before this one samples them
        uint32_t tex = (regs[base] & 0x0FFFFFFF) << 3;
        for (const Draw &d : batch)
            if (tex && (tex == d.st.color_addr || tex == d.st.depth_addr)) {
                flush_draws();
                break;
            }
        wide_before_texture(tex);
    }
    cur.color_addr = color_addr, cur.depth_addr = depth_addr;
    ShadowTarget shadow;
    cur.wide = cur.color && wide_target(color_addr, cur.depth ? depth_addr : 0, cur.width, cur.height, cur.color_format,
                                      cur.depth_format, &shadow);
    if (cur.wide) {
        cur.wide_k = float(cur.height + 2 * uint32_t(shadow.margin)) / float(cur.height);
        cur.color = shadow.color, cur.depth = shadow.depth;
        cur.width = shadow.width, cur.height = shadow.height;
        cur.scale = shadow.scale, cur.margin = shadow.margin;
    }

    for (int i = 0; i < 3; i++) cur.tex[i] = load_texture(i);
    cur.tex2_uses_tex1 = (regs[TEX_CONFIG] >> 13) & 1;
    cur.tex0_projected = ((regs[TEX0_PARAM] >> 28) & 7) == 3;
    cur.lighting = !(regs[LIGHTING_OFF] & 1);
    cur.light = cur.lighting ? latch_lighting() : nullptr;
    uint32_t fog = regs[TEV_BUFFER_CONFIG];
    cur.fog = (fog & 7) == 5, cur.fog_flip = (fog >> 16) & 1;
    if (cur.fog) std::memcpy(cur.fog_lut, fog_lut, sizeof cur.fog_lut);
    uint32_t fc = regs[FOG_COLOR];
    cur.fog_color = {uint8_t(fc), uint8_t(fc >> 8), uint8_t(fc >> 16), 255};
    latch_tev();
    latch_output_merger();
    latch_rasterizer();
    triangles.clear();
    cur.skip = false;
}

void end_draw() {
    if (gpu::active()) return gpu::end_draw();
    if (cur.skip || triangles.empty()) return;
    for (const Triangle &t : triangles) batch_pixels += int64_t(t.maxx - t.minx + 1) * (t.maxy - t.miny + 1);
    batch.push_back({cur, std::move(triangles)});
    triangles.clear();
}

void flush_draws() {
    if (gpu::active()) return gpu::flush();
    if (batch.empty()) return;
    static const bool stats = std::getenv("YW2_FPS") != nullptr;
    static int flushes = 0;
    static int64_t draws = 0, pixels = 0;
    if (stats) {
        draws += int64_t(batch.size()), pixels += batch_pixels;
        if (++flushes % 500 == 0) {
            LOG("raster: %.1f draws and %.0f bounding-box pixels per batch", double(draws) / 500, double(pixels) / 500);
            draws = pixels = 0;
        }
    }
    if (batch_pixels < 8192) { // small batches aren't worth waking the pool
        for (const Draw &d : batch)
            for (const Triangle &t : d.tris) rasterize(d.st, t, t.miny, t.maxy);
    } else {
        pool().run();
    }
    batch.clear();
    batch_pixels = 0;
}

void draw_triangle(const Vertex &v0, const Vertex &v1, const Vertex &v2) {
    if (gpu::active()) return gpu::triangle(v0, v1, v2);
    if (cur.skip) return;
    Vertex buf_a[16], buf_b[16];
    Vertex *in = buf_a, *out = buf_b;
    in[0] = v0, in[1] = v1, in[2] = v2;
    int n = 3;
    // widescreen: 3D (perspective) triangles see a wider frustum; 2D triangles keep their
    // size in the middle, except full-screen layers (backgrounds, fades, tints; see
    // draw_fills_screen), which are stretched; HUD bars that only span the width (like "Objective") keep their size
    float ky = 1.0f;
    if (cur.wide) {
        bool perspective = false;
        float lo = 1e9f, hi = -1e9f, xlo = 1e9f, xhi = -1e9f;
        for (int i = 0; i < 3; i++) {
            float w = in[i].a[POS + 3];
            perspective |= std::fabs(w - 1.0f) > 1e-4f;
            lo = std::min(lo, in[i].a[POS + 1] / w), hi = std::max(hi, in[i].a[POS + 1] / w);
            xlo = std::min(xlo, in[i].a[POS] / w), xhi = std::max(xhi, in[i].a[POS] / w);
        }
        if (draw_fills_screen || (!perspective && lo <= -0.99f && hi >= 0.99f && xlo <= -0.99f && xhi >= 0.99f)) {
            for (int i = 0; i < 3; i++) in[i].a[POS + 1] *= cur.wide_k;
            ky = cur.wide_k;
        } else if (perspective) {
            ky = cur.wide_k;
        } else if (draw_anchor) { // 2D on the sides keeps its distance from the (new) screen edge
            for (int i = 0; i < 3; i++) in[i].a[POS + 1] += float(draw_anchor) * (cur.wide_k - 1.0f) * in[i].a[POS + 3];
            ky = cur.wide_k; // clipped at the wide screen's edges
        }
    }
    // clipping is only needed when a vertex is outside
    bool inside = true;
    for (int i = 0; i < 3 && inside; i++)
        for (int plane = 0; plane < 7 && inside; plane++) inside = plane_distance(in[i], plane, ky) >= 0;
    for (int plane = 0; plane < 7 && n >= 3 && !inside; plane++) {
        int m = 0;
        for (int i = 0; i < n; i++) {
            const Vertex &a = in[i], &b = in[(i + 1) % n];
            float da = plane_distance(a, plane, ky), db = plane_distance(b, plane, ky);
            if (da >= 0) {
                if (m < 15) out[m++] = a;
            }
            if ((da >= 0) != (db >= 0) && m < 15) out[m++] = lerp(a, b, da / (da - db));
        }
        std::swap(in, out);
        n = m;
    }
    if (n < 3) return;
    ScreenVertex s[16];
    for (int i = 0; i < n; i++) s[i] = project(in[i]);
    for (int i = 1; i + 1 < n; i++) {
        triangles.emplace_back();
        if (!setup(s[0], s[i], s[i + 1], triangles.back())) triangles.pop_back();
    }
}

} // namespace pica
