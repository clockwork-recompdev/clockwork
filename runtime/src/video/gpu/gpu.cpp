// GPU renderer (SDL_GPU). See gpu.h.
//
// Surfaces: render targets live in GPU textures keyed by guest address, size and format, at
// the requested resolution scale (and 16:9 margin for top-screen targets). They hold rows in
// memory order, like the PICA's tiled buffers, so they can be sampled as textures directly
// (render-to-texture) with the same coordinate rule as uploaded textures. Clears and display
// transfers to the LCDs stay on the GPU; transfers elsewhere read the surface back to guest
// memory, and CPU-side writes to a surface's memory make it reload from guest memory.
//
// Draws: triangles arrive shaded and clip-space from the CPU; culling and the widescreen
// adjustment happen per triangle here. Each draw records its pipeline (blend/depth/stencil/
// mask state), textures and a uniform block for the uber-shader (TEV, lighting, fog, alpha
// test). A command list's draws are recorded into one command buffer when it ends.
#include "gpu.h"

#include "log.h"
#include "memory.h"
#include "video/pica.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <unordered_set>
#include <unordered_map>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace pica::gpu {

Costs costs;

namespace {

#include "shaders.inc"

struct GpuVertex {
    float pos[4], color[4], tc01[4], tc2w[4], quat[4], view[4];
};

// std140 layout matching pica.frag's Frag block
struct FragUniforms {
    int32_t tev_src[6][4];
    int32_t tev_op[6][4];
    float tev_const[6][4];
    float buffer_init[4];
    int32_t misc[4];
    int32_t flags[4];
    float fog_color[4];
    int32_t tex_wrap[3][4];
    float tex_border[3][4];
    int32_t light_misc[4];
    int32_t lut_use[7][4];
    float lut_scale[7][4];
    float ambient[4];
    float light[56][4];
};
static_assert(sizeof(FragUniforms) == 1600, "must match the shader's uniform block");

struct BlitUniforms {
    float size[4];
    int32_t mode[4];
};

SDL_GPUDevice *dev = nullptr;
std::atomic<int> readbacks{0}; // GPU -> guest memory copies, for YW2_FPS
bool ok = false;
SDL_GPUShader *vs_pica, *fs_pica, *vs_full, *fs_blit;
SDL_GPUTextureFormat depth_format;
SDL_GPUTexture *lut_tex, *dummy_tex;
SDL_GPUSampler *nearest_clamp, *linear_clamp;
SDL_GPUGraphicsPipeline *blit_pipeline;

// ---- surfaces ----

struct Surface {
    uint32_t addr, width, height, format; // guest buffer: address, size, color or depth format
    bool depth;
    int scale, margin;
    uint32_t tw, th; // texture size
    SDL_GPUTexture *tex;
    bool reload; // guest memory has content the texture lacks
    bool fresh;  // just created: cleared before first use (GPU memory starts with leftovers)
    bool drawn;  // the GPU drew into it since guest memory last had its contents
    // guest memory when it and the texture last agreed (loaded, downloaded or filled): if it
    // changes, the CPU put something else there (memory is reused, e.g. for streamed map data)
    uint64_t guest_hash;
    uint64_t last_used; // use_clock when last drawn to or sampled

    uint32_t bpp() const { return depth ? (format == 0 ? 2 : format == 3 ? 4 : 3) : fb_bpp(format); }
    uint32_t bytes() const { return width * height * bpp(); }
};
std::vector<std::unique_ptr<Surface>> surfaces;
uint64_t use_clock = 0; // top-screen frames so far, for evicting what the game stopped using

uint64_t hash_bytes(const uint8_t *p, size_t n);

uint64_t guest_hash(const Surface &s) {
    const uint8_t *g = phys(s.addr, s.bytes());
    return g ? hash_bytes(g, s.bytes()) : 0;
}

// true (and the texture is dropped) if the CPU has replaced the surface's memory since it and
// the texture last agreed; the GPU's image is then obsolete and must never be written back
bool overwritten(Surface *s) {
    if (s->depth) return false;
    if (guest_hash(*s) == s->guest_hash) return false;
    s->reload = true, s->drawn = false;
    return true;
}

uint32_t to_physical(uint32_t addr) {
    if (addr >= mem::VRAM_BASE && addr < mem::VRAM_BASE + mem::VRAM_SIZE) return addr - mem::VRAM_BASE + 0x18000000;
    if (addr >= mem::LINEAR_BASE && addr < mem::LINEAR_BASE + 0x08000000) return addr - mem::LINEAR_BASE + 0x20000000;
    return addr;
}

SDL_GPUTexture *make_texture(uint32_t w, uint32_t h, SDL_GPUTextureFormat format, SDL_GPUTextureUsageFlags usage) {
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = format;
    info.usage = usage;
    info.width = w, info.height = h;
    info.layer_count_or_depth = 1, info.num_levels = 1;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    SDL_GPUTexture *t = SDL_CreateGPUTexture(dev, &info);
    if (!t) fatal("GPU texture %ux%u: %s", w, h, SDL_GetError());
    return t;
}

Surface *get_surface(uint32_t addr, uint32_t w, uint32_t h, uint32_t format, bool depth) {
    int margin, scale;
    requested_enhancement(&margin, &scale);
    if (h != 400) margin = 0; // only top-screen targets widen
    for (auto &s : surfaces)
        if (s->addr == addr && s->width == w && s->height == h && s->format == format && s->depth == depth &&
            s->scale == scale && s->margin == margin) {
            s->last_used = use_clock;
            return s.get();
        }
    auto s = std::make_unique<Surface>();
    s->addr = addr, s->width = w, s->height = h, s->format = format, s->depth = depth;
    s->scale = scale, s->margin = margin;
    s->tw = w * uint32_t(scale), s->th = (h + 2 * uint32_t(margin)) * uint32_t(scale);
    s->tex = depth ? make_texture(s->tw, s->th, depth_format, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)
                   : make_texture(s->tw, s->th, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                                  SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
    s->reload = true, s->fresh = true, s->drawn = false, s->guest_hash = 0, s->last_used = use_clock;
    surfaces.push_back(std::move(s));
    return surfaces.back().get();
}

Surface *find_color_surface(uint32_t addr) {
    Surface *best = nullptr;
    for (auto &s : surfaces)
        if (!s->depth && s->addr == addr && (!best || s->width * s->height > best->width * best->height))
            best = s.get();
    return best;
}

// ---- textures, samplers, pipelines ----

struct CachedTexture {
    SDL_GPUTexture *tex;
    uint64_t hash;
    uint64_t last_used; // use_clock
};
std::map<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>, CachedTexture> textures;
std::map<int, SDL_GPUSampler *> samplers;
std::map<std::string, SDL_GPUGraphicsPipeline *> pipelines;

uint64_t hash_bytes(const uint8_t *p, size_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        std::memcpy(&w, p + i, 8);
        h = (h ^ w) * 0x100000001b3ull;
        h ^= h >> 29;
    }
    for (; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

SDL_GPUSampler *sampler(bool linear, uint32_t wrap_s, uint32_t wrap_t) {
    int key = int(linear) | int(wrap_s & 7) << 1 | int(wrap_t & 7) << 4;
    auto it = samplers.find(key);
    if (it != samplers.end()) return it->second;
    auto mode = [](uint32_t w) {
        switch (w & 7) {
        case 2:
        case 6:
        case 7: return SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
        case 3: return SDL_GPU_SAMPLERADDRESSMODE_MIRRORED_REPEAT;
        default: return SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        }
    };
    SDL_GPUSamplerCreateInfo info{};
    info.min_filter = info.mag_filter = linear ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
    info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    info.address_mode_u = mode(wrap_s), info.address_mode_v = mode(wrap_t);
    info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    SDL_GPUSampler *s = SDL_CreateGPUSampler(dev, &info);
    samplers[key] = s;
    return s;
}

struct PipelineKey {
    uint32_t blend_cfg;
    uint8_t blend, color_mask, depth_test, depth_func, depth_write, stencil, stencil_func, stencil_fail,
        stencil_zfail, stencil_zpass, stencil_read, stencil_write, has_depth, pad[3];
};

SDL_GPUBlendFactor blend_factor(uint32_t f) {
    static constexpr SDL_GPUBlendFactor F[16] = {
        SDL_GPU_BLENDFACTOR_ZERO,           SDL_GPU_BLENDFACTOR_ONE,
        SDL_GPU_BLENDFACTOR_SRC_COLOR,      SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR,
        SDL_GPU_BLENDFACTOR_DST_COLOR,      SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_COLOR,
        SDL_GPU_BLENDFACTOR_SRC_ALPHA,      SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
        SDL_GPU_BLENDFACTOR_DST_ALPHA,      SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA,
        SDL_GPU_BLENDFACTOR_CONSTANT_COLOR, SDL_GPU_BLENDFACTOR_ONE_MINUS_CONSTANT_COLOR,
        SDL_GPU_BLENDFACTOR_CONSTANT_COLOR, SDL_GPU_BLENDFACTOR_ONE_MINUS_CONSTANT_COLOR, // constant alpha
        SDL_GPU_BLENDFACTOR_SRC_ALPHA_SATURATE, SDL_GPU_BLENDFACTOR_ONE,
    };
    return F[f & 0xF];
}

SDL_GPUBlendOp blend_op(uint32_t e) {
    static constexpr SDL_GPUBlendOp E[8] = {SDL_GPU_BLENDOP_ADD, SDL_GPU_BLENDOP_SUBTRACT,
                                            SDL_GPU_BLENDOP_REVERSE_SUBTRACT, SDL_GPU_BLENDOP_MIN,
                                            SDL_GPU_BLENDOP_MAX, SDL_GPU_BLENDOP_ADD,
                                            SDL_GPU_BLENDOP_ADD, SDL_GPU_BLENDOP_ADD};
    return E[e & 7];
}

SDL_GPUCompareOp compare_op(uint32_t f) {
    static constexpr SDL_GPUCompareOp C[8] = {SDL_GPU_COMPAREOP_NEVER,         SDL_GPU_COMPAREOP_ALWAYS,
                                              SDL_GPU_COMPAREOP_EQUAL,         SDL_GPU_COMPAREOP_NOT_EQUAL,
                                              SDL_GPU_COMPAREOP_LESS,          SDL_GPU_COMPAREOP_LESS_OR_EQUAL,
                                              SDL_GPU_COMPAREOP_GREATER,       SDL_GPU_COMPAREOP_GREATER_OR_EQUAL};
    return C[f & 7];
}

SDL_GPUStencilOp stencil_op(uint32_t o) {
    static constexpr SDL_GPUStencilOp S[8] = {
        SDL_GPU_STENCILOP_KEEP,      SDL_GPU_STENCILOP_ZERO,          SDL_GPU_STENCILOP_REPLACE,
        SDL_GPU_STENCILOP_INCREMENT_AND_CLAMP, SDL_GPU_STENCILOP_DECREMENT_AND_CLAMP, SDL_GPU_STENCILOP_INVERT,
        SDL_GPU_STENCILOP_INCREMENT_AND_WRAP, SDL_GPU_STENCILOP_DECREMENT_AND_WRAP};
    return S[o & 7];
}

int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

SDL_GPUGraphicsPipeline *pipeline(const PipelineKey &k) {
    std::string key(reinterpret_cast<const char *>(&k), sizeof k);
    auto it = pipelines.find(key);
    if (it != pipelines.end()) return it->second;
    int64_t t0 = now_ns();

    SDL_GPUVertexBufferDescription vb{};
    vb.slot = 0, vb.pitch = sizeof(GpuVertex), vb.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    SDL_GPUVertexAttribute attrs[6];
    for (uint32_t i = 0; i < 6; i++)
        attrs[i] = {i, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, i * 16};

    SDL_GPUColorTargetDescription color{};
    color.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    auto &b = color.blend_state;
    b.enable_blend = k.blend;
    b.color_blend_op = blend_op(k.blend_cfg), b.alpha_blend_op = blend_op(k.blend_cfg >> 8);
    b.src_color_blendfactor = blend_factor(k.blend_cfg >> 16), b.dst_color_blendfactor = blend_factor(k.blend_cfg >> 20);
    b.src_alpha_blendfactor = blend_factor(k.blend_cfg >> 24), b.dst_alpha_blendfactor = blend_factor(k.blend_cfg >> 28);
    b.enable_color_write_mask = true;
    b.color_write_mask = k.color_mask;

    SDL_GPUGraphicsPipelineCreateInfo info{};
    info.vertex_shader = vs_pica, info.fragment_shader = fs_pica;
    info.vertex_input_state.vertex_buffer_descriptions = &vb;
    info.vertex_input_state.num_vertex_buffers = 1;
    info.vertex_input_state.vertex_attributes = attrs;
    info.vertex_input_state.num_vertex_attributes = 6;
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE; // culled per triangle on the CPU
    info.rasterizer_state.enable_depth_clip = false; // clamp, as the PICA does (planes are clip distances)
    auto &ds = info.depth_stencil_state;
    ds.enable_depth_test = k.has_depth; // depth writes need the test enabled; ALWAYS when the game's test is off
    ds.compare_op = k.depth_test ? compare_op(k.depth_func) : SDL_GPU_COMPAREOP_ALWAYS;
    ds.enable_depth_write = k.has_depth && k.depth_write;
    ds.enable_stencil_test = k.has_depth && k.stencil;
    SDL_GPUStencilOpState so{stencil_op(k.stencil_fail), stencil_op(k.stencil_zpass), stencil_op(k.stencil_zfail),
                             compare_op(k.stencil_func)};
    ds.front_stencil_state = ds.back_stencil_state = so;
    ds.compare_mask = k.stencil_read, ds.write_mask = k.stencil_write;
    info.target_info.color_target_descriptions = &color;
    info.target_info.num_color_targets = 1;
    info.target_info.has_depth_stencil_target = k.has_depth;
    info.target_info.depth_stencil_format = depth_format;
    SDL_GPUGraphicsPipeline *p = SDL_CreateGPUGraphicsPipeline(dev, &info);
    if (!p) fatal("GPU pipeline: %s", SDL_GetError());
    pipelines[key] = p;
    costs.pipelines++, costs.pipeline_ns += now_ns() - t0;
    return p;
}

// ---- the batch ----

struct DrawRec {
    SDL_GPUGraphicsPipeline *pipeline;
    Surface *color, *depth;
    SDL_GPUViewport viewport;
    SDL_Rect scissor;
    uint8_t stencil_ref;
    SDL_FColor blend_const;
    SDL_GPUTexture *tex[3];
    SDL_GPUSampler *samp[3];
    FragUniforms fu;
    float depth_map[4];
    uint32_t first, count;
};

struct TextureUpload {
    SDL_GPUTexture *tex;
    std::vector<uint8_t> rgba;
    uint32_t w, h;
};
struct SurfaceLoad {
    Surface *surface;
    std::vector<uint8_t> rgba; // native size, rows in memory order
};

std::vector<DrawRec> batch;
std::vector<GpuVertex> vertices;
std::vector<TextureUpload> texture_uploads;
std::vector<SurfaceLoad> surface_loads;
std::vector<Surface *> depth_clears; // new depth surfaces start at the far plane
bool lut_upload = false;
uint32_t lut_version_done = ~0u, fog_done[128];
std::vector<float> lut_pixels(256 * 25 * 2);

DrawRec cur;
bool cur_skip = true;
float cur_ky = 1.0f; // widescreen: clip-space y factor for triangles that keep their size
uint32_t cur_cull = 0;
uint32_t cur_tex0 = 0; // texture unit 0's address, to recognize a HUD element between frames

// decodes a guest color buffer to RGBA rows in memory order
std::vector<uint8_t> read_guest(const Surface &s) {
    std::vector<uint8_t> out(size_t(s.width) * s.height * 4, 0);
    const uint8_t *g = phys(s.addr, s.bytes());
    if (!g) return out;
    for (uint32_t y = 0; y < s.height; y++)
        for (uint32_t x = 0; x < s.width; x++) {
            Rgba c = fb_decode(s.format, g + tiled_index(x, y, s.width) * s.bpp());
            uint8_t *o = &out[(size_t(y) * s.width + x) * 4];
            o[0] = c.r, o[1] = c.g, o[2] = c.b, o[3] = c.a;
        }
    return out;
}

void prepare_surface(Surface *s) {
    if (!s->reload) return;
    s->reload = false;
    if (s->depth) {
        depth_clears.push_back(s);
        return;
    }
    surface_loads.push_back({s, read_guest(*s)});
    s->guest_hash = guest_hash(*s);
}

// Copies a surface's image back into guest memory (downsampled to the game's size), so CPU-side
// transfers and copies out of it see what the GPU drew: e.g. 3D models the game renders
// off-screen and then copies into a texture.
void download_surface(Surface *s) {
    readbacks++;
    int64_t t0 = now_ns();
    flush();
    uint32_t w = s->width, h = s->height;
    SDL_GPUTexture *tmp = make_texture(w, h, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                                       SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET);
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
    SDL_GPUBlitInfo blit{};
    blit.source.texture = s->tex;
    blit.source.y = uint32_t(s->margin * s->scale);
    blit.source.w = s->tw, blit.source.h = h * uint32_t(s->scale);
    blit.destination.texture = tmp;
    blit.destination.w = w, blit.destination.h = h;
    blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
    blit.filter = SDL_GPU_FILTER_LINEAR;
    SDL_BlitGPUTexture(cmd, &blit);
    SDL_GPUTransferBufferCreateInfo tbi{SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, w * h * 4, 0};
    SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(dev, &tbi);
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion src{};
    src.texture = tmp, src.w = w, src.h = h, src.d = 1;
    SDL_GPUTextureTransferInfo dst{tb, 0, w, h};
    SDL_DownloadFromGPUTexture(copy, &src, &dst);
    SDL_EndGPUCopyPass(copy);
    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    SDL_WaitForGPUFences(dev, true, &fence, 1);
    SDL_ReleaseGPUFence(dev, fence);
    const auto *rgba = static_cast<const uint8_t *>(SDL_MapGPUTransferBuffer(dev, tb, false));
    if (uint8_t *guest = phys(s->addr, s->bytes())) {
        uint32_t bpp = s->bpp();
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++) {
                const uint8_t *p = rgba + (size_t(y) * w + x) * 4;
                fb_encode(s->format, {p[0], p[1], p[2], p[3]}, guest + tiled_index(x, y, w) * bpp);
            }
    }
    SDL_UnmapGPUTransferBuffer(dev, tb);
    SDL_ReleaseGPUTransferBuffer(dev, tb);
    SDL_ReleaseGPUTexture(dev, tmp);
    s->drawn = false;
    s->guest_hash = guest_hash(*s);
    costs.downloads++, costs.download_ns += now_ns() - t0;
}

SDL_GPUTexture *texture_for(const Texture &t) {
    // render-to-texture: sample the GPU copy, unless the memory has since been given new
    // contents (then it is an ordinary texture, decoded below)
    Surface *s = find_color_surface(t.addr);
    if (s && s->width == t.width && s->height == t.height && !s->reload && !overwritten(s)) {
        s->last_used = use_clock;
        return s->tex;
    }
    before_cpu_read(t.addr, t.bytes); // a texture inside a GPU-drawn buffer (not at its start)
    uint64_t h = hash_bytes(t.data, t.bytes);
    auto key = std::make_tuple(t.addr, t.width, t.height, t.format);
    auto it = textures.find(key);
    if (it != textures.end() && it->second.hash == h) {
        it->second.last_used = use_clock;
        return it->second.tex;
    }
    int64_t t0 = now_ns();
    if (it == textures.end())
        it = textures.emplace(key, CachedTexture{make_texture(t.width, t.height, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                                                              SDL_GPU_TEXTUREUSAGE_SAMPLER),
                                                 h, use_clock})
                 .first;
    it->second.last_used = use_clock;
    it->second.hash = h;
    TextureUpload u{it->second.tex, std::vector<uint8_t>(size_t(t.width) * t.height * 4), t.width, t.height};
    decode_texture(t, u.rgba.data());
    texture_uploads.push_back(std::move(u));
    costs.textures++, costs.texture_ns += now_ns() - t0;
    return it->second.tex;
}

void update_luts(bool lighting, bool fog) {
    bool lights_changed = lighting && lut_version_done != lighting_lut_version;
    bool fog_changed = fog && std::memcmp(fog_done, fog_lut, sizeof fog_done) != 0;
    if (!lights_changed && !fog_changed) return;
    if (!batch.empty()) flush(); // earlier draws keep the old tables
    for (int t = 0; t < 24; t++)
        for (int i = 0; i < 256; i++) {
            uint32_t e = lighting_lut[t][i];
            float diff = float((e >> 12) & 0x7FF) / 2047.0f;
            lut_pixels[(t * 256 + i) * 2] = float(e & 0xFFF) / 4095.0f;
            lut_pixels[(t * 256 + i) * 2 + 1] = (e >> 23) & 1 ? -diff : diff;
        }
    for (int i = 0; i < 128; i++) {
        uint32_t e = fog_lut[i];
        lut_pixels[(24 * 256 + i) * 2] = float((e >> 13) & 0x7FF) / 2047.0f;
        lut_pixels[(24 * 256 + i) * 2 + 1] = float(int32_t(e << 19) >> 19) / 2047.0f;
    }
    lut_version_done = lighting_lut_version;
    std::memcpy(fog_done, fog_lut, sizeof fog_done);
    lut_upload = true;
}

float f16(uint32_t v) {
    if ((v & 0x7FFF) == 0) return 0.0f;
    uint32_t bits = ((v & 0x8000) << 16) | ((((v >> 10) & 0x1F) + 127 - 15) << 23) | ((v & 0x3FF) << 13);
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}
float f20(uint32_t v) {
    if ((v & 0x7FFFF) == 0) return 0.0f;
    uint32_t bits = ((v & 0x80000) << 12) | ((((v >> 12) & 0x7F) + 127 - 63) << 23) | ((v & 0xFFF) << 11);
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

void fill_lighting(FragUniforms &u) {
    uint32_t c0 = regs[LIGHTING_CONFIG0], c1 = regs[LIGHTING_CONFIG1];
    uint32_t config = (c0 >> 4) & 0xF;
    int count = int(regs[LIGHTING_NUM_LIGHTS] & 7) + 1;
    u.light_misc[0] = count, u.light_misc[1] = int32_t(c0), u.light_misc[2] = int32_t(c1), u.light_misc[3] = int32_t(config);
    auto color = [](uint32_t c, float *o) {
        o[0] = float((c >> 20) & 0x3FF) / 255.0f, o[1] = float((c >> 10) & 0x3FF) / 255.0f;
        o[2] = float(c & 0x3FF) / 255.0f, o[3] = 1.0f;
    };
    color(regs[LIGHTING_AMBIENT], u.ambient);
    auto supported = [config](int sampler) {
        switch (sampler) {
        case 0: return config != 1;
        case 1: return config != 0 && config != 1 && config != 5;
        case 8: return config != 2 && config != 3;
        case 3: return config != 0 && config != 2 && config != 4;
        case 6: return config != 3;
        default: return config == 4 || config == 5 || config == 8;
        }
    };
    static constexpr float SCALES[8] = {1, 2, 4, 8, 0, 0, 0.25f, 0.5f};
    // d0, d1, sp, fr, rr, rg, rb: register shift, config1 disable bit, sampler id
    static constexpr int SHIFT[7] = {0, 4, 8, 12, 24, 20, 16}, DISABLE[7] = {16, 17, -1, 19, 20, 21, 22},
                         SAMPLER[7] = {0, 1, 8, 3, 6, 5, 4};
    uint32_t abs = regs[LIGHTING_LUT_ABS], input = regs[LIGHTING_LUT_INPUT], scale = regs[LIGHTING_LUT_SCALE];
    for (int i = 0; i < 7; i++) {
        bool on = (DISABLE[i] < 0 || !((c1 >> DISABLE[i]) & 1)) && supported(SAMPLER[i]);
        u.lut_use[i][0] = on, u.lut_use[i][1] = !((abs >> (SHIFT[i] + 1)) & 1);
        u.lut_use[i][2] = int32_t((input >> SHIFT[i]) & 7);
        u.lut_scale[i][0] = SCALES[(scale >> SHIFT[i]) & 7];
    }
    uint32_t perm = regs[LIGHTING_PERMUTATION];
    for (int i = 0; i < count; i++) {
        int num = int((perm >> (4 * i)) & 7);
        const uint32_t *r = &regs[LIGHT0 + 0x10 * num];
        float(*l)[4] = &u.light[i * 7];
        color(r[0], l[0]), color(r[1], l[1]), color(r[2], l[2]), color(r[3], l[3]);
        l[4][0] = f16(r[4] & 0xFFFF), l[4][1] = f16(r[4] >> 16), l[4][2] = f16(r[5] & 0xFFFF);
        l[4][3] = float(r[9] & 1);
        auto s13 = [](uint32_t v) { return float(int32_t(v << 19) >> 19) / 2047.0f; };
        l[5][0] = s13(r[6] & 0x1FFF), l[5][1] = s13((r[6] >> 16) & 0x1FFF), l[5][2] = s13(r[7] & 0x1FFF);
        l[5][3] = float((r[9] >> 1) & 1);
        l[6][0] = f20(r[10] & 0xFFFFF), l[6][1] = f20(r[11] & 0xFFFFF), l[6][2] = float(num);
        l[6][3] = float((r[9] >> 2) & 3);
    }
}

void begin_render_pass(SDL_GPUCommandBuffer *cmd, SDL_GPURenderPass *&pass, Surface *&bound_color,
                       Surface *&bound_depth, Surface *color, Surface *depth) {
    if (pass && bound_color == color && bound_depth == depth) return;
    if (pass) SDL_EndGPURenderPass(pass);
    SDL_GPUColorTargetInfo ct{};
    ct.texture = color->tex;
    ct.load_op = SDL_GPU_LOADOP_LOAD, ct.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPUDepthStencilTargetInfo dt{};
    if (depth) {
        dt.texture = depth->tex;
        dt.load_op = dt.stencil_load_op = SDL_GPU_LOADOP_LOAD;
        dt.store_op = dt.stencil_store_op = SDL_GPU_STOREOP_STORE;
    }
    pass = SDL_BeginGPURenderPass(cmd, &ct, 1, depth ? &dt : nullptr);
    bound_color = color, bound_depth = depth;
}

// Frees textures the game hasn't used for `idle` frames, and render targets whose content is in
// guest memory (not drawn since). Streaming new areas keeps putting textures at new addresses,
// so without this GPU memory fills up and allocations start failing. Only called between
// batches, when nothing queued refers to them.
void trim_caches(uint64_t idle) {
    for (auto it = textures.begin(); it != textures.end();) {
        if (use_clock - it->second.last_used > idle) {
            SDL_ReleaseGPUTexture(dev, it->second.tex);
            it = textures.erase(it);
        } else {
            ++it;
        }
    }
    std::erase_if(surfaces, [&](const std::unique_ptr<Surface> &s) {
        if (use_clock - s->last_used <= idle || s->drawn) return false;
        SDL_ReleaseGPUTexture(dev, s->tex);
        return true;
    });
}

// GPU memory ran out: frees everything not used this frame, and says so once
void out_of_gpu_memory() {
    static bool warned = false;
    if (!warned) LOG("gpu: out of GPU memory; freeing cached textures");
    warned = true;
    trim_caches(0);
}

// copies pending texture, LUT, surface and vertex data to the GPU (one copy pass)
void upload_pending(SDL_GPUCommandBuffer *cmd, SDL_GPUBuffer **vbuf) {
    size_t total = 0;
    for (auto &u : texture_uploads) total += u.rgba.size();
    for (auto &l : surface_loads) total += l.rgba.size();
    if (lut_upload) total += lut_pixels.size() * sizeof(float);
    size_t vbytes = vertices.size() * sizeof(GpuVertex);
    total += vbytes;
    if (!total) return;
    SDL_GPUTransferBufferCreateInfo tbi{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, uint32_t(total), 0};
    SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(dev, &tbi);
    if (!tb) out_of_gpu_memory(), tb = SDL_CreateGPUTransferBuffer(dev, &tbi);
    if (!tb) { // drop this batch's uploads; textures are uploaded again when next used
        for (auto &u : texture_uploads)
            for (auto &[key, c] : textures)
                if (c.tex == u.tex) c.hash = 0;
        for (auto &l : surface_loads) l.surface->reload = true;
        texture_uploads.clear(), surface_loads.clear();
        return;
    }
    auto *map = static_cast<uint8_t *>(SDL_MapGPUTransferBuffer(dev, tb, false));
    size_t at = 0;
    struct Item {
        size_t offset;
        SDL_GPUTexture *tex;
        uint32_t w, h;
    };
    std::vector<Item> items;
    std::vector<SDL_GPUTexture *> loaded; // native-size staging textures for surface loads
    for (auto &u : texture_uploads) {
        std::memcpy(map + at, u.rgba.data(), u.rgba.size());
        items.push_back({at, u.tex, u.w, u.h});
        at += u.rgba.size();
    }
    for (auto &l : surface_loads) {
        std::memcpy(map + at, l.rgba.data(), l.rgba.size());
        SDL_GPUTexture *staging = make_texture(l.surface->width, l.surface->height, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                                               SDL_GPU_TEXTUREUSAGE_SAMPLER);
        loaded.push_back(staging);
        items.push_back({at, staging, l.surface->width, l.surface->height});
        at += l.rgba.size();
    }
    size_t lut_at = at;
    if (lut_upload) {
        std::memcpy(map + at, lut_pixels.data(), lut_pixels.size() * sizeof(float));
        at += lut_pixels.size() * sizeof(float);
    }
    size_t vert_at = at;
    if (vbytes) std::memcpy(map + at, vertices.data(), vbytes);
    SDL_UnmapGPUTransferBuffer(dev, tb);

    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
    for (auto &it : items) {
        SDL_GPUTextureTransferInfo src{tb, uint32_t(it.offset), it.w, it.h};
        SDL_GPUTextureRegion dst{};
        dst.texture = it.tex, dst.w = it.w, dst.h = it.h, dst.d = 1;
        SDL_UploadToGPUTexture(copy, &src, &dst, false);
    }
    if (lut_upload) {
        SDL_GPUTextureTransferInfo src{tb, uint32_t(lut_at), 256, 25};
        SDL_GPUTextureRegion dst{};
        dst.texture = lut_tex, dst.w = 256, dst.h = 25, dst.d = 1;
        SDL_UploadToGPUTexture(copy, &src, &dst, false);
    }
    if (vbytes) {
        SDL_GPUBufferCreateInfo bi{SDL_GPU_BUFFERUSAGE_VERTEX, uint32_t(vbytes), 0};
        *vbuf = SDL_CreateGPUBuffer(dev, &bi);
        if (!*vbuf) out_of_gpu_memory(), *vbuf = SDL_CreateGPUBuffer(dev, &bi);
        if (*vbuf) { // otherwise this batch isn't drawn
            SDL_GPUTransferBufferLocation src{tb, uint32_t(vert_at)};
            SDL_GPUBufferRegion dst{*vbuf, 0, uint32_t(vbytes)};
            SDL_UploadToGPUBuffer(copy, &src, &dst, false);
        }
    }
    SDL_EndGPUCopyPass(copy);
    SDL_ReleaseGPUTransferBuffer(dev, tb);

    // surface loads: scale the guest image into the (possibly larger, wider) surface
    for (size_t i = 0; i < surface_loads.size(); i++) {
        Surface *s = surface_loads[i].surface;
        SDL_GPUBlitInfo blit{};
        blit.source.texture = loaded[i];
        blit.source.w = s->width, blit.source.h = s->height;
        blit.destination.texture = s->tex;
        blit.destination.x = 0, blit.destination.y = uint32_t(s->margin * s->scale);
        blit.destination.w = s->tw, blit.destination.h = s->height * uint32_t(s->scale);
        blit.load_op = SDL_GPU_LOADOP_LOAD;
        blit.filter = SDL_GPU_FILTER_LINEAR;
        SDL_BlitGPUTexture(cmd, &blit);
        SDL_ReleaseGPUTexture(dev, loaded[i]);
    }
    texture_uploads.clear();
    surface_loads.clear();
    lut_upload = false;
}

void clear_fresh(SDL_GPUCommandBuffer *cmd) {
    for (auto &sp : surfaces) {
        Surface *s = sp.get();
        if (!s->fresh) continue;
        s->fresh = false;
        if (s->depth) continue; // cleared below
        SDL_GPUColorTargetInfo ct{};
        ct.texture = s->tex;
        ct.clear_color = {0, 0, 0, 0};
        ct.load_op = SDL_GPU_LOADOP_CLEAR, ct.store_op = SDL_GPU_STOREOP_STORE;
        SDL_EndGPURenderPass(SDL_BeginGPURenderPass(cmd, &ct, 1, nullptr));
    }
}

void clear_new_depth(SDL_GPUCommandBuffer *cmd) {
    for (Surface *s : depth_clears) {
        SDL_GPUDepthStencilTargetInfo dt{};
        dt.texture = s->tex;
        dt.clear_depth = 1.0f, dt.clear_stencil = 0;
        dt.load_op = dt.stencil_load_op = SDL_GPU_LOADOP_CLEAR;
        dt.store_op = dt.stencil_store_op = SDL_GPU_STOREOP_STORE;
        SDL_EndGPURenderPass(SDL_BeginGPURenderPass(cmd, nullptr, 0, &dt));
    }
    depth_clears.clear();
}

SDL_GPUShader *make_shader(const unsigned char *code, size_t size, SDL_GPUShaderStage stage, uint32_t samplers_n,
                           uint32_t uniforms) {
    SDL_GPUShaderCreateInfo info{};
    info.code = code, info.code_size = size;
    info.entrypoint = "main";
    info.format = SDL_GPU_SHADERFORMAT_SPIRV;
    info.stage = stage;
    info.num_samplers = samplers_n;
    info.num_uniform_buffers = uniforms;
    SDL_GPUShader *s = SDL_CreateGPUShader(dev, &info);
    if (!s) LOG("gpu: shader: %s", SDL_GetError());
    return s;
}

// ---- LCD frames ----

struct LcdFrames {
    SDL_GPUTexture *tex[3] = {};
    int w = 0, h = 0, next = 0, latest = -1;
};
std::map<uint32_t, LcdFrames> lcd; // by guest virtual address of the LCD framebuffer
SDL_Mutex *lcd_lock;

} // namespace

bool init(SDL_GPUDevice *device) {
    dev = device;
    if (!(SDL_GetGPUShaderFormats(dev) & SDL_GPU_SHADERFORMAT_SPIRV)) {
        LOG("gpu: this GPU backend doesn't take SPIR-V; using the software renderer");
        return false;
    }
    vs_pica = make_shader(SPIRV_pica_vert, sizeof SPIRV_pica_vert, SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
    fs_pica = make_shader(SPIRV_pica_frag, sizeof SPIRV_pica_frag, SDL_GPU_SHADERSTAGE_FRAGMENT, 4, 1);
    vs_full = make_shader(SPIRV_fullscreen_vert, sizeof SPIRV_fullscreen_vert, SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
    fs_blit = make_shader(SPIRV_blit_lcd_frag, sizeof SPIRV_blit_lcd_frag, SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
    if (!vs_pica || !fs_pica || !vs_full || !fs_blit) return false;

    depth_format = SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT;
    if (!SDL_GPUTextureSupportsFormat(dev, depth_format, SDL_GPU_TEXTURETYPE_2D,
                                      SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET))
        depth_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT;

    lut_tex = make_texture(256, 25, SDL_GPU_TEXTUREFORMAT_R32G32_FLOAT, SDL_GPU_TEXTUREUSAGE_SAMPLER);
    dummy_tex = make_texture(1, 1, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTUREUSAGE_SAMPLER);
    SDL_GPUSamplerCreateInfo si{};
    si.address_mode_u = si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    nearest_clamp = SDL_CreateGPUSampler(dev, &si);
    si.min_filter = si.mag_filter = SDL_GPU_FILTER_LINEAR;
    linear_clamp = SDL_CreateGPUSampler(dev, &si);

    SDL_GPUColorTargetDescription color{};
    color.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    SDL_GPUGraphicsPipelineCreateInfo pi{};
    pi.vertex_shader = vs_full, pi.fragment_shader = fs_blit;
    pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pi.target_info.color_target_descriptions = &color;
    pi.target_info.num_color_targets = 1;
    blit_pipeline = SDL_CreateGPUGraphicsPipeline(dev, &pi);
    if (!blit_pipeline) {
        LOG("gpu: %s", SDL_GetError());
        return false;
    }
    lcd_lock = SDL_CreateMutex();
    std::memset(fog_done, 0xFF, sizeof fog_done);
    ok = true;
    LOG("gpu: renderer active (%s)", SDL_GetGPUDeviceDriver(dev));
    return true;
}

bool active() { return ok; }
int take_readback_count() { return readbacks.exchange(0); }

void begin_draw() {
    cur_skip = true;
    uint32_t dim = regs[FB_DIM];
    uint32_t w = dim & 0x7FF, h = ((dim >> 12) & 0x3FF) + 1;
    if (!w || h < 2) return;
    uint32_t color_addr = (regs[COLOR_ADDR] & 0x0FFFFFFF) << 3, depth_addr = (regs[DEPTH_ADDR] & 0x0FFFFFFF) << 3;
    uint32_t cfmt = (regs[COLOR_FORMAT] >> 16) & 7, dfmt = regs[DEPTH_FORMAT] & 3;
    if (!color_addr || !phys(color_addr, w * h * fb_bpp(cfmt))) return;
    bool depth_used = (regs[DEPTH_STENCIL_READ_ALLOW] || regs[DEPTH_STENCIL_WRITE_ALLOW]) && depth_addr;

    DrawRec &d = cur;
    d = DrawRec{};
    d.color = get_surface(color_addr, w, h, cfmt, false);
    d.depth = depth_used ? get_surface(depth_addr, w, h, dfmt, true) : nullptr;
    prepare_surface(d.color);
    if (d.depth) prepare_surface(d.depth);
    int s = d.color->scale, m = d.color->margin;
    cur_ky = m ? float(h) / float(h + 2 * uint32_t(m)) : 1.0f;
    cur_cull = regs[CULL_MODE] & 3;
    cur_tex0 = regs[TEX0_ADDR];

    FragUniforms &u = d.fu;
    std::memset(&u, 0, sizeof u);
    // textures
    bool lighting = !(regs[LIGHTING_OFF] & 1);
    uint32_t fogreg = regs[TEV_BUFFER_CONFIG];
    bool fog = (fogreg & 7) == 5;
    static constexpr uint32_t TEV_STAGE[6] = {0x0C0, 0x0C8, 0x0D0, 0x0D8, 0x0F0, 0x0F8};
    int stages = 6;
    auto passthrough = [](const uint32_t *r) {
        return (r[2] & 0xF) == 0 && ((r[2] >> 16) & 0xF) == 0 && (r[0] & 0xF) == 0xF && ((r[0] >> 16) & 0xF) == 0xF &&
               (r[1] & 0xF) == 0 && ((r[1] >> 12) & 7) == 0 && (r[4] & 3) == 0 && ((r[4] >> 16) & 3) == 0;
    };
    while (stages > 0 && passthrough(&regs[TEV_STAGE[stages - 1]])) stages--;
    bool used[16] = {};
    for (int i = 0; i < 6; i++) {
        const uint32_t *r = &regs[TEV_STAGE[i]];
        uint32_t csrc = r[0] & 0xFFF, asrc = (r[0] >> 16) & 0xFFF;
        uint32_t cop = r[1] & 0xFFF, aop = 0;
        for (int k = 0; k < 3; k++) aop |= ((r[1] >> (12 + 4 * k)) & 7) << (4 * k);
        u.tev_src[i][0] = int32_t(csrc), u.tev_src[i][1] = int32_t(asrc);
        u.tev_src[i][2] = int32_t(cop), u.tev_src[i][3] = int32_t(aop);
        uint32_t cs = r[4] & 3, as = (r[4] >> 16) & 3;
        u.tev_op[i][0] = int32_t(r[2] & 0xF), u.tev_op[i][1] = int32_t((r[2] >> 16) & 0xF);
        u.tev_op[i][2] = int32_t(cs == 3 ? 2 : cs), u.tev_op[i][3] = int32_t(as == 3 ? 2 : as);
        for (int c = 0; c < 4; c++) u.tev_const[i][c] = float((r[3] >> (8 * c)) & 0xFF) / 255.0f;
        if (i < stages)
            for (int k = 0; k < 3; k++) used[(csrc >> (4 * k)) & 0xF] = used[(asrc >> (4 * k)) & 0xF] = true;
    }
    uint32_t b = regs[TEV_BUFFER_COLOR];
    for (int c = 0; c < 4; c++) u.buffer_init[c] = float((b >> (8 * c)) & 0xFF) / 255.0f;
    u.misc[0] = stages;
    u.misc[1] = int32_t(((fogreg >> 8) & 0xF) | (((fogreg >> 12) & 0xF) << 4));
    uint32_t at = regs[ALPHA_TEST];
    u.misc[2] = int32_t((at & 1) << 3 | ((at >> 4) & 7)), u.misc[3] = int32_t((at >> 8) & 0xFF);
    bool tex2_uses_tex1 = (regs[TEX_CONFIG] >> 13) & 1;
    uint32_t c0 = regs[LIGHTING_CONFIG0];
    bool lighting_reads_tex = lighting && ((c0 & 1) || ((c0 >> 28) & 3));
    int sampled = 0;
    for (int unit = 0; unit < 3; unit++) {
        d.tex[unit] = dummy_tex, d.samp[unit] = nearest_clamp;
        Texture t = load_texture(unit);
        if (!t.data || !(used[3 + unit] || lighting_reads_tex)) continue;
        d.tex[unit] = texture_for(t);
        d.samp[unit] = sampler(t.mag_linear, t.wrap_s, t.wrap_t);
        u.tex_wrap[unit][0] = (t.wrap_s & 3) == 1, u.tex_wrap[unit][1] = (t.wrap_t & 3) == 1;
        u.tex_border[unit][0] = t.border.r / 255.0f, u.tex_border[unit][1] = t.border.g / 255.0f;
        u.tex_border[unit][2] = t.border.b / 255.0f, u.tex_border[unit][3] = t.border.a / 255.0f;
        sampled |= 1 << unit;
    }
    bool wbuffer = !(regs[DEPTHMAP_ENABLE] & 1);
    u.flags[0] = int32_t(lighting) | int32_t(fog) << 1 | int32_t((fogreg >> 16) & 1) << 2 | int32_t(tex2_uses_tex1) << 3 |
                 int32_t(((regs[TEX0_PARAM] >> 28) & 7) == 3) << 4 | int32_t(wbuffer) << 5;
    u.flags[1] = sampled;
    uint32_t fc = regs[FOG_COLOR];
    for (int c = 0; c < 3; c++) u.fog_color[c] = float((fc >> (8 * c)) & 0xFF) / 255.0f;
    if (lighting) fill_lighting(u);
    update_luts(lighting, fog);
    d.depth_map[0] = f24(regs[DEPTH_SCALE]), d.depth_map[1] = f24(regs[DEPTH_OFFSET]);

    // fixed-function state
    PipelineKey k{};
    k.blend = (regs[COLOR_OP] >> 8) & 1;
    k.blend_cfg = k.blend ? regs[BLEND_FUNC] : 0;
    uint32_t dm = regs[DEPTH_COLOR_MASK];
    k.color_mask = regs[COLOR_WRITE_ALLOW] ? uint8_t((dm >> 8) & 0xF) : 0;
    k.has_depth = d.depth != nullptr;
    bool can_write = regs[DEPTH_STENCIL_WRITE_ALLOW] != 0;
    k.depth_test = dm & 1, k.depth_func = uint8_t((dm >> 4) & 7), k.depth_write = ((dm >> 12) & 1) && can_write;
    uint32_t st = regs[STENCIL_TEST], so = regs[STENCIL_OP];
    k.stencil = dfmt == 3 && (st & 1);
    k.stencil_func = uint8_t((st >> 4) & 7);
    k.stencil_fail = uint8_t(so & 7), k.stencil_zfail = uint8_t((so >> 4) & 7), k.stencil_zpass = uint8_t((so >> 8) & 7);
    k.stencil_read = uint8_t(st >> 24), k.stencil_write = can_write ? uint8_t(st >> 8) : 0;
    d.stencil_ref = uint8_t(st >> 16);
    d.pipeline = pipeline(k);
    uint32_t bc = regs[BLEND_COLOR];
    d.blend_const = {float(bc & 0xFF) / 255.0f, float((bc >> 8) & 0xFF) / 255.0f, float((bc >> 16) & 0xFF) / 255.0f,
                     float((bc >> 24) & 0xFF) / 255.0f};

    // viewport and scissor, in the surface's pixels (top-left origin; rows in memory order)
    auto sext10 = [](uint32_t v) { return int32_t(v << 22) >> 22; };
    float hw = f24(regs[VIEWPORT_WIDTH]), hh = f24(regs[VIEWPORT_HEIGHT]);
    float vx = float(sext10(regs[VIEWPORT_XY] & 0x3FF)), vy = float(sext10((regs[VIEWPORT_XY] >> 16) & 0x3FF));
    float H = float(h);
    d.viewport = {vx * float(s), (H - (vy + 2 * hh)) * float(s), 2 * hw * float(s), (2 * hh + 2 * float(m)) * float(s),
                  0.0f, 1.0f};
    int sx0 = 0, sy0 = 0, sx1 = int(w) - 1, sy1 = int(h) - 1 + 2 * m;
    if (regs[SCISSOR_MODE] & 3) { // include mode (exclude mode is rare and not done here)
        int x0 = int(regs[SCISSOR_MIN] & 0x3FF), y0 = int((regs[SCISSOR_MIN] >> 16) & 0x3FF);
        int x1 = int(regs[SCISSOR_MAX] & 0x3FF), y1 = int((regs[SCISSOR_MAX] >> 16) & 0x3FF);
        if ((regs[SCISSOR_MODE] & 3) == 3) {
            bool full = y0 <= 0 && y1 >= int(h) - 1;
            sx0 = x0, sx1 = x1;
            if (!full) sy0 = y0 + m, sy1 = y1 + m;
        }
    }
    int total_rows = int(h) + 2 * m;
    int top = total_rows - 1 - sy1; // raster rows run bottom to top; texture rows top to bottom
    d.scissor = {sx0 * s, top * s, std::max(0, sx1 - sx0 + 1) * s, std::max(0, sy1 - sy0 + 1) * s};
    d.first = uint32_t(vertices.size());
    cur_skip = false;
}

void triangle(const Vertex &v0, const Vertex &v1, const Vertex &v2) {
    if (cur_skip) return;
    const Vertex *v[3] = {&v0, &v1, &v2};
    float ky[3] = {1, 1, 1}, shift = 0;
    if (cur_ky != 1.0f) { // widescreen: see widescreen.cpp
        bool perspective = false;
        float lo = 1e9f, hi = -1e9f, xlo = 1e9f, xhi = -1e9f;
        for (auto *x : v) {
            float w = x->a[POS + 3];
            perspective |= std::fabs(w - 1.0f) > 1e-4f;
            lo = std::min(lo, x->a[POS + 1] / w), hi = std::max(hi, x->a[POS + 1] / w);
            xlo = std::min(xlo, x->a[POS] / w), xhi = std::max(xhi, x->a[POS] / w);
        }
        // only full-screen layers (backgrounds, fades, tints) stretch; HUD bars keep their size
        bool full_screen =
            draw_fills_screen || (!perspective && lo <= -0.99f && hi >= 0.99f && xlo <= -0.99f && xhi >= 0.99f);
        if (!full_screen) ky[0] = ky[1] = ky[2] = cur_ky;
        // 2D on the sides keeps its distance from the (new) screen edge
        if (!full_screen && !perspective) shift = float(draw_anchor) * (1.0f - cur_ky);
    }
    // cull (mode 1 keeps clockwise, 2 counter-clockwise), when the triangle is in front
    if (cur_cull == 1 || cur_cull == 2) {
        float w0 = v0.a[POS + 3], w1 = v1.a[POS + 3], w2 = v2.a[POS + 3];
        if (w0 > 0 && w1 > 0 && w2 > 0) {
            float x0 = v0.a[POS] / w0, y0 = v0.a[POS + 1] / w0, x1 = v1.a[POS] / w1, y1 = v1.a[POS + 1] / w1;
            float x2 = v2.a[POS] / w2, y2 = v2.a[POS + 1] / w2;
            float area = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0);
            if ((cur_cull == 1 && area >= 0) || (cur_cull == 2 && area <= 0)) return;
        }
    }
    for (int i = 0; i < 3; i++) {
        const float *a = v[i]->a;
        GpuVertex g;
        g.pos[0] = a[POS], g.pos[1] = a[POS + 1] * ky[i] + shift * a[POS + 3], g.pos[2] = a[POS + 2], g.pos[3] = a[POS + 3];
        for (int c = 0; c < 4; c++) g.color[c] = a[COLOR + c], g.quat[c] = a[QUAT + c];
        g.tc01[0] = a[TEX0], g.tc01[1] = a[TEX0 + 1], g.tc01[2] = a[TEX1], g.tc01[3] = a[TEX1 + 1];
        g.tc2w[0] = a[TEX2], g.tc2w[1] = a[TEX2 + 1], g.tc2w[2] = a[TEX0_W], g.tc2w[3] = 0;
        g.view[0] = a[VIEW], g.view[1] = a[VIEW + 1], g.view[2] = a[VIEW + 2], g.view[3] = 0;
        vertices.push_back(g);
    }
}

// HUD size: shrinks the top screen's HUD, meaning 2D at the screen's edges (dialogue boxes, the
// "Objective" bar, area names, icons) and anything drawn over it in the same frame (a portrait's
// face over its body, a name tag over its box). 2D floating over the scene keeps its size and
// place: it points at something in the 3D world (the bug net's and the Yo-kai Lens's aim).
// HUD in the lower part shrinks toward the bottom center of the screen: one point for all of it
// keeps its pieces together. HUD entirely in the upper half (an area name, status icons) shrinks
// toward its own corner or the top center, so it stays where the game put it. 3D and large 2D
// (backdrops) keep their size.
struct HudBox {
    float xlo, xhi, ylo, yhi;
    float ax, ay; // the point it shrinks toward
};
std::vector<HudBox> hud_boxes; // this frame's HUD, unscaled; cleared when the frame goes to the LCD
// elements (texture and vertex count) seen away from the edges and HUD: they float over the scene
// and are never HUD, even when moved against an edge (the Yo-kai Lens while scanning). Deciding
// per frame made them jump between full and HUD size. Counted in frames in a row: an element only
// floats after FLOAT_FRAMES of it, and any frame as HUD starts it over, so a dialogue box (and the
// text on it, whose key is just the font and its length) floating while it opens never does.
std::unordered_map<uint64_t, int> floating;
constexpr int FLOAT_FRAMES = 30;
// this frame's draws that look like they float, until the frame (or batch) is done: HUD drawn
// over them later claims them instead (a portrait's face comes before the body that reaches the
// edge, and shares its texture and vertex count)
struct Candidate {
    uint32_t first, count;
    HudBox box;
    uint64_t key;
};
std::vector<Candidate> candidates;

// Widescreen, per group: this frame's top-screen 2D draws still in the batch. Each was shifted to a
// side when drawn (see side_of), one at a time, so pieces of one thing could disagree (a speech
// bubble's background left centered while its portrait and text moved to the edge). Once the frame
// (or batch) is drawn, pieces touching each other are placed together: centered if any reaches past
// the middle (a dialogue box: its portrait stays with it), else at the edge one of them touches.
struct PlacedDraw {
    uint32_t first, count;
    DrawGeom g;
    float ky;     // the target's widescreen factor (cur_ky) when drawn
    float k = 1;  // HUD scale applied to it since (a shift before scaling moves it k times as far)
};
std::vector<PlacedDraw> placed;

void anchor_groups() {
    size_t n = placed.size();
    if (n < 2) return;
    std::vector<size_t> parent(n);
    for (size_t i = 0; i < n; i++) parent[i] = i;
    auto root = [&](size_t i) {
        while (parent[i] != i) i = parent[i] = parent[parent[i]];
        return i;
    };
    // Pieces belong together when one lies mostly on the other (a bubble's text and portrait on its
    // background), or when they touch and keep to the same side (the rows of a panel). Merely
    // touching isn't enough: a chat bubble flying past the area name would be pulled to its side
    constexpr float EPS = 0.01f;
    auto area = [](const DrawGeom &g) { return std::max(g.xhi - g.xlo, 0.0f) * std::max(g.yhi - g.ylo, 0.0f); };
    for (size_t i = 0; i < n; i++)
        for (size_t j = i + 1; j < n; j++) {
            const DrawGeom &a = placed[i].g, &b = placed[j].g;
            if (!(a.xlo <= b.xhi + EPS && b.xlo <= a.xhi + EPS && a.ylo <= b.yhi + EPS && b.ylo <= a.yhi + EPS)) continue;
            float common = std::max(std::min(a.xhi, b.xhi) - std::max(a.xlo, b.xlo), 0.0f) *
                           std::max(std::min(a.yhi, b.yhi) - std::max(a.ylo, b.ylo), 0.0f);
            if (common >= 0.5f * std::min(area(a), area(b)) || (a.own && a.own == b.own)) parent[root(i)] = root(j);
        }
    std::vector<int> side(n, 2); // per root: 2 undecided, 3 conflicting, else -1/0/1
    std::vector<bool> pinned(n, false);
    for (size_t i = 0; i < n; i++) {
        size_t r = root(i);
        if (placed[i].g.pins) pinned[r] = true;
        int own = placed[i].g.own;
        if (own) side[r] = side[r] == 2 || side[r] == own ? own : 3;
    }
    for (size_t i = 0; i < n; i++) {
        size_t r = root(i);
        int want = pinned[r] || side[r] == 3 ? 0 : side[r] == 2 ? placed[i].g.applied : side[r];
        PlacedDraw &d = placed[i];
        char note[200];
        std::snprintf(note, sizeof note, "  group %zu: x=[%.2f,%.2f] y=[%.2f,%.2f] own=%d pins=%d side %d -> %d k=%.2f\n", r, d.g.xlo,
                      d.g.xhi, d.g.ylo, d.g.yhi, d.g.own, d.g.pins, d.g.applied, want, d.k);
        pica::draw_log_note(note);
        if (want == d.g.applied) continue;
        float delta = float(want - d.g.applied) * (1.0f - d.ky) * d.k;
        for (uint32_t v = d.first; v < d.first + d.count; v++) vertices[v].pos[1] += delta * vertices[v].pos[3];
        d.g.applied = want;
    }
}

void shrink(uint32_t first, uint32_t count, float ax, float ay, float k) {
    for (PlacedDraw &d : placed)
        if (d.first == first) d.k *= k;
    for (uint32_t i = first; i < first + count; i++) {
        float *p = vertices[i].pos;
        p[0] = ax + (p[0] - ax) * k;
        p[1] = ay + (p[1] - ay) * k;
    }
}

// the candidates nothing claimed float
void settle_candidates() {
    if (floating.size() > 4096) floating.clear();
    for (const Candidate &c : candidates) {
        int &n = floating[c.key];
        n = std::min(n + 1, FLOAT_FRAMES);
    }
    candidates.clear();
}

void scale_hud(uint32_t first, uint32_t count, float k) {
    float xlo = 1e9f, xhi = -1e9f, ylo = 1e9f, yhi = -1e9f;
    for (uint32_t i = first; i < first + count; i++) {
        const float *p = vertices[i].pos;
        if (std::fabs(p[3] - 1.0f) > 1e-4f) return; // 3D
        xlo = std::min(xlo, p[0]), xhi = std::max(xhi, p[0]), ylo = std::min(ylo, p[1]), yhi = std::max(yhi, p[1]);
    }
    // backdrops, sky and light overlays (half the screen or more) keep their size
    if ((xhi - xlo) / 2 * (yhi - ylo) / (2 * cur_ky) >= 0.5f) return;
    // over HUD: mostly on it (text on a dialogue box), not just passing over a corner of it (a chat
    // bubble sliding under the area name, which would suddenly shrink)
    auto mostly_on = [](float axlo, float axhi, float aylo, float ayhi, const HudBox &b) {
        float common = std::max(std::min(axhi, b.xhi) - std::max(axlo, b.xlo), 0.0f) *
                       std::max(std::min(ayhi, b.yhi) - std::max(aylo, b.ylo), 0.0f);
        return common > 0.0f && common >= 0.5f * (axhi - axlo) * (ayhi - aylo);
    };
    auto overlaps = [&](const HudBox &b) { return mostly_on(xlo, xhi, ylo, yhi, b); };
    // reaching the edge itself: the Objective bar, dialogue boxes and area names do; the bug net's
    // pointer floating just above the bottom (-0.92) and the Yo-kai Lens's ring (0.81) do not
    bool at_edge = xlo < -0.95f || xhi > 0.95f || ylo < -0.95f || yhi > 0.95f;
    auto under = std::find_if(hud_boxes.begin(), hud_boxes.end(), overlaps);
    uint64_t key = uint64_t(cur_tex0) << 32 | count;
    if (!at_edge && under == hud_boxes.end()) {
        candidates.push_back({first, count, {xlo, xhi, ylo, yhi, 0, 0}, key});
        return;
    }
    // pressed against an edge with nothing under it: a floating element (the Lens) stays as it is.
    // Over HUD, it is part of it (text on a dialogue box)
    if (under == hud_boxes.end()) {
        auto f = floating.find(key);
        if (f != floating.end() && f->second >= FLOAT_FRAMES) return;
    }
    floating.erase(key);
    // the top screen is rotated: clip x runs along its height (-1 at the bottom), y along its width.
    // Drawn over HUD: toward the same point, so it stays with it (a name tag over its box)
    float ax = -1.0f, ay = 0.0f; // the bottom center
    if (under != hud_boxes.end()) {
        ax = under->ax, ay = under->ay;
    } else if (xlo > 0.0f) {
        float center = (ylo + yhi) / 2;
        ax = 1.0f, ay = center < -1.0f / 3 ? -1.0f : center > 1.0f / 3 ? 1.0f : 0.0f;
    }
    char note[160];
    std::snprintf(note, sizeof note, "  hud x=[%.2f,%.2f] y=[%.2f,%.2f] toward (%.2f,%.2f)%s k=%.2f\n", xlo, xhi, ylo, yhi, ax, ay,
                  under != hud_boxes.end() ? " (over hud)" : "", k);
    pica::draw_log_note(note);
    hud_boxes.push_back({xlo, xhi, ylo, yhi, ax, ay});
    shrink(first, count, ax, ay, k);
    // earlier pieces it covers belong to it, and so do the pieces they cover in turn (a portrait's
    // face over its body, the body over the dialogue box's backdrop)
    std::vector<HudBox> claimed{{xlo, xhi, ylo, yhi, ax, ay}};
    while (!claimed.empty()) {
        HudBox b = claimed.back();
        claimed.pop_back();
        for (size_t i = 0; i < candidates.size();) {
            const HudBox &c = candidates[i].box;
            if (!mostly_on(c.xlo, c.xhi, c.ylo, c.yhi, b)) {
                i++;
                continue;
            }
            Candidate piece = candidates[i];
            candidates.erase(candidates.begin() + long(i));
            floating.erase(piece.key);
            HudBox box{piece.box.xlo, piece.box.xhi, piece.box.ylo, piece.box.yhi, ax, ay};
            hud_boxes.push_back(box);
            claimed.push_back(box);
            shrink(piece.first, piece.count, ax, ay, k);
        }
    }
}

void end_draw() {
    if (cur_skip) return;
    cur.count = uint32_t(vertices.size()) - cur.first;
    if (cur.count && cur.color->height == 400 && cur_ky != 1.0f && draw_geom.valid)
        placed.push_back({cur.first, cur.count, draw_geom, cur_ky});
    float hud = hud_scale();
    if (cur.count && hud < 1.0f && cur.color->height == 400 && top_has_3d()) scale_hud(cur.first, cur.count, hud);
    if (cur.count) batch.push_back(cur);
}

void flush() {
    if (batch.empty() && texture_uploads.empty() && surface_loads.empty() && depth_clears.empty() && !lut_upload)
        return;
    anchor_groups(); // before the vertices go to the GPU
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
    SDL_GPUBuffer *vbuf = nullptr;
    clear_fresh(cmd);
    upload_pending(cmd, &vbuf);
    clear_new_depth(cmd);
    if (!vbuf) batch.clear(); // out of GPU memory: this batch is lost
    SDL_GPURenderPass *pass = nullptr;
    Surface *bc = nullptr, *bd = nullptr;
    for (const DrawRec &d : batch) {
        begin_render_pass(cmd, pass, bc, bd, d.color, d.depth);
        d.color->drawn = true;
        SDL_BindGPUGraphicsPipeline(pass, d.pipeline);
        SDL_SetGPUViewport(pass, &d.viewport);
        SDL_SetGPUScissor(pass, &d.scissor);
        SDL_SetGPUStencilReference(pass, d.stencil_ref);
        SDL_SetGPUBlendConstants(pass, d.blend_const);
        SDL_GPUBufferBinding vb{vbuf, 0};
        SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);
        SDL_GPUTextureSamplerBinding tex[4] = {
            {d.tex[0], d.samp[0]}, {d.tex[1], d.samp[1]}, {d.tex[2], d.samp[2]}, {lut_tex, nearest_clamp}};
        SDL_BindGPUFragmentSamplers(pass, 0, tex, 4);
        SDL_PushGPUVertexUniformData(cmd, 0, d.depth_map, sizeof d.depth_map);
        SDL_PushGPUFragmentUniformData(cmd, 0, &d.fu, sizeof d.fu);
        SDL_DrawGPUPrimitives(pass, d.count, 1, d.first, 0);
    }
    if (pass) SDL_EndGPURenderPass(pass);
    SDL_SubmitGPUCommandBuffer(cmd);
    if (vbuf) SDL_ReleaseGPUBuffer(dev, vbuf);
    batch.clear();
    vertices.clear();
    placed.clear();
    settle_candidates(); // their vertices are gone
    static uint64_t trimmed = 0;
    if (use_clock - trimmed >= 120) trimmed = use_clock, trim_caches(600); // ~10 s unused
}

bool memory_fill(uint32_t start, uint32_t end, uint32_t value, uint32_t control) {
    if (!ok || !start || end <= start) return false;
    uint32_t p = to_physical(start);
    bool hit = false;
    SDL_GPUCommandBuffer *cmd = nullptr;
    for (auto &s : surfaces) {
        if (s->addr != p) continue;
        if (!hit) {
            flush();
            cmd = SDL_AcquireGPUCommandBuffer(dev);
            hit = true;
        }
        s->reload = false;
        if (s->depth) {
            SDL_GPUDepthStencilTargetInfo dt{};
            dt.texture = s->tex;
            float max = s->format == 0 ? 65535.0f : 16777215.0f;
            dt.clear_depth = float(value & (s->format == 0 ? 0xFFFF : 0xFFFFFF)) / max;
            dt.clear_stencil = s->format == 3 ? uint8_t(value >> 24) : 0;
            dt.load_op = dt.stencil_load_op = SDL_GPU_LOADOP_CLEAR;
            dt.store_op = dt.stencil_store_op = SDL_GPU_STOREOP_STORE;
            SDL_EndGPURenderPass(SDL_BeginGPURenderPass(cmd, nullptr, 0, &dt));
        } else {
            uint8_t bytes[4];
            std::memcpy(bytes, &value, 4);
            uint32_t width = (control >> 8) & 3;
            Rgba c = width == 2 && s->format == FB_RGBA8 ? fb_decode(FB_RGBA8, bytes) : fb_decode(s->format, bytes);
            s->drawn = false, s->guest_hash = guest_hash(*s); // the fill already wrote guest memory
            SDL_GPUColorTargetInfo ct{};
            ct.texture = s->tex;
            ct.clear_color = {c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f};
            ct.load_op = SDL_GPU_LOADOP_CLEAR, ct.store_op = SDL_GPU_STOREOP_STORE;
            SDL_EndGPURenderPass(SDL_BeginGPURenderPass(cmd, &ct, 1, nullptr));
        }
    }
    if (cmd) SDL_SubmitGPUCommandBuffer(cmd);
    (void)end;
    return hit;
}

bool display_transfer(uint32_t in, uint32_t out, uint32_t in_w, uint32_t in_h, uint32_t out_w, uint32_t out_h,
                      uint32_t flags) {
    if (!ok) return false;
    if (in_h == 400) { // a top-screen frame is done
        anchor_groups();
        placed.clear();
        hud_boxes.clear(), settle_candidates(), use_clock++;
    }
    // YW2_TEST_READBACK: every frame goes GPU -> guest memory -> CPU transfer (tests readback)
    static const bool test_readback = std::getenv("YW2_TEST_READBACK") != nullptr;
    if (test_readback) {
        before_cpu_read(in, in_w * in_h * 4);
        return false;
    }
    uint32_t p = to_physical(in);
    Surface *src = nullptr;
    for (auto &s : surfaces)
        if (!s->depth && s->addr == p && s->width == in_w && s->height == in_h) src = s.get();
    // the LCDs take linear RGB output; other transfers (into textures) go through guest memory
    bool to_lcd = !(flags & 2) && !(flags & 0x20) && (((flags >> 12) & 7) <= 1);
    if (!src || !to_lcd) return false;
    flush();
    uint32_t hs = ((flags >> 24) & 3) ? 1 : 0, vs = ((flags >> 24) & 3) == 2 ? 1 : 0;
    int w = int(src->th >> vs), h = int(src->tw >> hs); // upright: rows of the output become columns
    (void)out_w, (void)out_h;

    SDL_LockMutex(lcd_lock);
    LcdFrames &f = lcd[out];
    if (f.w != w || f.h != h) {
        for (auto *t : f.tex)
            if (t) SDL_ReleaseGPUTexture(dev, t);
        for (auto *&t : f.tex)
            t = make_texture(uint32_t(w), uint32_t(h), SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                             SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
        f.w = w, f.h = h, f.latest = -1;
    }
    int slot = f.next;
    f.next = (f.next + 1) % 3;
    SDL_GPUTexture *target = f.tex[slot];
    SDL_UnlockMutex(lcd_lock);

    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
    SDL_GPUColorTargetInfo ct{};
    ct.texture = target;
    ct.load_op = SDL_GPU_LOADOP_DONT_CARE, ct.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &ct, 1, nullptr);
    SDL_BindGPUGraphicsPipeline(pass, blit_pipeline);
    SDL_GPUTextureSamplerBinding b{src->tex, linear_clamp};
    SDL_BindGPUFragmentSamplers(pass, 0, &b, 1);
    BlitUniforms u{{float(w), float(h), float(src->tw), float(src->th)}, {int32_t(flags & 1), int32_t(hs), int32_t(vs), 0}};
    SDL_PushGPUFragmentUniformData(cmd, 0, &u, sizeof u);
    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    SDL_EndGPURenderPass(pass);
    SDL_SubmitGPUCommandBuffer(cmd);

    SDL_LockMutex(lcd_lock);
    f.latest = slot;
    SDL_UnlockMutex(lcd_lock);
    return true;
}

void before_cpu_read(uint32_t addr, uint32_t size) {
    if (!ok || !size) return;
    uint32_t p = to_physical(addr);
    // draws still in the batch are what the read wants, and a surface only counts as drawn once
    // they're sent: the Yo-kai Lens copies its view out right after drawing it, which otherwise
    // found the view untouched and read the (previous frame's) top screen around it instead
    auto overlaps = [&](const Surface *s) { return p < s->addr + s->bytes() && s->addr < p + size; };
    if (std::any_of(batch.begin(), batch.end(), [&](const DrawRec &d) { return overlaps(d.color); })) flush();
    // A read of exactly one surface's buffer only needs that surface. The Yo-kai Lens draws its
    // small view at the top screen's own address and copies it out every frame; downloading the
    // whole (upscaled, widescreen) top screen along with it each time made the Lens crawl.
    bool exact = std::any_of(surfaces.begin(), surfaces.end(), [&](const std::unique_ptr<Surface> &s) {
        return !s->depth && s->drawn && s->addr == p && s->bytes() == size;
    });
    for (auto &s : surfaces) {
        if (s->depth || !s->drawn || !overlaps(s.get())) continue;
        if (exact && (s->addr != p || s->bytes() != size)) continue;
        if (!overwritten(s.get())) download_surface(s.get());
    }
}

void invalidate(uint32_t addr, uint32_t size) {
    if (!ok) return;
    uint32_t p = to_physical(addr);
    for (auto &s : surfaces)
        if (p < s->addr + s->bytes() && s->addr < p + size) s->reload = true, s->drawn = false; // guest is newer
    SDL_LockMutex(lcd_lock);
    lcd.erase(addr); // an LCD buffer written by the CPU shows that, not an older GPU frame
    SDL_UnlockMutex(lcd_lock);
}

SDL_GPUTexture *lcd_frame(uint32_t addr, int *width, int *height) {
    if (!ok) return nullptr;
    SDL_LockMutex(lcd_lock);
    SDL_GPUTexture *t = nullptr;
    auto it = lcd.find(addr);
    if (it != lcd.end() && it->second.latest >= 0) {
        t = it->second.tex[it->second.latest];
        *width = it->second.w, *height = it->second.h;
    }
    SDL_UnlockMutex(lcd_lock);
    return t;
}

} // namespace pica::gpu
