#pragma once
// PICA200, the 3DS GPU: command lists, vertex processing and a software rasterizer that draws
// straight into guest VRAM/FCRAM, so render targets, textures and display transfers all see
// the same memory the game does.
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace pica {

// GPU physical address -> guest virtual address (VRAM, FCRAM linear heap); 0 if not visible
uint32_t to_virtual(uint32_t paddr);
// host pointer for `size` bytes at a GPU physical address, or nullptr if any of it is unmapped
uint8_t *phys(uint32_t paddr, uint32_t size);

extern std::atomic<int64_t> time_commands, time_raster, time_transfer, time_setup, time_vertices, vertex_count;

// What the game is drawing, per frame, so the window can arrange the screens to suit:
// the top screen's draws (and how many are 3D) and the textures the bottom screen uses
struct SceneInfo {
    int top_draws = 0, top_3d_draws = 0, bottom_draws = 0;
    std::vector<uint64_t> bottom_textures; // signatures
    uint64_t serial = 0;
};
SceneInfo scene_info();
// records every draw of the next top-screen frame into a text file (F12 captures)
void request_draw_log(const std::string &path);
// the same for the next `frames` top frames in a row: <dir>/000_draws.txt, 001_draws.txt, ...
void request_draw_burst(const std::string &dir, int frames);
// adds a line to the draw log being recorded (the renderer's own decisions)
void draw_log_note(const char *line);
// a screen's frame is finished (its display transfer): `height` 400 top, 320 bottom
void scene_frame_done(uint32_t height);

// GX commands from gsp::Gpu (addresses are guest virtual)
void run_command_list(uint32_t addr, uint32_t size);
void display_transfer(uint32_t in, uint32_t out, uint32_t in_dims, uint32_t out_dims, uint32_t flags);
void texture_copy(uint32_t in, uint32_t out, uint32_t size, uint32_t in_line, uint32_t out_line, uint32_t flags);

// Enhanced top screen (see widescreen.cpp): 16:9 instead of 5:3, and an internal resolution
// of 1-4x the native 400x240
void set_widescreen(bool on);
void set_resolution_scale(int scale);
// the size of the top screen's 2D HUD (dialogue boxes etc.), 0.4-1 (GPU renderer)
void set_hud_scale(float scale);
// widescreen: the 3D camera's zoom, 0.6-1 (1: the game's own; less pulls back to show more of the
// scene around the middle as well as at the sides)
void set_camera_zoom(float zoom);
// GPU renderer: draws an in-between frame for games running at 30 fps, so they're shown at 60
void set_frame_interpolation(bool on);
// F10: saving each new texture the game uses, for HD texture packs (see gpu.cpp); returns whether on
bool toggle_texture_dump();
// after a GX MemoryFill of [start, end): clears the matching widescreen shadow too
void wide_memory_filled(uint32_t start, uint32_t end);
// the enhanced version of the frame in an LCD framebuffer, if it came from a shadow target:
// `columns` screen columns of `column_height` pixels
const uint8_t *wide_lcd(uint32_t addr, uint32_t *columns, uint32_t *column_height, uint32_t *stride,
                        uint32_t *format);
// converts LCD framebuffer memory at a host pointer (`height` pixels per column), as read_lcd
void read_lcd_host(const uint8_t *fb, uint32_t stride, uint32_t format, int width, int height, uint8_t *out);

// Reads an LCD framebuffer (column-major, as the screens scan out) into `out` as RGBA bytes,
// row-major `width` x 240. `addr` may be virtual or physical.
void read_lcd(uint32_t addr, uint32_t stride, uint32_t format, int width, uint8_t *out);

} // namespace pica
