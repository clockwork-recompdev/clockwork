#pragma once
// Save data in the launcher: importing a save made on a 3DS (or elsewhere) for one game.
#include <cstdint>
#include <filesystem>
#include <string>

namespace frontend {

// the game's save folder, and whether it has a save yet
std::filesystem::path save_folder(uint64_t program_id);
bool has_save(uint64_t program_id);

// Replaces the game's save with the files in folder `from`: a save manager's backup (Checkpoint,
// JKSM), or another emulator's save folder. The current save is kept in save-backups/ first.
// `message` says what happened, or why nothing did.
bool import_save(uint64_t program_id, const std::filesystem::path &from, std::string &message);

} // namespace frontend
