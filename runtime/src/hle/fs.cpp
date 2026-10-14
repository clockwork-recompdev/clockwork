// fs:USER: archives and files. RomFS reads come straight from the player's dump; save data
// lives in a folder in the user's data directory.
#include "service.h"

#include "dump.h"
#include "log.h"
#include "memory.h"
#include "platform.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace hle {
namespace {

// result codes (module FS)
constexpr uint32_t ERR_FILE_NOT_FOUND = 0xC8804470;
constexpr uint32_t ERR_PATH_NOT_FOUND = 0xC8804478;
constexpr uint32_t ERR_NOT_FORMATTED = 0xC8A04554;
constexpr uint32_t ERR_ALREADY_EXISTS = 0xC82044BE;
constexpr uint32_t ERR_DIR_ALREADY_EXISTS = 0xC82044B9;
constexpr uint32_t ERR_ARCHIVE_NOT_FOUND = 0xC8804465;
constexpr uint32_t ERR_UNSUPPORTED = 0xE0C04702;
constexpr uint32_t ERR_EXTDATA_NOT_FOUND = 0xC8A04478; // games create their extdata on this

enum ArchiveId : uint32_t {
    SELF_NCCH = 0x00000003,
    SAVE_DATA = 0x00000004,
    EXT_SAVE_DATA = 0x00000006,
    SHARED_EXT_SAVE_DATA = 0x00000007,
    SYSTEM_SAVE_DATA = 0x00000008,
    SDMC = 0x00000009,
    NCCH = 0x2345678A,
};

enum PathType : uint32_t { PATH_INVALID = 0, PATH_EMPTY = 1, PATH_BINARY = 2, PATH_ASCII = 3, PATH_UTF16 = 4 };

struct Path {
    uint32_t type;
    std::vector<uint8_t> data;

    std::string text() const {
        std::string s;
        if (type == PATH_ASCII) {
            for (uint8_t c : data)
                if (c) s.push_back(char(c));
        } else if (type == PATH_UTF16) {
            for (size_t i = 0; i + 1 < data.size(); i += 2) {
                uint16_t c = uint16_t(data[i] | (data[i + 1] << 8));
                if (!c) break;
                // save paths are ASCII in practice; keep anything else visible but harmless
                s.push_back(c < 0x80 ? char(c) : '_');
            }
        }
        return s;
    }
};

Path read_path(uint32_t type, uint32_t size, uint32_t addr) {
    Path p{type, {}};
    p.data.assign(mem::ptr(addr), mem::ptr(addr) + size);
    return p;
}

// Maps a guest path under `root`, refusing anything that would escape it.
std::optional<fs::path> host_path(const fs::path &root, const Path &p) {
    fs::path out = root;
    std::string text = p.text();
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('/', start);
        if (end == std::string::npos) end = text.size();
        std::string part = text.substr(start, end - start);
        if (part == "..") return std::nullopt;
        if (!part.empty() && part != ".") out /= part;
        start = end + 1;
    }
    return out;
}

// ---- files ----

struct File : Service {
    // RomFS-backed (read-only) or a host file
    bool romfs = false;
    std::fstream host;
    fs::path host_path;

    File() : Service("fs file") {}

    uint64_t size() {
        if (romfs) return dump().romfs_size();
        return fs::file_size(host_path);
    }

    void handle(Request &req) override {
        switch (req.id()) {
        case 0x0802: { // Read(offset, size) -> bytes read, into a mapped buffer
            uint64_t offset = uint64_t(req.arg(0)) | (uint64_t(req.arg(1)) << 32);
            uint32_t len = req.arg(2), dest = req.arg(4);
            uint64_t got;
            if (romfs) {
                got = dump().read_romfs(offset, mem::ptr(dest), len);
            } else {
                host.clear();
                host.seekg(std::streamoff(offset));
                host.read(reinterpret_cast<char *>(mem::ptr(dest)), len);
                got = uint64_t(host.gcount());
            }
            LOG_IPC("fs Read %s %#llx+%#x -> %#llx", romfs ? "romfs" : "file", (unsigned long long)offset, len,
                    (unsigned long long)got);
            uint32_t desc = req.arg(3);
            auto r = req.reply(2, 2);
            r.push(uint32_t(got));
            r.push(desc);
            r.push(dest);
            break;
        }
        case 0x0803: { // Write(offset, size, flush flags) from a mapped buffer -> bytes written
            uint64_t offset = uint64_t(req.arg(0)) | (uint64_t(req.arg(1)) << 32);
            uint32_t len = req.arg(2), desc = req.arg(4), src = req.arg(5);
            if (romfs) {
                req.reply(1, 0, ERR_UNSUPPORTED);
                break;
            }
            host.clear();
            host.seekp(std::streamoff(offset));
            host.write(reinterpret_cast<const char *>(mem::ptr(src)), len);
            if (req.arg(3) & 1) host.flush();
            auto r = req.reply(2, 2);
            r.push(len);
            r.push(desc);
            r.push(src);
            break;
        }
        case 0x0804: { // GetSize
            auto r = req.reply(3, 0);
            r.push64(size());
            break;
        }
        case 0x0805: { // SetSize(size)
            uint64_t n = uint64_t(req.arg(0)) | (uint64_t(req.arg(1)) << 32);
            if (!romfs) {
                host.flush();
                fs::resize_file(host_path, n);
            }
            req.reply(1, 0);
            break;
        }
        case 0x0808: // Close
            host.close();
            req.reply(1, 0);
            break;
        case 0x0809: // Flush
            host.flush();
            req.reply(1, 0);
            break;
        case 0x080A: // SetPriority
            req.reply(1, 0);
            break;
        case 0x080B: { // GetPriority
            auto r = req.reply(2, 0);
            r.push(0);
            break;
        }
        case 0x080C: { // OpenLinkFile -> another session to this same file
            auto r = req.reply(1, 2);
            auto self = std::static_pointer_cast<File>(shared_from_this());
            r.push_handles({kernel::add_handle(std::make_shared<kernel::Session>(self))}, true);
            break;
        }
        default:
            unimplemented(*this, req);
        }
    }
};

// ---- directories ----

struct Directory : Service {
    struct Entry {
        std::string name;
        bool dir;
        uint64_t size;
    };
    std::vector<Entry> entries;
    size_t next = 0;

    Directory() : Service("fs directory") {}

    void handle(Request &req) override {
        switch (req.id()) {
        case 0x0801: { // Read(count) -> entries read, into a mapped buffer of 0x228-byte entries
            uint32_t count = req.arg(0), desc = req.arg(1), dest = req.arg(2), n = 0;
            for (; n < count && next < entries.size(); n++, next++) {
                uint8_t *e = mem::ptr(dest + n * 0x228);
                std::memset(e, 0, 0x228);
                const Entry &en = entries[next];
                std::string name = en.name;
                for (size_t i = 0; i < name.size() && i < 0x105; i++) {
                    e[i * 2] = uint8_t(name[i]); // UTF-16LE
                }
                // 8.3 short name and extension, upper case
                std::string stem = fs::path(name).stem().string(), ext = fs::path(name).extension().string();
                if (!ext.empty()) ext = ext.substr(1);
                for (size_t i = 0; i < 8 && i < stem.size(); i++) e[0x20C + i] = uint8_t(std::toupper(stem[i]));
                for (size_t i = 0; i < 3 && i < ext.size(); i++) e[0x215 + i] = uint8_t(std::toupper(ext[i]));
                e[0x219] = 1;
                bool dir = en.dir;
                e[0x21B] = dir;
                uint64_t size = dir ? 0 : en.size;
                std::memcpy(e + 0x220, &size, 8);
            }
            auto r = req.reply(2, 2);
            r.push(n);
            r.push(desc);
            r.push(dest);
            break;
        }
        case 0x0802: // Close
            req.reply(1, 0);
            break;
        default:
            unimplemented(*this, req);
        }
    }
};

// ---- archives ----

struct VersionFamily;

struct Archive {
    uint32_t id;
    fs::path root; // empty for RomFS-backed archives
    // another game's link data, whose missing versions are filled in from the ones there
    const VersionFamily *versions = nullptr;
};

// shared extdata holding the system-wide Play Coin count
constexpr uint32_t PLAY_COINS_EXTDATA = 0xF000000B;

void create_play_coins(const fs::path &root) {
    fs::path file = root / "gamecoin.dat";
    if (fs::exists(file)) return;
    fs::create_directories(root);
    // magic, total coins, coins today, total steps, steps today, last date (year, month, day)
    struct {
        uint32_t magic = 0x4F00;
        uint16_t total_coins = 42, coins_today = 0;
        uint32_t total_steps = 0, steps_today = 0;
        uint16_t year = 2014;
        uint8_t month = 12, day = 29;
    } coins;
    std::ofstream(file, std::ios::binary).write(reinterpret_cast<const char *>(&coins), sizeof coins);
    LOG("fs: created %s", file.string().c_str());
}

// Each game's save data (and its secure values) is kept apart, by program id, under games/<id> in
// the data folder. What lives on the SD card stays at the top and is shared, as on the console:
// extdata is found by its own id, and the three versions of Yo-kai Watch 2 use the same one.
// Before there was more than one game, Psychic Specters' save was at the top.
constexpr uint64_t PSYCHIC_SPECTERS = 0x00040000001B2700;

// Region bridge for links: games read other games' link data from those games' extdata, by id, and
// a Japanese release looks for the Japanese releases' ids. When an id isn't there, its other
// region's game is used instead, if that one's export is. Experimental: the two regions' exports may
// not match.
struct ExtdataBridge {
    uint32_t wanted, instead;
    const char *what;
};
constexpr ExtdataBridge EXTDATA_BRIDGES[] = {
    {0x16C7, 0x1CEB, "Busters (Japan) -> Blasters (USA)"},
    {0x1910, 0x1D67, "Yo-kai Watch 3 (Japan) -> Yo-kai Watch 3 (USA)"},
};

// Link data: each version of a game exports its own folder in the game's extdata ("yw-b_a" Red Cat
// Corps, "yw-b_b" White Dog Squad). A game linking with it is shown every version: a version you
// haven't played reads as one you have, so one copy of a game gives every version's link. Only
// for reading another game's link data; a game writing its own exports always writes its own.
struct VersionFamily {
    uint32_t extdata;
    const char *prefix, *letters; // folders prefix + letter
    uint32_t own[4];              // the extdata ids of the games that export here (unique ids)
};
constexpr VersionFamily VERSION_FAMILIES[] = {
    {0x19A9, "yw2_", "abc", {0x19A9, 0x19AA, 0x1B27}},  // Bony Spirits, Fleshy Souls, Psychic Specters
    {0x1CEB, "yw-b_", "ab", {0x1CEB, 0x1CEF}},          // Red Cat Corps, White Dog Squad
    {0x1D67, "yw3_", "abc", {0x1D67}},                  // Yo-kai Watch 3
};

// `p` (under the archive's root), or the same file in another version's folder if `p` isn't there
fs::path fill_version(const Archive &a, const fs::path &p) {
    std::error_code ec;
    if (!a.versions || fs::exists(p, ec)) return p;
    fs::path rel = fs::relative(p, a.root, ec);
    auto it = rel.begin();
    if (ec || it == rel.end()) return p;
    std::string first = it->string(), prefix = a.versions->prefix;
    if (first.size() != prefix.size() + 1 || first.compare(0, prefix.size(), prefix) != 0) return p;
    fs::path rest;
    for (++it; it != rel.end(); ++it) rest /= *it;
    for (const char *l = a.versions->letters; *l; l++) {
        fs::path other = a.root / (prefix + *l) / rest;
        if (fs::exists(other, ec)) return other;
    }
    return p;
}

// moves the save from before per-game folders into Psychic Specters' (the only game there was)
void move_old_saves() {
    fs::path top = platform::data_dir(), to = platform::game_dir(PSYCHIC_SPECTERS);
    std::error_code ec;
    if (!fs::exists(top / "save", ec)) return;
    if (fs::exists(to / "save", ec)) {
        LOG("fs: both %s and %s exist; leaving the old one alone", (top / "save").string().c_str(),
            (to / "save").string().c_str());
        return;
    }
    fs::create_directories(to, ec);
    auto move = [&](const fs::path &from, const fs::path &dest) {
        fs::rename(from, dest, ec);
        if (ec) LOG("fs: could not move %s: %s", from.string().c_str(), ec.message().c_str());
    };
    std::vector<std::pair<fs::path, fs::path>> moves{{top / "save", to / "save"}}; // listed first, then moved
    for (auto &e : fs::directory_iterator(top, ec))
        if (e.path().filename().string().starts_with("save_secure_value_"))
            moves.push_back({e.path(), to / e.path().filename()});
    for (auto &[from, dest] : moves) move(from, dest);
    LOG("fs: moved Psychic Specters' save data to %s", to.string().c_str());
}

// this game's own data folder
const fs::path &game_root() {
    static const fs::path root = [] {
        move_old_saves();
        fs::path r = platform::game_dir(dump().program_id());
        fs::create_directories(r);
        return r;
    }();
    return root;
}

struct FsUser : Service {
    std::map<uint64_t, Archive> archives;
    uint64_t next_archive = 1;

    FsUser() : Service("fs:USER") {}

    fs::path save_root() const { return game_root() / "save"; }
    // SD-card extra data, by extdata id
    fs::path extdata_root(uint32_t low, uint32_t high) const {
        char name[24];
        std::snprintf(name, sizeof name, "%08x%08x", high, low);
        return platform::data_dir() / "extdata" / name;
    }
    fs::path secure_value_path(uint32_t slot) const {
        return game_root() / ("save_secure_value_" + std::to_string(slot) + ".bin");
    }

    // opens archive `id`; result code on failure
    uint32_t open_archive(uint32_t id, const Path &path, Archive *out) {
        switch (id) {
        case SELF_NCCH:
        case NCCH:
            *out = {id, {}};
            return kernel::RESULT_OK;
        case SAVE_DATA:
            if (!fs::exists(save_root())) return ERR_NOT_FORMATTED;
            *out = {id, save_root()};
            return kernel::RESULT_OK;
        case EXT_SAVE_DATA: { // binary path: media type, extdata id low, high
            uint32_t w[3] = {};
            std::memcpy(w, path.data.data(), std::min<size_t>(path.data.size(), sizeof w));
            fs::path root = extdata_root(w[1], w[2]);
            if (!fs::exists(root) && w[2] == 0)
                for (const ExtdataBridge &b : EXTDATA_BRIDGES)
                    if (w[1] == b.wanted && fs::exists(extdata_root(b.instead, 0))) {
                        LOG("fs: extdata %08x isn't here; linking with %08x instead (%s)", b.wanted, b.instead, b.what);
                        root = extdata_root(b.instead, 0);
                    }
            if (!fs::exists(root)) return ERR_EXTDATA_NOT_FOUND;
            *out = {id, root};
            uint32_t self = uint32_t(dump().program_id() >> 8) & 0xFFFFF; // the running game's unique id
            for (const VersionFamily &v : VERSION_FAMILIES)
                if (w[2] == 0 && root == extdata_root(v.extdata, 0) &&
                    std::find(std::begin(v.own), std::end(v.own), self) == std::end(v.own))
                    out->versions = &v;
            return kernel::RESULT_OK;
        }
        case SDMC: {
            fs::path root = platform::data_dir() / "sdmc";
            fs::create_directories(root);
            *out = {id, root};
            return kernel::RESULT_OK;
        }
        case SHARED_EXT_SAVE_DATA: {
            // binary path: media type, extdata id (u64)
            uint32_t ext_id = path.data.size() >= 8 ? uint32_t(path.data[4] | (path.data[5] << 8) |
                                                                (path.data[6] << 16) | (path.data[7] << 24))
                                                     : 0;
            char name[16];
            std::snprintf(name, sizeof name, "%08x", ext_id);
            fs::path root = platform::data_dir() / "extdata" / "shared" / name;
            if (ext_id == PLAY_COINS_EXTDATA) create_play_coins(root);
            if (!fs::exists(root)) {
                LOG("fs: shared extdata %08x not present", ext_id);
                return ERR_ARCHIVE_NOT_FOUND;
            }
            *out = {id, root};
            return kernel::RESULT_OK;
        }
        default: {
            std::string bytes;
            for (uint8_t b : path.data) {
                char buf[4];
                std::snprintf(buf, sizeof buf, "%02x", b);
                bytes += buf;
            }
            LOG("fs: archive %#x (path type %u: %s) not supported; reporting it as not found", id, path.type,
                bytes.c_str());
            return ERR_ARCHIVE_NOT_FOUND;
        }
        }
    }

    Archive *archive(uint32_t lo, uint32_t hi) {
        auto it = archives.find(uint64_t(lo) | (uint64_t(hi) << 32));
        return it == archives.end() ? nullptr : &it->second;
    }

    // opens a file in an archive; result code, and the file on success
    uint32_t open_file(const Archive &a, const Path &path, uint32_t flags, std::shared_ptr<File> *out) {
        auto f = std::make_shared<File>();
        if (a.root.empty()) {
            // RomFS: binary path whose first word is the content type (0 = RomFS)
            uint32_t type = 0;
            if (path.data.size() >= 4) std::memcpy(&type, path.data.data(), 4);
            if (type != 0) {
                LOG("fs: NCCH content type %u not supported", type);
                return ERR_FILE_NOT_FOUND;
            }
            f->romfs = true;
            *out = f;
            return kernel::RESULT_OK;
        }
        auto p = host_path(a.root, path);
        if (!p) return ERR_PATH_NOT_FOUND;
        bool write = flags & 2, create = flags & 4;
        if (!write && !create) p = fill_version(a, *p);
        if (!fs::exists(*p)) {
            if (!create) return ERR_FILE_NOT_FOUND;
            std::ofstream(*p, std::ios::binary);
        }
        f->host_path = *p;
        f->host.open(*p, std::ios::binary | std::ios::in | (write ? std::ios::out : std::ios::openmode{}));
        if (!f->host) return ERR_FILE_NOT_FOUND;
        *out = f;
        return kernel::RESULT_OK;
    }

    void reply_file(Request &req, uint32_t result, const std::shared_ptr<File> &f, const char *what,
                    const Path &path) {
        LOG_IPC("fs %s \"%s\" -> %08x", what, path.type == PATH_BINARY ? "<binary>" : path.text().c_str(), result);
        if (result != kernel::RESULT_OK) {
            req.reply(1, 0, result);
            return;
        }
        auto r = req.reply(1, 2);
        r.push_handles({kernel::add_handle(std::make_shared<kernel::Session>(f))}, true);
    }

    void handle(Request &req) override {
        switch (req.id()) {
        case 0x0801: // Initialize
        case 0x0861: // InitializeWithSdkVersion
        case 0x0862: // SetPriority
            req.reply(1, 0);
            break;
        case 0x0863: { // GetPriority
            auto r = req.reply(2, 0);
            r.push(0);
            break;
        }
        case 0x080C: { // OpenArchive(id, path type, size, path) -> archive handle
            Archive a;
            Path path = read_path(req.arg(1), req.arg(2), req.arg(4));
            uint32_t result = open_archive(req.arg(0), path, &a);
            if (req.arg(0) == EXT_SAVE_DATA && path.data.size() >= 12) { // which game's extdata
                uint32_t low, high;
                std::memcpy(&low, path.data.data() + 4, 4);
                std::memcpy(&high, path.data.data() + 8, 4);
                LOG_IPC("fs OpenArchive %#x (extdata %08x%08x) -> %08x", req.arg(0), high, low, result);
            } else {
                LOG_IPC("fs OpenArchive %#x -> %08x", req.arg(0), result);
            }
            if (result != kernel::RESULT_OK) {
                req.reply(1, 0, result);
                break;
            }
            uint64_t h = next_archive++;
            archives[h] = a;
            auto r = req.reply(3, 0);
            r.push64(h);
            break;
        }
        case 0x080D: // ControlArchive(archive, action, ...): action 0 commits save data, already on disk
            req.reply(1, 0);
            break;
        case 0x080E: // CloseArchive(handle)
            archives.erase(uint64_t(req.arg(0)) | (uint64_t(req.arg(1)) << 32));
            req.reply(1, 0);
            break;
        case 0x0802: { // OpenFile(transaction, archive, path type, size, flags, attributes, path)
            Archive *a = archive(req.arg(1), req.arg(2));
            Path path = read_path(req.arg(3), req.arg(4), req.arg(8));
            std::shared_ptr<File> f;
            uint32_t result = a ? open_file(*a, path, req.arg(5), &f) : kernel::RESULT_INVALID_HANDLE;
            reply_file(req, result, f, "OpenFile", path);
            break;
        }
        case 0x0803: { // OpenFileDirectly(transaction, archive id, archive path, file path, flags, attrs)
            Archive a;
            Path apath = read_path(req.arg(2), req.arg(3), req.arg(9));
            Path fpath = read_path(req.arg(4), req.arg(5), req.arg(11));
            uint32_t result = open_archive(req.arg(1), apath, &a);
            std::shared_ptr<File> f;
            if (result == kernel::RESULT_OK) result = open_file(a, fpath, req.arg(6), &f);
            reply_file(req, result, f, "OpenFileDirectly", fpath);
            break;
        }
        case 0x0804:   // DeleteFile(transaction, archive, path type, size, path)
        case 0x0806:   // DeleteDirectory
        case 0x0807: { // DeleteDirectoryRecursively
            Archive *a = archive(req.arg(1), req.arg(2));
            Path path = read_path(req.arg(3), req.arg(4), req.arg(6));
            auto p = a && !a->root.empty() ? host_path(a->root, path) : std::nullopt;
            uint32_t result;
            if (!p || !fs::exists(*p)) {
                result = req.id() == 0x0804 ? ERR_FILE_NOT_FOUND : ERR_PATH_NOT_FOUND;
            } else {
                std::error_code ec;
                if (req.id() == 0x0807) fs::remove_all(*p, ec);
                else fs::remove(*p, ec);
                result = ec ? ERR_PATH_NOT_FOUND : kernel::RESULT_OK;
            }
            LOG_IPC("fs Delete \"%s\" -> %08x", path.text().c_str(), result);
            req.reply(1, 0, result);
            break;
        }
        case 0x0805:   // RenameFile(transaction, src archive, src type, src size, dst archive, dst type, dst size)
        case 0x080A: { // RenameDirectory
            Archive *sa = archive(req.arg(1), req.arg(2)), *da = archive(req.arg(5), req.arg(6));
            Path src = read_path(req.arg(3), req.arg(4), req.arg(10));
            Path dst = read_path(req.arg(7), req.arg(8), req.arg(12));
            auto sp = sa && !sa->root.empty() ? host_path(sa->root, src) : std::nullopt;
            auto dp = da && !da->root.empty() ? host_path(da->root, dst) : std::nullopt;
            uint32_t result = ERR_PATH_NOT_FOUND;
            if (sp && dp && fs::exists(*sp)) {
                if (fs::exists(*dp)) {
                    result = ERR_ALREADY_EXISTS;
                } else {
                    std::error_code ec;
                    fs::rename(*sp, *dp, ec);
                    result = ec ? ERR_PATH_NOT_FOUND : kernel::RESULT_OK;
                }
            }
            req.reply(1, 0, result);
            break;
        }
        case 0x0808: { // CreateFile(transaction, archive, path type, size, attributes, file size, path)
            Archive *a = archive(req.arg(1), req.arg(2));
            Path path = read_path(req.arg(3), req.arg(4), req.arg(9));
            uint64_t size = uint64_t(req.arg(6)) | (uint64_t(req.arg(7)) << 32);
            auto p = a && !a->root.empty() ? host_path(a->root, path) : std::nullopt;
            uint32_t result;
            if (!p || !fs::exists(p->parent_path())) {
                result = ERR_PATH_NOT_FOUND;
            } else if (fs::exists(*p)) {
                result = ERR_ALREADY_EXISTS;
            } else {
                std::ofstream(*p, std::ios::binary);
                fs::resize_file(*p, size);
                result = kernel::RESULT_OK;
            }
            LOG_IPC("fs CreateFile \"%s\" (%llu bytes) -> %08x", path.text().c_str(), (unsigned long long)size,
                    result);
            req.reply(1, 0, result);
            break;
        }
        case 0x0809: { // CreateDirectory(transaction, archive, path type, size, attributes, path)
            Archive *a = archive(req.arg(1), req.arg(2));
            Path path = read_path(req.arg(3), req.arg(4), req.arg(7));
            auto p = a && !a->root.empty() ? host_path(a->root, path) : std::nullopt;
            uint32_t result = !p                 ? ERR_PATH_NOT_FOUND
                              : fs::exists(*p)   ? ERR_DIR_ALREADY_EXISTS
                              : fs::create_directory(*p) ? kernel::RESULT_OK
                                                         : ERR_PATH_NOT_FOUND;
            req.reply(1, 0, result);
            break;
        }
        case 0x080B: { // OpenDirectory(archive, path type, size, path) -> directory
            Archive *a = archive(req.arg(0), req.arg(1));
            Path path = read_path(req.arg(2), req.arg(3), req.arg(5));
            auto p = a && !a->root.empty() ? host_path(a->root, path) : std::nullopt;
            if (p) p = fill_version(*a, *p);
            if (!p || !fs::is_directory(*p)) {
                req.reply(1, 0, ERR_PATH_NOT_FOUND);
                break;
            }
            auto d = std::make_shared<Directory>();
            std::error_code ec;
            for (auto &e : fs::directory_iterator(*p, ec))
                d->entries.push_back({e.path().filename().string(), e.is_directory(ec), e.is_directory(ec) ? 0 : e.file_size(ec)});
            // another game's link data: every version's folder, if any one is there
            if (a->versions && fs::equivalent(*p, a->root, ec)) {
                std::string prefix = a->versions->prefix;
                bool any = std::any_of(d->entries.begin(), d->entries.end(), [&](auto &e) {
                    return e.dir && e.name.size() == prefix.size() + 1 && e.name.compare(0, prefix.size(), prefix) == 0;
                });
                for (const char *l = a->versions->letters; any && *l; l++) {
                    std::string name = prefix + *l;
                    if (std::none_of(d->entries.begin(), d->entries.end(), [&](auto &e) { return e.name == name; })) {
                        d->entries.push_back({name, true, 0});
                        LOG("fs: link data: %s shown from another version", name.c_str());
                    }
                }
            }
            auto r = req.reply(1, 2);
            r.push_handles({kernel::add_handle(std::make_shared<kernel::Session>(d))}, true);
            break;
        }
        case 0x0812: { // GetFreeBytes(archive)
            auto r = req.reply(3, 0);
            r.push64(512ull << 20);
            break;
        }
        case 0x0817:   // IsSdmcDetected: the user data folder stands in for the SD card
        case 0x0818: { // IsSdmcWritable
            auto r = req.reply(2, 0);
            r.push(1);
            break;
        }
        case 0x0851: { // CreateExtSaveData(media, id low, id high, unknown, dirs, files, size lo/hi, icon size, icon)
            fs::path root = extdata_root(req.arg(1), req.arg(2));
            std::error_code ec;
            fs::create_directories(root, ec);
            LOG("fs: created extra save data %s", root.string().c_str());
            req.reply(1, 0);
            break;
        }
        case 0x0852: { // DeleteExtSaveData(media, id low, id high, unknown)
            std::error_code ec;
            fs::remove_all(extdata_root(req.arg(1), req.arg(2)), ec);
            req.reply(1, 0);
            break;
        }
        case 0x0821: { // CardSlotIsInserted
            auto r = req.reply(2, 0);
            r.push(1);
            break;
        }
        case 0x0845: { // GetFormatInfo(archive id, path) -> total size, directories, files, duplicate data
            if (req.arg(0) == SAVE_DATA && !fs::exists(save_root())) {
                req.reply(1, 0, ERR_NOT_FORMATTED);
                break;
            }
            auto r = req.reply(5, 0);
            r.push(512 << 10);
            r.push(32);
            r.push(32);
            r.push(0);
            break;
        }
        // Secure values guard against restoring old saves; the game stores one per slot and
        // checks it against the save. Kept next to the save directory, not inside it.
        case 0x0875: { // SetThisSaveDataSecureValue(?, ?, slot, value lo, value hi, ?)
            uint64_t value = req.arg(3) | (uint64_t(req.arg(4)) << 32);
            std::ofstream(secure_value_path(req.arg(2)), std::ios::binary)
                .write(reinterpret_cast<const char *>(&value), sizeof value);
            req.reply(1, 0);
            break;
        }
        case 0x086F: { // GetThisSaveDataSecureValue(slot) -> exists, is gamecard, value
            uint64_t value = 0;
            bool exists = bool(std::ifstream(secure_value_path(req.arg(0)), std::ios::binary)
                                   .read(reinterpret_cast<char *>(&value), sizeof value));
            auto r = req.reply(5, 0);
            r.push(exists);
            r.push(1);
            r.push64(exists ? value : 0);
            break;
        }
        case 0x084C: // FormatSaveData(archive id, path, block size, dirs, files, buckets, duplicate data)
            if (req.arg(0) == SAVE_DATA) {
                std::error_code ec;
                fs::remove_all(save_root(), ec);
                for (auto &e : fs::directory_iterator(game_root(), ec))
                    if (e.path().filename().string().starts_with("save_secure_value_")) fs::remove(e.path(), ec);
                fs::create_directories(save_root());
                LOG("fs: formatted save data at %s", save_root().string().c_str());
            }
            req.reply(1, 0);
            break;
        default:
            unimplemented(*this, req);
        }
    }
};

} // namespace

void register_fs() { register_service({"fs:USER"}, std::make_shared<FsUser>()); }

} // namespace hle
