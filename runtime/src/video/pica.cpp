// PICA200 command processor: register writes from command lists, shader uploads, vertex
// loading, vertex shading and primitive assembly. Triangles go to raster.cpp.
#include "internal.h"
#include "pica.h"
#include "gpu/gpu.h"

#include "log.h"
#include "memory.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>
#include <cstring>

namespace pica {

bool draw_fills_screen = false;
int draw_anchor = 0;
DrawGeom draw_geom;

uint32_t regs[REG_COUNT];
ShaderSetup vs;
uint32_t lighting_lut[24][256];
uint32_t lighting_lut_version = 0;
uint32_t fog_lut[128];

uint32_t to_virtual(uint32_t paddr) {
    if (paddr >= 0x18000000 && paddr < 0x18000000 + mem::VRAM_SIZE) return paddr - 0x18000000 + mem::VRAM_BASE;
    if (paddr >= 0x20000000 && paddr < 0x28000000) return paddr - 0x20000000 + mem::LINEAR_BASE;
    return 0;
}

uint8_t *phys(uint32_t paddr, uint32_t size) {
    uint32_t v = to_virtual(paddr);
    if (!v || !size) return v ? mem::ptr(v) : nullptr;
    for (uint32_t page = mem::page_down(v); page < v + size; page += mem::PAGE)
        if (!mem::is_committed(page)) return nullptr;
    return mem::ptr(v);
}

namespace {

void count_draw(bool is_3d);

// A pool of threads for parallel vertex shading: run(count, fn) calls fn(begin, end) on slices
// of [0, count) and returns when all are done.
class Workers {
public:
    Workers() {
        unsigned hw = std::thread::hardware_concurrency();
        lanes = int(std::clamp(hw ? hw : 1u, 1u, 8u));
        for (int i = 1; i < lanes; i++) std::thread([this, i] { loop(i); }).detach();
    }
    void run(size_t count, const std::function<void(size_t, size_t)> &fn) {
        if (lanes == 1 || count < 64) return fn(0, count);
        {
            std::lock_guard lk(lock);
            job = &fn, job_count = count, pending = lanes - 1, generation++;
        }
        wake.notify_all();
        slice(0, fn, count);
        std::unique_lock lk(lock);
        done.wait(lk, [&] { return pending == 0; });
    }

private:
    int lanes = 1, pending = 0;
    uint64_t generation = 0;
    size_t job_count = 0;
    const std::function<void(size_t, size_t)> *job = nullptr;
    std::mutex lock;
    std::condition_variable wake, done;

    void slice(int lane, const std::function<void(size_t, size_t)> &fn, size_t count) {
        size_t per = (count + size_t(lanes) - 1) / size_t(lanes);
        size_t begin = std::min(count, per * size_t(lane)), end = std::min(count, begin + per);
        if (begin < end) fn(begin, end);
    }
    void loop(int lane) {
        uint64_t seen = 0;
        for (;;) {
            const std::function<void(size_t, size_t)> *fn;
            size_t count;
            {
                std::unique_lock lk(lock);
                wake.wait(lk, [&] { return generation != seen; });
                seen = generation, fn = job, count = job_count;
            }
            slice(lane, *fn, count);
            std::lock_guard lk(lock);
            if (--pending == 0) done.notify_one();
        }
    }
};

Workers &workers() {
    static Workers w;
    return w;
}

// ---- uniform and program uploads ----

uint32_t float_buffer[4];
int float_words = 0;
uint32_t fixed_buffer[3];
int fixed_words = 0;

// default values of fixed (non-array) attributes, and the immediate-mode vertex being built
Vec4 fixed_attr[16];
Vec4 immediate[16];
int immediate_count = 0;

// unpacks four f24 values from three words (w in the high bits of the first)
Vec4 unpack_f24x4(const uint32_t *w) {
    return {{f24(w[2] & 0xFFFFFF), f24(((w[1] & 0xFFFF) << 8) | (w[2] >> 24)), f24(((w[0] & 0xFF) << 16) | (w[1] >> 16)),
             f24(w[0] >> 8)}};
}

void write_float_uniform(uint32_t value) {
    bool f32 = regs[VS_FLOAT_INDEX] >> 31;
    float_buffer[float_words++] = value;
    if (float_words < (f32 ? 4 : 3)) return;
    float_words = 0;
    uint32_t index = regs[VS_FLOAT_INDEX] & 0xFF;
    Vec4 v;
    if (f32) {
        for (int i = 0; i < 4; i++) std::memcpy(&v[i], &float_buffer[3 - i], 4);
    } else {
        v = unpack_f24x4(float_buffer);
    }
    if (index < 96) vs.f[index] = v;
    regs[VS_FLOAT_INDEX] = (regs[VS_FLOAT_INDEX] & ~0xFFu) | ((index + 1) & 0xFF);
}

// ---- vertex loading ----

struct AttrSource {
    const uint8_t *data;
    uint32_t stride, type, count;
};

uint32_t attr_type_size(uint32_t type) { return type == 3 ? 4 : type == 2 ? 2 : 1; }

struct VertexLoader {
    AttrSource attrs[12] = {};
    uint32_t attr_count = 0, fixed_mask = 0;
    bool ok = true;

    VertexLoader() {
        uint32_t base = (regs[ATTR_BASE] & 0x1FFFFFFE) << 3;
        uint64_t formats = regs[ATTR_FORMAT_LO] | (uint64_t(regs[ATTR_FORMAT_HI] & 0xFFFF) << 32);
        fixed_mask = (regs[ATTR_FORMAT_HI] >> 16) & 0xFFF;
        attr_count = (regs[ATTR_FORMAT_HI] >> 28) + 1;
        for (uint32_t l = 0; l < 12; l++) {
            const uint32_t *r = &regs[ATTR_LOADER + 3 * l];
            uint32_t comps = r[2] >> 28, stride = (r[2] >> 16) & 0xFF;
            uint64_t list = r[1] | (uint64_t(r[2] & 0xFFFF) << 32);
            uint32_t offset = 0;
            for (uint32_t c = 0; c < comps; c++) {
                uint32_t attr = (list >> (4 * c)) & 0xF;
                if (attr >= 12) { // padding of 4, 8, 12 or 16 bytes
                    offset += (attr - 11) * 4;
                    continue;
                }
                uint32_t f = (formats >> (4 * attr)) & 0xF, type = f & 3, count = (f >> 2) + 1;
                uint32_t size = attr_type_size(type);
                offset = (offset + size - 1) & ~(size - 1);
                attrs[attr] = {nullptr, stride, type, count};
                uint32_t addr = base + r[0] + offset;
                // array data is validated lazily by `read` against the committed range
                uint32_t v = to_virtual(addr);
                attrs[attr].data = v && mem::is_committed(mem::page_down(v)) ? mem::ptr(v) : nullptr;
                offset += size * count;
            }
        }
    }

    void read(uint32_t index, Vec4 out[16]) const {
        for (uint32_t i = 0; i < attr_count && i < 12; i++) {
            const AttrSource &a = attrs[i];
            if ((fixed_mask >> i) & 1 || !a.data) {
                out[i] = fixed_attr[i];
                continue;
            }
            Vec4 v = {{0, 0, 0, 1}};
            const uint8_t *p = a.data + size_t(index) * a.stride;
            for (uint32_t c = 0; c < a.count; c++) {
                switch (a.type) {
                case 0: v[c] = float(int8_t(p[c])); break;
                case 1: v[c] = float(p[c]); break;
                case 2: v[c] = float(int16_t(load16(p + 2 * c))); break;
                default: std::memcpy(&v[c], p + 4 * c, 4);
                }
            }
            out[i] = v;
        }
    }
};

// ---- vertex shading and primitive assembly ----

// vertex shader outputs -> semantics, per the output map registers
struct OutputMap {
    int8_t semantic[16][4];
    uint32_t mask;

    OutputMap() {
        std::memset(semantic, -1, sizeof semantic);
        mask = regs[VS_OUTPUT_MASK] & 0xFFFF;
        uint32_t count = regs[VS_OUTPUT_COUNT] & 7, reg = 0;
        for (uint32_t k = 0; k < count; k++) {
            while (reg < 16 && !((mask >> reg) & 1)) reg++;
            if (reg >= 16) break;
            uint32_t m = regs[VS_OUTPUT_MAP + k];
            for (int c = 0; c < 4; c++) {
                uint32_t sem = (m >> (8 * c)) & 0x1F;
                semantic[reg][c] = sem < SEMANTIC_COUNT ? int8_t(sem) : -1;
            }
            reg++;
        }
    }
};

Vertex shade(const Vec4 attrs[16], const OutputMap &map) {
    Vec4 in[16] = {}, out[16] = {};
    uint32_t inputs = (regs[VS_INPUT_CONFIG] & 0xF) + 1;
    uint64_t input_map = regs[VS_INPUT_MAP_LO] | (uint64_t(regs[VS_INPUT_MAP_HI]) << 32);
    for (uint32_t i = 0; i < inputs; i++) in[(input_map >> (4 * i)) & 0xF] = attrs[i];
    run_shader(vs, regs[VS_ENTRY] & 0xFFFF, in, out);

    Vertex v{};
    for (int r = 0; r < 16; r++)
        for (int c = 0; c < 4; c++)
            if (map.semantic[r][c] >= 0) v.a[map.semantic[r][c]] = out[r][c];
    // colors are clamped to [0, 1] by magnitude
    for (int c = 0; c < 4; c++) v.a[COLOR + c] = std::fmin(std::fabs(v.a[COLOR + c]), 1.0f);
    return v;
}

// Widescreen: whether a draw fills the screen can only be told from all of its triangles (a
// tint drawn as a grid of quads, or as a camera-facing quad in 3D), so top-screen draws are
// collected first and passed on once the draw is complete.
std::vector<Vertex> deferred; // whole triangles
bool deferring = false;

// every vertex on the original screen, together spanning its whole width and at least half its
// height as one mostly solid layer (a sunlight wedge covers ~83% of its bounding box and stops
// above the "Objective" bar; the bar itself is a thin strip, which keeps its size). The top screen is rotated: clip y runs along its width.
bool fills_screen(const std::vector<Vertex> &tris) {
    if (tris.empty()) return false;
    float xlo = 1e9f, xhi = -1e9f, ylo = 1e9f, yhi = -1e9f, area = 0;
    for (size_t i = 0; i + 2 < tris.size(); i += 3) {
        float x[3], y[3];
        for (int k = 0; k < 3; k++) {
            const float *a = tris[i + size_t(k)].a;
            if (a[POS + 3] <= 0) return false;
            x[k] = a[POS] / a[POS + 3], y[k] = a[POS + 1] / a[POS + 3];
            if (std::fabs(x[k]) > 1.02f || std::fabs(y[k]) > 1.02f) return false;
            xlo = std::min(xlo, x[k]), xhi = std::max(xhi, x[k]), ylo = std::min(ylo, y[k]), yhi = std::max(yhi, y[k]);
        }
        area += std::fabs((x[1] - x[0]) * (y[2] - y[0]) - (y[1] - y[0]) * (x[2] - x[0])) / 2;
    }
    float height = xhi - xlo;
    if (ylo > -0.98f || yhi < 0.98f) return false;
    // reaching all four edges: a frame around the screen (e.g. the name entry's backdrop ring),
    // however little of the middle it covers
    if (xlo <= -0.98f && xhi >= 0.98f) return true;
    return height >= 1.0f && area >= 0.75f * height * (yhi - ylo);
}

// ---- draw log: every draw of one frame, written with an F12 capture ----

std::mutex draw_log_lock;
std::string draw_log_path, draw_log_text;
int draw_log_state = 0; // 0 off, 1 armed (starts with the next top frame), 2 recording
struct DrawStats {
    uint32_t tris, behind; // triangles, and those with a vertex behind the eye (w <= 0)
    float xlo, xhi, ylo, yhi; // normalized device coordinates of the vertices in front
    bool fills;
    int anchor; // widescreen: the side a 2D draw keeps its distance from (see side_of)
} stats;

void log_triangle(const Vertex *const v[3]) {
    stats.tris++;
    bool behind = false;
    for (int k = 0; k < 3; k++) {
        float w = v[k]->a[POS + 3];
        if (w <= 0) {
            behind = true;
            continue;
        }
        float x = std::clamp(v[k]->a[POS] / w, -9.0f, 9.0f), y = std::clamp(v[k]->a[POS + 1] / w, -9.0f, 9.0f);
        stats.xlo = std::min(stats.xlo, x), stats.xhi = std::max(stats.xhi, x);
        stats.ylo = std::min(stats.ylo, y), stats.yhi = std::max(stats.yhi, y);
    }
    stats.behind += behind;
}

void log_draw_begin() {
    stats = {0, 0, 1e9f, -1e9f, 1e9f, -1e9f, false, 0};
}

void log_draw_end(uint32_t n, bool indexed, float w) {
    if (draw_log_state != 2) return;
    char line[512];
    std::snprintf(line, sizeof line,
                  "fb=%08x %ux%u n=%u%s topo=%u 3d=%d tris=%u behind=%u x=[%.2f,%.2f] y=[%.2f,%.2f] fills=%d anchor=%d "
                  "cull=%u depth=%x scissor=%u tex0=%08x %ux%u f%u blend=%u mask=%x vs=%x\n",
                  regs[COLOR_ADDR] << 3, regs[FB_DIM] & 0x7FF, ((regs[FB_DIM] >> 12) & 0x3FF) + 1, n,
                  indexed ? "i" : "", (regs[PRIMITIVE_CONFIG] >> 8) & 3, n && std::fabs(w - 1.0f) > 1e-4f, stats.tris,
                  stats.behind, stats.xlo, stats.xhi, stats.ylo, stats.yhi, stats.fills, stats.anchor, regs[CULL_MODE] & 3,
                  regs[DEPTH_COLOR_MASK] & 0x10F1, regs[SCISSOR_MODE] & 3, regs[TEX0_ADDR] << 3,
                  (regs[TEX0_DIM] >> 16) & 0x7FF, regs[TEX0_DIM] & 0x7FF, regs[TEX0_FORMAT] & 0xF,
                  (regs[COLOR_OP] >> 8) & 1, (regs[DEPTH_COLOR_MASK] >> 8) & 0xF, regs[VS_ENTRY] & 0xFFFF);
    std::lock_guard lk(draw_log_lock);
    draw_log_text += line;
}

// Which side a 2D draw is anchored to: -1 or 1 if it reaches that edge of the screen's width (an
// area name in the corner), sits in that half against the top or bottom edge (the loading
// screen's Jibanyan), or is drawn over something anchored (its text, if drawn apart); 0 for
// everything else: pieces that merely sit to one side of a centered element (a dialogue box's
// "continue" arrow), things reaching one edge but past the middle (most of a dialogue box), and 3D.
struct AnchoredBox {
    float xlo, xhi, ylo, yhi;
    int side;
};
std::vector<AnchoredBox> anchored; // this frame's, cleared when the top screen's frame is done
// this frame's 2D draws that reach neither side: something reaching a side but drawn over one of
// them is part of a centered whole (a talk sprite's body after its face) and stays with it
std::vector<AnchoredBox> centered;

int side_of(const std::vector<Vertex> &tris) {
    float xlo = 1e9f, xhi = -1e9f, ylo = 1e9f, yhi = -1e9f;
    for (const Vertex &v : tris) {
        if (std::fabs(v.a[POS + 3] - 1.0f) > 1e-4f) return 0;
        xlo = std::min(xlo, v.a[POS]), xhi = std::max(xhi, v.a[POS]);
        ylo = std::min(ylo, v.a[POS + 1]), yhi = std::max(yhi, v.a[POS + 1]);
    }
    auto overlaps = [&](const AnchoredBox &b) { return xlo < b.xhi && b.xlo < xhi && ylo < b.yhi && b.ylo < yhi; };
    bool at_side = ylo <= -0.97f || yhi >= 0.97f, at_top_or_bottom = xlo <= -0.97f || xhi >= 0.97f;
    // across the whole width (a banner along the bottom, a menu's title bar): it stretches to the
    // new edges, so it takes no side, and what's on it keeps to its own (an icon at the banner's end)
    if (ylo <= -0.95f && yhi >= 0.95f) {
        stats.anchor = 0;
        draw_geom = {true, xlo, xhi, ylo, yhi, 0, 0, false, true, true};
        return 0;
    }
    // particles scattered over the scene (Blasters' battle sparkles: one draw, many small quads),
    // not a piece of HUD: over half the screen both ways, made of triangles far smaller than that.
    // Its box overlaps everything, so it neither takes a side nor keeps the HUD it covers from its
    // own. A menu window as large has large triangles and still holds its pieces together
    if (xhi - xlo > 1.0f && yhi - ylo > 1.0f && tris.size() >= 24) {
        float largest = 0;
        for (size_t i = 0; i + 2 < tris.size(); i += 3) {
            const float *a = tris[i].a, *b = tris[i + 1].a, *c = tris[i + 2].a;
            largest = std::max(largest, std::fabs((b[POS] - a[POS]) * (c[POS + 1] - a[POS + 1]) -
                                                  (b[POS + 1] - a[POS + 1]) * (c[POS] - a[POS])) / 2);
        }
        if (largest < 0.02f * (xhi - xlo) * (yhi - ylo)) {
            stats.anchor = 0;
            draw_geom = {true, xlo, xhi, ylo, yhi, 0, 0, false, true, false};
            return 0;
        }
    }
    int side = 0;
    if (ylo <= -0.97f && yhi < 0.0f) side = -1;
    else if (yhi >= 0.97f && ylo > 0.0f) side = 1;
    else if (at_top_or_bottom && !at_side && (yhi < 0.0f || ylo > 0.0f)) side = yhi < 0.0f ? -1 : 1;
    int own = side;
    if (!side && !at_side) // drawn over an anchored element?
        for (const AnchoredBox &b : anchored)
            if (overlaps(b)) side = b.side;
    if (side && at_side)
        for (const AnchoredBox &b : centered)
            if (overlaps(b)) side = 0;
    if (side) anchored.push_back({xlo, xhi, ylo, yhi, side});
    else if (!at_side) centered.push_back({xlo, xhi, ylo, yhi, 0});
    stats.anchor = side;
    draw_geom = {true, xlo, xhi, ylo, yhi, side, own, at_side && !own, false, false};
    return side;
}

void emit_triangle(const Vertex &v0, const Vertex &v1, const Vertex &v2) {
    if (draw_log_state == 2) {
        const Vertex *v[3] = {&v0, &v1, &v2};
        log_triangle(v);
    }
    if (!deferring) return draw_triangle(v0, v1, v2);
    deferred.push_back(v0), deferred.push_back(v1), deferred.push_back(v2);
}

void start_deferring() {
    int margin = 0, scale = 1;
    requested_enhancement(&margin, &scale);
    deferring = margin && ((regs[FB_DIM] >> 12) & 0x3FF) + 1 == 400;
    draw_fills_screen = false;
}

void finish_deferring() {
    draw_geom.valid = false;
    if (!deferring) return;
    draw_fills_screen = stats.fills = fills_screen(deferred);
    draw_anchor = draw_fills_screen ? 0 : side_of(deferred);
    for (size_t i = 0; i + 2 < deferred.size(); i += 3) draw_triangle(deferred[i], deferred[i + 1], deferred[i + 2]);
    deferred.clear();
    deferring = draw_fills_screen = false;
    draw_anchor = 0;
}

// triangle lists, strips and fans; strips alternate which buffered vertex is replaced so every
// triangle keeps the same winding
struct Assembler {
    Vertex buffer[2];
    int next = 0;
    bool ready = false; // two vertices buffered (strip/fan)
    int list_count = 0;

    void reset() { next = 0, ready = false, list_count = 0; }

    void push(const Vertex &v) {
        uint32_t topology = (regs[PRIMITIVE_CONFIG] >> 8) & 3;
        if (topology == 1 || topology == 2) {
            if (ready) emit_triangle(buffer[0], buffer[1], v);
            buffer[next] = v;
            ready |= next == 1;
            next = topology == 1 ? 1 - next : 1;
            return;
        }
        // list (and geometry-shader topology, treated as a list without a GS)
        if (list_count < 2) {
            buffer[list_count++] = v;
            return;
        }
        emit_triangle(buffer[0], buffer[1], v);
        list_count = 0;
    }
} assembler;

void draw(bool indexed) {
    auto t_setup = std::chrono::steady_clock::now();
    begin_draw();
    start_deferring();
    log_draw_begin();
    auto t_shade = std::chrono::steady_clock::now();
    time_setup += std::chrono::duration_cast<std::chrono::nanoseconds>(t_shade - t_setup).count();
    LOG_GPU("draw%s n=%u topo=%u fb=%08x %ux%u fmt=%u tex0=%08x %ux%u f%u type%u tex1=%08x f%u cfg=%x light=%u "
            "fog=%x vs=%x attrs=%u/%x", indexed ? "i" : "", regs[VERTEX_COUNT], (regs[PRIMITIVE_CONFIG] >> 8) & 3,
            regs[COLOR_ADDR] << 3, regs[FB_DIM] & 0x7FF, ((regs[FB_DIM] >> 12) & 0x3FF) + 1,
            (regs[COLOR_FORMAT] >> 16) & 7, regs[TEX0_ADDR] << 3, (regs[TEX0_DIM] >> 16) & 0x7FF,
            regs[TEX0_DIM] & 0x7FF, regs[TEX0_FORMAT] & 0xF, (regs[TEX0_PARAM] >> 28) & 7, regs[0x095] << 3,
            regs[0x096] & 0xF, regs[TEX_CONFIG], !(regs[LIGHTING_OFF] & 1), regs[TEV_BUFFER_CONFIG] & 7,
            regs[VS_ENTRY] & 0xFFFF, (regs[ATTR_FORMAT_HI] >> 28) + 1, (regs[ATTR_FORMAT_HI] >> 16) & 0xFFF);
    if (log_cfg::gpu) {
        static const uint32_t stages[6] = {0x0C0, 0x0C8, 0x0D0, 0x0D8, 0x0F0, 0x0F8};
        for (uint32_t b : stages)
            LOG("    tev %08x %08x %08x %08x %08x", regs[b], regs[b + 1], regs[b + 2], regs[b + 3], regs[b + 4]);
    }
    VertexLoader loader;
    OutputMap map;
    uint32_t n = regs[VERTEX_COUNT];
    const uint8_t *indices = nullptr;
    bool wide = regs[INDEX_BUFFER] >> 31;
    if (indexed) {
        uint32_t base = (regs[ATTR_BASE] & 0x1FFFFFFE) << 3;
        indices = phys(base + (regs[INDEX_BUFFER] & 0x0FFFFFFF), n * (wide ? 2 : 1));
        if (!indices) {
            LOG("pica: index buffer outside GPU memory; draw skipped");
            deferring = false;
            return;
        }
    }

    float w = 1.0f; // of the last vertex: perspective (3D) draws have w != 1
    auto index_at = [&](uint32_t i) {
        return indexed ? (wide ? load16(indices + 2 * i) : indices[i]) : regs[VERTEX_OFFSET] + i;
    };
    if (n >= 512) {
        // big draws: shade each distinct vertex once, spread across the CPU's cores, then
        // assemble in order
        static std::vector<int32_t> slot;   // vertex index -> position in `unique`, or -1
        static std::vector<uint32_t> unique;
        static std::vector<Vertex> shaded;
        unique.clear();
        for (uint32_t i = 0; i < n; i++) {
            uint32_t index = index_at(i);
            if (index >= slot.size()) slot.resize(index + 1, -1);
            if (slot[index] < 0) {
                slot[index] = int32_t(unique.size());
                unique.push_back(index);
            }
        }
        shaded.resize(unique.size());
        workers().run(unique.size(), [&](size_t begin, size_t end) {
            Vec4 attrs[16];
            for (size_t k = begin; k < end; k++) {
                loader.read(unique[k], attrs);
                shaded[k] = shade(attrs, map);
            }
        });
        for (uint32_t i = 0; i < n; i++) assembler.push(shaded[size_t(slot[index_at(i)])]);
        if (!shaded.empty()) w = shaded.back().a[POS + 3];
        for (uint32_t index : unique) slot[index] = -1;
    } else {
        // indexed draws reuse shaded vertices
        struct CacheEntry {
            uint32_t index = ~0u;
            Vertex v;
        };
        static CacheEntry cache[256];
        for (auto &e : cache) e.index = ~0u;
        Vec4 attrs[16];
        for (uint32_t i = 0; i < n; i++) {
            uint32_t index = index_at(i);
            if (indexed) {
                CacheEntry &e = cache[index & 255];
                if (e.index != index) {
                    loader.read(index, attrs);
                    e.v = shade(attrs, map);
                    e.index = index;
                }
                assembler.push(e.v);
                w = e.v.a[POS + 3];
            } else {
                loader.read(index, attrs);
                Vertex v = shade(attrs, map);
                assembler.push(v);
                w = v.a[POS + 3];
            }
        }
    }
    time_vertices += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t_shade).count();
    vertex_count += n;
    finish_deferring();
    end_draw();
    log_draw_end(n, indexed, w);
    count_draw(n && std::fabs(w - 1.0f) > 1e-4f);
}

// ---- scene statistics (for the frontend's context-aware layout) ----

std::mutex scene_lock;
SceneInfo scene_published;
SceneInfo scene_building; // the frames being drawn now (top and bottom passes)

// a cheap identity for a texture: size, format and its first bytes
uint64_t texture_signature(const Texture &t) {
    uint64_t h = (uint64_t(t.width) << 40) ^ (uint64_t(t.height) << 24) ^ t.format;
    for (uint32_t i = 0; i < std::min<uint32_t>(t.bytes, 256); i++) h = (h ^ t.data[i]) * 0x100000001b3ull;
    return h;
}

void count_draw(bool is_3d) {
    uint32_t height = ((regs[FB_DIM] >> 12) & 0x3FF) + 1;
    if (height == 400) {
        scene_building.top_draws++;
        if (is_3d) scene_building.top_3d_draws++;
    } else if (height == 320) {
        scene_building.bottom_draws++;
        Texture t = load_texture(0);
        if (t.data) {
            uint64_t sig = texture_signature(t);
            auto &v = scene_building.bottom_textures;
            if (std::find(v.begin(), v.end(), sig) == v.end() && v.size() < 64) v.push_back(sig);
        }
    }
}

// immediate mode: attributes written one by one through the fixed-attribute registers
void write_fixed_attr(uint32_t value) {
    fixed_buffer[fixed_words++] = value;
    if (fixed_words < 3) return;
    fixed_words = 0;
    Vec4 v = unpack_f24x4(fixed_buffer);
    uint32_t index = regs[FIXED_ATTR_INDEX] & 0xF;
    if (index != 0xF) {
        if (index < 16) fixed_attr[index] = v;
        return;
    }
    immediate[immediate_count++] = v;
    if (immediate_count > int(regs[VS_INPUT_LAST] & 0xF)) {
        immediate_count = 0;
        begin_draw();
        assembler.push(shade(immediate, OutputMap()));
        end_draw();
    }
}

// per-register write handling; returns a command list to jump to (index 0/1), or -1
int write(uint32_t id, uint32_t value, uint32_t mask) {
    if (id >= REG_COUNT) return -1;
    regs[id] = (regs[id] & ~mask) | (value & mask);
    value = regs[id];

    switch (id) {
    case DRAW_ARRAYS: draw(false); break;
    case DRAW_INDEXED: draw(true); break;
    case FIXED_ATTR_INDEX:
        fixed_words = 0;
        immediate_count = 0;
        break;
    case FIXED_ATTR_DATA:
    case FIXED_ATTR_DATA + 1:
    case FIXED_ATTR_DATA + 2: write_fixed_attr(value); break;
    case PRIMITIVE_CONFIG:
    case PRIMITIVE_RESTART: assembler.reset(); break;
    case CMDBUF_JUMP0: return 0;
    case CMDBUF_JUMP1: return 1;

    case VS_FLOAT_INDEX: float_words = 0; break;
    case VS_INT:
    case VS_INT + 1:
    case VS_INT + 2:
    case VS_INT + 3:
        for (int c = 0; c < 4; c++) vs.i[id - VS_INT][c] = uint8_t(value >> (8 * c));
        break;
    case VS_BOOL: vs.b = uint16_t(value); break;
    case VS_CODE_INDEX:
    case VS_SWIZZLE_INDEX: break;
    default:
        if (id >= VS_FLOAT_DATA && id < VS_FLOAT_DATA + 8) {
            write_float_uniform(value);
        } else if (id >= VS_CODE_DATA && id < VS_CODE_DATA + 8) {
            uint32_t &at = regs[VS_CODE_INDEX];
            vs.code[at & 4095] = value;
            at = (at & ~0xFFFu) | ((at + 1) & 0xFFF);
        } else if (id >= LIGHTING_LUT_DATA && id < LIGHTING_LUT_DATA + 8) {
            // index in bits 0-7 and table in bits 8-12 of the index register, auto-incrementing
            uint32_t &at = regs[LIGHTING_LUT_INDEX];
            uint32_t table = (at >> 8) & 0x1F;
            if (table < 24) lighting_lut[table][at & 0xFF] = value, lighting_lut_version++;
            at = (at & ~0xFFu) | ((at + 1) & 0xFF);
        } else if (id >= FOG_LUT_DATA && id < FOG_LUT_DATA + 8) {
            uint32_t &at = regs[FOG_LUT_INDEX];
            fog_lut[at & 127] = value;
            at = (at & ~0x7Fu) | ((at + 1) & 0x7F);
        } else if (id >= VS_SWIZZLE_DATA && id < VS_SWIZZLE_DATA + 8) {
            uint32_t &at = regs[VS_SWIZZLE_INDEX];
            vs.swizzle[at & 127] = value;
            at = (at & ~0x7Fu) | ((at + 1) & 0x7F);
        }
    }
    return -1;
}

uint32_t byte_mask(uint32_t nibble) {
    return (nibble & 1 ? 0xFFu : 0) | (nibble & 2 ? 0xFF00u : 0) | (nibble & 4 ? 0xFF0000u : 0) |
           (nibble & 8 ? 0xFF000000u : 0);
}

} // namespace

namespace {
void process_command_list(uint32_t addr, uint32_t size) {
    for (int lists = 0; lists < 4096; lists++) {
        if (!mem::is_committed(mem::page_down(addr))) {
            LOG("pica: command list at %08x is unmapped", addr);
            return;
        }
        const uint32_t *p = reinterpret_cast<const uint32_t *>(mem::ptr(addr));
        const uint32_t *end = p + size / 4;
        int jump = -1;
        while (p + 2 <= end && jump < 0) {
            uint32_t value = p[0], header = p[1];
            p += 2;
            uint32_t id = header & 0xFFFF, mask = byte_mask((header >> 16) & 0xF);
            uint32_t extra = (header >> 20) & 0x7FF;
            bool consecutive = header >> 31;
            jump = write(id, value, mask);
            for (uint32_t i = 0; i < extra && p + i < end && jump < 0; i++) {
                if (consecutive) id++;
                jump = write(id, p[i], mask);
            }
            p += extra + (extra & 1);
        }
        if (jump < 0) return;
        // the command buffer "jump" registers switch to another list in GPU memory
        uint32_t target = to_virtual((regs[CMDBUF_ADDR0 + jump] & 0x0FFFFFFF) << 3);
        if (!target) {
            LOG("pica: command buffer jump to unmapped %08x", regs[CMDBUF_ADDR0 + jump] << 3);
            return;
        }
        addr = target;
        size = (regs[CMDBUF_SIZE0 + jump] & 0x1FFFFF) << 3;
    }
}

} // namespace

std::atomic<int64_t> time_commands{0}, time_raster{0}, time_transfer{0}, time_setup{0}, time_vertices{0}, vertex_count{0};

// a burst: the next `frames` top frames, one after another, as <dir>/000_draws.txt and on
std::string draw_burst_dir;
int draw_burst_left = 0, draw_burst_at = 0;

void request_draw_log(const std::string &path) {
    std::lock_guard lk(draw_log_lock);
    draw_log_path = path, draw_log_text.clear();
    draw_log_state = 1;
}

void draw_log_note(const char *line) {
    std::lock_guard lk(draw_log_lock);
    if (draw_log_state == 2) draw_log_text += line;
}

void request_draw_burst(const std::string &dir, int frames) {
    std::lock_guard lk(draw_log_lock);
    draw_burst_dir = dir, draw_burst_left = frames, draw_burst_at = 0;
    char name[16];
    std::snprintf(name, sizeof name, "/%03d_draws.txt", draw_burst_at++);
    draw_log_path = dir + name, draw_log_text.clear();
    draw_log_state = 1;
}

void scene_frame_done(uint32_t height) {
    if (height == 400) anchored.clear(), centered.clear();
    if (height == 400 && draw_log_state) {
        std::lock_guard lk(draw_log_lock);
        if (draw_log_state == 2) {
            if (FILE *f = std::fopen(draw_log_path.c_str(), "w")) {
                std::fputs("# every draw of one top-screen frame, in order. x/y: normalized device coordinates of\n"
                           "# the vertices in front of the eye; on the top screen x runs along its height (-1 =\n"
                           "# bottom) and y along its width\n", f);
                std::fputs(draw_log_text.c_str(), f);
                std::fclose(f);
            }
            draw_log_state = 0;
            if (--draw_burst_left > 0) { // the next frame of a burst
                char name[16];
                std::snprintf(name, sizeof name, "/%03d_draws.txt", draw_burst_at++);
                draw_log_path = draw_burst_dir + name, draw_log_text.clear();
                draw_log_state = 2;
            }
        } else {
            draw_log_state = 2;
        }
    }
    std::lock_guard lk(scene_lock);
    if (height == 400) {
        scene_published.top_draws = scene_building.top_draws;
        scene_published.top_3d_draws = scene_building.top_3d_draws;
        scene_building.top_draws = scene_building.top_3d_draws = 0;
    } else if (height == 320) {
        scene_published.bottom_draws = scene_building.bottom_draws;
        scene_published.bottom_textures = std::move(scene_building.bottom_textures);
        scene_building.bottom_draws = 0;
        scene_building.bottom_textures.clear();
    }
    scene_published.serial++;
}

bool top_has_3d() { return scene_published.top_3d_draws > 0 || scene_building.top_3d_draws > 0; }

SceneInfo scene_info() {
    std::lock_guard lk(scene_lock);
    return scene_published;
}

void run_command_list(uint32_t addr, uint32_t size) {
    auto t0 = std::chrono::steady_clock::now();
    process_command_list(addr, size);
    auto t1 = std::chrono::steady_clock::now();
    flush_draws(); // the GPU is done with this list once everything in it is drawn
    auto t2 = std::chrono::steady_clock::now();
    int64_t commands = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    int64_t raster = std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count();
    time_commands += commands;
    time_raster += raster;
    // YW2_FPS: name what a slow list (over half a 60 fps frame) spent its time on
    static const bool stats = std::getenv("YW2_FPS") != nullptr;
    gpu::Costs &c = gpu::costs;
    if (stats && commands + raster > 8'000'000)
        LOG("slow list: %.1f ms (commands %.1f, submit %.1f) | %d new pipelines %.1f ms, %d texture uploads %.1f ms,"
            " %d readbacks %.1f ms",
            (commands + raster) / 1e6, commands / 1e6, raster / 1e6, c.pipelines, c.pipeline_ns / 1e6, c.textures,
            c.texture_ns / 1e6, c.downloads, c.download_ns / 1e6);
    c = {};
}

} // namespace pica
