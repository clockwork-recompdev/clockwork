#pragma once
// AI texture upscaling: each texture the game uses (and no HD pack replaces) is upscaled in the
// background by Real-ESRGAN (realesrgan-ncnn-vulkan, a separate program the launcher downloads to
// tools/realesrgan/), once, into upscaled/<game>/<hash>_x<factor>.png in the data folder. The GPU
// renderer draws those like an HD texture pack (see gpu.cpp), packs winning.
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace upscale {

// where the launcher installs Real-ESRGAN, and its program in there (empty if not installed)
std::filesystem::path tool_folder();
std::filesystem::path tool();
// the game's upscaled textures
std::filesystem::path cache_folder();

// on or off for this session, upscaling by `factor` (2 or 4; large textures get 2)
void configure(bool on, int factor);
bool on();

// a texture the game uses, decoded (rows bottom-up, RGBA): upscaled unless it was already, or is
// queued, or failed
void request(uint64_t hash, uint32_t width, uint32_t height, const uint8_t *rgba);

// textures upscaled since the last call: (hash, PNG)
std::vector<std::pair<uint64_t, std::string>> take_ready();

} // namespace upscale
