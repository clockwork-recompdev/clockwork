#version 450
// PICA200 fragment pipeline as one shader, configured per draw: texture units 0-2, fragment
// lighting, the six-stage TEV combiner (in 8-bit integer math, like the hardware), fog and
// the alpha test. Blending, depth, stencil and the write mask are pipeline state.
//
// Images are stored in memory row order (row 0 = the PICA's top row in memory = the bottom of
// the picture), so texture coordinate v samples row (1 - v).

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec4 v_tc01;
layout(location = 2) in vec4 v_tc2w;
layout(location = 3) in vec4 v_quat;
layout(location = 4) in vec4 v_view;
layout(location = 5) in float v_wdepth;

layout(set = 2, binding = 0) uniform sampler2D tex0;
layout(set = 2, binding = 1) uniform sampler2D tex1;
layout(set = 2, binding = 2) uniform sampler2D tex2;
layout(set = 2, binding = 3) uniform sampler2D luts; // 256 x 25, RG = value, difference to next;
                                                     // rows 0-23 lighting, row 24 fog

layout(set = 3, binding = 0) uniform Frag {
    ivec4 tev_src[6];   // packed 4-bit ids: color sources, alpha sources, color operands, alpha operands
    ivec4 tev_op[6];    // color combiner, alpha combiner, color scale shift, alpha scale shift
    vec4 tev_const[6];
    vec4 buffer_init;
    ivec4 misc;         // x: stages, y: buffer update bits (color 0-3, alpha 4-7), z: alpha test (bit 3 on, 0-2 func), w: ref
    ivec4 flags;        // x: bit 0 lighting, 1 fog, 2 fog flip, 3 tex2 uses tex1 coords, 4 tex0 projected, 5 W-buffer;
                        // y: sampled texture units (bits 0-2)
    vec4 fog_color;
    ivec4 tex_wrap[3];  // x: border on s, y: border on t
    vec4 tex_border[3];
    // fragment lighting
    ivec4 light_misc;   // x: lights, y: config0, z: config1, w: lighting config (LUT set)
    ivec4 lut_use[7];   // d0, d1, sp, fr, rr, rg, rb: x enabled, y abs, z input
    vec4 lut_scale[7];
    vec4 ambient;
    vec4 light[8 * 7];  // per light: specular0, specular1, diffuse, ambient,
                        // position (w: directional), spot direction (w: two-sided), (bias, scale, number, geometric factor bits)
};

layout(location = 0) out vec4 out_color;

int field(int packed, int i) { return (packed >> (4 * i)) & 0xF; }
ivec4 to_u8(vec4 c) { return ivec4(clamp(c, 0.0, 1.0) * 255.0 + 0.5); }
int div255(int x) { return (x + 1 + (x >> 8)) >> 8; }

vec4 sample_unit(int unit, sampler2D s, vec2 uv) {
    if (((flags.y >> unit) & 1) == 0) return vec4(0.0);
    if ((tex_wrap[unit].x != 0 && (uv.x < 0.0 || uv.x > 1.0)) || (tex_wrap[unit].y != 0 && (uv.y < 0.0 || uv.y > 1.0)))
        return tex_border[unit];
    return texture(s, vec2(uv.x, 1.0 - uv.y));
}

// ---- fragment lighting ----

float lut_value(int table, int index, float delta) {
    vec2 e = texelFetch(luts, ivec2(index & 255, table), 0).rg;
    return e.x + e.y * delta;
}

vec3 rotate(vec4 q, vec3 v) { return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v); }

// lights' primary (diffuse) and secondary (specular) colors
void lighting(ivec4 tex[4], out ivec4 primary, out ivec4 secondary) {
    int c0 = light_misc.y, c1 = light_misc.z, config = light_misc.w;
    vec4 q = v_quat;
    float ql = length(q);
    if (ql > 0.0) q /= ql;
    vec3 view = v_view.xyz;

    vec4 shadow = vec4(1.0);
    if ((c0 & 1) != 0) {
        shadow = vec4(tex[(c0 >> 24) & 3]) / 255.0;
        if (((c0 >> 18) & 1) != 0) shadow = 1.0 - shadow;
    }
    vec3 surface_normal = vec3(0, 0, 1), surface_tangent = vec3(1, 0, 0);
    int bump = (c0 >> 28) & 3;
    if (bump == 1 || bump == 2) {
        vec3 p = vec3(tex[(c0 >> 22) & 3].rgb) / 127.5 - 1.0;
        if (bump == 1) {
            if (((c0 >> 30) & 1) == 0) p.z = sqrt(max(1.0 - dot(p.xy, p.xy), 0.0));
            surface_normal = p;
        } else {
            surface_tangent = p;
        }
    }
    vec3 normal = rotate(q, surface_normal), tangent = rotate(q, surface_tangent);
    vec3 nview = normalize(view);
    vec3 diffuse_sum = vec3(0), specular_sum = vec3(0);
    float diffuse_alpha = 1.0, specular_alpha = 1.0;

    for (int i = 0; i < light_misc.x; i++) {
        vec4 spec0 = light[i * 7 + 0], spec1 = light[i * 7 + 1], diffuse = light[i * 7 + 2], amb = light[i * 7 + 3];
        vec4 pos = light[i * 7 + 4], spot = light[i * 7 + 5], att = light[i * 7 + 6];
        int num = int(att.z), geo = int(att.w);
        bool two_sided = spot.w != 0.0;
        vec3 lv = pos.w != 0.0 ? pos.xyz : pos.xyz + view;
        float len = length(lv);
        lv = len > 0.0 ? lv / len : lv;
        vec3 half_v = nview + lv;

        float dist_atten = 1.0;
        if (((c1 >> (24 + num)) & 1) == 0) {
            float loc = clamp(att.y * len + att.x, 0.0, 1.0);
            int index = int(clamp(floor(loc * 256.0), 0.0, 255.0));
            dist_atten = lut_value(16 + num, index, loc * 256.0 - float(index));
        }

        float lutv[7];
        for (int u = 0; u < 7; u++) {
            lutv[u] = 1.0;
            if (lut_use[u].x == 0) continue;
            float x = 0.0;
            int sel = lut_use[u].z;
            if (sel == 0) x = dot(normal, normalize(half_v));
            else if (sel == 1) x = dot(nview, normalize(half_v));
            else if (sel == 2) x = dot(normal, nview);
            else if (sel == 3) x = dot(lv, normal);
            else if (sel == 4) x = dot(lv, spot.xyz);
            else if (sel == 5 && config == 8) {
                vec3 h = normalize(half_v);
                x = dot(h - normal * dot(normal, h), tangent);
            }
            int index;
            float delta;
            if (lut_use[u].y != 0) {
                x = two_sided ? abs(x) : max(x, 0.0);
                index = int(clamp(floor(x * 256.0), 0.0, 255.0));
                delta = x * 256.0 - float(index);
            } else {
                int s = int(clamp(floor(x * 128.0), -128.0, 127.0));
                delta = x * 128.0 - float(s);
                index = s & 0xFF;
            }
            // tables: d0 0, d1 1, spot 8 + light, fresnel 3, reflect r/g/b 6/5/4
            const int TABLE[7] = int[7](0, 1, 8, 3, 6, 5, 4);
            int table = u == 2 ? 8 + num : TABLE[u];
            lutv[u] = lut_scale[u].x * lut_value(table, index, delta);
        }
        float spot_atten = ((c1 >> (8 + num)) & 1) == 0 ? lutv[2] : 1.0;
        vec3 specular0 = spec0.rgb * lutv[0];
        vec3 refl;
        refl.r = lutv[4];
        refl.g = lut_use[5].x != 0 ? lutv[5] : refl.r;
        refl.b = lut_use[6].x != 0 ? lutv[6] : refl.r;
        vec3 specular1 = refl * spec1.rgb * lutv[1];
        if (i == light_misc.x - 1 && lut_use[3].x != 0) {
            if (((c0 >> 2) & 1) != 0) diffuse_alpha = lutv[3];
            if (((c0 >> 3) & 1) != 0) specular_alpha = lutv[3];
        }
        float nl = dot(lv, normal);
        nl = two_sided ? abs(nl) : max(nl, 0.0);
        float clamp_h = ((c0 >> 27) & 1) != 0 && nl == 0.0 ? 0.0 : 1.0;
        if (geo != 0) {
            float g = dot(half_v, half_v);
            g = g == 0.0 ? 0.0 : min(nl / g, 1.0);
            if ((geo & 1) != 0) specular0 *= g;
            if ((geo & 2) != 0) specular1 *= g;
        }
        bool shadowed = ((c1 >> num) & 1) == 0;
        vec3 sp = ((c0 >> 16) & 1) != 0 && shadowed ? shadow.rgb : vec3(1);
        vec3 ss = ((c0 >> 17) & 1) != 0 && shadowed ? shadow.rgb : vec3(1);
        diffuse_sum += (diffuse.rgb * nl * sp + amb.rgb) * dist_atten * spot_atten;
        specular_sum += (specular0 + specular1) * ss * clamp_h * dist_atten * spot_atten;
    }
    if (((c0 >> 19) & 1) != 0) {
        if (((c0 >> 2) & 1) != 0) diffuse_alpha *= shadow.a;
        if (((c0 >> 3) & 1) != 0) specular_alpha *= shadow.a;
    }
    diffuse_sum += ambient.rgb;
    primary = ivec4(clamp(vec4(diffuse_sum, diffuse_alpha), 0.0, 1.0) * 255.0);
    secondary = ivec4(clamp(vec4(specular_sum, specular_alpha), 0.0, 1.0) * 255.0);
}

// ---- TEV ----

ivec3 color_operand(ivec4 c, int op) {
    if (op == 1) return 255 - c.rgb;
    if (op == 2) return ivec3(c.a);
    if (op == 3) return ivec3(255 - c.a);
    if (op == 4) return ivec3(c.r);
    if (op == 5) return ivec3(255 - c.r);
    if (op == 8) return ivec3(c.g);
    if (op == 9) return ivec3(255 - c.g);
    if (op == 12) return ivec3(c.b);
    if (op == 13) return ivec3(255 - c.b);
    return c.rgb;
}

int alpha_operand(ivec4 c, int op) {
    op &= 7;
    if (op == 0) return c.a;
    if (op == 1) return 255 - c.a;
    if (op == 2) return c.r;
    if (op == 3) return 255 - c.r;
    if (op == 4) return c.g;
    if (op == 5) return 255 - c.g;
    if (op == 6) return c.b;
    return 255 - c.b;
}

ivec3 combine3(int op, ivec3 a, ivec3 b, ivec3 c) {
    if (op == 1) return (a * b + 1 + ((a * b) >> 8)) >> 8;
    if (op == 2) return min(a + b, 255);
    if (op == 3) return clamp(a + b - 128, 0, 255);
    if (op == 4) { ivec3 x = a * c + b * (255 - c); return (x + 1 + (x >> 8)) >> 8; }
    if (op == 5) return max(a - b, 0);
    if (op == 8) return min((a * b + 255 * c) / 255, 255);
    if (op == 9) { ivec3 x = min(a + b, 255) * c; return (x + 1 + (x >> 8)) >> 8; }
    return a;
}

int combine1(int op, int a, int b, int c) { return combine3(op, ivec3(a), ivec3(b), ivec3(c)).x; }

int dot3(ivec3 a, ivec3 b) {
    ivec3 p = ((a * 2 - 255) * (b * 2 - 255) + 128) / 256;
    return clamp(p.x + p.y + p.z, 0, 255);
}

bool test(int func, int a, int b) {
    if (func == 0) return false;
    if (func == 1) return true;
    if (func == 2) return a == b;
    if (func == 3) return a != b;
    if (func == 4) return a < b;
    if (func == 5) return a <= b;
    if (func == 6) return a > b;
    return a >= b;
}

void main() {
    vec2 uv0 = v_tc01.xy;
    if ((flags.x & 16) != 0 && v_tc2w.z != 0.0) uv0 /= v_tc2w.z;
    vec2 uv2 = (flags.x & 8) != 0 ? v_tc01.zw : v_tc2w.xy;
    ivec4 tex[4];
    tex[0] = to_u8(sample_unit(0, tex0, uv0));
    tex[1] = to_u8(sample_unit(1, tex1, v_tc01.zw));
    tex[2] = to_u8(sample_unit(2, tex2, uv2));
    tex[3] = ivec4(0);

    ivec4 frag_primary = ivec4(0), frag_secondary = ivec4(0);
    if ((flags.x & 1) != 0) lighting(tex, frag_primary, frag_secondary);

    ivec4 src[16];
    for (int i = 0; i < 16; i++) src[i] = ivec4(0);
    src[0] = to_u8(v_color);
    src[1] = frag_primary;
    src[2] = frag_secondary;
    src[3] = tex[0];
    src[4] = tex[1];
    src[5] = tex[2];
    ivec4 next_buffer = to_u8(buffer_init);
    src[13] = next_buffer;
    for (int i = 0; i < misc.x; i++) {
        src[14] = to_u8(tev_const[i]);
        ivec3 c[3];
        int a[3];
        for (int k = 0; k < 3; k++) {
            c[k] = color_operand(src[field(tev_src[i].x, k)], field(tev_src[i].z, k));
            a[k] = alpha_operand(src[field(tev_src[i].y, k)], field(tev_src[i].w, k));
        }
        int cop = tev_op[i].x, aop = tev_op[i].y;
        ivec4 o;
        if (cop == 6 || cop == 7) {
            o.rgb = ivec3(min(dot3(c[0], c[1]) << tev_op[i].z, 255));
        } else {
            o.rgb = min(combine3(cop, c[0], c[1], c[2]) << tev_op[i].z, 255);
        }
        o.a = cop == 7 ? o.r : min(combine1(aop, a[0], a[1], a[2]) << tev_op[i].w, 255);
        src[15] = o;
        src[13] = next_buffer; // the buffer source lags one stage behind its update
        if (i < 4) {
            if (((misc.y >> i) & 1) != 0) next_buffer.rgb = o.rgb;
            if (((misc.y >> (4 + i)) & 1) != 0) next_buffer.a = o.a;
        }
    }
    ivec4 color = src[15];

    float depth = gl_FragCoord.z;
    if ((flags.x & 32) != 0) {
        depth = clamp(v_wdepth, 0.0, 1.0);
        gl_FragDepth = depth;
    } else {
        gl_FragDepth = gl_FragCoord.z;
    }

    if ((flags.x & 2) != 0) { // fog, by depth
        float index = ((flags.x & 4) != 0 ? 1.0 - depth : depth) * 128.0;
        float i = clamp(floor(index), 0.0, 127.0);
        float f = clamp(lut_value(24, int(i), index - i), 0.0, 1.0);
        color.rgb = ivec3(f * vec3(color.rgb) + (1.0 - f) * fog_color.rgb * 255.0);
    }

    if ((misc.z & 8) != 0 && !test(misc.z & 7, color.a, misc.w)) discard;
    out_color = vec4(color) / 255.0;
}
