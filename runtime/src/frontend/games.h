#pragma once
// The games Clockwork knows about: the Yo-kai Watch engine titles on the 3DS, in release order,
// and the dumps it recognizes. Each recompiled game is its own executable (see CMakeLists.txt);
// a game can be played when the executable for one of its dumps is built.
#include <cstdint>
#include <cstring>

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

// a dump Clockwork recognizes, by the program id in its header
struct KnownDump {
    uint64_t program_id;
    const char *game;    // GameInfo::id
    const char *version; // empty if the game has just the one
    const char *binary;  // the executable with its recompiled code
};

inline constexpr KnownDump KNOWN_DUMPS[] = {
    {0x0004000000167700, "yw1", "", "yw1"},                          // USA
    {0x000400000019A900, "yw2", "Bony Spirits", "yw2bf"},     // USA
    {0x000400000019AA00, "yw2", "Fleshy Souls", "yw2bf"},     // USA
    {0x00040000001B2700, "yw2", "Psychic Specters", "yw2"},   // USA
    {0x00040000001CEB00, "blasters", "Red Cat Corps", "blasters"},   // USA
    {0x00040000001CEF00, "blasters", "White Dog Squad", "blasters"}, // USA
    {0x00040000001D6700, "yw3", "", "yw3"},                          // USA
    {0x00040000001C9400, "busters2", "Sword", "busters2"},           // Japan (Rev 1)
    {0x00040000001C9C00, "busters2", "Magnum", "busters2"},          // Japan (Rev 1)
};

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
