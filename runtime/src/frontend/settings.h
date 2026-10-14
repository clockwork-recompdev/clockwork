#pragma once
// User settings (settings.ini in the user data folder), edited in the launcher and by the
// in-game hotkeys.
#include <cstdint>
#include <map>
#include <string>

namespace frontend {

// what can be bound: the 3DS buttons and the four circle-pad directions
enum Control : int {
    C_A, C_B, C_X, C_Y, C_L, C_R, C_ZL, C_ZR, C_START, C_SELECT, C_UP, C_DOWN, C_LEFT, C_RIGHT,
    C_CIRCLE_UP, C_CIRCLE_DOWN, C_CIRCLE_LEFT, C_CIRCLE_RIGHT, CONTROL_COUNT
};
struct ControlInfo {
    const char *id, *label;
    uint32_t button; // HID bit; 0 for circle-pad directions
};
extern const ControlInfo CONTROLS[CONTROL_COUNT];

// controller binding codes: SDL_GamepadButton values, or these
constexpr int PAD_NONE = -1, PAD_LEFT_TRIGGER = 100, PAD_RIGHT_TRIGGER = 101;

enum Filter : int { FILTER_SHARP = 0, FILTER_SMOOTH = 1, FILTER_NEAREST = 2 };

struct Settings {
    std::string game_path; // the dump played last (or next)
    std::string dump_folder; // where the player's dumps are; the launcher looks there for more
    std::map<uint64_t, std::string> dumps; // the dumps found, by program id
    bool show_launcher = true;

    // display
    bool single_screen = false, swap_screens = false, picture_in_picture = true;
    bool smart_layout = true; // one screen: arrange by what the game shows (menus, map, battles)
    bool widescreen = false, fullscreen = false, vsync = true;
    int filter = FILTER_SHARP;
    int resolution = 1; // internal resolution of the top screen's 3D, times native (1-4)
    int hud_size = 80;  // percent: the top screen's dialogue boxes and other 2D HUD
    bool gpu_renderer = true; // draw on the GPU (SDL_GPU); off: the software renderer
    bool prefer_integrated_gpu = false; // on laptops with two GPUs: the power-saving one
    // one-screen layouts chosen with F5: "<game>.<kind of scene>" -> layout (see sdl.cpp)
    std::map<std::string, std::string> layouts;

    // audio
    int volume = 100;            // percent
    std::string audio_device;    // a playback device by name; empty: the system's default
    bool mute_in_background = false; // silent while the window isn't focused
    bool mono = false;           // left and right mixed together
    int audio_latency = 1;       // buffering: 0 low, 1 normal, 2 high (fewer crackles on a busy PC)

    // local wireless over the internet (the Rooms tab; see net/air.h)
    std::string relay;        // "host[:port]" of a relay; empty: this PC only (two windows)
    std::string player_name;  // shown to the others in the room
    std::string room, room_password;

    int keys[CONTROL_COUNT]; // SDL_Scancode
    int pad[CONTROL_COUNT];  // controller binding codes

    Settings() { reset_controls(); }
    void reset_controls();
    void load();
    void save() const;
};
Settings &settings();

// display names for bindings
std::string key_name(int scancode);
std::string pad_name(int code);

} // namespace frontend
