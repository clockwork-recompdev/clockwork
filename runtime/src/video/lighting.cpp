// PICA200 fragment lighting: per-pixel diffuse and specular from up to 8 lights, shaped by
// lookup tables the game uploads (distribution, reflection, Fresnel, spotlight and distance
// attenuation). The results feed the TEV as the "fragment primary/secondary color" sources.
// The math follows the hardware as documented by 3dbrew and Citra.
#include "internal.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

namespace pica {
namespace {

// LUT ids
enum Sampler { D0 = 0, D1 = 1, FR = 3, RB = 4, RG = 5, RR = 6, SPOT = 8, DIST = 16 };
// LUT input selectors
enum Input { NH = 0, VH = 1, NV = 2, LN = 3, SP = 4, CP = 5 };

struct Vec3 {
    float x, y, z;
    Vec3 operator+(Vec3 o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(Vec3 o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    Vec3 operator*(Vec3 o) const { return {x * o.x, y * o.y, z * o.z}; }
};
float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
Vec3 normalized(Vec3 v) {
    float l = std::sqrt(dot(v, v));
    return l > 0 ? v * (1.0f / l) : v;
}

// 1.5.10 and 1.7.12 floats used by the lighting registers
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
Vec3 light_color(uint32_t c) {
    return {float((c >> 20) & 0x3FF) / 255.0f, float((c >> 10) & 0x3FF) / 255.0f, float(c & 0x3FF) / 255.0f};
}

struct Light {
    Vec3 specular0, specular1, diffuse, ambient, position, spot;
    bool directional, two_sided, geo0, geo1;
    float dist_bias, dist_scale;
    int num;
};

struct LutUse {
    bool enabled, abs;
    uint32_t input;
    float scale;
};



// which LUTs each lighting configuration provides
bool supported(uint32_t config, int sampler) {
    switch (sampler) {
    case D0: return config != 1;
    case D1: return config != 0 && config != 1 && config != 5;
    case SPOT: return config != 2 && config != 3;
    case FR: return config != 0 && config != 2 && config != 4;
    case RR: return config != 3;
    default: return config == 4 || config == 5 || config == 8; // RG, RB
    }
}


Vec3 rotate(const float q[4], Vec3 v) {
    Vec3 u = {q[0], q[1], q[2]};
    return v + cross(u, cross(u, v) + v * q[3]) * 2.0f;
}

} // namespace

struct LightingState {
    int count = 0;
    Light lights[8];
    Vec3 ambient{};
    uint32_t config = 0;
    bool shadow = false, shadow_invert = false, shadow_primary = false, shadow_secondary = false, shadow_alpha = false;
    bool primary_alpha = false, secondary_alpha = false, clamp_highlights = false, bump_renorm = true;
    uint32_t shadow_selector = 0, bump_selector = 0, bump_mode = 0;
    uint32_t disable_shadow = 0, disable_spot = 0, disable_dist = 0;
    LutUse d0, d1, fr, rr, rg, rb, sp;
    uint32_t luts[24][256];
};

namespace {
float lut(const LightingState &ls, int table, int index, float delta) {
    uint32_t e = ls.luts[table][index & 255];
    float value = float(e & 0xFFF) / 4095.0f;
    float diff = float((e >> 12) & 0x7FF) / 2047.0f;
    return value + ((e >> 23) & 1 ? -diff : diff) * delta;
}
} // namespace

std::shared_ptr<const LightingState> latch_lighting() {
    // reuse the last snapshot while the lighting registers and LUTs are unchanged
    static std::shared_ptr<LightingState> last;
    static uint32_t last_regs[0x1DA - 0x140], last_version = ~0u;
    if (last && last_version == lighting_lut_version &&
        std::memcmp(last_regs, &regs[0x140], sizeof last_regs) == 0)
        return last;
    std::memcpy(last_regs, &regs[0x140], sizeof last_regs);
    last_version = lighting_lut_version;
    last = std::make_shared<LightingState>();
    LightingState &ls = *last;
    std::memcpy(ls.luts, lighting_lut, sizeof ls.luts);
    ls.count = int(regs[LIGHTING_NUM_LIGHTS] & 7) + 1;
    uint32_t c0 = regs[LIGHTING_CONFIG0], c1 = regs[LIGHTING_CONFIG1];
    ls.config = (c0 >> 4) & 0xF;
    ls.shadow = c0 & 1, ls.primary_alpha = (c0 >> 2) & 1, ls.secondary_alpha = (c0 >> 3) & 1;
    ls.shadow_primary = (c0 >> 16) & 1, ls.shadow_secondary = (c0 >> 17) & 1, ls.shadow_invert = (c0 >> 18) & 1;
    ls.shadow_alpha = (c0 >> 19) & 1, ls.bump_selector = (c0 >> 22) & 3, ls.shadow_selector = (c0 >> 24) & 3;
    ls.clamp_highlights = (c0 >> 27) & 1, ls.bump_mode = (c0 >> 28) & 3, ls.bump_renorm = !((c0 >> 30) & 1);
    ls.disable_shadow = c1 & 0xFF, ls.disable_spot = (c1 >> 8) & 0xFF, ls.disable_dist = (c1 >> 24) & 0xFF;
    ls.ambient = light_color(regs[LIGHTING_AMBIENT]);

    uint32_t abs = regs[LIGHTING_LUT_ABS], input = regs[LIGHTING_LUT_INPUT], scale = regs[LIGHTING_LUT_SCALE];
    auto use = [&](int shift, int disable_bit, int sampler) {
        static constexpr float SCALES[8] = {1, 2, 4, 8, 0, 0, 0.25f, 0.5f};
        bool on = (disable_bit < 0 || !((c1 >> disable_bit) & 1)) && supported(ls.config, sampler);
        return LutUse{on, !((abs >> (shift + 1)) & 1), (input >> shift) & 7, SCALES[(scale >> shift) & 7]};
    };
    ls.d0 = use(0, 16, D0), ls.d1 = use(4, 17, D1), ls.sp = use(8, -1, SPOT), ls.fr = use(12, 19, FR);
    ls.rb = use(16, 22, RB), ls.rg = use(20, 21, RG), ls.rr = use(24, 20, RR);

    uint32_t perm = regs[LIGHTING_PERMUTATION];
    for (int i = 0; i < ls.count; i++) {
        int num = int((perm >> (4 * i)) & 7);
        const uint32_t *r = &regs[LIGHT0 + 0x10 * num];
        Light &l = ls.lights[i];
        l.num = num;
        l.specular0 = light_color(r[0]), l.specular1 = light_color(r[1]);
        l.diffuse = light_color(r[2]), l.ambient = light_color(r[3]);
        l.position = {f16(r[4] & 0xFFFF), f16(r[4] >> 16), f16(r[5] & 0xFFFF)};
        auto s13 = [](uint32_t v) { return float(int32_t(v << 19) >> 19) / 2047.0f; };
        l.spot = {s13(r[6] & 0x1FFF), s13((r[6] >> 16) & 0x1FFF), s13(r[7] & 0x1FFF)};
        l.directional = r[9] & 1, l.two_sided = (r[9] >> 1) & 1, l.geo0 = (r[9] >> 2) & 1, l.geo1 = (r[9] >> 3) & 1;
        l.dist_bias = f20(r[10] & 0xFFFFF), l.dist_scale = f20(r[11] & 0xFFFFF);
    }
    return last;
}

void fragment_lighting(const LightingState &ls, const float quat[4], const float view_in[3], const Rgba tex[4],
                       Rgba &primary, Rgba &secondary) {
    float q[4] = {quat[0], quat[1], quat[2], quat[3]};
    float ql = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (ql > 0)
        for (float &c : q) c /= ql;
    Vec3 view = {view_in[0], view_in[1], view_in[2]};

    float shadow[4] = {1, 1, 1, 1};
    if (ls.shadow) {
        const Rgba &t = tex[ls.shadow_selector];
        shadow[0] = t.r / 255.0f, shadow[1] = t.g / 255.0f, shadow[2] = t.b / 255.0f, shadow[3] = t.a / 255.0f;
        if (ls.shadow_invert)
            for (float &c : shadow) c = 1.0f - c;
    }

    Vec3 surface_normal = {0, 0, 1}, surface_tangent = {1, 0, 0};
    if (ls.bump_mode == 1 || ls.bump_mode == 2) {
        const Rgba &t = tex[ls.bump_selector];
        Vec3 p = {t.r / 127.5f - 1.0f, t.g / 127.5f - 1.0f, t.b / 127.5f - 1.0f};
        if (ls.bump_mode == 1) {
            if (ls.bump_renorm) p.z = std::sqrt(std::max(1.0f - (p.x * p.x + p.y * p.y), 0.0f));
            surface_normal = p;
        } else {
            surface_tangent = p;
        }
    }
    Vec3 normal = rotate(q, surface_normal), tangent = rotate(q, surface_tangent);

    Vec3 diffuse_sum{}, specular_sum{};
    float diffuse_alpha = 1.0f, specular_alpha = 1.0f;
    Vec3 norm_view = normalized(view);

    for (int i = 0; i < ls.count; i++) {
        const Light &l = ls.lights[i];
        Vec3 light_vector = l.directional ? l.position : l.position + view;
        float length = std::sqrt(dot(light_vector, light_vector));
        light_vector = normalized(light_vector);
        Vec3 half = norm_view + light_vector;

        float dist_atten = 1.0f;
        if (!((ls.disable_dist >> l.num) & 1)) {
            float loc = std::clamp(l.dist_scale * length + l.dist_bias, 0.0f, 1.0f);
            int index = int(std::clamp(std::floor(loc * 256.0f), 0.0f, 255.0f));
            dist_atten = lut(ls, DIST + l.num, index, loc * 256.0f - float(index));
        }

        auto lut_value = [&](const LutUse &u, int table) {
            float x = 0;
            switch (u.input) {
            case NH: x = dot(normal, normalized(half)); break;
            case VH: x = dot(norm_view, normalized(half)); break;
            case NV: x = dot(normal, norm_view); break;
            case LN: x = dot(light_vector, normal); break;
            case SP: x = dot(light_vector, l.spot); break;
            case CP:
                if (ls.config == 8) {
                    Vec3 h = normalized(half);
                    x = dot(h - normal * dot(normal, h), tangent);
                }
                break;
            default: break;
            }
            int index;
            float delta;
            if (u.abs) {
                x = l.two_sided ? std::fabs(x) : std::max(x, 0.0f);
                index = int(std::clamp(std::floor(x * 256.0f), 0.0f, 255.0f));
                delta = x * 256.0f - float(index);
            } else {
                int s = int(std::clamp(std::floor(x * 128.0f), -128.0f, 127.0f));
                delta = x * 128.0f - float(s);
                index = s & 0xFF;
            }
            return u.scale * lut(ls, table, index, delta);
        };

        float spot_atten = 1.0f;
        if (!((ls.disable_spot >> l.num) & 1) && ls.sp.enabled) spot_atten = lut_value(ls.sp, SPOT + l.num);
        float d0 = ls.d0.enabled ? lut_value(ls.d0, D0) : 1.0f;
        Vec3 specular0 = l.specular0 * d0;
        Vec3 refl;
        refl.x = ls.rr.enabled ? lut_value(ls.rr, RR) : 1.0f;
        refl.y = ls.rg.enabled ? lut_value(ls.rg, RG) : refl.x;
        refl.z = ls.rb.enabled ? lut_value(ls.rb, RB) : refl.x;
        float d1 = ls.d1.enabled ? lut_value(ls.d1, D1) : 1.0f;
        Vec3 specular1 = refl * l.specular1 * d1;

        if (i == ls.count - 1 && ls.fr.enabled) {
            float f = lut_value(ls.fr, FR);
            if (ls.primary_alpha) diffuse_alpha = f;
            if (ls.secondary_alpha) specular_alpha = f;
        }

        float nl = dot(light_vector, normal);
        nl = l.two_sided ? std::fabs(nl) : std::max(nl, 0.0f);
        float clamp = ls.clamp_highlights && nl == 0.0f ? 0.0f : 1.0f;
        if (l.geo0 || l.geo1) {
            float g = dot(half, half);
            g = g == 0.0f ? 0.0f : std::min(nl / g, 1.0f);
            if (l.geo0) specular0 = specular0 * g;
            if (l.geo1) specular1 = specular1 * g;
        }
        bool shadowed = !((ls.disable_shadow >> l.num) & 1);
        Vec3 sp = ls.shadow_primary && shadowed ? Vec3{shadow[0], shadow[1], shadow[2]} : Vec3{1, 1, 1};
        Vec3 ss = ls.shadow_secondary && shadowed ? Vec3{shadow[0], shadow[1], shadow[2]} : Vec3{1, 1, 1};
        diffuse_sum = diffuse_sum + (l.diffuse * nl * sp + l.ambient) * (dist_atten * spot_atten);
        specular_sum = specular_sum + (specular0 + specular1) * ss * (clamp * dist_atten * spot_atten);
    }
    if (ls.shadow_alpha) {
        if (ls.primary_alpha) diffuse_alpha *= shadow[3];
        if (ls.secondary_alpha) specular_alpha *= shadow[3];
    }
    diffuse_sum = diffuse_sum + ls.ambient;
    auto u8c = [](float v) { return uint8_t(std::clamp(v, 0.0f, 1.0f) * 255.0f); };
    primary = {u8c(diffuse_sum.x), u8c(diffuse_sum.y), u8c(diffuse_sum.z), u8c(diffuse_alpha)};
    secondary = {u8c(specular_sum.x), u8c(specular_sum.y), u8c(specular_sum.z), u8c(specular_alpha)};
}

} // namespace pica
