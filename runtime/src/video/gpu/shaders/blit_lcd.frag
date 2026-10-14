#version 450
// Display transfer to an LCD image: reads a render target (rows in memory order, as the PICA
// lays them out) and writes the screen as it appears, upright. The LCDs scan out rotated, so
// a screen column is a row of the transfer's output. Downscaling averages 2x1 or 2x2 blocks
// by sampling bilinearly at the block's center.

layout(set = 2, binding = 0) uniform sampler2D src;

layout(set = 3, binding = 0) uniform Blit {
    vec4 size;    // x: output columns (screen width), y: output rows (screen height), z/w: source size
    ivec4 mode;   // x: flip, y: horizontal downscale (0/1), z: vertical downscale (0/1)
};

layout(location = 0) out vec4 color;

void main() {
    int sx = int(gl_FragCoord.x), sy = int(gl_FragCoord.y);
    int rows = int(size.x), column = int(size.y);
    // transfer output pixel (x, y): x along the screen's height, y along its width
    int x = column - 1 - sy;
    int y = mode.x != 0 ? rows - 1 - sx : sx;
    vec2 block = vec2(1 << mode.y, 1 << mode.z);
    vec2 texel = vec2(x, y) * block + block * 0.5;
    color = vec4(textureLod(src, texel / size.zw, 0.0).rgb, 1.0);
}
