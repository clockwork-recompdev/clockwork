#pragma once
// Mods, applied to the RomFS when a game starts (the dump itself is never changed). In the data
// folder:
//   mods/<game>/<mod>/          installed mods for a game ("yw2"), in name order; a mod is off while
//                               it has a file named "disabled". Each may have:
//     include/<path>            files inside the game's .fa archives ("data/res/..."), as
//                               GameBanana's Yo-kai Watch mods ship them (with a ywml.json)
//     romfs/<path>              whole RomFS files (LayeredFS-style, as translations ship)
//     loose/<path>              game files without their full path (some mods ship "t131d20/x.cfg.bin"):
//                               each matched to the one archive path ending with it
//   mods/<program id>/romfs/    whole RomFS files for one version, applied last
// Later mods win where two change the same file.
#include <filesystem>
#include <string>

class Dump;

namespace mods {

// the folder of a game's installed mods
std::filesystem::path game_folder(const std::string &game);

void apply(Dump &dump, const std::string &game, uint64_t program_id);

} // namespace mods
