// The launcher: shown in the game window before the game starts. Lists the games, picks the
// dump, and edits display, controls and audio settings; Play saves them and starts the game.
#include "launcher.h"
#include "menu_music.h"

#include "autobuild.h"
#include "dump.h"
#include "gamebanana.h"
#include "mods.h"
#include "games.h"
#include "handoff.h"
#include "log.h"
#include "net/relay.h"
#include "platform.h"
#include "saves.h"
#include "settings.h"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#include <algorithm>
#include <cfloat>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <memory>
#include <mutex>
#include <thread>
#include <set>
#include <vector>

namespace frontend {
namespace {

// the file dialog reports back on another thread
std::mutex picked_lock;
std::string picked;

void SDLCALL on_file_picked(void *, const char *const *files, int) {
    if (!files || !files[0]) return;
    std::lock_guard lk(picked_lock);
    picked = files[0];
}

// a save folder to import (picked_lock)
std::string picked_save;

void SDLCALL on_save_picked(void *, const char *const *files, int) {
    if (!files || !files[0]) return;
    std::lock_guard lk(picked_lock);
    picked_save = files[0];
}

bool dump_exists(const std::string &path) {
    std::error_code ec;
    return !path.empty() && std::filesystem::is_regular_file(path, ec);
}

// remembers `path` if it's a dump Clockwork recognizes, or an update of one; true if so
bool remember(Settings &s, const std::string &path) {
    auto id = peek_program_id(path);
    if (!id) return false;
    if (auto game = update_of(*id); game && find_dump(*game)) return s.updates[*game] = path, true;
    if (!find_dump(*id)) return false;
    s.dumps[*id] = path;
    return true;
}

// looks in `folder` (not below it) for dumps Clockwork recognizes and their updates, and remembers them
void find_dumps(Settings &s, const std::filesystem::path &folder) {
    std::error_code ec;
    for (auto &e : std::filesystem::directory_iterator(folder, ec)) {
        std::string ext = e.path().extension().string();
        for (char &c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
        if ((ext != ".3ds" && ext != ".cci" && ext != ".cxi" && ext != ".cia" && ext != ".ncch") || !e.is_regular_file(ec))
            continue;
        remember(s, e.path().string());
    }
}

// forgets dumps that are gone, and finds new ones next to the ones known
void refresh_dumps(Settings &s) {
    for (auto it = s.dumps.begin(); it != s.dumps.end();) it = dump_exists(it->second) ? std::next(it) : s.dumps.erase(it);
    for (auto it = s.updates.begin(); it != s.updates.end();) it = dump_exists(it->second) ? std::next(it) : s.updates.erase(it);
    std::set<std::filesystem::path> folders;
    if (!s.dump_folder.empty()) folders.insert(s.dump_folder);
    if (dump_exists(s.game_path)) folders.insert(std::filesystem::path(s.game_path).parent_path());
    for (auto &[id, path] : s.dumps) folders.insert(std::filesystem::path(path).parent_path());
    for (auto &f : folders) find_dumps(s, f);
}

// the dump found for a game, preferring `version` (any, if not found); empty if none
std::string dump_for(const Settings &s, const char *game) {
    for (const KnownDump &d : KNOWN_DUMPS)
        if (std::strcmp(d.game, game) == 0)
            if (auto it = s.dumps.find(d.program_id); it != s.dumps.end()) return it->second;
    return "";
}

// a binding being captured: the next key or controller input goes to it
struct Capture {
    int control = -1;
    bool pad = false;
};

// The look of Yo-kai Watch Blasters' version select (Red Cat Corps / Moon Rabbit Crew): deep blue
// scanlines with a light band across, angled panels, cards with thick bevelled borders, and bars
// with pointed ends ("VIP Link") lettered in heavy outlined italics. Nothing from the game is used:
// it's all drawn here.
const ImVec4 NAVY = {0.07f, 0.13f, 0.47f, 1}, NAVY_DARK = {0.03f, 0.06f, 0.27f, 1}, ROYAL = {0.14f, 0.30f, 0.80f, 1};
const ImVec4 CYAN = {0.38f, 0.82f, 1.0f, 1}, EDGE = {0.56f, 0.76f, 1.0f, 1};
const ImVec4 GOLD = {1.0f, 0.80f, 0.10f, 1}, GOLD_LIGHT = {1.0f, 0.94f, 0.50f, 1}, ORANGE = {1.0f, 0.56f, 0.06f, 1};
const ImVec4 LILAC = {0.80f, 0.74f, 0.99f, 1}, LILAC_DARK = {0.52f, 0.43f, 0.90f, 1}, RED = {0.88f, 0.13f, 0.13f, 1};
const ImVec4 SLATE = {0.55f, 0.60f, 0.76f, 1};
// the names the rest of the launcher uses
const ImVec4 BLUE = CYAN, BLUE_DARK = NAVY_DARK, BLUE_LIGHT = EDGE;
const ImVec4 BACKGROUND = NAVY_DARK, PANEL = {0.05f, 0.10f, 0.36f, 0.88f}, PANEL_HOVER = {0.10f, 0.20f, 0.56f, 0.95f};
const ImVec4 TEXT = {1, 1, 1, 1}, TEXT_DIM = {0.70f, 0.80f, 1.0f, 1}, BORDER = {0.56f, 0.76f, 1.0f, 0.55f};
const ImVec4 WHITE = {1, 1, 1, 1}, WARN = {1.0f, 0.70f, 0.25f, 1};

ImVec4 mix(ImVec4 a, ImVec4 b, float t) {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t};
}
ImVec4 alpha(ImVec4 c, float a) { return {c.x, c.y, c.z, a}; }

ImU32 col(ImVec4 c) { return ImGui::GetColorU32(c); }

// the heavy italic for titles and buttons (a system font: see load_fonts); null: the body font
ImFont *display_font = nullptr;

// fonts from the system: a bold one for text and a black italic one for titles and buttons, the
// nearest to the games' lettering that every PC has. ImGui's own font if none is found
void load_fonts(ImGuiIO &io) {
    static const char *BODY[] = {
#if defined(_WIN32)
        "C:\\Windows\\Fonts\\seguisb.ttf", "C:\\Windows\\Fonts\\segoeuib.ttf", "C:\\Windows\\Fonts\\arialbd.ttf",
#elif defined(__APPLE__)
        "/System/Library/Fonts/Supplemental/Arial Rounded Bold.ttf", "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
#else
        "/usr/share/fonts/noto/NotoSans-Bold.ttf", "/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf",
        "/usr/share/fonts/google-noto/NotoSans-Bold.ttf", "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf",
#endif
    };
    static const char *DISPLAY[] = {
#if defined(_WIN32)
        "C:\\Windows\\Fonts\\seguibli.ttf", "C:\\Windows\\Fonts\\seguibl.ttf", "C:\\Windows\\Fonts\\ariblk.ttf",
#elif defined(__APPLE__)
        "/System/Library/Fonts/Supplemental/Arial Rounded Bold.ttf", "/System/Library/Fonts/Supplemental/Arial Black.ttf",
#else
        "/usr/share/fonts/noto/NotoSans-BlackItalic.ttf", "/usr/share/fonts/truetype/noto/NotoSans-BlackItalic.ttf",
        "/usr/share/fonts/google-noto/NotoSans-BlackItalic.ttf", "/usr/share/fonts/noto/NotoSans-Black.ttf",
        "/usr/share/fonts/TTF/DejaVuSans-BoldOblique.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans-BoldOblique.ttf",
#endif
    };
    auto first = [](const char *const *list, size_t n) -> const char * {
        std::error_code ec;
        for (size_t i = 0; i < n; i++)
            if (std::filesystem::is_regular_file(list[i], ec)) return list[i];
        return nullptr;
    };
    if (const char *body = first(BODY, std::size(BODY))) io.FontDefault = io.Fonts->AddFontFromFileTTF(body, 15.0f);
    if (!io.FontDefault) io.FontDefault = io.Fonts->AddFontDefault();
    if (const char *disp = first(DISPLAY, std::size(DISPLAY))) display_font = io.Fonts->AddFontFromFileTTF(disp, 15.0f);
    if (!display_font) display_font = io.FontDefault;
}

void apply_theme(ImGuiStyle &style) {
    ImGui::StyleColorsDark(&style);
    style.WindowRounding = 0, style.ChildRounding = 10, style.FrameRounding = 10, style.PopupRounding = 10;
    style.GrabRounding = 10, style.TabRounding = 10, style.ScrollbarRounding = 10;
    style.WindowPadding = {16, 12}, style.FramePadding = {10, 6}, style.ItemSpacing = {10, 7};
    style.WindowBorderSize = 0, style.FrameBorderSize = 1.5f, style.PopupBorderSize = 2, style.SeparatorTextBorderSize = 2;
    style.ScrollbarSize = 12;
    ImVec4 *c = style.Colors;
    c[ImGuiCol_Text] = TEXT, c[ImGuiCol_TextDisabled] = TEXT_DIM;
    c[ImGuiCol_WindowBg] = {0, 0, 0, 0}, c[ImGuiCol_ChildBg] = {0, 0, 0, 0}, c[ImGuiCol_PopupBg] = alpha(NAVY_DARK, 0.97f);
    c[ImGuiCol_Border] = BORDER, c[ImGuiCol_Separator] = BORDER;
    c[ImGuiCol_FrameBg] = PANEL, c[ImGuiCol_FrameBgHovered] = PANEL_HOVER, c[ImGuiCol_FrameBgActive] = mix(PANEL_HOVER, ROYAL, 0.5f);
    c[ImGuiCol_Button] = LILAC_DARK, c[ImGuiCol_ButtonHovered] = mix(LILAC_DARK, LILAC, 0.45f);
    c[ImGuiCol_ButtonActive] = mix(LILAC_DARK, NAVY, 0.3f);
    c[ImGuiCol_Header] = alpha(CYAN, 0.30f), c[ImGuiCol_HeaderHovered] = alpha(CYAN, 0.42f), c[ImGuiCol_HeaderActive] = alpha(CYAN, 0.55f);
    c[ImGuiCol_CheckMark] = GOLD, c[ImGuiCol_SliderGrab] = GOLD, c[ImGuiCol_SliderGrabActive] = ORANGE;
    c[ImGuiCol_TableHeaderBg] = alpha(ROYAL, 0.8f), c[ImGuiCol_TableRowBg] = alpha(NAVY_DARK, 0.55f);
    c[ImGuiCol_TableRowBgAlt] = alpha(NAVY, 0.55f), c[ImGuiCol_TableBorderLight] = BORDER;
    c[ImGuiCol_ScrollbarBg] = {0, 0, 0, 0}, c[ImGuiCol_ScrollbarGrab] = alpha(EDGE, 0.5f);
    c[ImGuiCol_ScrollbarGrabHovered] = alpha(EDGE, 0.7f), c[ImGuiCol_ScrollbarGrabActive] = EDGE;
    c[ImGuiCol_NavCursor] = GOLD, c[ImGuiCol_TextSelectedBg] = alpha(CYAN, 0.35f);
    c[ImGuiCol_ModalWindowDimBg] = alpha(NAVY_DARK, 0.6f);
}

// text with a thick outline and a drop shadow, like the games' lettering
void outlined_text(ImDrawList *dl, float size, ImVec2 p, ImU32 fill, ImU32 outline, const char *text, ImFont *font = nullptr) {
    float o = std::max(1.0f, size * 0.085f);
    dl->AddText(font, size, {p.x + o * 0.6f, p.y + o * 1.6f}, outline, text); // shadow
    constexpr int STEPS = 12;
    for (int i = 0; i < STEPS; i++) {
        float a = 6.2831853f * float(i) / STEPS;
        dl->AddText(font, size, {p.x + std::cos(a) * o, p.y + std::sin(a) * o}, outline, text);
    }
    dl->AddText(font, size, p, fill, text);
}

ImVec2 display_size(float size, const char *text) {
    ImFont *f = display_font ? display_font : ImGui::GetFont();
    return f->CalcTextSizeA(size, FLT_MAX, 0, text);
}

// the backdrop: deep blue scanlines, a light band across, and the top screen's orange sunburst
// showing at both edges
void draw_backdrop(ImDrawList *dl, ImVec2 a, ImVec2 b, float unit) {
    dl->AddRectFilledMultiColor(a, b, col(NAVY), col(NAVY), col(NAVY_DARK), col(NAVY_DARK));
    float h = b.y - a.y, band = a.y + h * 0.58f, bh = h * 0.16f;
    dl->AddRectFilledMultiColor({a.x, band - bh}, {b.x, band}, col(alpha(CYAN, 0)), col(alpha(CYAN, 0)),
                                col(alpha(CYAN, 0.30f)), col(alpha(CYAN, 0.30f)));
    dl->AddRectFilledMultiColor({a.x, band}, {b.x, band + bh}, col(alpha(CYAN, 0.30f)), col(alpha(CYAN, 0.30f)),
                                col(alpha(CYAN, 0)), col(alpha(CYAN, 0)));
    for (float y = a.y; y < b.y; y += unit * 0.42f) // scanlines
        dl->AddRectFilled({a.x, y}, {b.x, y + unit * 0.14f}, col(alpha(ROYAL, 0.22f)));
    float side = unit * 1.6f;
    for (int s = 0; s < 2; s++) { // the sunburst strips
        float x0 = s ? b.x - side : a.x, x1 = s ? b.x : a.x + side;
        dl->AddRectFilledMultiColor({x0, a.y}, {x1, b.y}, col(GOLD_LIGHT), col(GOLD_LIGHT), col(ORANGE), col(ORANGE));
        dl->PushClipRect({x0, a.y}, {x1, b.y}, true);
        for (float y = a.y - side; y < b.y + side; y += unit * 0.9f)
            dl->AddQuadFilled({x0, y}, {x1, y - side * 0.6f}, {x1, y - side * 0.6f + unit * 0.35f}, {x0, y + unit * 0.35f},
                              col(alpha(WHITE, 0.28f)));
        dl->PopClipRect();
        float edge = s ? x0 : x1; // a dark line where the blue starts
        dl->AddLine({edge, a.y}, {edge, b.y}, col(NAVY_DARK), unit * 0.12f);
    }
}

// a panel with its corners cut off, filled and outlined
void chamfer_panel(ImDrawList *dl, ImVec2 a, ImVec2 b, float cut, ImU32 fill, ImU32 edge, float thick) {
    ImVec2 pts[8] = {{a.x + cut, a.y}, {b.x - cut, a.y}, {b.x, a.y + cut}, {b.x, b.y - cut},
                     {b.x - cut, b.y}, {a.x + cut, b.y}, {a.x, b.y - cut}, {a.x, a.y + cut}};
    dl->AddConvexPolyFilled(pts, 8, fill);
    dl->AddPolyline(pts, 8, edge, ImDrawFlags_Closed, thick);
}

// The games' buttons: a bar with pointed ends, a white rim, two tones and outlined lettering
struct Palette {
    ImVec4 light, dark, rim, outline;
};
const Palette LILAC_BAR{LILAC, LILAC_DARK, WHITE, {0.22f, 0.14f, 0.50f, 1}};
const Palette GOLD_BAR{GOLD_LIGHT, ORANGE, WHITE, {0.50f, 0.20f, 0.02f, 1}};
const Palette CYAN_BAR{{0.62f, 0.92f, 1, 1}, {0.12f, 0.58f, 0.95f, 1}, WHITE, {0.03f, 0.18f, 0.48f, 1}};
const Palette NAVY_BAR{{0.24f, 0.40f, 0.88f, 1}, {0.09f, 0.18f, 0.56f, 1}, EDGE, {0.03f, 0.06f, 0.27f, 1}};
const Palette OFF_BAR{{0.62f, 0.66f, 0.80f, 1}, {0.38f, 0.42f, 0.58f, 1}, {0.80f, 0.84f, 0.92f, 1}, {0.15f, 0.18f, 0.30f, 1}};

void bar_points(ImVec2 a, ImVec2 b, float inset, ImVec2 *pts) {
    float h = b.y - a.y, k = h * 0.38f, my = (a.y + b.y) / 2, ki = inset * 0.5f; // the points' depth
    pts[0] = {a.x + k + ki, a.y + inset}, pts[1] = {b.x - k - ki, a.y + inset}, pts[2] = {b.x - inset, my};
    pts[3] = {b.x - k - ki, b.y - inset}, pts[4] = {a.x + k + ki, b.y - inset}, pts[5] = {a.x + inset, my};
}

void draw_bar(ImDrawList *dl, ImVec2 a, ImVec2 b, Palette p, bool hot) {
    if (hot) p.light = mix(p.light, WHITE, 0.25f), p.dark = mix(p.dark, WHITE, 0.18f);
    float h = b.y - a.y, rim = std::max(2.0f, h * 0.09f);
    ImVec2 o[6], in[6];
    bar_points({a.x, a.y + h * 0.06f}, {b.x, b.y + h * 0.06f}, 0, o); // shadow
    dl->AddConvexPolyFilled(o, 6, col(alpha(NAVY_DARK, 0.55f)));
    bar_points(a, b, 0, o);
    dl->AddConvexPolyFilled(o, 6, col(p.rim));
    dl->AddPolyline(o, 6, col(alpha(p.outline, 0.8f)), ImDrawFlags_Closed, std::max(1.0f, rim * 0.45f));
    bar_points(a, b, rim, in);
    dl->AddConvexPolyFilled(in, 6, col(p.dark));
    // the upper half lighter, like the games' glossy buttons
    ImVec2 upper[4] = {in[0], in[1], in[2], in[5]};
    dl->AddConvexPolyFilled(upper, 4, col(p.light));
    dl->AddLine({in[0].x + rim, in[0].y + rim * 0.8f}, {in[1].x - rim, in[1].y + rim * 0.8f}, col(alpha(WHITE, 0.55f)), rim * 0.5f);
}

// a button shaped like the games': `label` up to "##" is shown, outlined
bool bar_button(const char *label, ImVec2 size, const Palette &p, bool enabled = true, bool selected = false) {
    ImVec2 at = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton(label, size) && enabled;
    bool hovered = enabled && ImGui::IsItemHovered(), held = enabled && ImGui::IsItemActive();
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float press = held ? size.y * 0.05f : 0;
    ImVec2 a{at.x, at.y + press}, b{at.x + size.x, at.y + size.y + press};
    const Palette &use = enabled ? p : OFF_BAR;
    if (selected) draw_bar(dl, {a.x - 3, a.y - 3}, {b.x + 3, b.y + 3}, {GOLD, GOLD, GOLD, GOLD}, false);
    draw_bar(dl, a, b, use, hovered);
    const char *hidden = std::strstr(label, "##");
    std::string text = hidden ? std::string(label, hidden) : std::string(label);
    float fs = size.y * 0.46f;
    ImVec2 ts = display_size(fs, text.c_str());
    float room = size.x - size.y * 0.9f; // inside the points
    if (ts.x > room && ts.x > 0) fs *= room / ts.x, ts = display_size(fs, text.c_str());
    outlined_text(dl, fs, {a.x + (size.x - ts.x) / 2, a.y + (size.y - ts.y) / 2 - fs * 0.04f}, col(WHITE),
                  col(enabled ? use.outline : OFF_BAR.outline), text.c_str(), display_font);
    return clicked;
}

// a section's title: a cyan chip with the outlined title, and a line on along the panel
void section(const char *title) {
    ImGui::Spacing();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float h = ImGui::GetFrameHeight() * 0.95f, fs = h * 0.55f;
    ImVec2 ts = display_size(fs, title), p = ImGui::GetCursorScreenPos();
    float w = ts.x + h * 1.4f;
    draw_bar(dl, p, {p.x + w, p.y + h}, CYAN_BAR, false);
    outlined_text(dl, fs, {p.x + (w - ts.x) / 2, p.y + (h - ts.y) / 2}, col(WHITE), col(CYAN_BAR.outline), title, display_font);
    float x1 = p.x + ImGui::GetContentRegionAvail().x;
    dl->AddLine({p.x + w + h * 0.2f, p.y + h / 2}, {x1, p.y + h / 2}, col(alpha(EDGE, 0.55f)), 2);
    ImGui::Dummy({w, h});
    ImGui::Spacing();
}

// the logo: a clock face inside a gear, hands at ten past ten
void draw_logo(ImDrawList *dl, ImVec2 c, float r) {
    const ImU32 rim = col(WHITE), face = col(ROYAL), mark = col(EDGE), hands = col(WHITE);
    constexpr int TEETH = 10;
    constexpr float PI = 3.14159265f;
    dl->AddCircleFilled({c.x, c.y + r * 0.08f}, r * 0.98f, col(alpha(NAVY_DARK, 0.6f)), 48); // shadow
    for (int i = 0; i < TEETH; i++) { // trapezoid teeth around the rim
        float a = 2 * PI * float(i) / TEETH, wo = PI / TEETH * 0.45f, wi = PI / TEETH * 0.7f;
        auto at = [&](float ang, float rad) { return ImVec2{c.x + std::cos(ang) * rad, c.y + std::sin(ang) * rad}; };
        dl->AddQuadFilled(at(a - wi, r * 0.8f), at(a - wo, r), at(a + wo, r), at(a + wi, r * 0.8f), rim);
    }
    dl->AddCircleFilled(c, r * 0.84f, rim, 48);
    dl->AddCircleFilled(c, r * 0.68f, col(GOLD), 48);
    dl->AddCircleFilled(c, r * 0.6f, face, 48);
    for (int i = 0; i < 12; i++) { // hour marks
        float a = 2 * PI * float(i) / 12, len = i % 3 == 0 ? 0.16f : 0.08f;
        ImVec2 o{std::cos(a), std::sin(a)};
        dl->AddLine({c.x + o.x * r * (0.54f - len), c.y + o.y * r * (0.54f - len)}, {c.x + o.x * r * 0.54f, c.y + o.y * r * 0.54f},
                    i % 3 == 0 ? hands : mark, std::max(1.0f, r * 0.05f));
    }
    auto hand = [&](float hours_angle, float len, float width) {
        float a = hours_angle - PI / 2;
        dl->AddLine(c, {c.x + std::cos(a) * r * len, c.y + std::sin(a) * r * len}, hands, width);
    };
    hand(2 * PI * 10.17f / 12, 0.30f, std::max(1.5f, r * 0.09f)); // hour hand at 10
    hand(2 * PI * 2 / 12, 0.44f, std::max(1.0f, r * 0.06f));     // minute hand at 2
    dl->AddCircleFilled(c, r * 0.08f, col(GOLD), 16);
}

// a game's name without the series name: "Blasters", "Busters 2", "3"; the series for the first
std::string short_title(const GameInfo &g) {
    std::string rest = g.title;
    const std::string brand = "Yo-kai Watch";
    if (rest.rfind(brand, 0) == 0) rest = rest.substr(brand.size());
    while (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
    return rest.empty() ? brand : rest;
}

// a game with no title-screen image of its own: its name on a blue burst
void draw_placeholder(ImDrawList *dl, ImVec2 a, ImVec2 b, const GameInfo &g, float rounding) {
    dl->AddRectFilledMultiColor(a, b, col(ROYAL), col(ROYAL), col(NAVY), col(NAVY));
    ImVec2 c{(a.x + b.x) / 2, (a.y + b.y) / 2};
    dl->PushClipRect(a, b, true);
    for (int i = 0; i < 12; i++) { // rays
        float t0 = 6.2831853f * float(i) / 12, t1 = t0 + 0.26f, r = (b.x - a.x);
        dl->AddTriangleFilled(c, {c.x + std::cos(t0) * r, c.y + std::sin(t0) * r}, {c.x + std::cos(t1) * r, c.y + std::sin(t1) * r},
                              col(alpha(CYAN, 0.18f)));
    }
    dl->PopClipRect();
    (void)rounding;
    std::string rest = short_title(g);
    bool series = rest == "Yo-kai Watch";
    float w = b.x - a.x, h = b.y - a.y, small = h * 0.15f, big = h * 0.30f;
    ImVec2 bs = display_size(small, "YO-KAI WATCH");
    float big_w = series ? 0 : display_size(big, rest.c_str()).x;
    if (big_w > w * 0.86f) big *= w * 0.86f / big_w, big_w = w * 0.86f; // fit long names
    float total = small + (series ? 0 : big);
    float y = a.y + (h - total) / 2;
    if (series) small *= 1.4f, bs = display_size(small, "YO-KAI WATCH"), y = a.y + (h - small) / 2;
    outlined_text(dl, small, {a.x + (w - bs.x) / 2, y}, col(WHITE), col(NAVY_DARK), "YO-KAI WATCH", display_font);
    if (!series) outlined_text(dl, big, {a.x + (w - big_w) / 2, y + small}, col(GOLD_LIGHT), col(RED), rest.c_str(), display_font);
}

// a ribbon with notched ends, like the team logos' banners
void draw_ribbon(ImDrawList *dl, ImVec2 a, ImVec2 b, ImVec4 c) {
    float h = b.y - a.y, notch = h * 0.35f, tail = h * 0.55f;
    // the tails, darker, behind
    ImVec4 d = mix(c, NAVY_DARK, 0.45f);
    ImVec2 l[5] = {{a.x - tail, a.y + h * 0.2f}, {a.x + tail, a.y + h * 0.2f}, {a.x + tail, b.y + h * 0.2f},
                   {a.x - tail, b.y + h * 0.2f}, {a.x - tail + notch, (a.y + b.y) / 2 + h * 0.2f}};
    ImVec2 r[5] = {{b.x + tail, a.y + h * 0.2f}, {b.x - tail, a.y + h * 0.2f}, {b.x - tail, b.y + h * 0.2f},
                   {b.x + tail, b.y + h * 0.2f}, {b.x + tail - notch, (a.y + b.y) / 2 + h * 0.2f}};
    for (ImVec2 *t : {l, r}) {
        ImVec2 q1[3] = {t[0], t[1], t[4]}, q2[3] = {t[4], t[1], t[2]}, q3[3] = {t[4], t[2], t[3]};
        for (ImVec2 *q : {q1, q2, q3}) dl->AddConvexPolyFilled(q, 3, col(d));
    }
    dl->AddRectFilled({a.x + 2, a.y + 3}, {b.x + 2, b.y + 3}, col(alpha(NAVY_DARK, 0.5f)));
    dl->AddRectFilledMultiColor(a, b, col(mix(c, WHITE, 0.2f)), col(mix(c, WHITE, 0.2f)), col(c), col(c));
    dl->AddRect(a, b, col(WHITE), 0, 0, std::max(1.5f, h * 0.07f));
}

// a game in the library, as a card like the version select's: its title screen in a thick bevelled
// frame (gold when chosen), its name on a ribbon, its versions below, and whether it can be played
bool game_tile(int i, bool selected, bool playable, bool have_dump, SDL_Texture *image, float width, bool capturing) {
    const GameInfo &g = GAMES[i];
    float line = ImGui::GetTextLineHeight();
    float border = width * 0.05f, rounding = width * 0.07f;
    float image_h = (width - border * 2) * 3 / 5; // the 3DS top screen: 400x240
    float card_h = image_h + border * 2, ribbon_h = line * 1.7f;
    float h = card_h + ribbon_h * 0.6f + line * 2.6f + line * 0.6f;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(i);
    bool clicked = ImGui::InvisibleButton("tile", {width, h}) && playable;
    bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();
    if (hovered)
        ImGui::SetItemTooltip("%s%s%s%s%s%s", g.title, *g.subtitle ? "\n" : "", g.subtitle, *g.note ? "\n" : "", g.note,
                               !playable ? "\nNot supported yet" : have_dump ? "" : "\nNo dump found: Browse for it below");
    if (hovered && playable) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    ImDrawList *dl = ImGui::GetWindowDrawList();
    float lift = hovered && playable ? -line * 0.15f : 0;
    ImVec2 a{p.x, p.y + lift}, b{p.x + width, p.y + card_h + lift};
    ImVec4 light = selected ? GOLD_LIGHT : playable ? LILAC : SLATE;
    ImVec4 dark = selected ? ORANGE : playable ? LILAC_DARK : mix(SLATE, NAVY_DARK, 0.4f);
    if (selected) dl->AddRectFilled({a.x - 4, a.y - 4}, {b.x + 4, b.y + 4}, col(alpha(GOLD, 0.35f)), rounding + 4); // glow
    dl->AddRectFilled({a.x + 2, a.y + 5}, {b.x + 2, b.y + 5}, col(alpha(NAVY_DARK, 0.6f)), rounding); // shadow
    dl->AddRectFilled(a, b, col(dark), rounding);
    dl->AddRectFilled(a, {b.x, (a.y + b.y) / 2}, col(light), rounding, ImDrawFlags_RoundCornersTop); // bevel
    dl->PushClipRect(a, b, true); // stripes across the frame
    for (float x = a.x - card_h; x < b.x; x += border * 1.1f)
        dl->AddLine({x, b.y}, {x + card_h, a.y}, col(alpha(WHITE, 0.16f)), border * 0.35f);
    dl->PopClipRect();
    dl->AddRect(a, b, col(WHITE), rounding, 0, std::max(1.5f, border * 0.22f));
    ImVec2 ia{a.x + border, a.y + border}, ib{b.x - border, b.y - border};
    float ir = rounding * 0.5f;
    if (image) { // cover the area, cropping the image to its shape
        float iw = 0, ih = 0;
        SDL_GetTextureSize(image, &iw, &ih);
        float want = (ib.x - ia.x) / (ib.y - ia.y), have = iw / ih;
        ImVec2 uv0{0, 0}, uv1{1, 1};
        if (have > want) uv0.x = (1 - want / have) / 2, uv1.x = 1 - uv0.x;
        else uv0.y = (1 - have / want) / 2, uv1.y = 1 - uv0.y;
        dl->AddImageRounded(ImTextureID(intptr_t(image)), ia, ib, uv0, uv1, col(WHITE), ir);
    } else {
        dl->PushClipRect(ia, ib, true);
        draw_placeholder(dl, ia, ib, g, ir);
        dl->PopClipRect();
    }
    if (!playable) dl->AddRectFilled(ia, ib, col(alpha(NAVY_DARK, 0.5f)), ir);
    dl->AddRect(ia, ib, col(alpha(NAVY_DARK, 0.8f)), ir, 0, 2);

    // status chip over the image's corner
    const char *status = !playable ? "NOT IMPORTED" : capturing ? "CAPTURING" : have_dump ? "READY" : "NO DUMP";
    float ch = line * 1.25f, cfs = ch * 0.55f;
    ImVec2 cts = display_size(cfs, status);
    float cw = cts.x + ch * 1.2f;
    ImVec2 ca{ib.x - cw - border * 0.3f, ia.y + border * 0.3f};
    const Palette &chip = !playable ? OFF_BAR : capturing || !have_dump ? CYAN_BAR : GOLD_BAR;
    draw_bar(dl, ca, {ca.x + cw, ca.y + ch}, chip, false);
    outlined_text(dl, cfs, {ca.x + (cw - cts.x) / 2, ca.y + (ch - cts.y) / 2}, col(WHITE), col(chip.outline), status, display_font);

    // the name on a ribbon across the frame's foot
    std::string name = short_title(g);
    float rfs = ribbon_h * 0.58f;
    ImVec2 rts = display_size(rfs, name.c_str());
    float rw = std::min(width * 0.86f, rts.x + ribbon_h * 1.6f);
    if (rts.x > rw - ribbon_h) rfs *= (rw - ribbon_h) / rts.x, rts = display_size(rfs, name.c_str());
    ImVec2 ra{a.x + (width - rw) / 2, b.y - ribbon_h * 0.4f};
    draw_ribbon(dl, ra, {ra.x + rw, ra.y + ribbon_h}, selected ? RED : playable ? ROYAL : mix(SLATE, NAVY, 0.5f));
    outlined_text(dl, rfs, {ra.x + (rw - rts.x) / 2, ra.y + (ribbon_h - rts.y) / 2}, col(WHITE), col(NAVY_DARK),
                  name.c_str(), display_font);

    // the versions (and a warning), kept inside the tile: the tooltip has them in full
    float y = ra.y + ribbon_h + line * 0.45f;
    dl->PushClipRect({p.x, y}, {p.x + width, p.y + h}, true);
    float below = y;
    if (*g.note) {
        dl->AddText(nullptr, 0, {p.x + border, below}, col(WARN), g.note, nullptr, width - border * 2);
        below += ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, width - border * 2, g.note).y;
    }
    if (*g.subtitle) {
        ImVec2 sz = ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, width - border * 2, g.subtitle);
        dl->AddText(nullptr, 0, {p.x + (width - std::min(sz.x, width - border * 2)) / 2, below},
                    col(playable ? TEXT_DIM : alpha(TEXT_DIM, 0.6f)), g.subtitle, nullptr, width - border * 2);
    }
    dl->PopClipRect();
    return clicked;
}

// Capturing a game's title screen for its card, from the player's dump: the game runs headless
// (YW2_CAPTURE_TITLE, sdl.cpp) with a throwaway state folder, so it never touches the real saves. One
// game at a time, in the background
struct TitleJob {
    std::mutex lock;
    SDL_Process *process = nullptr; // while it runs (lock)
    int game = -1;                  // GAMES index being captured; -1: none
    std::atomic<bool> finished{false};
    std::thread thread;
};

void start_title_capture(TitleJob &job, int game, const std::filesystem::path &exe, const std::string &dump,
                         const std::string &update, const std::filesystem::path &out, const std::filesystem::path &scratch) {
    job.game = game, job.finished = false;
    job.thread = std::thread([&job, exe, dump, update, out, scratch] {
        std::error_code ec;
        std::filesystem::remove_all(scratch, ec);
        std::filesystem::create_directories(scratch, ec);
        std::filesystem::create_directories(out.parent_path(), ec);
        std::filesystem::path part = out;
        part += ".part";
        SDL_Environment *env = SDL_CreateEnvironment(true);
        SDL_SetEnvironmentVariable(env, "YW2_HEADLESS", "1", true);
        SDL_SetEnvironmentVariable(env, "YW2_CAPTURE_TITLE", part.string().c_str(), true);
        SDL_SetEnvironmentVariable(env, "YW2_STATE_DIR", scratch.string().c_str(), true);
        std::vector<std::string> args{exe.string()};
        if (!update.empty()) args.push_back("--update"), args.push_back(update);
        args.push_back(dump);
        std::vector<const char *> argv;
        for (auto &a : args) argv.push_back(a.c_str());
        argv.push_back(nullptr);
        SDL_PropertiesID props = SDL_CreateProperties();
        SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, argv.data());
        SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ENVIRONMENT_POINTER, env);
        SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_NULL);
        SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_NULL);
        SDL_Process *proc = SDL_CreateProcessWithProperties(props);
        SDL_DestroyProperties(props);
        SDL_DestroyEnvironment(env);
        if (!proc) LOG("launcher: couldn't start %s to capture its title: %s", exe.string().c_str(), SDL_GetError());
        {
            std::lock_guard lk(job.lock);
            job.process = proc;
        }
        int code = -1;
        // the game gives up after 90 s of frames; this, if it never gets that far
        for (int waited = 0; proc && !SDL_WaitProcess(proc, false, &code); waited += 100) {
            if (waited > 150000) SDL_KillProcess(proc, true);
            SDL_Delay(100);
        }
        {
            std::lock_guard lk(job.lock);
            job.process = nullptr;
        }
        if (proc) SDL_DestroyProcess(proc);
        if (code == 0) std::filesystem::rename(part, out, ec);
        else std::filesystem::remove(part, ec);
        LOG("launcher: title capture of %s %s (exit %d)", dump.c_str(), code == 0 && !ec ? "done" : "failed", code);
        std::filesystem::remove_all(scratch, ec);
        job.finished = true;
    });
}

void stop_title_capture(TitleJob &job) {
    {
        std::lock_guard lk(job.lock);
        if (job.process) SDL_KillProcess(job.process, true);
    }
    if (job.thread.joinable()) job.thread.join();
    job.game = -1;
}

// The Mods tab's state: GameBanana requests run on their own threads and report back here
struct ModsState {
    std::mutex lock;
    std::string game; // what the list below is for
    std::vector<gamebanana::Mod> list;
    int page = 0;
    bool more = false, loading = false;
    std::string message; // the last thing that happened (an error, or "installed")
    // installing: choosing among a mod's files, then downloading
    long long choosing = 0; // a mod whose files are listed
    gamebanana::Mod choosing_mod;
    std::vector<gamebanana::File> files;
    bool busy = false;
    std::atomic<float> progress{0};
    std::string busy_what;
    bool installed_changed = true;
};

struct InstalledMod {
    std::filesystem::path dir;
    std::string name;
    long long gamebanana = 0;
    bool enabled = true;
};

std::vector<InstalledMod> installed_mods(const std::string &game) {
    std::vector<InstalledMod> out;
    std::error_code ec;
    for (auto &e : std::filesystem::directory_iterator(mods::game_folder(game), ec)) {
        if (!e.is_directory(ec)) continue;
        InstalledMod m{e.path(), e.path().filename().string()};
        m.enabled = !std::filesystem::exists(e.path() / "disabled", ec);
        std::ifstream ini(e.path() / "mod.ini");
        for (std::string line; std::getline(ini, line);) {
            if (line.rfind("name=", 0) == 0) m.name = line.substr(5);
            if (line.rfind("gamebanana=", 0) == 0) m.gamebanana = std::atoll(line.c_str() + 11);
        }
        out.push_back(std::move(m));
    }
    std::sort(out.begin(), out.end(), [](auto &a, auto &b) { return a.dir < b.dir; });
    return out;
}

void install_async(const std::shared_ptr<ModsState> &st, const std::string &game, const gamebanana::Mod &mod,
                   const gamebanana::File &file) {
    {
        std::lock_guard lk(st->lock);
        st->busy = true, st->progress = 0, st->busy_what = "Installing " + mod.name, st->choosing = 0;
    }
    std::thread([st, game, mod, file] {
        std::string error;
        bool ok = gamebanana::install(game, mod, file, st->progress, error);
        std::lock_guard lk(st->lock);
        st->busy = false, st->installed_changed = true;
        st->message = ok ? mod.name + " installed. It's used the next time the game starts." : error;
        if (!ok) LOG("mods: couldn't install %s: %s", mod.name.c_str(), error.c_str());
    }).detach();
}

// the Mods tab, for the chosen game
void mods_tab(const std::shared_ptr<ModsState> &st, const std::string &game, const char *title,
              std::vector<InstalledMod> &installed) {
    ImGuiStyle &style = ImGui::GetStyle();
    if (game.empty()) {
        ImGui::TextWrapped("Choose a game in the Library first: its mods show here.");
        return;
    }
    std::lock_guard lk(st->lock);
    if (st->game != game) { // another game: start over
        st->game = game, st->list.clear(), st->page = 0, st->more = false, st->message.clear(), st->choosing = 0;
        st->installed_changed = true;
    }
    if (st->installed_changed) installed = installed_mods(game), st->installed_changed = false;

    section((std::string("Installed for ") + title).c_str());
    if (installed.empty()) ImGui::TextDisabled("None yet.");
    for (InstalledMod &m : installed) {
        ImGui::PushID(m.dir.string().c_str());
        if (ImGui::Checkbox(m.name.c_str(), &m.enabled)) {
            std::error_code ec;
            if (m.enabled) std::filesystem::remove(m.dir / "disabled", ec);
            else std::ofstream(m.dir / "disabled") << "";
        }
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("Remove").x - style.FramePadding.x);
        if (ImGui::SmallButton("Remove")) ImGui::OpenPopup("remove");
        if (ImGui::BeginPopup("remove")) {
            ImGui::Text("Remove %s?", m.name.c_str());
            if (ImGui::Button("Remove it")) {
                std::error_code ec;
                std::filesystem::remove_all(m.dir, ec);
                st->installed_changed = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Keep")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, TEXT_DIM);
    ImGui::TextWrapped("Mods are used when the game starts. Where two change the same file, the one lower in the "
                       "list wins.");
    ImGui::PopStyleColor();

    section("On GameBanana");
    int gb = gamebanana::game_id(game);
    if (!gb) {
        ImGui::TextDisabled("Clockwork doesn't know this game's GameBanana page.");
        return;
    }
    if (st->busy) {
        ImGui::TextUnformatted(st->busy_what.c_str());
        ImGui::ProgressBar(st->progress, {-1, 0});
    }
    if (!st->message.empty()) ImGui::TextWrapped("%s", st->message.c_str());
    if (st->choosing) { // a mod with several files: which one
        ImGui::Text("%s has several files:", st->choosing_mod.name.c_str());
        for (const gamebanana::File &f : st->files) {
            ImGui::PushID(int(f.id));
            ImGui::BeginDisabled(st->busy);
            if (ImGui::SmallButton("Install")) {
                gamebanana::Mod mod = st->choosing_mod;
                gamebanana::File file = f;
                st->lock.unlock();
                install_async(st, game, mod, file);
                st->lock.lock();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::Text("%s (%.1f MB)", f.name.c_str(), double(f.size) / 1e6);
            if (!f.description.empty()) ImGui::TextDisabled("    %s", f.description.c_str());
            ImGui::PopID();
        }
        if (ImGui::SmallButton("Cancel")) st->choosing = 0;
        ImGui::Separator();
    }
    if (st->list.empty() && !st->loading && ImGui::Button("Show mods from gamebanana.com")) st->page = 0, st->more = true;
    // the next page, when asked for
    auto load_page = [&] {
        st->loading = true;
        int page = st->page + 1;
        std::thread([st, gb, page, game] {
            std::vector<gamebanana::Mod> got;
            bool more = false;
            std::string error;
            bool ok = gamebanana::list_mods(gb, page, got, more, error);
            std::lock_guard lk(st->lock);
            st->loading = false;
            if (st->game != game) return;
            if (!ok) {
                st->message = error, st->more = false;
                return;
            }
            st->list.insert(st->list.end(), got.begin(), got.end());
            st->page = page, st->more = more;
        }).detach();
    };
    if (st->list.empty() && st->more && !st->loading) load_page();
    for (const gamebanana::Mod &m : st->list) {
        ImGui::PushID(int(m.id));
        bool have = std::any_of(installed.begin(), installed.end(), [&](auto &i) { return i.gamebanana == m.id; });
        ImGui::BeginDisabled(st->busy);
        if (ImGui::SmallButton(have ? "Reinstall" : "Install")) {
            gamebanana::Mod mod = m;
            st->message.clear();
            st->busy = true, st->busy_what = "Looking at " + m.name + "'s files", st->progress = 0;
            std::thread([st, mod, game] {
                std::vector<gamebanana::File> files;
                std::string error;
                bool ok = gamebanana::mod_files(mod.id, files, error);
                std::unique_lock lk(st->lock);
                st->busy = false;
                if (!ok) {
                    st->message = error;
                    return;
                }
                if (files.size() == 1) {
                    lk.unlock();
                    install_async(st, game, mod, files[0]);
                    return;
                }
                st->choosing = mod.id, st->choosing_mod = mod, st->files = std::move(files);
            }).detach();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextUnformatted(m.name.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%s%s%s  %lld likes", m.category.c_str(), m.submitter.empty() ? "" : " by ",
                            m.submitter.c_str(), m.likes);
        ImGui::PopID();
    }
    if (st->loading) ImGui::TextDisabled("Loading...");
    else if (!st->list.empty() && st->more && ImGui::Button("More")) load_page();
    ImGui::PushStyleColor(ImGuiCol_Text, TEXT_DIM);
    ImGui::TextWrapped("From gamebanana.com; each mod is its author's. Clockwork installs mods with include/ or romfs/ "
                       "folders; ones that need their own tools or change the game's code won't work.");
    ImGui::PopStyleColor();
}

// The Rooms tab's state: the relay is asked who's there on its own thread
struct RoomsState {
    std::mutex lock;
    std::string address; // what `rooms` is from
    std::vector<net::RoomInfo> rooms;
    std::string error;
    bool asking = false, answered = false;
    std::chrono::steady_clock::time_point asked{};
};

// "Yo-kai Watch 2: Psychic Specters" for a program id
std::string game_name(uint64_t program_id) {
    const KnownDump *d = find_dump(program_id);
    const GameInfo *g = d ? find_game(d->game) : nullptr;
    if (!g) return "another game";
    std::string v = version_label(*d);
    return std::string(g->title) + (v.empty() ? "" : ": ") + v;
}

// a text field editing `value`, up to the relay's limit
void text_field(const char *label, std::string &value, float width, ImGuiInputTextFlags flags = 0) {
    char buf[net::MAX_TEXT + 1];
    std::snprintf(buf, sizeof buf, "%s", value.c_str());
    ImGui::SetNextItemWidth(width);
    if (ImGui::InputText(label, buf, sizeof buf, flags)) value = buf;
}

// asks the chosen relay who's there every few seconds (from when the launcher opens, so the
// Rooms tab has its answer when it's chosen)
void ask_relay(const std::shared_ptr<RoomsState> &st, const Settings &s) {
    std::string address = s.relay.empty() ? "127.0.0.1" : s.relay;
    std::lock_guard lk(st->lock);
    if (st->address != address) st->address = address, st->rooms.clear(), st->error.clear(), st->answered = false;
    if (st->asking || std::chrono::steady_clock::now() - st->asked < std::chrono::seconds(2)) return;
    st->asking = true;
    std::thread([st, address] {
        std::vector<net::RoomInfo> rooms;
        std::string error;
        bool ok = net::relay_list(address, rooms, error);
        std::lock_guard lk(st->lock);
        st->asking = false, st->asked = std::chrono::steady_clock::now();
        if (st->address != address) return; // changed meanwhile
        st->answered = ok, st->rooms = std::move(rooms), st->error = std::move(error);
    }).detach();
}

// a build's progress, or why it failed (with a way to try again)
template <typename Retry> void build_status(const std::shared_ptr<BuildJob> &job, Retry retry) {
    std::unique_lock lk(job->lock);
    if (!job->running && !(job->finished && !job->ok)) return;
    if (job->running) {
        ImGui::TextColored(BLUE, "Building %s, step %d of %d: %s", job->title.c_str(), job->step_index, job->step_count,
                           job->step.c_str());
        char overlay[32] = "";
        if (job->progress >= 0) std::snprintf(overlay, sizeof overlay, "%d%%", int(job->progress * 100));
        float bar = job->progress >= 0 ? job->progress : float(std::fmod(ImGui::GetTime() * 0.4, 1.0)); // working
        ImGui::ProgressBar(bar, {-1, 0}, overlay);
        ImGui::PushStyleColor(ImGuiCol_Text, TEXT_DIM);
        ImGui::TextUnformatted(job->last_line.substr(0, 160).c_str());
        ImGui::PopStyleColor();
        return;
    }
    ImGui::TextColored(WARN, "Couldn't build %s: %s", job->title.c_str(), job->error.c_str());
    ImGui::TextDisabled("Everything the tools printed is in %s", job->log_path.c_str());
    lk.unlock();
    if (ImGui::Button("Try again")) retry();
}

// dim text, wrapped
void note(const char *text) {
    ImGui::PushStyleColor(ImGuiCol_Text, TEXT_DIM);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

// the Rooms tab: local wireless with players elsewhere, through a relay
void rooms_tab(const std::shared_ptr<RoomsState> &st, Settings &s, const char *chosen_game) {
    ImGuiStyle &style = ImGui::GetStyle();
    float field = ImGui::GetFontSize() * 16;
    static bool use_relay = !s.relay.empty();
    static std::string relay_text = s.relay;

    section("Where");
    int where = use_relay ? 1 : 0;
    ImGui::RadioButton("This PC only (two game windows)", &where, 0);
    ImGui::RadioButton("A relay server:", &where, 1);
    use_relay = where == 1;
    ImGui::SameLine();
    ImGui::BeginDisabled(!use_relay);
    {
        char buf[256];
        std::snprintf(buf, sizeof buf, "%s", relay_text.c_str());
        ImGui::SetNextItemWidth(field);
        if (ImGui::InputTextWithHint("##relay", "host or host:port", buf, sizeof buf)) relay_text = buf;
    }
    ImGui::EndDisabled();
    s.relay = use_relay ? relay_text : "";
    note("Everyone on the same relay, in the same room, plays local wireless together: battles, trades and co-op, "
         "as if side by side.");

    section("You");
    if (s.player_name.empty()) {
        const char *user = std::getenv("USER");
        if (!user) user = std::getenv("USERNAME");
        s.player_name = user && *user ? user : "Player";
    }
    text_field("Your name", s.player_name, field);
    text_field("Room", s.room, field);
    text_field("Room password", s.room_password, field, ImGuiInputTextFlags_Password);
    note("Any name makes a room; the first one in sets its password (or none). The game joins the room when it "
         "starts.");

    std::lock_guard lk(st->lock);
    section(s.relay.empty() ? "Rooms on this PC" : ("Rooms on " + s.relay).c_str());
    if (!st->answered) {
        if (s.relay.empty()) ImGui::TextDisabled("No game is running on this PC yet.");
        else if (!st->error.empty()) ImGui::TextColored(WARN, "%s", st->error.c_str());
        else ImGui::TextDisabled("Asking...");
    } else if (st->rooms.empty()) {
        ImGui::TextDisabled("Nobody is in a room yet.");
    } else if (ImGui::BeginTable("rooms", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("Room", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 9);
        ImGui::TableSetupColumn("Players", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("Choose").x + style.FramePadding.x * 2);
        ImGui::TableHeadersRow();
        for (const net::RoomInfo &room : st->rooms) {
            ImGui::PushID(room.name.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(room.name.empty() ? "(no name)" : room.name.c_str());
            if (room.password) ImGui::TextDisabled("password");
            ImGui::TableNextColumn();
            for (const net::RoomMember &m : room.members) {
                const KnownDump *d = find_dump(m.program_id);
                bool other = chosen_game && *chosen_game && (!d || std::strcmp(d->game, chosen_game) != 0);
                ImGui::TextUnformatted(m.name.c_str());
                ImGui::SameLine();
                if (other) ImGui::TextColored(WARN, "%s", game_name(m.program_id).c_str());
                else ImGui::TextDisabled("%s", game_name(m.program_id).c_str());
            }
            ImGui::TableNextColumn();
            if (room.name == s.room) ImGui::TextColored(BLUE, "Yours");
            else if (ImGui::SmallButton("Choose")) s.room = room.name;
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    section("Hosting a relay");
    note("Run clockwork-relay (next to the games) on a PC the others can reach, and forward UDP port 47830 to it. "
         "Everyone needs the same game (versions that play together on a 3DS do here too).");
}

} // namespace

bool run_launcher(SDL_Window *window, SDL_Renderer *renderer, const std::string &gpu) {
    Settings &s = settings();
    SDL_SetWindowFullscreen(window, s.launcher_fullscreen);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    load_fonts(io);
    ImGuiStyle &style = ImGui::GetStyle();
    apply_theme(style);
    const ImGuiStyle base_style = style;
    // scale with the window, in points (ImGui's built-in font is drawn for about 600x500 of them);
    // redone when the window changes size, as tiling window managers resize it after it opens
    float dpi = 0;
    auto rescale = [&] {
        int w = 0, h = 0;
        SDL_GetWindowSize(window, &w, &h);
        float density = SDL_GetWindowPixelDensity(window);
        float content = density > 0 ? SDL_GetWindowDisplayScale(window) / density : 1; // system UI scale
        float want = std::max({content, 1.0f, std::min(float(h) / 600.0f, float(w) / 500.0f)});
        if (want == dpi) return;
        dpi = want;
        style = base_style;
        style.ScaleAllSizes(dpi);
        style.FontScaleDpi = dpi;
    };
    rescale();
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);
    SDL_SetRenderLogicalPresentation(renderer, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED);

    std::vector<SDL_Gamepad *> pads;
    Capture capture;
    bool play = false, quit = false;
    static const char *FILTERS[] = {"Sharp pixels (recommended)", "Smooth", "Nearest neighbor"};
    static const SDL_DialogFileFilter DUMP_TYPES[] = {{"3DS game dumps and updates", "3ds;cci;cxi;cia;ncch"}, {"All files", "*"}};

    // YW2_LAUNCHER_SCREENSHOT=<prefix>: saves each tab as a PNG and quits (for testing the UI)
    const char *shot = std::getenv("YW2_LAUNCHER_SCREENSHOT");
    int frame = 0, shot_tab = 0;
    int top_tab = 0, settings_tab = 0; // the page shown
    uint64_t frame_count = 0;
    // top tabs, and the Settings tab's own
    static const char *TABS[] = {"Library", "Mods", "Rooms", "Settings"};
    static const char *SETTINGS_TABS[] = {"Display", "Controls", "Audio"};
    // screenshot pages: each top tab, and each settings tab
    static const char *SHOT_NAMES[] = {"Library", "Mods", "Rooms", "Display", "Controls", "Audio"};
    static const int SHOT_TOP[] = {0, 1, 2, 3, 3, 3}, SHOT_SUB[] = {-1, -1, -1, 0, 1, 2};
    auto mods_state = std::make_shared<ModsState>();
    std::vector<InstalledMod> installed;
    auto rooms_state = std::make_shared<RoomsState>();
    // audio devices, for the Audio tab
    std::vector<std::string> audio_devices;
    {
        int n = 0;
        if (SDL_AudioDeviceID *ids = SDL_GetAudioPlaybackDevices(&n)) {
            for (int i = 0; i < n; i++)
                if (const char *name = SDL_GetAudioDeviceName(ids[i])) audio_devices.push_back(name);
            SDL_free(ids);
        }
    }
    // a game is playable when the executable for one of its dumps is built
    bool playable[GAME_COUNT] = {};
    auto find_playable = [&] {
        for (int i = 0; i < GAME_COUNT; i++)
            for (const KnownDump &d : KNOWN_DUMPS)
                if (std::strcmp(d.game, GAMES[i].id) == 0 && binary_available(d.binary)) playable[i] = true;
    };
    find_playable();
    // games built from their dumps here (autobuild.h): the dumps found decide what to build, one
    // executable at a time; each is tried once a session (a failure stays on screen until retried)
    auto build_job = std::make_shared<BuildJob>();
    std::set<std::string> build_tried;
    bool check_builds = true; // something changed: look for an executable to build
    // the chosen dump: which game and version it is, read again when the path changes
    if (s.dump_folder.empty() && dump_exists(s.game_path))
        s.dump_folder = std::filesystem::path(s.game_path).parent_path().string();
    refresh_dumps(s);
    for (auto &[id, path] : s.dumps) LOG("launcher: dump %016llx at %s", (unsigned long long)id, path.c_str());
    std::string notice; // under the games: why a click did nothing
    uint64_t save_target = 0; // the game a save is being imported for
    std::string save_message;
    bool save_ok = false;
    std::string routed_path = "\x01";
    Route dump_route{Route::UNREADABLE};

    // title-screen images: titles/<id>.png in the data folder if the player put one there, else
    // titles/auto/<id>.png, captured from their dump (TitleJob). None ship
    std::error_code ec;
    const std::filesystem::path title_dir = platform::data_dir() / "titles", auto_dir = title_dir / "auto";
    std::filesystem::create_directories(title_dir, ec);
    std::vector<SDL_Texture *> title_images(GAME_COUNT, nullptr);
    auto load_title = [&](int i) {
        for (const auto &dir : {title_dir, auto_dir}) {
            std::filesystem::path png = dir / (std::string(GAMES[i].id) + ".png");
            if (!std::filesystem::is_regular_file(png, ec)) continue;
            if (SDL_Surface *surface = SDL_LoadPNG(png.string().c_str())) {
                if (title_images[i]) SDL_DestroyTexture(title_images[i]);
                title_images[i] = SDL_CreateTextureFromSurface(renderer, surface);
                SDL_DestroySurface(surface);
                if (title_images[i]) SDL_SetTextureScaleMode(title_images[i], SDL_SCALEMODE_LINEAR);
                return;
            }
        }
    };
    for (int i = 0; i < GAME_COUNT; i++) load_title(i);
    TitleJob title_job;
    std::set<int> title_tried; // this session: captured, or tried and failed

    while (!play && !quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            // a binding capture takes the next key / controller input before the UI sees it
            if (capture.control >= 0) {
                if (!capture.pad && e.type == SDL_EVENT_KEY_DOWN) {
                    if (e.key.scancode != SDL_SCANCODE_ESCAPE) s.keys[capture.control] = e.key.scancode;
                    capture.control = -1;
                    continue;
                }
                if (capture.pad && e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                    s.pad[capture.control] = e.gbutton.button;
                    capture.control = -1;
                    continue;
                }
                if (capture.pad && e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION && e.gaxis.value > 20000 &&
                    (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || e.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)) {
                    s.pad[capture.control] =
                        e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? PAD_LEFT_TRIGGER : PAD_RIGHT_TRIGGER;
                    capture.control = -1;
                    continue;
                }
                if (capture.pad && e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_ESCAPE) {
                    capture.control = -1;
                    continue;
                }
            }
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT) quit = true;
            if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.scancode == SDL_SCANCODE_F11 && capture.control < 0) {
                s.launcher_fullscreen = !s.launcher_fullscreen;
                SDL_SetWindowFullscreen(window, s.launcher_fullscreen);
            }
            if (e.type == SDL_EVENT_GAMEPAD_ADDED)
                if (SDL_Gamepad *g = SDL_OpenGamepad(e.gdevice.which)) pads.push_back(g);
        }
        {
            std::lock_guard lk(picked_lock);
            if (!picked.empty()) { // remember its folder, and find the other dumps there
                // an update is added to its game (which stays chosen); anything else is the dump to play
                auto id = peek_program_id(picked);
                bool is_update = id && update_of(*id) && find_dump(*update_of(*id));
                if (!is_update) s.game_path = picked;
                s.dump_folder = std::filesystem::path(picked).parent_path().string();
                remember(s, picked);
                if (is_update) {
                    if (auto it = s.dumps.find(*update_of(*id)); it != s.dumps.end()) s.game_path = it->second;
                    routed_path = "\x01"; // route it again: it now plays the update's code
                }
                find_dumps(s, s.dump_folder);
                notice.clear();
                picked.clear();
                check_builds = true;
            }
            if (!picked_save.empty()) {
                save_ok = import_save(save_target, picked_save, save_message);
                picked_save.clear();
            }
        }

        bool building = false, build_failed = false;
        std::string build_binary;
        {
            std::lock_guard lk(build_job->lock);
            building = build_job->running, build_binary = build_job->binary;
            build_failed = build_job->finished && !build_job->ok;
            if (build_job->finished && build_job->ok) { // done: it can be played, and the next one built
                build_job->finished = false;
                find_playable();
                routed_path = "\x01";
                check_builds = true;
            }
        }
        if (check_builds && !building && can_build_games()) {
            check_builds = false;
            for (auto &[id, path] : s.dumps) {
                const KnownDump *d = find_dump(id);
                if (!d || !dump_exists(path)) continue;
                std::string binary = code_binary(*d);
                if (build_tried.count(binary) || !needs_build(*d)) continue;
                // every version sharing the executable: their updates' code, when it's built from updates
                std::vector<std::pair<uint64_t, std::string>> versions;
                for (auto &[other, other_path] : s.dumps)
                    if (const KnownDump *o = find_dump(other); o && code_binary(*o) == binary && dump_exists(other_path)) {
                        std::string update = update_for(other);
                        versions.push_back({other, update.empty() ? other_path : update});
                    }
                build_tried.insert(binary);
                start_build(build_job, binary, versions);
                building = true, build_binary = binary;
                break;
            }
        }
        if (s.game_path != routed_path) {
            routed_path = s.game_path;
            dump_route = dump_exists(s.game_path) ? route(s.game_path) : Route{Route::UNREADABLE};
            // built, but not from this version's dump: it's rebuilt first (autobuild.h)
            if (dump_route.dump && (dump_route.kind == Route::HERE || dump_route.kind == Route::OTHER) &&
                needs_build(*dump_route.dump))
                dump_route.kind = Route::NOT_BUILT;
        }
        bool chosen_building = building && dump_route.dump && build_binary == dump_route.binary;
        bool dump_ok = (dump_route.kind == Route::HERE || dump_route.kind == Route::OTHER) && !chosen_building;
        std::string dump_name; // "Yo-kai Watch 2: Bony Spirits"
        if (dump_route.dump) {
            const GameInfo *g = find_game(dump_route.dump->game);
            std::string v = version_label(*dump_route.dump);
            dump_name = std::string(g ? g->title : "?") + (v.empty() ? "" : ": ") + v;
        }

        // title screens: one capture at a time, for a game with a playable dump and no image, when
        // nothing's being built (checked every second)
        if (title_job.game >= 0 && title_job.finished) {
            if (title_job.thread.joinable()) title_job.thread.join();
            load_title(title_job.game);
            title_job.game = -1;
        }
        if (title_job.game < 0 && !building && !shot && frame_count % 60 == 0)
            for (int i = 0; i < GAME_COUNT; i++) {
                if (title_images[i] || title_tried.count(i)) continue;
                std::string path = dump_for(s, GAMES[i].id);
                if (path.empty() || !dump_exists(path)) continue;
                Route r = route(path);
                if ((r.kind != Route::HERE && r.kind != Route::OTHER) || needs_build(*r.dump)) continue;
                title_tried.insert(i);
                start_title_capture(title_job, i, executable_for(r), path, update_for(r.dump->program_id),
                                    auto_dir / (std::string(GAMES[i].id) + ".png"),
                                    platform::data_dir() / "build" / "title-capture");
                break;
            }
        frame_count++;
        ask_relay(rooms_state, s);
        if (s.menu_music && !menu_music::started()) menu_music::start();
        {
            bool unfocused = !(SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS);
            menu_music::set_volume(s.menu_music && !(s.mute_in_background && unfocused) ? float(s.volume) / 100.0f : 0.0f);
        }
        if (s.launcher_fullscreen != bool(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN))
            SDL_SetWindowFullscreen(window, s.launcher_fullscreen); // the checkbox
        rescale();
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        if (shot) top_tab = SHOT_TOP[shot_tab], settings_tab = std::max(0, SHOT_SUB[shot_tab]);
        // the backdrop fills the window; the launcher sits between its sunburst strips
        float unit = ImGui::GetFontSize() * 2.0f, side = unit * 1.6f;
        draw_backdrop(ImGui::GetBackgroundDrawList(), {0, 0}, io.DisplaySize, unit);
        ImGui::SetNextWindowPos({side, 0});
        ImGui::SetNextWindowSize({io.DisplaySize.x - side * 2, io.DisplaySize.y});
        ImGui::Begin("launcher", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoBackground);

        { // header: the logo and name, and the pages as the games' buttons (on the right, or below
          // when the window is narrow)
            ImDrawList *dl = ImGui::GetWindowDrawList();
            float title_size = ImGui::GetFontSize() * 2.3f, logo = title_size * 1.3f;
            ImVec2 p = ImGui::GetCursorScreenPos();
            draw_logo(dl, {p.x + logo / 2, p.y + logo / 2}, logo / 2);
            float x = p.x + logo + style.ItemSpacing.x * 1.2f;
            ImVec2 ts = display_size(title_size, "CLOCKWORK");
            outlined_text(dl, title_size, {x, p.y + (logo - ts.y) / 2}, col(WHITE), col(NAVY_DARK), "CLOCKWORK", display_font);
            float bh = logo * 0.6f, bw = ImGui::GetFontSize() * 6.4f, gap = style.ItemSpacing.x * 0.5f;
            float right = p.x + ImGui::GetContentRegionAvail().x, row = bw * 4 + gap * 3;
            bool beside = right - row >= x + ts.x + gap * 4;
            float tx = beside ? right - row : p.x, ty = beside ? p.y + (logo - bh) / 2 : p.y + logo + style.ItemSpacing.y;
            for (int i = 0; i < 4; i++) {
                ImGui::SetCursorScreenPos({tx + float(i) * (bw + gap), ty});
                if (bar_button(TABS[i], {bw, bh}, top_tab == i ? GOLD_BAR : LILAC_BAR)) top_tab = i;
            }
            ImGui::SetCursorScreenPos({p.x, (beside ? p.y + logo : ty + bh) + style.ItemSpacing.y * 1.5f});
        }

        // the page, on an angled panel
        float footer = ImGui::GetFrameHeight() * 2.6f;
        {
            ImVec2 pa = ImGui::GetCursorScreenPos(), avail = ImGui::GetContentRegionAvail();
            ImVec2 pb{pa.x + avail.x, pa.y + avail.y - footer};
            ImDrawList *dl = ImGui::GetWindowDrawList();
            float cut = unit * 0.45f;
            chamfer_panel(dl, {pa.x + 3, pa.y + 6}, {pb.x + 3, pb.y + 6}, cut, col(alpha(NAVY_DARK, 0.6f)), 0, 0);
            chamfer_panel(dl, pa, pb, cut, col(alpha(NAVY_DARK, 0.78f)), col(EDGE), 3);
            chamfer_panel(dl, {pa.x + 5, pa.y + 5}, {pb.x - 5, pb.y - 5}, cut * 0.8f, 0, col(alpha(ROYAL, 0.8f)), 1.5f);
        }
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {unit * 0.55f, unit * 0.4f});
        ImGui::BeginChild("tabs", {0, -footer}, ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PopStyleVar();
        {
            auto tab = [&](int i) { return top_tab == i; };
            auto sub_tab = [&](int i) { return settings_tab == i; };
            if (tab(0)) {
                section("Games");
                { // a grid of tiles, as many across as fit
                    float avail = ImGui::GetContentRegionAvail().x, gap = style.ItemSpacing.x * 1.5f;
                    int columns = std::clamp(int((avail + gap) / (ImGui::GetFontSize() * 11 + gap)), 2, GAME_COUNT);
                    float width = (avail - gap * float(columns - 1)) / float(columns);
                    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {gap, gap});
                    for (int i = 0; i < GAME_COUNT; i++) {
                        if (i % columns) ImGui::SameLine();
                        bool chosen = dump_route.dump && std::strcmp(dump_route.dump->game, GAMES[i].id) == 0;
                        std::string found = dump_for(s, GAMES[i].id);
                        if (game_tile(i, chosen, playable[i], !found.empty(), title_images[i], width, title_job.game == i) &&
                            !chosen) {
                            if (found.empty()) notice = std::string("No dump of ") + GAMES[i].title +
                                                        " found yet: Browse for it below.";
                            else s.game_path = found, notice.clear();
                        }
                    }
                    ImGui::PopStyleVar();
                }
                if (dump_route.dump) { // the chosen game's versions found: pick one
                    std::vector<const KnownDump *> versions;
                    for (const KnownDump &d : KNOWN_DUMPS)
                        if (std::strcmp(d.game, dump_route.dump->game) == 0 && s.dumps.count(d.program_id))
                            versions.push_back(&d);
                    if (versions.size() > 1) {
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextUnformatted("Version:");
                        for (const KnownDump *d : versions) {
                            ImGui::SameLine();
                            bool on = d == dump_route.dump;
                            if (on) {
                                ImGui::PushStyleColor(ImGuiCol_Button, BLUE);
                                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, BLUE);
                                ImGui::PushStyleColor(ImGuiCol_Text, WHITE);
                            }
                            std::string label = version_label(*d);
                            if (label.empty()) label = region_name(d->region);
                            if (ImGui::Button((label + "##" + std::to_string(d->program_id)).c_str()))
                                s.game_path = s.dumps[d->program_id];
                            if (on) ImGui::PopStyleColor(3);
                        }
                    }
                }
                if (!notice.empty()) ImGui::TextColored(WARN, "%s", notice.c_str());
                if (dump_route.dump)
                    if (const GameInfo *g = find_game(dump_route.dump->game); g && *g->note)
                        ImGui::TextColored(WARN, "%s: %s", g->title, g->note);
                ImGui::PushStyleColor(ImGuiCol_Text, TEXT_DIM);
                ImGui::TextWrapped("Title screens are captured from your dumps. To use your own picture, put %s.png, "
                                   "%s.png ... in %s", GAMES[0].id, GAMES[1].id, title_dir.string().c_str());
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered())
                    ImGui::SetItemTooltip("yw1.png, yw2.png, blasters.png, yw3.png, busters2.png (any size; a 400x240 "
                                          "screenshot fits best). Read when the launcher opens. The captured ones are "
                                          "in titles/auto: delete one to capture it again.");
                section("Your game dumps");
                ImGui::TextWrapped("Decrypted dumps of your own games (.3ds, .cci, .cxi or .cia), and their updates "
                                   "(.cia or .cxi). Choose one, and the others in its folder are found too.");
                char buf[1024];
                std::snprintf(buf, sizeof buf, "%s", s.game_path.c_str());
                ImGui::SetNextItemWidth(-ImGui::CalcTextSize("Browse...").x - style.FramePadding.x * 4);
                if (ImGui::InputText("##path", buf, sizeof buf)) s.game_path = buf;
                ImGui::SameLine();
                if (ImGui::Button("Browse..."))
                    SDL_ShowOpenFileDialog(on_file_picked, nullptr, window, DUMP_TYPES, 2, nullptr, false);
                const ImVec4 warn{0.85f, 0.45f, 0.05f, 1}, bad{0.82f, 0.15f, 0.15f, 1};
                if (s.game_path.empty())
                    ImGui::TextColored(warn, "Choose a dump to play.");
                else if (!dump_exists(s.game_path))
                    ImGui::TextColored(bad, "File not found.");
                else if (dump_ok)
                    ImGui::TextColored(BLUE, "%s", dump_name.c_str());
                else if (!chosen_building && !(build_failed && dump_route.dump && build_binary == dump_route.binary))
                    ImGui::TextColored(bad, "%s", route_problem(dump_route).c_str());
                if (dump_route.kind == Route::NOT_BUILT && !building && !build_failed) {
                    std::string why;
                    if (!can_build_games(&why)) ImGui::TextDisabled("%s", why.c_str());
                }
                build_status(build_job, [&] { // try again
                    build_tried.erase(build_binary);
                    std::lock_guard lk(build_job->lock);
                    build_job->finished = false;
                    check_builds = true;
                });
                if (!s.dump_folder.empty())
                    ImGui::TextDisabled("%zu dump%s found in %s", s.dumps.size(), s.dumps.size() == 1 ? "" : "s",
                                        s.dump_folder.c_str());
                section("Save data");
                if (!dump_ok) {
                    ImGui::TextDisabled("Choose a game's dump first.");
                } else {
                    uint64_t id = dump_route.dump->program_id;
                    bool have = has_save(id);
                    if (have) ImGui::Text("%s has a save.", dump_name.c_str());
                    else ImGui::TextDisabled("No save yet for %s.", dump_name.c_str());
                    auto choose = [&] {
                        save_target = id, save_message.clear();
                        SDL_ShowOpenFolderDialog(on_save_picked, nullptr, window, nullptr, false);
                    };
                    if (ImGui::Button("Import save...")) {
                        if (have) ImGui::OpenPopup("replace save");
                        else choose();
                    }
                    if (ImGui::BeginPopup("replace save")) {
                        ImGui::Text("Replace the save for %s?", dump_name.c_str());
                        ImGui::TextDisabled("The current one is kept in a backup folder.");
                        if (ImGui::Button("Choose a save to import")) choose(), ImGui::CloseCurrentPopup();
                        ImGui::SameLine();
                        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
                        ImGui::EndPopup();
                    }
                    if (!save_message.empty() && save_target == id) {
                        ImGui::PushStyleColor(ImGuiCol_Text, save_ok ? BLUE : WARN);
                        ImGui::TextWrapped("%s", save_message.c_str());
                        ImGui::PopStyleColor();
                    }
                    ImGui::PushStyleColor(ImGuiCol_Text, TEXT_DIM);
                    ImGui::TextWrapped("Choose the folder with the save's files: a Checkpoint or JKSM backup from a "
                                       "3DS (on its SD card, under 3ds/Checkpoint/saves or JKSV), or another "
                                       "emulator's save folder for this game.");
                    ImGui::PopStyleColor();
                }
                if (dump_route.dump && dump_route.dump->region == Region::EUR) {
                    section("Language");
                    static const char *LANGUAGES[] = {"English", "French", "German", "Italian", "Spanish"};
                    int lang = std::clamp(s.language, 1, 5) - 1;
                    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
                    if (ImGui::Combo("Game language", &lang, LANGUAGES, 5)) s.language = lang + 1;
                    ImGui::TextDisabled("European releases: the language the console is set to.");
                }
                section("Startup");
                ImGui::Checkbox("Show this launcher when the game starts", &s.show_launcher);
                ImGui::TextDisabled("Hold Shift while starting, or pass --launcher, to bring it back.");
            }
            if (tab(1)) {
                const GameInfo *g = dump_route.dump ? find_game(dump_route.dump->game) : nullptr;
                mods_tab(mods_state, g ? g->id : "", g ? g->title : "", installed);
            }
            if (tab(2)) {
                rooms_tab(rooms_state, s, dump_route.dump ? dump_route.dump->game : "");
            }
            if (tab(3)) {
            { // the settings' own pages
                float sbh = ImGui::GetFrameHeight() * 1.2f, sbw = ImGui::GetFontSize() * 6.4f;
                for (int i = 0; i < 3; i++) {
                    if (i) ImGui::SameLine(0, style.ItemSpacing.x * 0.5f);
                    if (bar_button(SETTINGS_TABS[i], {sbw, sbh}, settings_tab == i ? CYAN_BAR : NAVY_BAR)) settings_tab = i;
                }
            if (sub_tab(0)) {
                section("Screens");
                int mode = s.single_screen ? 1 : 0;
                ImGui::RadioButton("Two screens (stacked, like the console)", &mode, 0);
                ImGui::RadioButton("One screen", &mode, 1);
                s.single_screen = mode == 1;
                ImGui::BeginDisabled(!s.single_screen);
                ImGui::Indent();
                ImGui::Checkbox("Adapt to what's on screen (map, Yo-kai Pad, battles)", &s.smart_layout);
                ImGui::Checkbox("Show the other screen in a corner (picture-in-picture)", &s.picture_in_picture);
                ImGui::Checkbox("Bottom screen large (swap)", &s.swap_screens);
                ImGui::Unindent();
                ImGui::EndDisabled();
                ImGui::Checkbox("Widescreen (16:9 top screen)", &s.widescreen);
                ImGui::TextDisabled("Shows more of the 3D scene at the sides; menus stay centered.");
                section("Graphics");
                ImGui::Checkbox("GPU renderer (recommended)", &s.gpu_renderer);
                ImGui::TextDisabled("Off: draw on the CPU instead (slower; for comparison or old GPUs).");
                if (!gpu.empty()) {
                    ImGui::TextWrapped("GPU in use: %s", gpu.c_str());
                    if (gpu.find("llvmpipe") != std::string::npos || gpu.find("SwiftShader") != std::string::npos)
                        ImGui::TextColored({1, 0.5f, 0.3f, 1}, "That is Vulkan emulated on the CPU (slow): install your GPU's Vulkan driver.");
                } else if (s.gpu_renderer) {
                    ImGui::TextColored({1, 0.5f, 0.3f, 1}, "No Vulkan GPU found: drawing on the CPU. Install your GPU's Vulkan driver.");
                }
                ImGui::Checkbox("Prefer the integrated GPU (laptops with two GPUs)", &s.prefer_integrated_gpu);
                ImGui::TextDisabled("Takes effect the next time the game starts.");
                static const char *RESOLUTIONS[] = {"1x native (400x240)", "2x (800x480)", "3x (1200x720)",
                                                    "4x (1600x960)"};
                int res = std::clamp(s.resolution, 1, 4) - 1;
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18);
                if (ImGui::Combo("Internal resolution", &res, RESOLUTIONS, 4)) s.resolution = res + 1;
                ImGui::TextDisabled("Renders sharper; with the software renderer, above 1x is slow.");
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18);
                ImGui::SliderInt("HUD size", &s.hud_size, 50, 100, "%d%%");
                ImGui::TextDisabled("Dialogue boxes and other 2D on the top screen (GPU renderer).");
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18);
                ImGui::SliderInt("Camera zoom", &s.camera_zoom, 60, 100, "%d%%");
                ImGui::TextDisabled("Widescreen: below 100%% pulls the camera back to show more of the scene.");
                ImGui::Checkbox("Frame interpolation (60 fps)", &s.frame_interpolation);
                ImGui::TextDisabled("GPU renderer: draws in-between frames for games running at 30 fps. "
                                    "Smoother, about two frames (33 ms) later.");
                section("Window");
                ImGui::Checkbox("Fullscreen", &s.fullscreen);
                ImGui::Checkbox("This launcher fullscreen (F11)", &s.launcher_fullscreen);
                ImGui::Checkbox("VSync", &s.vsync);
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18);
                ImGui::Combo("Scaling", &s.filter, FILTERS, 3);
                section("In-game keys");
                ImGui::TextDisabled("F6 adaptive layout   F7 widescreen   F8 picture-in-picture   F9 one/two screens");
                ImGui::TextDisabled("F12 saves a capture (screenshot + scene info) to the captures folder");
                ImGui::TextDisabled("Tab or right stick swap screens   F11 fullscreen");
            }
            if (sub_tab(1)) {
                ImGui::TextWrapped("Click a binding, then press a key or controller button (Esc cancels). "
                                   "The left stick is always the Circle Pad; click the bottom screen to touch.");
                if (ImGui::BeginTable("bindings", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
                    ImGui::TableSetupColumn("3DS");
                    ImGui::TableSetupColumn("Keyboard");
                    ImGui::TableSetupColumn("Controller");
                    ImGui::TableHeadersRow();
                    for (int i = 0; i < CONTROL_COUNT; i++) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextUnformatted(CONTROLS[i].label);
                        for (int col = 0; col < 2; col++) {
                            ImGui::TableNextColumn();
                            bool pad = col == 1, waiting = capture.control == i && capture.pad == pad;
                            std::string label = waiting ? "press..." : pad ? pad_name(s.pad[i]) : key_name(s.keys[i]);
                            label += "##" + std::to_string(i * 2 + col);
                            if (ImGui::Button(label.c_str(), {-1, 0})) capture = {i, pad};
                            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) { // right click clears
                                if (pad) s.pad[i] = PAD_NONE;
                                else s.keys[i] = SDL_SCANCODE_UNKNOWN;
                            }
                        }
                    }
                    ImGui::EndTable();
                }
                ImGui::TextDisabled("Right-click a binding to clear it.");
                if (ImGui::Button("Reset to defaults")) s.reset_controls();
            }
            if (sub_tab(2)) {
                section("Volume");
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
                ImGui::SliderInt("##volume", &s.volume, 0, 100, "%d%%");
                ImGui::Checkbox("Mute when the game window isn't in front", &s.mute_in_background);
                ImGui::Checkbox("Music in this launcher", &s.menu_music);
                ImGui::TextDisabled("Blasters' version-select theme, from your Blasters dump; or your own menu-music.wav "
                                    "in the data folder.");
                section("Output");
                std::string current = s.audio_device.empty() ? "System default" : s.audio_device;
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 22);
                if (ImGui::BeginCombo("Device", current.c_str())) {
                    if (ImGui::Selectable("System default", s.audio_device.empty())) s.audio_device.clear();
                    for (const std::string &d : audio_devices)
                        if (ImGui::Selectable(d.c_str(), s.audio_device == d)) s.audio_device = d;
                    ImGui::EndCombo();
                }
                if (!s.audio_device.empty() &&
                    std::find(audio_devices.begin(), audio_devices.end(), s.audio_device) == audio_devices.end())
                    ImGui::TextColored({0.85f, 0.45f, 0.05f, 1}, "Not connected now: the default is used until it is.");
                ImGui::Checkbox("Mono (left and right mixed together)", &s.mono);
                ImGui::TextDisabled("For one earbud, or a single speaker.");
                section("Latency");
                static const char *LATENCY[] = {"Low (25 ms)", "Normal (50 ms)", "High (100 ms)"};
                int latency = std::clamp(s.audio_latency, 0, 2);
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
                if (ImGui::Combo("##latency", &latency, LATENCY, 3)) s.audio_latency = latency;
                ImGui::TextDisabled("Lower answers sooner; higher crackles less when the PC is busy.");
            }
            }
            }
        }
        ImGui::EndChild();

        // Play, gold like the games' confirm buttons, and Quit
        ImGui::Dummy({0, ImGui::GetFrameHeight() * 0.25f});
        float fh = ImGui::GetFrameHeight() * 1.9f;
        std::string play_label = (dump_ok ? "Play  " + dump_name : std::string("Play")) + "##play";
        if (bar_button(play_label.c_str(), {ImGui::GetContentRegionAvail().x * 0.74f, fh}, GOLD_BAR, dump_ok)) play = true;
        ImGui::SameLine(0, style.ItemSpacing.x * 2);
        if (bar_button("Quit", {ImGui::GetContentRegionAvail().x, fh}, LILAC_BAR)) quit = true;
        ImGui::End();

        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, Uint8(BACKGROUND.x * 255), Uint8(BACKGROUND.y * 255), Uint8(BACKGROUND.z * 255), 255);
        SDL_RenderClear(renderer);
        // ImGui works in points; on high-density displays the renderer must scale them to pixels
        // (the backend only scales its clip rectangles)
        SDL_SetRenderScale(renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_SetRenderScale(renderer, 1, 1);
        if (shot && ++frame % 4 == 3) {
            if (SDL_Surface *surface = SDL_RenderReadPixels(renderer, nullptr)) {
                SDL_SavePNG(surface, (std::string(shot) + SHOT_NAMES[shot_tab] + ".png").c_str());
                SDL_DestroySurface(surface);
            }
            if (++shot_tab == int(std::size(SHOT_NAMES))) std::_Exit(0);
        }
        SDL_RenderPresent(renderer);
    }

    menu_music::stop();
    stop_title_capture(title_job);
    s.save();
    for (SDL_Texture *t : title_images)
        if (t) SDL_DestroyTexture(t);
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    for (SDL_Gamepad *g : pads) SDL_CloseGamepad(g);
    return play;
}

} // namespace frontend
