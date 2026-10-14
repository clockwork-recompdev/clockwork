#pragma once
// Host side: the SDL3 window showing both screens, and keyboard/controller/mouse input.
#include <cstdint>

struct SDL_GPUTexture;

namespace frontend {

constexpr int TOP_WIDTH = 400, BOTTOM_WIDTH = 320, SCREEN_HEIGHT = 240;

// 3DS button bits as HID reports them
enum Button : uint32_t {
    BTN_A = 1u << 0, BTN_B = 1u << 1, BTN_SELECT = 1u << 2, BTN_START = 1u << 3,
    BTN_RIGHT = 1u << 4, BTN_LEFT = 1u << 5, BTN_UP = 1u << 6, BTN_DOWN = 1u << 7,
    BTN_R = 1u << 8, BTN_L = 1u << 9, BTN_X = 1u << 10, BTN_Y = 1u << 11,
    BTN_ZL = 1u << 14, BTN_ZR = 1u << 15,
};

struct Input {
    uint32_t buttons = 0;
    int16_t circle_x = 0, circle_y = 0; // about +-156 at full tilt
    bool touching = false;
    uint16_t touch_x = 0, touch_y = 0; // bottom-screen pixels
};

// latest input; callable from any thread
Input input();

// hands a finished frame of one screen (0 top, 1 bottom) to the window: RGBA bytes, row-major,
// `width` x `height` (larger than 400x240 / 320x240 when enhanced). Any thread.
void present(int screen, const uint8_t *rgba, int width, int height);

// the same, for a frame the GPU renderer produced (upright RGBA8 texture on the shared device)
void present_gpu(int screen, SDL_GPUTexture *texture, int width, int height);

// queues `frames` stereo s16 samples at the DSP rate (32728 Hz) for playback; any thread
void play_audio(const int16_t *samples, int frames);

// Shows the launcher (unless turned off, or `play` is set), then runs `start(dump path)` on its own
// thread while this (main) thread runs the window; never returns. `cli_dump` (may be null)
// overrides the saved dump path. A dump whose game is another executable is handed to it (see
// handoff.h). With YW2_HEADLESS set there is no window and `start` runs here.
[[noreturn]] void run(void (*start)(const char *dump), const char *cli_dump, bool force_launcher, bool play);

} // namespace frontend
