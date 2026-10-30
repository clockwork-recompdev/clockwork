#pragma once
// GameBanana (gamebanana.com): lists a game's mods and installs one into the game's mods folder
// (see mods.h). Uses its public API; no account. Everything here blocks: call it off the UI thread.
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace gamebanana {

// the GameBanana game id for one of ours ("yw2"), or 0
int game_id(const std::string &game);

struct Mod {
    long long id = 0;
    std::string name, category, submitter;
    long long likes = 0, views = 0;
};
// a page (from 1) of a game's mods, newest first; false (and `error`) if it couldn't be fetched
bool list_mods(int game, int page, std::vector<Mod> &out, bool &more, std::string &error);

struct File {
    long long id = 0;
    std::string name, description, url;
    long long size = 0;
};
bool mod_files(long long mod, std::vector<File> &out, std::string &error);

// Downloads `file` and installs it as mods/<game>/<mod id>-<name>/, keeping what the game can use:
// its include/ (files inside the .fa archives) and romfs/ folders, wherever they are in the
// archive. `progress` goes from 0 to 1. False (and `error`, readable) if it can't be installed.
bool install(const std::string &game, const Mod &mod, const File &file, std::atomic<float> &progress,
             std::string &error);

// Downloads an archive (https) and unpacks it into `to` (emptied first). `progress` goes from 0 to 1.
bool fetch_archive(const std::string &url, const std::filesystem::path &to, std::atomic<float> &progress,
                   std::string &error);

} // namespace gamebanana
