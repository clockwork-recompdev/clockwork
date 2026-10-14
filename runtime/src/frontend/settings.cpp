#include "settings.h"

#include "frontend.h"
#include "platform.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>

namespace frontend {

const ControlInfo CONTROLS[CONTROL_COUNT] = {
    {"a", "A", BTN_A},
    {"b", "B", BTN_B},
    {"x", "X", BTN_X},
    {"y", "Y", BTN_Y},
    {"l", "L", BTN_L},
    {"r", "R", BTN_R},
    {"zl", "ZL", BTN_ZL},
    {"zr", "ZR", BTN_ZR},
    {"start", "Start", BTN_START},
    {"select", "Select", BTN_SELECT},
    {"up", "D-Pad Up", BTN_UP},
    {"down", "D-Pad Down", BTN_DOWN},
    {"left", "D-Pad Left", BTN_LEFT},
    {"right", "D-Pad Right", BTN_RIGHT},
    {"circle_up", "Circle Pad Up", 0},
    {"circle_down", "Circle Pad Down", 0},
    {"circle_left", "Circle Pad Left", 0},
    {"circle_right", "Circle Pad Right", 0},
};

void Settings::reset_controls() {
    static constexpr int KEYS[CONTROL_COUNT] = {
        SDL_SCANCODE_K,      SDL_SCANCODE_J,         SDL_SCANCODE_I,    SDL_SCANCODE_U,    SDL_SCANCODE_Q,
        SDL_SCANCODE_E,      SDL_SCANCODE_1,         SDL_SCANCODE_3,    SDL_SCANCODE_RETURN,
        SDL_SCANCODE_BACKSPACE, SDL_SCANCODE_UP,     SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
        SDL_SCANCODE_W,      SDL_SCANCODE_S,         SDL_SCANCODE_A,    SDL_SCANCODE_D,
    };
    // by position, Nintendo layout: east is A, south is B; the left stick is always the circle pad
    static constexpr int PAD[CONTROL_COUNT] = {
        SDL_GAMEPAD_BUTTON_EAST,      SDL_GAMEPAD_BUTTON_SOUTH,     SDL_GAMEPAD_BUTTON_NORTH,
        SDL_GAMEPAD_BUTTON_WEST,      SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
        PAD_LEFT_TRIGGER,             PAD_RIGHT_TRIGGER,            SDL_GAMEPAD_BUTTON_START,
        SDL_GAMEPAD_BUTTON_BACK,      SDL_GAMEPAD_BUTTON_DPAD_UP,   SDL_GAMEPAD_BUTTON_DPAD_DOWN,
        SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, PAD_NONE,
        PAD_NONE,                     PAD_NONE,                     PAD_NONE,
    };
    for (int i = 0; i < CONTROL_COUNT; i++) keys[i] = KEYS[i], pad[i] = PAD[i];
}

std::string key_name(int scancode) {
    if (scancode == SDL_SCANCODE_UNKNOWN) return "-";
    const char *n = SDL_GetScancodeName(SDL_Scancode(scancode));
    return n && *n ? n : "?";
}

std::string pad_name(int code) {
    if (code == PAD_NONE) return "-";
    if (code == PAD_LEFT_TRIGGER) return "Left Trigger";
    if (code == PAD_RIGHT_TRIGGER) return "Right Trigger";
    static const std::map<int, const char *> names = {
        {SDL_GAMEPAD_BUTTON_SOUTH, "South (B/Cross)"}, {SDL_GAMEPAD_BUTTON_EAST, "East (A/Circle)"},
        {SDL_GAMEPAD_BUTTON_WEST, "West (Y/Square)"},  {SDL_GAMEPAD_BUTTON_NORTH, "North (X/Triangle)"},
        {SDL_GAMEPAD_BUTTON_BACK, "Back/Select"},      {SDL_GAMEPAD_BUTTON_START, "Start"},
        {SDL_GAMEPAD_BUTTON_LEFT_STICK, "Left Stick"}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK, "Right Stick"},
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, "Left Bumper"}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, "Right Bumper"},
        {SDL_GAMEPAD_BUTTON_DPAD_UP, "D-Pad Up"},      {SDL_GAMEPAD_BUTTON_DPAD_DOWN, "D-Pad Down"},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT, "D-Pad Left"},  {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, "D-Pad Right"},
    };
    auto it = names.find(code);
    if (it != names.end()) return it->second;
    const char *n = SDL_GetGamepadStringForButton(SDL_GamepadButton(code));
    return n ? n : "?";
}

namespace {

std::filesystem::path file() { return platform::data_dir() / "settings.ini"; }

std::string pad_id(int code) {
    if (code == PAD_NONE) return "";
    if (code == PAD_LEFT_TRIGGER) return "lefttrigger";
    if (code == PAD_RIGHT_TRIGGER) return "righttrigger";
    const char *n = SDL_GetGamepadStringForButton(SDL_GamepadButton(code));
    return n ? n : "";
}

int pad_from_id(const std::string &s) {
    if (s.empty()) return PAD_NONE;
    if (s == "lefttrigger") return PAD_LEFT_TRIGGER;
    if (s == "righttrigger") return PAD_RIGHT_TRIGGER;
    SDL_GamepadButton b = SDL_GetGamepadButtonFromString(s.c_str());
    return b == SDL_GAMEPAD_BUTTON_INVALID ? PAD_NONE : int(b);
}

} // namespace

void Settings::load() {
    std::ifstream f(file());
    std::string line;
    while (std::getline(f, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        bool b = v == "1";
        if (k == "game_path") game_path = v;
        else if (k == "dump_folder") dump_folder = v;
        else if (k.rfind("dump.", 0) == 0) dumps[std::strtoull(k.c_str() + 5, nullptr, 16)] = v;
        else if (k.rfind("layout.", 0) == 0) layouts[k.substr(7)] = v;
        else if (k == "show_launcher") show_launcher = b;
        else if (k == "single_screen") single_screen = b;
        else if (k == "swap_screens") swap_screens = b;
        else if (k == "picture_in_picture") picture_in_picture = b;
        else if (k == "smart_layout") smart_layout = b;
        else if (k == "widescreen") widescreen = b;
        else if (k == "fullscreen") fullscreen = b;
        else if (k == "vsync") vsync = b;
        else if (k == "filter") filter = std::atoi(v.c_str());
        else if (k == "resolution") resolution = std::atoi(v.c_str());
        else if (k == "hud_size") hud_size = std::atoi(v.c_str());
        else if (k == "gpu_renderer") gpu_renderer = b;
        else if (k == "prefer_integrated_gpu") prefer_integrated_gpu = b;
        else if (k == "volume") volume = std::atoi(v.c_str());
        else if (k == "audio_device") audio_device = v;
        else if (k == "mute_in_background") mute_in_background = b;
        else if (k == "mono") mono = b;
        else if (k == "relay") relay = v;
        else if (k == "player_name") player_name = v;
        else if (k == "room") room = v;
        else if (k == "room_password") room_password = v;
        else if (k == "audio_latency") audio_latency = std::clamp(std::atoi(v.c_str()), 0, 2);
        else
            for (int i = 0; i < CONTROL_COUNT; i++) {
                if (k == std::string("key_") + CONTROLS[i].id) keys[i] = SDL_GetScancodeFromName(v.c_str());
                if (k == std::string("pad_") + CONTROLS[i].id) pad[i] = pad_from_id(v);
            }
    }
}

void Settings::save() const {
    std::ofstream f(file());
    f << "game_path=" << game_path << "\n"
      << "dump_folder=" << dump_folder << "\n";
    for (auto &[id, path] : dumps) {
        char key[32];
        std::snprintf(key, sizeof key, "dump.%016llx=", (unsigned long long)id);
        f << key << path << "\n";
    }
    f << "show_launcher=" << show_launcher << "\n"
      << "single_screen=" << single_screen << "\n"
      << "swap_screens=" << swap_screens << "\n"
      << "picture_in_picture=" << picture_in_picture << "\n"
      << "smart_layout=" << smart_layout << "\n"
      << "widescreen=" << widescreen << "\n"
      << "fullscreen=" << fullscreen << "\n"
      << "vsync=" << vsync << "\n"
      << "filter=" << filter << "\n"
      << "resolution=" << resolution << "\n"
      << "hud_size=" << hud_size << "\n"
      << "gpu_renderer=" << gpu_renderer << "\n"
      << "prefer_integrated_gpu=" << prefer_integrated_gpu << "\n"
      << "volume=" << volume << "\n"
      << "audio_device=" << audio_device << "\n"
      << "mute_in_background=" << mute_in_background << "\n"
      << "mono=" << mono << "\n"
      << "audio_latency=" << audio_latency << "\n"
      << "relay=" << relay << "\n"
      << "player_name=" << player_name << "\n"
      << "room=" << room << "\n"
      << "room_password=" << room_password << "\n";
    for (auto &[key, layout] : layouts) f << "layout." << key << "=" << layout << "\n";
    for (int i = 0; i < CONTROL_COUNT; i++) {
        f << "key_" << CONTROLS[i].id << "=" << (keys[i] ? SDL_GetScancodeName(SDL_Scancode(keys[i])) : "") << "\n";
        f << "pad_" << CONTROLS[i].id << "=" << pad_id(pad[i]) << "\n";
    }
}

Settings &settings() {
    static Settings s;
    return s;
}

} // namespace frontend
