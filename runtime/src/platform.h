#pragma once
// The few things that differ per OS.
#include <cstdint>
#include <filesystem>

namespace platform {
// per-user folder for saves and settings (created on first use):
// Linux $XDG_DATA_HOME/yw2-recomp or ~/.local/share/yw2-recomp,
// macOS ~/Library/Application Support/yw2-recomp, Windows %APPDATA%\yw2-recomp
std::filesystem::path data_dir();
// a game's own data (its save, secure values) in the data folder: games/<program id>
std::filesystem::path game_dir(uint64_t program_id);
// which of the Clockwork processes running at once this is (0 for the first): a lock held for the
// process's life. Two windows on one PC playing together are told apart by it
int instance_slot();
// this console's unique id (config blocks 0x00090000-2, and what identities derive from), in the
// format a 3DS uses: a 16-bit random number at bit 48 and the 35-bit local friend code seed below.
// Made once and kept in the data folder; another window running at the same time gets its own
// (games ignore a player whose console is their own)
uint64_t console_id();
} // namespace platform
