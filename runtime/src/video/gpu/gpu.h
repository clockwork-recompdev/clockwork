#pragma once
// The GPU renderer: PICA200 rasterization and per-pixel work on the host GPU through SDL_GPU.
// Command lists, vertex shading and primitive assembly stay on the CPU (pica.cpp); triangles,
// clears and display transfers come here when the GPU renderer is active.
#include "video/internal.h"

#include <cstdint>

struct SDL_GPUDevice;
struct SDL_GPUTexture;

namespace pica::gpu {

// starts the GPU renderer on `device`; false if it can't (the software renderer is used)
bool init(SDL_GPUDevice *device);
bool active();
int take_readback_count(); // readbacks since the last call

// the draw pipeline, mirroring the software rasterizer's entry points
void begin_draw();
void triangle(const Vertex &v0, const Vertex &v1, const Vertex &v2);
void end_draw();
void flush();

// GX operations: true when the GPU handled them (the target lives on the GPU)
bool memory_fill(uint32_t start, uint32_t end, uint32_t value, uint32_t control);
bool display_transfer(uint32_t in, uint32_t out, uint32_t in_w, uint32_t in_h, uint32_t out_w, uint32_t out_h,
                      uint32_t flags);
// the CPU is about to read [addr, addr + size): GPU-drawn images there are copied back first
void before_cpu_read(uint32_t addr, uint32_t size);
// the CPU wrote to [addr, addr + size): GPU copies of that memory must be reloaded
void invalidate(uint32_t addr, uint32_t size);

// the latest GPU-rendered image in an LCD framebuffer (upright, screen orientation), if any
SDL_GPUTexture *lcd_frame(uint32_t addr, int *width, int *height);

// where a command list's time went, for YW2_FPS's slow-list report (counts and nanoseconds)
struct Costs {
    int pipelines = 0, textures = 0, downloads = 0;
    int64_t pipeline_ns = 0, texture_ns = 0, download_ns = 0;
};
extern Costs costs;

} // namespace pica::gpu
