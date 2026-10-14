#pragma once
// Shared state of the PICA200 implementation (command processor, shaders, rasterizer).
#include "color.h"

#include <cstdint>
#include <cstring>
#include <atomic>
#include <memory>

namespace pica {

// Register indices (32-bit words). Names follow 3dbrew's GPU/Internal Registers.
enum Reg : uint32_t {
    CULL_MODE = 0x040,
    VIEWPORT_WIDTH = 0x041, // f24, half width
    VIEWPORT_HEIGHT = 0x043,
    DEPTH_SCALE = 0x04D, // f24
    DEPTH_OFFSET = 0x04E,
    VS_OUTPUT_COUNT = 0x04F,
    VS_OUTPUT_MAP = 0x050, // 7 registers, one semantic byte per component
    SCISSOR_MODE = 0x065,
    SCISSOR_MIN = 0x066,
    SCISSOR_MAX = 0x067,
    VIEWPORT_XY = 0x068,
    DEPTHMAP_ENABLE = 0x06D,

    TEX_CONFIG = 0x080,
    TEX0_BORDER = 0x081, // unit 0; units 1 and 2 are at 0x091 and 0x099
    TEX0_DIM = 0x082,
    TEX0_PARAM = 0x083,
    TEX0_ADDR = 0x085,
    TEX0_FORMAT = 0x08E,

    TEV_BUFFER_CONFIG = 0x0E0, // also fog mode (bits 0-2) and fog flip (bit 16)
    FOG_COLOR = 0x0E1,
    FOG_LUT_INDEX = 0x0E6,
    FOG_LUT_DATA = 0x0E8, // 8 registers
    TEV_BUFFER_COLOR = 0x0FD,

    COLOR_OP = 0x100,
    BLEND_FUNC = 0x101,
    LOGIC_OP = 0x102,
    BLEND_COLOR = 0x103,
    ALPHA_TEST = 0x104,
    STENCIL_TEST = 0x105,
    STENCIL_OP = 0x106,
    DEPTH_COLOR_MASK = 0x107,
    COLOR_WRITE_ALLOW = 0x113,
    DEPTH_STENCIL_READ_ALLOW = 0x114,
    DEPTH_STENCIL_WRITE_ALLOW = 0x115,
    DEPTH_FORMAT = 0x116,
    COLOR_FORMAT = 0x117,
    DEPTH_ADDR = 0x11C,
    COLOR_ADDR = 0x11D,
    FB_DIM = 0x11E,

    LIGHT0 = 0x140, // 8 lights of 0x10 registers
    LIGHTING_AMBIENT = 0x1C0,
    LIGHTING_NUM_LIGHTS = 0x1C2,
    LIGHTING_CONFIG0 = 0x1C3,
    LIGHTING_CONFIG1 = 0x1C4,
    LIGHTING_LUT_INDEX = 0x1C5,
    LIGHTING_OFF = 0x1C6, // bit 0 set = fragment lighting off
    LIGHTING_LUT_DATA = 0x1C8, // 8 registers
    LIGHTING_LUT_ABS = 0x1D0,
    LIGHTING_LUT_INPUT = 0x1D1,
    LIGHTING_LUT_SCALE = 0x1D2,
    LIGHTING_PERMUTATION = 0x1D9,

    ATTR_BASE = 0x200,
    ATTR_FORMAT_LO = 0x201,
    ATTR_FORMAT_HI = 0x202,
    ATTR_LOADER = 0x203, // 12 loaders of 3 registers
    INDEX_BUFFER = 0x227,
    VERTEX_COUNT = 0x228,
    VERTEX_OFFSET = 0x22A,
    DRAW_ARRAYS = 0x22E,
    DRAW_INDEXED = 0x22F,
    FIXED_ATTR_INDEX = 0x232,
    FIXED_ATTR_DATA = 0x233, // 3 registers
    CMDBUF_SIZE0 = 0x238,
    CMDBUF_ADDR0 = 0x23A,
    CMDBUF_JUMP0 = 0x23C,
    CMDBUF_JUMP1 = 0x23D,
    VS_INPUT_LAST = 0x242,
    PRIMITIVE_CONFIG = 0x25E,
    PRIMITIVE_RESTART = 0x25F,

    VS_BOOL = 0x2B0,
    VS_INT = 0x2B1, // 4 registers
    VS_INPUT_CONFIG = 0x2B9,
    VS_ENTRY = 0x2BA,
    VS_INPUT_MAP_LO = 0x2BB,
    VS_INPUT_MAP_HI = 0x2BC,
    VS_OUTPUT_MASK = 0x2BD,
    VS_FLOAT_INDEX = 0x2C0,
    VS_FLOAT_DATA = 0x2C1, // 8 registers
    VS_CODE_INDEX = 0x2CB,
    VS_CODE_DATA = 0x2CC, // 8 registers
    VS_SWIZZLE_INDEX = 0x2D5,
    VS_SWIZZLE_DATA = 0x2D6, // 8 registers

    REG_COUNT = 0x300,
};

extern uint32_t regs[REG_COUNT];

// lookup tables uploaded through registers: 24 lighting LUTs of 256 entries (12-bit value,
// signed 12-bit difference to the next entry) and the 128-entry fog LUT
extern uint32_t lighting_lut[24][256];
extern uint32_t fog_lut[128];

// 24-bit float: sign, 7-bit exponent (bias 63), 16-bit mantissa; no infinities or NaNs
inline float f24(uint32_t v) {
    if ((v & 0x7FFFFF) == 0) return (v & 0x800000) ? -0.0f : 0.0f;
    uint32_t bits = ((v & 0x800000) << 8) | ((((v >> 16) & 0x7F) + 64) << 23) | ((v & 0xFFFF) << 7);
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

struct Vec4 {
    float v[4];
    float &operator[](int i) { return v[i]; }
    float operator[](int i) const { return v[i]; }
};

// Vertex attributes after the vertex shader, indexed by output semantic
enum Semantic : int {
    POS = 0,      // x y z w
    QUAT = 4,     // x y z w
    COLOR = 8,    // r g b a
    TEX0 = 12,    // u v
    TEX1 = 14,    // u v
    TEX0_W = 16,
    VIEW = 18,    // x y z
    TEX2 = 22,    // u v
    SEMANTIC_COUNT = 24,
};
struct Vertex {
    float a[SEMANTIC_COUNT];
};

// Vertex shader program and uniforms
struct ShaderSetup {
    uint32_t code[4096];
    uint32_t swizzle[128];
    Vec4 f[96];
    uint8_t i[4][4]; // loop count, initial aL, increment, unused
    uint16_t b;
};
extern ShaderSetup vs;

// runs the vertex shader from `entry` on input registers v0-v15, writing o0-o15
void run_shader(const ShaderSetup &s, uint32_t entry, const Vec4 in[16], Vec4 out[16]);

// A texture unit's configuration, latched at draw time
struct Texture {
    const uint8_t *data = nullptr; // null when disabled or outside GPU memory
    uint32_t addr = 0, bytes = 0;  // GPU physical address and size of the texel data
    uint32_t width = 0, height = 0, format = 0, wrap_s = 0, wrap_t = 0;
    bool mag_linear = false, min_linear = false;
    Rgba border{};
};
// latches texture unit 0-2 from the registers
Texture load_texture(int unit);
// samples at normalized coordinates (v = 0 is the bottom row, as on the PICA)
Rgba sample_texture(const Texture &t, float u, float v);
// decodes the whole texture to RGBA bytes, rows in memory order (row 0 is v = 1)
void decode_texture(const Texture &t, uint8_t *rgba);

// the widescreen margin and resolution scale the user asked for
void requested_enhancement(int *margin, int *scale);
// the HUD size the user asked for (1 = the game's own; GPU renderer only)
float hud_scale();
// whether the top screen shows 3D (last frame, or so far in this one): on the game thread. Without
// it, the top screen is a menu, all of it UI, and the HUD size leaves it alone
bool top_has_3d();

// display transfer between host buffers (dims as in the GX command; output dims pre-scaling)
void convert_transfer(const uint8_t *src, uint32_t in_w, uint32_t in_h, uint8_t *dst, uint32_t out_w, uint32_t out_h,
                      uint32_t flags);

// ---- widescreen (widescreen.cpp) ----
extern int wide_margin; // extra rows on each side of top-screen targets; 0 = off
// widescreen: the current draw covers the whole (original) screen, like a fade, a tint or a
// light overlay, so it is stretched into the margins as one layer, whatever its triangles
extern bool draw_fills_screen;
// widescreen: the side a 2D draw is anchored to (-1, 0 or 1 along clip y, the top screen's width; see side_of). HUD on
// the sides moves out to the new edges (an area name in the top-left corner stays in the corner);
// what's in the middle stays centered
extern int draw_anchor;
// widescreen: the last 2D draw's box (clip space, before any shift) and how it was placed, for the
// GPU renderer to place pieces that belong together the same way once the frame is drawn (a
// speech bubble's background, its text and its portrait; see anchor_groups in gpu.cpp)
struct DrawGeom {
    bool valid = false; // a 2D draw on the top screen, not a full-screen layer
    float xlo, xhi, ylo, yhi;
    int applied; // the side it was shifted to when drawn
    int own;     // the side its own position says (touching an edge), ignoring its neighbours
    bool pins;   // reaches past the middle (a dialogue box): what it's part of stays centered
};
extern DrawGeom draw_geom;
struct ShadowTarget {
    uint8_t *color, *depth;
    uint32_t width, height; // in shadow pixels
    int scale, margin;      // resolution factor; extra (unscaled) rows on each side
};
// redirects a top-screen render target to its enhanced shadow; false if this target isn't one
bool wide_target(uint32_t color_addr, uint32_t depth_addr, uint32_t width, uint32_t height, uint32_t color_format,
                 uint32_t depth_format, ShadowTarget *out);
// call before a draw samples a texture at `addr`, so render-to-texture sees current pixels
void wide_before_texture(uint32_t addr);
// display-transfer hook: makes the wide frame and syncs the game's buffer (if `in` is shadowed)
void wide_display_transfer(uint32_t in, uint32_t out, uint32_t in_w, uint32_t in_h, uint32_t out_w, uint32_t flags);

// Fragment lighting: a snapshot of the lighting registers and LUTs, taken per draw (and reused
// while they don't change), so queued draws keep the lighting they were issued with
struct LightingState;
std::shared_ptr<const LightingState> latch_lighting();
// computes the primary (diffuse) and secondary (specular) fragment colors from the
// interpolated normal quaternion and view vector; `tex` holds texture units 0-3
void fragment_lighting(const LightingState &ls, const float quat[4], const float view[3], const Rgba tex[4],
                       Rgba &primary, Rgba &secondary);
extern uint32_t lighting_lut_version; // bumped by every LUT write

// clips, rasterizes and shades one triangle (clip-space vertices) into the current render target
void draw_triangle(const Vertex &v0, const Vertex &v1, const Vertex &v2);
// called when a draw starts, to latch per-draw state (render target, textures, TEV)
void begin_draw();
// wall-clock time spent per stage, for YW2_FPS (nanoseconds)
extern std::atomic<int64_t> time_commands, time_raster, time_transfer, time_setup, time_vertices, vertex_count;

// queues the triangles collected since begin_draw
void end_draw();
// rasterizes all queued draws (in parallel); called when a command list ends
void flush_draws();

} // namespace pica
