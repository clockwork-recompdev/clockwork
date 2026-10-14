#include "gamebanana.h"

#include "json.h"
#include "log.h"
#include "mods.h"
#include "platform.h"

#include <archive.h>
#include <archive_entry.h>
#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace fs = std::filesystem;

namespace gamebanana {
namespace {

const char *const API = "https://gamebanana.com/apiv11";

void init_curl() {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

// common settings: HTTPS only (redirects too), a user agent, timeouts
CURL *handle(const std::string &url) {
    init_curl();
    CURL *c = curl_easy_init();
    if (!c) return nullptr;
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(c, CURLOPT_USERAGENT, "Clockwork (Yo-kai Watch recompilation; github.com/clockwork-recompdev/clockwork)");
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L); // give up on a stalled transfer
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
    return c;
}

bool get_json(const std::string &url, json::Value &out, std::string &error) {
    CURL *c = handle(url);
    if (!c) return error = "Couldn't start a download.", false;
    std::string body;
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, +[](char *p, size_t s, size_t n, void *u) {
        static_cast<std::string *>(u)->append(p, s * n);
        return s * n;
    });
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
    CURLcode r = curl_easy_perform(c);
    curl_easy_cleanup(c);
    if (r != CURLE_OK) return error = std::string("GameBanana didn't answer: ") + curl_easy_strerror(r), false;
    out = json::parse(body);
    if (out.kind != json::Value::OBJECT) return error = "GameBanana's answer wasn't readable.", false;
    return true;
}

// a folder name from a mod's name: letters, digits and dashes
std::string slug(const std::string &name) {
    std::string s;
    for (char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c))) s += char(std::tolower(static_cast<unsigned char>(c)));
        else if (!s.empty() && s.back() != '-') s += '-';
        if (s.size() >= 40) break;
    }
    while (!s.empty() && s.back() == '-') s.pop_back();
    return s.empty() ? "mod" : s;
}

// unpacks `archive` into `to`, refusing anything that could escape it (absolute paths, "..",
// links) and anything but plain files and folders
bool extract(const fs::path &archive, const fs::path &to, std::string &error) {
    struct archive *a = archive_read_new();
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);
    if (archive_read_open_filename(a, archive.string().c_str(), 1 << 16) != ARCHIVE_OK) {
        error = std::string("Couldn't open the download: ") + archive_error_string(a);
        archive_read_free(a);
        return false;
    }
    struct archive *w = archive_write_disk_new();
    // entries are written to `to` / their (checked) name, an absolute path, so libarchive's own
    // absolute-path check can't be used; names are checked below instead
    archive_write_disk_set_options(w, ARCHIVE_EXTRACT_SECURE_NODOTDOT | ARCHIVE_EXTRACT_SECURE_SYMLINKS);
    bool ok = true;
    struct archive_entry *e;
    int r;
    while ((r = archive_read_next_header(a, &e)) == ARCHIVE_OK) {
        mode_t type = archive_entry_filetype(e);
        if (type != AE_IFREG && type != AE_IFDIR) continue; // no links, devices
        std::string name = archive_entry_pathname(e) ? archive_entry_pathname(e) : "";
        std::replace(name.begin(), name.end(), '\\', '/'); // archives made on Windows
        if (name.empty() || name[0] == '/' || name.find("..") != std::string::npos || name.find(':') != std::string::npos)
            continue; // absolute, escaping, or a drive letter
        archive_entry_set_pathname(e, (to / name).string().c_str());
        archive_entry_set_perm(e, type == AE_IFDIR ? 0755 : 0644);
        if (archive_write_header(w, e) != ARCHIVE_OK) {
            ok = false, error = std::string("Couldn't unpack ") + name + ": " + archive_error_string(w);
            break;
        }
        if (type == AE_IFREG) {
            const void *buf;
            size_t size;
            la_int64_t offset;
            while ((r = archive_read_data_block(a, &buf, &size, &offset)) == ARCHIVE_OK)
                if (archive_write_data_block(w, buf, size, offset) != ARCHIVE_OK) break;
            if (r != ARCHIVE_EOF) {
                ok = false, error = std::string("Couldn't unpack ") + name + ": " + archive_error_string(a);
                break;
            }
        }
        archive_write_finish_entry(w);
    }
    if (ok && r != ARCHIVE_EOF) ok = false, error = std::string("The download is damaged: ") + archive_error_string(a);
    archive_write_free(w);
    archive_read_free(a);
    return ok;
}

std::string lower(std::string s) {
    for (char &c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// readmes, pictures and links that come with mods: not game files
bool is_extra(const std::string &ext) {
    for (const char *x : {".txt", ".md", ".png", ".jpg", ".jpeg", ".gif", ".url", ".pdf", ".json", ".html", ".htm"})
        if (ext == x) return true;
    return false;
}

// the first folder named `name` under `root`, any case ("Include"), breadth first: the shallowest
fs::path find_folder(const fs::path &root, const char *name) {
    std::vector<fs::path> level{root};
    for (int depth = 0; depth < 6 && !level.empty(); depth++) {
        std::vector<fs::path> next;
        std::error_code ec;
        for (const fs::path &d : level)
            for (auto &e : fs::directory_iterator(d, ec)) {
                if (!e.is_directory(ec)) continue;
                if (lower(e.path().filename().string()) == name) return e.path();
                next.push_back(e.path());
            }
        level = std::move(next);
    }
    return {};
}

} // namespace

int game_id(const std::string &game) {
    if (game == "yw1") return 10248;
    if (game == "yw2") return 16730;
    if (game == "blasters") return 17273;
    if (game == "yw3") return 16705;
    if (game == "busters2") return 17683;
    return 0;
}

bool list_mods(int game, int page, std::vector<Mod> &out, bool &more, std::string &error) {
    json::Value v;
    char url[256];
    std::snprintf(url, sizeof url, "%s/Mod/Index?_aFilters%%5BGeneric_Game%%5D=%d&_nPerpage=20&_nPage=%d", API, game,
                  page);
    if (!get_json(url, v, error)) return false;
    for (const json::Value &r : v["_aRecords"].arr) {
        Mod m;
        m.id = r["_idRow"].integer();
        m.name = r["_sName"].string("(no name)");
        m.category = r["_aRootCategory"]["_sName"].string();
        m.submitter = r["_aSubmitter"]["_sName"].string();
        m.likes = r["_nLikeCount"].integer();
        m.views = r["_nViewCount"].integer();
        if (m.id) out.push_back(std::move(m));
    }
    more = !v["_aMetadata"]["_bIsComplete"].b;
    return true;
}

bool mod_files(long long mod, std::vector<File> &out, std::string &error) {
    json::Value v;
    if (!get_json(std::string(API) + "/Mod/" + std::to_string(mod) + "/ProfilePage", v, error)) return false;
    for (const json::Value &f : v["_aFiles"].arr) {
        File file;
        file.id = f["_idRow"].integer();
        file.name = f["_sFile"].string();
        file.description = f["_sDescription"].string();
        file.url = f["_sDownloadUrl"].string();
        file.size = f["_nFilesize"].integer();
        if (file.url.rfind("https://", 0) == 0) out.push_back(std::move(file));
    }
    if (out.empty()) error = "That mod has no files to download.";
    return !out.empty();
}

bool install(const std::string &game, const Mod &mod, const File &file, std::atomic<float> &progress,
             std::string &error) {
    std::error_code ec;
    fs::path work = platform::data_dir() / "mods" / ".downloads" / std::to_string(file.id);
    fs::remove_all(work, ec);
    fs::create_directories(work / "unpacked", ec);
    fs::path download = work / "download";
    progress = 0;

    { // download
        std::ofstream out(download, std::ios::binary);
        CURL *c = handle(file.url);
        if (!c || !out) return error = "Couldn't start the download.", false;
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, +[](char *p, size_t s, size_t n, void *u) {
            return static_cast<std::ofstream *>(u)->write(p, std::streamsize(s * n)) ? s * n : 0;
        });
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &out);
        curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(c, CURLOPT_XFERINFODATA, &progress);
        curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION,
                         +[](void *u, curl_off_t total, curl_off_t now, curl_off_t, curl_off_t) {
                             if (total > 0) *static_cast<std::atomic<float> *>(u) = 0.9f * float(now) / float(total);
                             return 0;
                         });
        CURLcode r = curl_easy_perform(c);
        curl_easy_cleanup(c);
        if (r != CURLE_OK) {
            fs::remove_all(work, ec);
            return error = std::string("The download failed: ") + curl_easy_strerror(r), false;
        }
    }
    if (!extract(download, work / "unpacked", error)) {
        fs::remove_all(work, ec);
        if (is_extra(lower(fs::path(file.name).extension().string())))
            error = "This download is only instructions or a link, not the mod's files. See its GameBanana page.";
        return false;
    }
    progress = 0.95f;

    // what the game can use: include/ (inside its archives) and romfs/ (whole files); failing those,
    // loose game files, matched to the archives' paths when the game starts (see mods.h)
    fs::path unpacked = work / "unpacked";
    fs::path include = find_folder(unpacked, "include"), romfs = find_folder(unpacked, "romfs"), loose;
    if (include.empty() && romfs.empty()) {
        // what's there: game data, emulator texture packs, or just instructions
        int data = 0, textures = 0;
        for (auto &e : fs::recursive_directory_iterator(unpacked, ec)) {
            if (!e.is_regular_file(ec)) continue;
            std::string name = lower(e.path().filename().string()), ext = lower(e.path().extension().string());
            if (name.rfind("tex1_", 0) == 0 && ext == ".png") textures++;
            else if (!is_extra(ext)) data++;
        }
        if (textures && !data)
            error = "This is a texture pack for the Citra/Azahar emulators (tex1_... images). Clockwork can't use "
                    "those yet.";
        else if (!data)
            error = "This download is only instructions or a link (readme files), not the mod's files. See its "
                    "GameBanana page.";
        else
            loose = unpacked; // game files without include/: matched by path when the game starts
        if (loose.empty()) return fs::remove_all(work, ec), false;
    }
    fs::path dest = mods::game_folder(game) / (std::to_string(mod.id) + "-" + slug(mod.name));
    fs::remove_all(dest, ec); // reinstalling
    fs::create_directories(dest, ec);
    if (!include.empty()) fs::rename(include, dest / "include", ec);
    if (!romfs.empty() && !ec) fs::rename(romfs, dest / "romfs", ec);
    if (!loose.empty() && !ec) {
        fs::rename(loose, dest / "loose", ec);
        // readmes and pictures aren't game files
        std::vector<fs::path> extra;
        for (auto &e : fs::recursive_directory_iterator(dest / "loose", ec)) {
            if (e.is_regular_file() && is_extra(lower(e.path().extension().string()))) extra.push_back(e.path());
        }
        for (auto &x : extra) fs::remove(x, ec);
        ec.clear();
    }
    if (ec) {
        fs::remove_all(dest, ec);
        fs::remove_all(work, ec);
        return error = "Couldn't move the mod into place.", false;
    }
    std::ofstream(dest / "mod.ini") << "name=" << mod.name << "\ngamebanana=" << mod.id << "\nfile=" << file.name
                                    << "\nsubmitter=" << mod.submitter << "\n";
    fs::remove_all(work, ec);
    LOG("mods: installed %s into %s", mod.name.c_str(), dest.string().c_str());
    progress = 1;
    return true;
}

} // namespace gamebanana
