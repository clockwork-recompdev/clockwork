// The launcher: shown in the game window before the game starts. Lists the games, picks the
// dump, and edits display, controls and audio settings; Play saves them and starts the game.
#include "launcher.h"

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

// looks in `folder` (not below it) for dumps Clockwork recognizes, and remembers them
void find_dumps(Settings &s, const std::filesystem::path &folder) {
    std::error_code ec;
    for (auto &e : std::filesystem::directory_iterator(folder, ec)) {
        std::string ext = e.path().extension().string();
        for (char &c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
        if ((ext != ".3ds" && ext != ".cci" && ext != ".cxi") || !e.is_regular_file(ec)) continue;
        if (auto id = peek_program_id(e.path()); id && find_dump(*id)) s.dumps[*id] = e.path().string();
    }
}

// forgets dumps that are gone, and finds new ones next to the ones known
void refresh_dumps(Settings &s) {
    for (auto it = s.dumps.begin(); it != s.dumps.end();) it = dump_exists(it->second) ? std::next(it) : s.dumps.erase(it);
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

// Yo-kai Watch's colors: white, with a strong blue
const ImVec4 BLUE = {0.12f, 0.45f, 0.85f, 1}, BLUE_DARK = {0.06f, 0.25f, 0.56f, 1}, BLUE_LIGHT = {0.62f, 0.82f, 0.99f, 1};
const ImVec4 BACKGROUND = {0.96f, 0.975f, 1.0f, 1}, PANEL = {1, 1, 1, 1}, PANEL_HOVER = {0.90f, 0.95f, 1.0f, 1};
const ImVec4 TEXT = {0.08f, 0.14f, 0.26f, 1}, TEXT_DIM = {0.42f, 0.49f, 0.60f, 1}, BORDER = {0.80f, 0.87f, 0.95f, 1};
const ImVec4 WHITE = {1, 1, 1, 1}, WARN = {0.85f, 0.45f, 0.05f, 1};

ImVec4 mix(ImVec4 a, ImVec4 b, float t) {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t};
}

ImU32 col(ImVec4 c) { return ImGui::GetColorU32(c); }

void apply_theme(ImGuiStyle &style) {
    ImGui::StyleColorsLight(&style);
    style.WindowRounding = 0, style.ChildRounding = 6, style.FrameRounding = 6, style.PopupRounding = 6;
    style.GrabRounding = 6, style.TabRounding = 6, style.ScrollbarRounding = 6;
    style.WindowPadding = {14, 12}, style.FramePadding = {8, 5}, style.ItemSpacing = {8, 6};
    style.WindowBorderSize = 0, style.FrameBorderSize = 1, style.TabBarBorderSize = 2, style.SeparatorTextBorderSize = 1;
    ImVec4 *c = style.Colors;
    c[ImGuiCol_Text] = TEXT, c[ImGuiCol_TextDisabled] = TEXT_DIM;
    c[ImGuiCol_WindowBg] = c[ImGuiCol_PopupBg] = BACKGROUND, c[ImGuiCol_ChildBg] = {0, 0, 0, 0};
    c[ImGuiCol_Border] = BORDER, c[ImGuiCol_Separator] = BORDER;
    c[ImGuiCol_FrameBg] = PANEL, c[ImGuiCol_FrameBgHovered] = PANEL_HOVER, c[ImGuiCol_FrameBgActive] = mix(PANEL_HOVER, BLUE, 0.15f);
    c[ImGuiCol_Button] = PANEL, c[ImGuiCol_ButtonHovered] = PANEL_HOVER, c[ImGuiCol_ButtonActive] = mix(PANEL_HOVER, BLUE, 0.25f);
    c[ImGuiCol_Header] = mix(PANEL, BLUE, 0.12f), c[ImGuiCol_HeaderHovered] = mix(PANEL, BLUE, 0.2f);
    c[ImGuiCol_HeaderActive] = mix(PANEL, BLUE, 0.3f);
    c[ImGuiCol_Tab] = PANEL, c[ImGuiCol_TabHovered] = PANEL_HOVER;
    c[ImGuiCol_TabSelected] = BLUE, c[ImGuiCol_TabSelectedOverline] = BLUE_DARK;
    c[ImGuiCol_TabDimmed] = PANEL, c[ImGuiCol_TabDimmedSelected] = mix(PANEL, BLUE, 0.5f);
    c[ImGuiCol_CheckMark] = BLUE, c[ImGuiCol_SliderGrab] = BLUE, c[ImGuiCol_SliderGrabActive] = BLUE_DARK;
    c[ImGuiCol_TableHeaderBg] = PANEL_HOVER, c[ImGuiCol_TableRowBg] = PANEL, c[ImGuiCol_TableRowBgAlt] = mix(PANEL, BACKGROUND, 0.8f);
    c[ImGuiCol_TableBorderLight] = BORDER;
    c[ImGuiCol_ScrollbarBg] = {0, 0, 0, 0}, c[ImGuiCol_ScrollbarGrab] = BORDER;
    c[ImGuiCol_NavCursor] = BLUE, c[ImGuiCol_TextSelectedBg] = mix(PANEL, BLUE, 0.3f);
}

void section(const char *title) {
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, BLUE);
    ImGui::SeparatorText(title);
    ImGui::PopStyleColor();
}

// the logo: a clock face inside a gear, hands at ten past ten
void draw_logo(ImDrawList *dl, ImVec2 c, float r) {
    const ImU32 rim = col(WHITE), face = col(BLUE), mark = col(BLUE_LIGHT), hands = col(WHITE);
    constexpr int TEETH = 10;
    constexpr float PI = 3.14159265f;
    for (int i = 0; i < TEETH; i++) { // trapezoid teeth around the rim
        float a = 2 * PI * float(i) / TEETH, wo = PI / TEETH * 0.45f, wi = PI / TEETH * 0.7f;
        auto at = [&](float ang, float rad) { return ImVec2{c.x + std::cos(ang) * rad, c.y + std::sin(ang) * rad}; };
        dl->AddQuadFilled(at(a - wi, r * 0.8f), at(a - wo, r), at(a + wo, r), at(a + wi, r * 0.8f), rim);
    }
    dl->AddCircleFilled(c, r * 0.84f, rim, 48);
    dl->AddCircleFilled(c, r * 0.66f, face, 48);
    for (int i = 0; i < 12; i++) { // hour marks
        float a = 2 * PI * float(i) / 12, len = i % 3 == 0 ? 0.16f : 0.08f;
        ImVec2 o{std::cos(a), std::sin(a)};
        dl->AddLine({c.x + o.x * r * (0.58f - len), c.y + o.y * r * (0.58f - len)}, {c.x + o.x * r * 0.58f, c.y + o.y * r * 0.58f},
                    i % 3 == 0 ? hands : mark, std::max(1.0f, r * 0.05f));
    }
    auto hand = [&](float hours_angle, float len, float width) {
        float a = hours_angle - PI / 2;
        dl->AddLine(c, {c.x + std::cos(a) * r * len, c.y + std::sin(a) * r * len}, hands, width);
    };
    hand(2 * PI * 10.17f / 12, 0.32f, std::max(1.5f, r * 0.09f)); // hour hand at 10
    hand(2 * PI * 2 / 12, 0.48f, std::max(1.0f, r * 0.06f));     // minute hand at 2
    dl->AddCircleFilled(c, r * 0.08f, hands, 16);
}

// text with a drop outline, like the games' logos
void outlined_text(ImDrawList *dl, float size, ImVec2 p, ImU32 fill, ImU32 outline, const char *text) {
    float o = std::max(1.0f, size * 0.07f);
    for (int dx = -1; dx <= 1; dx++)
        for (int dy = -1; dy <= 1; dy++)
            if (dx || dy) dl->AddText(nullptr, size, {p.x + dx * o, p.y + dy * o}, outline, text);
    dl->AddText(nullptr, size, p, fill, text);
}

// a game with no title-screen image of its own: its name on a blue sky
void draw_placeholder(ImDrawList *dl, ImVec2 a, ImVec2 b, const GameInfo &g, float rounding) {
    dl->AddRectFilledMultiColor(a, b, col(BLUE_LIGHT), col(BLUE_LIGHT), col(BLUE), col(BLUE));
    // round the top corners back into the background
    dl->AddRect({a.x - 1, a.y - 1}, {b.x + 1, b.y + 1}, col(PANEL), rounding + 1, ImDrawFlags_RoundCornersTop, 2.5f);
    std::string rest = g.title;
    const std::string brand = "Yo-kai Watch";
    if (rest.rfind(brand, 0) == 0) rest = rest.substr(brand.size());
    while (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
    float w = b.x - a.x, h = b.y - a.y, small = h * 0.16f, big = h * 0.26f;
    ImVec2 bs = ImGui::GetFont()->CalcTextSizeA(small, FLT_MAX, 0, "YO-KAI WATCH");
    float big_w = rest.empty() ? 0 : ImGui::GetFont()->CalcTextSizeA(big, FLT_MAX, 0, rest.c_str()).x;
    if (big_w > w * 0.9f) big *= w * 0.9f / big_w, big_w = w * 0.9f; // fit long names
    float total = small + (rest.empty() ? 0 : big);
    float y = a.y + (h - total) / 2;
    outlined_text(dl, small, {a.x + (w - bs.x) / 2, y}, col(WHITE), col(BLUE_DARK), "YO-KAI WATCH");
    if (!rest.empty()) outlined_text(dl, big, {a.x + (w - big_w) / 2, y + small}, col(WHITE), col(BLUE_DARK), rest.c_str());
}

// a game in the library: its title screen, name and versions, and whether it can be played yet
bool game_tile(int i, bool selected, bool playable, bool have_dump, SDL_Texture *image, float width) {
    const GameInfo &g = GAMES[i];
    ImGuiStyle &style = ImGui::GetStyle();
    float line = ImGui::GetTextLineHeight(), pad = style.FramePadding.x, rounding = style.FrameRounding;
    float image_h = width * 3 / 5; // the 3DS top screen: 400x240
    float h = image_h + pad * 1.5f + line * 4 + pad; // the name, then up to three lines of versions
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(i);
    bool clicked = ImGui::InvisibleButton("tile", {width, h}) && playable;
    bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();
    if (hovered)
        ImGui::SetItemTooltip("%s%s%s%s%s%s", g.title, *g.subtitle ? "\n" : "", g.subtitle, *g.note ? "\n" : "", g.note,
                               !playable ? "\nNot supported yet" : have_dump ? "" : "\nNo dump found: Browse for it below");

    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 q{p.x + width, p.y + h}, img_b{q.x, p.y + image_h};
    dl->AddRectFilled({p.x + 1, p.y + 2}, {q.x + 1, q.y + 2}, col({0.06f, 0.2f, 0.45f, 0.08f}), rounding); // shadow
    dl->AddRectFilled(p, q, col(hovered && playable ? PANEL_HOVER : PANEL), rounding);
    if (image) { // cover the area, cropping the image to its shape
        float iw = 0, ih = 0;
        SDL_GetTextureSize(image, &iw, &ih);
        float want = width / image_h, have = iw / ih;
        ImVec2 uv0{0, 0}, uv1{1, 1};
        if (have > want) uv0.x = (1 - want / have) / 2, uv1.x = 1 - uv0.x;
        else uv0.y = (1 - have / want) / 2, uv1.y = 1 - uv0.y;
        dl->AddImageRounded(ImTextureID(intptr_t(image)), p, img_b, uv0, uv1, col(WHITE), rounding, ImDrawFlags_RoundCornersTop);
    } else {
        draw_placeholder(dl, p, img_b, g, rounding);
    }
    if (!playable) dl->AddRectFilled(p, img_b, col({1, 1, 1, 0.45f}), rounding, ImDrawFlags_RoundCornersTop);

    // status pill over the image's corner
    const char *status = !playable ? "Not imported" : have_dump ? "Ready" : "No dump";
    ImVec2 ts = ImGui::CalcTextSize(status);
    float pw = ts.x + pad * 1.6f, ph = ts.y + style.FramePadding.y * 0.8f;
    ImVec2 pp{q.x - pad - pw, p.y + pad};
    dl->AddRectFilled(pp, {pp.x + pw, pp.y + ph}, col(!playable ? mix(TEXT_DIM, WHITE, 0.2f) : have_dump ? BLUE : mix(BLUE, WHITE, 0.45f)), ph / 2);
    dl->AddText({pp.x + (pw - ts.x) / 2, pp.y + (ph - ts.y) / 2}, col(WHITE), status);

    // name, then the versions, kept inside the tile (the tooltip has them in full)
    float x = p.x + pad, y = img_b.y + pad * 0.75f;
    dl->PushClipRect({x, y}, {q.x - pad, q.y - pad * 0.5f}, true);
    dl->AddText({x, y}, col(playable ? TEXT : TEXT_DIM), g.title);
    float below = y + line;
    if (*g.note) { // a warning, before the versions
        dl->AddText(nullptr, 0, {x, below}, col(WARN), g.note, nullptr, width - pad * 2);
        below += ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, width - pad * 2, g.note).y;
    }
    if (*g.subtitle) dl->AddText(nullptr, 0, {x, below}, col(TEXT_DIM), g.subtitle, nullptr, width - pad * 2);
    dl->PopClipRect();
    if (selected) dl->AddRect(p, q, col(BLUE), rounding, 0, 2.5f);
    else dl->AddRect(p, q, col(BORDER), rounding);
    return clicked;
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
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
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
    static const SDL_DialogFileFilter DUMP_TYPES[] = {{"3DS game dumps", "3ds;cci;cxi"}, {"All files", "*"}};

    // YW2_LAUNCHER_SCREENSHOT=<prefix>: saves each tab as a PNG and quits (for testing the UI)
    const char *shot = std::getenv("YW2_LAUNCHER_SCREENSHOT");
    int frame = 0, shot_tab = 0;
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

    // title-screen images: titles/<id>.png in the data folder, supplied by the player (none ship)
    std::error_code ec;
    const std::filesystem::path title_dir = platform::data_dir() / "titles";
    std::filesystem::create_directories(title_dir, ec);
    std::vector<SDL_Texture *> title_images(GAME_COUNT, nullptr);
    for (int i = 0; i < GAME_COUNT; i++) {
        std::filesystem::path png = title_dir / (std::string(GAMES[i].id) + ".png");
        if (!std::filesystem::is_regular_file(png, ec)) continue;
        if (SDL_Surface *surface = SDL_LoadPNG(png.string().c_str())) {
            title_images[i] = SDL_CreateTextureFromSurface(renderer, surface);
            SDL_DestroySurface(surface);
            if (title_images[i]) SDL_SetTextureScaleMode(title_images[i], SDL_SCALEMODE_LINEAR);
        }
    }

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
            if (e.type == SDL_EVENT_GAMEPAD_ADDED)
                if (SDL_Gamepad *g = SDL_OpenGamepad(e.gdevice.which)) pads.push_back(g);
        }
        {
            std::lock_guard lk(picked_lock);
            if (!picked.empty()) { // remember its folder, and find the other dumps there
                s.game_path = picked;
                s.dump_folder = std::filesystem::path(picked).parent_path().string();
                if (auto id = peek_program_id(picked); id && find_dump(*id)) s.dumps[*id] = picked;
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
                if (!d || !dump_exists(path) || build_tried.count(d->binary) || !needs_build(*d)) continue;
                std::vector<std::pair<uint64_t, std::string>> versions; // every version sharing the executable
                for (auto &[other, other_path] : s.dumps)
                    if (const KnownDump *o = find_dump(other); o && std::strcmp(o->binary, d->binary) == 0 && dump_exists(other_path))
                        versions.push_back({other, other_path});
                build_tried.insert(d->binary);
                start_build(build_job, d->binary, versions);
                building = true, build_binary = d->binary;
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
        bool chosen_building = building && dump_route.dump && build_binary == dump_route.dump->binary;
        bool dump_ok = (dump_route.kind == Route::HERE || dump_route.kind == Route::OTHER) && !chosen_building;
        std::string dump_name; // "Yo-kai Watch 2: Bony Spirits"
        if (dump_route.dump) {
            const GameInfo *g = find_game(dump_route.dump->game);
            std::string v = version_label(*dump_route.dump);
            dump_name = std::string(g ? g->title : "?") + (v.empty() ? "" : ": ") + v;
        }

        ask_relay(rooms_state, s);
        rescale();
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("launcher", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);

        { // header: a blue band with the logo and name
            ImDrawList *dl = ImGui::GetWindowDrawList();
            float title_size = ImGui::GetFontSize() * 2.2f, logo = title_size * 1.25f;
            ImVec2 p = ImGui::GetCursorScreenPos(), win = ImGui::GetWindowPos();
            float band_bottom = p.y + logo + style.WindowPadding.y;
            dl->AddRectFilledMultiColor(win, {win.x + ImGui::GetWindowWidth(), band_bottom}, col(BLUE), col(BLUE),
                                        col(BLUE_DARK), col(BLUE_DARK));
            draw_logo(dl, {p.x + logo / 2, p.y + logo / 2}, logo / 2);
            float x = p.x + logo + style.ItemSpacing.x * 1.5f;
            outlined_text(dl, title_size, {x, p.y + (logo - title_size) / 2}, col(WHITE), col(BLUE_DARK), "CLOCKWORK");
            ImGui::Dummy({ImGui::GetContentRegionAvail().x, band_bottom - p.y});
            ImGui::Spacing();
        }

        float footer = ImGui::GetFrameHeightWithSpacing() * 2.2f;
        ImGui::BeginChild("tabs", {0, -footer});
        if (ImGui::BeginTabBar("top")) {
            auto tab = [&](int i) {
                return ImGui::BeginTabItem(TABS[i], nullptr,
                                           shot && SHOT_TOP[shot_tab] == i && frame % 4 == 0 ? ImGuiTabItemFlags_SetSelected : 0);
            };
            auto sub_tab = [&](int i) {
                return ImGui::BeginTabItem(SETTINGS_TABS[i], nullptr,
                                           shot && SHOT_SUB[shot_tab] == i && frame % 4 == 0 ? ImGuiTabItemFlags_SetSelected : 0);
            };
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
                        if (game_tile(i, chosen, playable[i], !found.empty(), title_images[i], width) && !chosen) {
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
                ImGui::TextWrapped("Title screens: put %s.png, %s.png ... in %s", GAMES[0].id, GAMES[1].id,
                                   title_dir.string().c_str());
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered())
                    ImGui::SetItemTooltip("yw1.png, yw2.png, blasters.png, yw3.png, busters2.png (any size; a 400x240 "
                                          "screenshot fits best). Read when the launcher opens.");
                section("Your game dumps");
                ImGui::TextWrapped("Decrypted dumps of your own cartridges (.3ds, .cci or .cxi). Choose one, and the "
                                   "others in its folder are found too.");
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
                else if (!chosen_building && !(build_failed && dump_route.dump && build_binary == dump_route.dump->binary))
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
                ImGui::EndTabItem();
            }
            if (tab(1)) {
                const GameInfo *g = dump_route.dump ? find_game(dump_route.dump->game) : nullptr;
                mods_tab(mods_state, g ? g->id : "", g ? g->title : "", installed);
                ImGui::EndTabItem();
            }
            if (tab(2)) {
                rooms_tab(rooms_state, s, dump_route.dump ? dump_route.dump->game : "");
                ImGui::EndTabItem();
            }
            if (tab(3)) {
            if (ImGui::BeginTabBar("settings")) {
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
                ImGui::Checkbox("VSync", &s.vsync);
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18);
                ImGui::Combo("Scaling", &s.filter, FILTERS, 3);
                section("In-game keys");
                ImGui::TextDisabled("F6 adaptive layout   F7 widescreen   F8 picture-in-picture   F9 one/two screens");
                ImGui::TextDisabled("F12 saves a capture (screenshot + scene info) to the captures folder");
                ImGui::TextDisabled("Tab or right stick swap screens   F11 fullscreen");
                ImGui::EndTabItem();
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
                ImGui::EndTabItem();
            }
            if (sub_tab(2)) {
                section("Volume");
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
                ImGui::SliderInt("##volume", &s.volume, 0, 100, "%d%%");
                ImGui::Checkbox("Mute when the game window isn't in front", &s.mute_in_background);
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
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar(); // settings
            }
            ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::EndChild();

        ImGui::Separator();
        ImGui::BeginDisabled(!dump_ok);
        ImGui::PushStyleColor(ImGuiCol_Button, BLUE);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, mix(BLUE, BLUE_LIGHT, 0.3f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, BLUE_DARK);
        ImGui::PushStyleColor(ImGuiCol_Text, WHITE);
        std::string play_label = dump_ok ? "Play " + dump_name : std::string("Play");
        if (ImGui::Button(play_label.c_str(), {ImGui::GetContentRegionAvail().x * 0.7f, ImGui::GetFrameHeight() * 1.6f}))
            play = true;
        ImGui::PopStyleColor(4);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Quit", {-1, ImGui::GetFrameHeight() * 1.6f})) quit = true;
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
