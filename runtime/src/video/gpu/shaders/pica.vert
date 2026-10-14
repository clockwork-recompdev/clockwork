#version 450
// PICA200 vertices arrive already shaded by the CPU (the game's vertex shader) in clip space.
// The PICA clips against z in [-w, 0], then scales and offsets depth and *clamps* the result
// to [0, 1] (the game relies on that: e.g. dialogue portraits sit at a negative depth). So the
// near/far planes are clip distances here, and the pipeline clamps depth instead of clipping.

layout(location = 0) in vec4 a_pos;
layout(location = 1) in vec4 a_color;
layout(location = 2) in vec4 a_tc01;  // tex0 uv, tex1 uv
layout(location = 3) in vec4 a_tc2w;  // tex2 uv, tex0 w
layout(location = 4) in vec4 a_quat;  // normal quaternion
layout(location = 5) in vec4 a_view;  // view vector

layout(set = 1, binding = 0) uniform Vert {
    vec4 depth; // x: scale, y: offset
};

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec4 v_tc01;
layout(location = 2) out vec4 v_tc2w;
layout(location = 3) out vec4 v_quat;
layout(location = 4) out vec4 v_view;
layout(location = 5) out float v_wdepth; // depth * w, for W-buffering

void main() {
    float z = a_pos.z * depth.x + depth.y * a_pos.w;
    gl_Position = vec4(a_pos.xy, z, a_pos.w);
    gl_ClipDistance[0] = -a_pos.z;          // z <= 0
    gl_ClipDistance[1] = a_pos.z + a_pos.w; // z >= -w
    v_color = a_color;
    v_tc01 = a_tc01;
    v_tc2w = a_tc2w;
    v_quat = a_quat;
    v_view = a_view;
    v_wdepth = z;
}
