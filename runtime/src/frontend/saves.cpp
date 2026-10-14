#include "saves.h"

#include "log.h"
#include "platform.h"

#include <ctime>

namespace fs = std::filesystem;

namespace frontend {
namespace {

// 3DS cartridge saves are at most 8 MiB (most are 512 KiB); anything bigger is the wrong folder
constexpr uintmax_t MAX_SAVE_BYTES = 8u << 20;

bool is_secure_value(const fs::path &p) { return p.filename().string().starts_with("save_secure_value_"); }

std::string timestamp() {
    char buf[32];
    std::time_t now = std::time(nullptr);
    std::strftime(buf, sizeof buf, "%Y%m%d-%H%M%S", std::localtime(&now));
    return buf;
}

} // namespace

fs::path save_folder(uint64_t program_id) { return platform::game_dir(program_id) / "save"; }

bool has_save(uint64_t program_id) {
    std::error_code ec;
    return !fs::is_empty(save_folder(program_id), ec) && !ec;
}

bool import_save(uint64_t program_id, const fs::path &from, std::string &message) {
    std::error_code ec;
    const fs::path game = platform::game_dir(program_id), save = save_folder(program_id);
    if (!fs::is_directory(from, ec)) {
        message = "Choose the folder that holds the save's files.";
        return false;
    }
    if (fs::equivalent(from, save, ec)) {
        message = "That's this game's save folder already.";
        return false;
    }
    // what's in it: files at the top (a save manager's backup has its files there)
    size_t top_files = 0;
    uintmax_t bytes = 0;
    for (auto it = fs::recursive_directory_iterator(from, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        if (it.depth() == 0) top_files++;
        bytes += it->file_size(ec);
    }
    if (ec) {
        message = "Couldn't read " + from.string() + ": " + ec.message();
        return false;
    }
    if (!top_files) {
        message = "That folder has no files in it. Choose the folder with the save files themselves "
                  "(with Checkpoint, one backup's folder).";
        return false;
    }
    if (bytes > MAX_SAVE_BYTES) {
        message = "That folder holds " + std::to_string(bytes >> 20) + " MiB: too much for a save. Choose the "
                  "folder with just the save files.";
        return false;
    }

    // copied next to the save first, so a failure leaves the current one untouched
    fs::path incoming = game / "save.importing";
    fs::remove_all(incoming, ec);
    fs::create_directories(game, ec);
    fs::copy(from, incoming, fs::copy_options::recursive, ec);
    if (ec) {
        fs::remove_all(incoming, ec);
        message = "Couldn't copy the save: " + ec.message();
        return false;
    }
    // the current save, and its secure values, go to save-backups/<when>
    fs::path backup;
    if (has_save(program_id)) {
        backup = game / "save-backups" / timestamp();
        for (int n = 2; fs::exists(backup, ec); n++) // two imports within a second
            backup = game / "save-backups" / (timestamp() + "-" + std::to_string(n));
        fs::create_directories(backup, ec);
        fs::rename(save, backup / "save", ec);
        if (ec) {
            fs::remove_all(incoming, ec);
            message = "Couldn't set the current save aside: " + ec.message();
            return false;
        }
    }
    fs::remove_all(save, ec); // an empty folder
    // Secure values tie a save to the console it was made on, to stop old saves being restored;
    // without one the game takes the save it finds (as on a console with a fresh cartridge)
    for (auto &e : fs::directory_iterator(game, ec))
        if (is_secure_value(e.path())) {
            std::error_code ec2;
            if (!backup.empty()) fs::rename(e.path(), backup / e.path().filename(), ec2);
            else fs::remove(e.path(), ec2);
        }
    fs::rename(incoming, save, ec);
    if (ec) {
        message = "Couldn't put the save in place: " + ec.message() + ". It was copied to " + incoming.string() +
                  (backup.empty() ? "." : "; your previous save is in " + backup.string() + ".");
        return false;
    }
    LOG("saves: imported %s into %s%s%s", from.string().c_str(), save.string().c_str(),
        backup.empty() ? "" : "; the old save is in ", backup.string().c_str());
    message = "Save imported (" + std::to_string(top_files) + " file" + (top_files == 1 ? "" : "s") + ").";
    if (!backup.empty()) message += " Your previous save is kept in " + backup.string() + ".";
    return true;
}

} // namespace frontend
