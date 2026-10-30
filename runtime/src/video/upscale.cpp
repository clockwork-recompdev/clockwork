#include "upscale.h"

#include "log.h"
#include "mods.h"
#include "platform.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_set>

namespace fs = std::filesystem;

namespace upscale {
namespace {

#if defined(_WIN32)
const char *const PROGRAM = "realesrgan-ncnn-vulkan.exe";
#else
const char *const PROGRAM = "realesrgan-ncnn-vulkan";
#endif
constexpr uint32_t MAX_SIDE = 2048;           // an upscaled texture's longest side: VRAM goes fast past it
constexpr size_t MAX_QUEUED = 256u << 20;      // bytes of textures waiting; past it, later ones wait for a revisit

struct Job {
    uint64_t hash;
    uint32_t w, h;
    int factor;
    std::vector<uint8_t> rgba; // bottom-up
};

std::atomic<bool> enabled{false};
int want_factor = 2;
std::mutex lock;
std::condition_variable wake;
std::vector<Job> jobs;
size_t queued_bytes = 0;
std::unordered_set<uint64_t> seen; // queued, done or failed this session
std::vector<std::pair<uint64_t, std::string>> ready;
bool worker_started = false;
int failures = 0;

// runs Real-ESRGAN on every PNG in `in`, writing them to `out`; true if it exited cleanly
bool run_tool(const fs::path &program, const fs::path &in, const fs::path &out, int factor) {
    std::string model = "realesr-animevideov3-x" + std::to_string(factor), scale = std::to_string(factor);
    std::string models = (program.parent_path() / "models").string(), ins = in.string(), outs = out.string(),
                prog = program.string();
    const char *args[] = {prog.c_str(), "-i", ins.c_str(), "-o", outs.c_str(), "-m", models.c_str(), "-n",
                          model.c_str(), "-s", scale.c_str(), "-f", "png", nullptr};
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, args);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_Process *p = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    if (!p) {
        LOG("upscale: couldn't start %s: %s", prog.c_str(), SDL_GetError());
        return false;
    }
    int code = -1;
    SDL_WaitProcess(p, true, &code);
    SDL_DestroyProcess(p);
    if (code != 0) LOG("upscale: Real-ESRGAN exited with %d", code);
    return code == 0;
}

// one batch of one factor: write the inputs, upscale them, move the results into the cache
void upscale_batch(const std::vector<Job> &batch, int factor) {
    std::error_code ec;
    fs::path program = tool();
    if (program.empty()) return;
    fs::path work = cache_folder() / ".work", in = work / "in", out = work / "out";
    fs::remove_all(work, ec);
    fs::create_directories(in, ec), fs::create_directories(out, ec);
    char name[32];
    for (const Job &j : batch) {
        size_t row = size_t(j.w) * 4;
        std::vector<uint8_t> upright(j.rgba.size());
        for (uint32_t y = 0; y < j.h; y++) std::copy_n(&j.rgba[(j.h - 1 - y) * row], row, &upright[y * row]);
        SDL_Surface *s = SDL_CreateSurfaceFrom(int(j.w), int(j.h), SDL_PIXELFORMAT_RGBA32, upright.data(), int(row));
        std::snprintf(name, sizeof name, "%016llx.png", (unsigned long long)j.hash);
        if (s) SDL_SavePNG(s, (in / name).string().c_str()), SDL_DestroySurface(s);
    }
    auto t0 = std::chrono::steady_clock::now();
    bool ok = run_tool(program, in, out, factor);
    int done = 0;
    for (const Job &j : batch) {
        std::snprintf(name, sizeof name, "%016llx.png", (unsigned long long)j.hash);
        char final_name[40];
        std::snprintf(final_name, sizeof final_name, "%016llx_x%d.png", (unsigned long long)j.hash, factor);
        if (!fs::exists(out / name, ec)) continue;
        fs::rename(out / name, cache_folder() / final_name, ec);
        if (ec) continue;
        std::lock_guard lk(lock);
        ready.emplace_back(j.hash, (cache_folder() / final_name).string());
        done++;
    }
    fs::remove_all(work, ec);
    double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    LOG("upscale: %d of %zu texture%s at %dx in %.1f s", done, batch.size(), batch.size() == 1 ? "" : "s", factor, s);
    std::lock_guard lk(lock);
    if (!ok && done == 0 && ++failures >= 2) {
        LOG("upscale: Real-ESRGAN keeps failing; AI upscaling is off until the game restarts");
        enabled = false;
    } else if (done) {
        failures = 0;
    }
}

void worker() {
    for (;;) {
        std::vector<Job> batch;
        {
            std::unique_lock lk(lock);
            wake.wait(lk, [] { return !jobs.empty(); });
        }
        // gather what an area brings in at once: the program loads its model once per batch
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        {
            std::lock_guard lk(lock);
            batch = std::move(jobs);
            jobs.clear(), queued_bytes = 0;
        }
        std::map<int, std::vector<Job>> by_factor;
        for (Job &j : batch) by_factor[j.factor].push_back(std::move(j));
        for (auto &[factor, list] : by_factor)
            if (enabled) upscale_batch(list, factor);
    }
}

} // namespace

fs::path tool_folder() { return platform::data_dir() / "tools" / "realesrgan"; }

fs::path tool() {
    static fs::path found;
    std::error_code ec;
    if (!found.empty() && fs::exists(found, ec)) return found;
    found.clear();
    for (auto it = fs::recursive_directory_iterator(tool_folder(), ec); it != fs::recursive_directory_iterator();
         it.increment(ec))
        if (it->path().filename() == PROGRAM) return found = it->path();
    return {};
}

fs::path cache_folder() {
    return platform::data_dir() / "upscaled" / (mods::game().empty() ? "game" : mods::game());
}

void configure(bool on, int factor) {
    want_factor = factor >= 4 ? 4 : 2;
    if (!on) return void(enabled = false);
    if (tool().empty()) {
        LOG("upscale: AI upscaling is on but Real-ESRGAN isn't installed (Graphics settings in the launcher)");
        return void(enabled = false);
    }
    std::error_code ec;
    fs::create_directories(cache_folder(), ec);
    enabled = true;
    LOG("upscale: AI upscaling at %dx with %s", want_factor, tool().string().c_str());
}

bool on() { return enabled; }

void request(uint64_t hash, uint32_t width, uint32_t height, const uint8_t *rgba) {
    if (!enabled || width < 8 || height < 8) return;
    int factor = want_factor;
    while (factor > 2 && std::max(width, height) * uint32_t(factor) > MAX_SIDE) factor /= 2;
    if (std::max(width, height) * uint32_t(factor) > 2 * MAX_SIDE) return;
    size_t bytes = size_t(width) * height * 4;
    std::lock_guard lk(lock);
    if (queued_bytes + bytes > MAX_QUEUED || !seen.insert(hash).second) return;
    jobs.push_back({hash, width, height, factor, std::vector<uint8_t>(rgba, rgba + bytes)});
    queued_bytes += bytes;
    if (!worker_started) std::thread(worker).detach(), worker_started = true;
    wake.notify_one();
}

std::vector<std::pair<uint64_t, std::string>> take_ready() {
    std::vector<std::pair<uint64_t, std::string>> out;
    std::lock_guard lk(lock);
    out.swap(ready);
    return out;
}

} // namespace upscale
