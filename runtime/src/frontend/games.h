#pragma once
// The games Clockwork knows about: the Yo-kai Watch engine titles on the 3DS, in release order,
// and the dumps it recognizes. Each recompiled game is its own executable (see CMakeLists.txt);
// a game can be played when the executable for one of its dumps is built.
#include <cstdint>
#include <cstring>
#include <string>

namespace frontend {

struct GameInfo {
    const char *id;       // short name: its title-screen image is titles/<id>.png in the data folder
    const char *title;    // as shown in the launcher
    const char *subtitle; // its versions; empty if just the one
    const char *note = ""; // a warning shown with it (empty: none)
};

inline constexpr GameInfo GAMES[] = {
    {"yw1", "Yo-kai Watch", ""},
    {"yw2", "Yo-kai Watch 2", "Bony Spirits / Fleshy Souls / Psychic Specters"},
    {"blasters", "Yo-kai Watch Blasters", "Red Cat Corps / White Dog Squad"},
    {"yw3", "Yo-kai Watch 3", "Sushi / Tempura / Sukiyaki"},
    {"busters2", "Yo-kai Watch Busters 2", "Sword / Magnum"},
};
inline constexpr int GAME_COUNT = int(sizeof GAMES / sizeof GAMES[0]);

// the region a dump was released for: what the console it runs on says about itself (see cfg)
enum class Region : uint8_t { JPN, USA, EUR, KOR };

// a dump Clockwork recognizes, by the program id in its header. Each region's release is its own
// code (laid out differently), so it has its own executable; versions released together in one
// region share one, as on the USA side
struct KnownDump {
    uint64_t program_id;
    const char *game;    // GameInfo::id
    const char *version; // empty if the game has just the one
    const char *binary;  // the executable with its recompiled code
    Region region = Region::USA;
};

inline constexpr KnownDump KNOWN_DUMPS[] = {
    // USA
    {0x0004000000167700, "yw1", "", "yw1"},
    {0x000400000019A900, "yw2", "Bony Spirits", "yw2bf"},
    {0x000400000019AA00, "yw2", "Fleshy Souls", "yw2bf"},
    {0x00040000001B2700, "yw2", "Psychic Specters", "yw2"},
    {0x00040000001CEB00, "blasters", "Red Cat Corps", "blasters"},
    {0x00040000001CEF00, "blasters", "White Dog Squad", "blasters"},
    {0x00040000001D6700, "yw3", "", "yw3"},
    // Europe
    {0x0004000000167800, "yw1", "", "yw1_eur", Region::EUR},
    {0x000400000019AB00, "yw2", "Bony Spirits", "yw2bf_eur", Region::EUR},
    {0x000400000019AC00, "yw2", "Fleshy Souls", "yw2bf_eur", Region::EUR},
    {0x00040000001B2900, "yw2", "Psychic Specters", "yw2_eur", Region::EUR},
    {0x00040000001CEC00, "blasters", "Red Cat Corps", "blasters_eur", Region::EUR},
    {0x00040000001CF000, "blasters", "White Dog Squad", "blasters_eur", Region::EUR},
    {0x00040000001D6800, "yw3", "", "yw3_eur", Region::EUR},
    // Japan (Ganso, Honke and Shin'uchi are Bony Spirits, Fleshy Souls and Psychic Specters; Busters
    // is Blasters)
    {0x00040000000CF400, "yw1", "", "yw1_jpn", Region::JPN},
    {0x000400000012F900, "yw2", "Bony Spirits", "yw2bf_jpn", Region::JPN},
    {0x000400000012F800, "yw2", "Fleshy Souls", "yw2bf_jpn", Region::JPN},
    {0x0004000000155100, "yw2", "Psychic Specters", "yw2_jpn", Region::JPN},
    {0x000400000016C700, "blasters", "Red Cat Corps", "blasters_jpn", Region::JPN},
    {0x000400000016C600, "blasters", "White Dog Squad", "blasters_jpn", Region::JPN},
    {0x0004000000191000, "yw3", "Sushi", "yw3_jpn", Region::JPN},
    {0x0004000000191100, "yw3", "Tempura", "yw3_jpn", Region::JPN},
    {0x00040000001AF400, "yw3", "Sukiyaki", "yw3sk_jpn", Region::JPN},
    {0x00040000001C9400, "busters2", "Sword", "busters2", Region::JPN},  // Rev 1
    {0x00040000001C9C00, "busters2", "Magnum", "busters2", Region::JPN}, // Rev 1
    // Korea
    {0x0004000000167600, "yw1", "", "yw1_kor", Region::KOR},
    {0x000400000019AE00, "yw2", "Bony Spirits", "yw2bf_kor", Region::KOR},
    {0x000400000019AF00, "yw2", "Fleshy Souls", "yw2bf_kor", Region::KOR},
    {0x00040000001B2A00, "yw2", "Psychic Specters", "yw2_kor", Region::KOR},
    {0x00040000001CED00, "blasters", "Red Cat Corps", "blasters_kor", Region::KOR},
    {0x00040000001CF200, "blasters", "White Dog Squad", "blasters_kor", Region::KOR},
};

inline const char *region_name(Region r) {
    switch (r) {
    case Region::JPN: return "Japan";
    case Region::EUR: return "Europe";
    case Region::KOR: return "Korea";
    default: return "USA";
    }
}

// whether its labels name its region: for a game released in more than one, every region but the USA
inline bool region_noted(const KnownDump &d) {
    for (const KnownDump &o : KNOWN_DUMPS)
        if (std::strcmp(o.game, d.game) == 0 && o.region != d.region) return d.region != Region::USA;
    return false;
}

// "Psychic Specters", "Psychic Specters (Europe)", "Europe" (a game with one version), or ""
inline std::string version_label(const KnownDump &d) {
    std::string v = d.version;
    if (!region_noted(d)) return v;
    return v.empty() ? region_name(d.region) : v + " (" + region_name(d.region) + ")";
}

inline const KnownDump *find_dump(uint64_t program_id) {
    for (const KnownDump &d : KNOWN_DUMPS)
        if (d.program_id == program_id) return &d;
    return nullptr;
}

inline const GameInfo *find_game(const char *id) {
    for (const GameInfo &g : GAMES)
        if (std::strcmp(g.id, id) == 0) return &g;
    return nullptr;
}

} // namespace frontend
