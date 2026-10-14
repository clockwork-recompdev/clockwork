#include "dump.h"

#include "log.h"
#include "platform.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>

namespace {
constexpr uint64_t MEDIA_UNIT = 0x200;
std::unique_ptr<Dump> instance;

uint32_t u32(const uint8_t *p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

// Backwards LZ77 used on ExeFS .code (same as tools/ctrx/src/lz.rs)
std::vector<uint8_t> blz_decompress(const std::vector<uint8_t> &in) {
    size_t len = in.size();
    if (len < 8) fatal(".code is too short to be compressed");
    uint32_t info = u32(&in[len - 8]), extra = u32(&in[len - 4]);
    size_t footer = info >> 24, region = info & 0xFFFFFF;
    if (footer > len || region > len) fatal(".code compression footer is invalid");
    std::vector<uint8_t> out(len + extra);
    std::memcpy(out.data(), in.data(), len);
    size_t src = len - footer, stop = len - region, dst = out.size();
    while (src > stop) {
        uint8_t control = in[--src];
        for (int i = 0; i < 8 && src > stop; i++, control <<= 1) {
            if (control & 0x80) {
                if (src < stop + 2) fatal(".code: truncated back-reference");
                src -= 2;
                size_t pair = in[src] | (in[src + 1] << 8);
                size_t count = ((pair >> 12) & 0xF) + 3, offset = (pair & 0xFFF) + 2;
                if (dst < count) fatal(".code: output underflow");
                for (size_t j = 0; j < count; j++) {
                    if (dst + offset >= out.size()) fatal(".code: back-reference past end");
                    out[dst - 1] = out[dst + offset];
                    dst--;
                }
            } else {
                if (dst == 0) fatal(".code: output underflow");
                out[--dst] = in[--src];
            }
        }
    }
    return out;
}
} // namespace

std::optional<uint64_t> peek_program_id(const std::filesystem::path &path) {
    std::ifstream f(path, std::ios::binary);
    uint8_t header[0x200];
    if (!f.read(reinterpret_cast<char *>(header), sizeof header)) return std::nullopt;
    uint64_t ncch = 0;
    if (std::memcmp(header + 0x100, "NCSD", 4) == 0) ncch = uint64_t(u32(header + 0x120)) * MEDIA_UNIT;
    else if (std::memcmp(header + 0x100, "NCCH", 4) != 0) return std::nullopt;
    uint8_t h[0x200];
    if (!f.seekg(std::streamoff(ncch)) || !f.read(reinterpret_cast<char *>(h), sizeof h)) return std::nullopt;
    if (std::memcmp(h + 0x100, "NCCH", 4) != 0 || !(h[0x18F] & 0x4)) return std::nullopt;
    uint64_t id;
    std::memcpy(&id, h + 0x118, 8);
    return id;
}

Dump::Dump(const std::filesystem::path &path) : file_(path, std::ios::binary) {
    if (!file_) fatal("could not open the game dump %s", path.string().c_str());

    uint8_t header[0x200];
    read_at(0, header, sizeof header);
    uint64_t ncch = 0;
    if (std::memcmp(header + 0x100, "NCSD", 4) == 0) {
        ncch = uint64_t(u32(header + 0x120)) * MEDIA_UNIT; // partition 0: the game
    } else if (std::memcmp(header + 0x100, "NCCH", 4) != 0) {
        fatal("%s is not a .3ds/.cci or .cxi dump", path.string().c_str());
    }
    uint8_t h[0x200];
    read_at(ncch, h, sizeof h);
    if (std::memcmp(h + 0x100, "NCCH", 4) != 0) fatal("dump has no NCCH partition");
    if (!(h[0x18F] & 0x4)) fatal("the dump is encrypted; decrypt it with GodMode9 first");
    std::memcpy(&program_id_, h + 0x118, 8);

    exheader_.resize(0x400);
    read_at(ncch + 0x200, exheader_.data(), 0x400);

    // ExeFS: find .code
    uint64_t exefs = ncch + uint64_t(u32(h + 0x1A0)) * MEDIA_UNIT;
    uint8_t exefs_header[0x200];
    read_at(exefs, exefs_header, sizeof exefs_header);
    for (int i = 0; i < 10; i++) {
        const uint8_t *e = exefs_header + i * 16;
        if (std::strncmp(reinterpret_cast<const char *>(e), ".code", 8) != 0) continue;
        std::vector<uint8_t> packed(u32(e + 12));
        read_at(exefs + 0x200 + u32(e + 8), packed.data(), packed.size());
        bool compressed = exheader_[0xD] & 1;
        code_ = compressed ? blz_decompress(packed) : std::move(packed);
    }
    if (code_.empty()) fatal("dump has no .code");

    // RomFS: skip the IVFC header to level 3, which is what the game reads
    uint64_t romfs = ncch + uint64_t(u32(h + 0x1B0)) * MEDIA_UNIT;
    uint64_t romfs_total = uint64_t(u32(h + 0x1B4)) * MEDIA_UNIT;
    uint8_t ivfc[0x60];
    read_at(romfs, ivfc, sizeof ivfc);
    if (std::memcmp(ivfc, "IVFC", 4) != 0) fatal("dump has no RomFS");
    uint64_t master_hash = u32(ivfc + 0x08), block = uint64_t(1) << u32(ivfc + 0x4C);
    uint64_t level3 = (0x60 + master_hash + block - 1) / block * block;
    uint64_t level3_size;
    std::memcpy(&level3_size, ivfc + 0x44, 8);
    romfs_offset_ = romfs + level3;
    romfs_size_ = std::min(level3_size, romfs_total - level3);
    LOG("dump: program %016llx, .code %#zx bytes, RomFS %.1f MiB", (unsigned long long)program_id_, code_.size(),
        double(romfs_size_) / (1 << 20));
}

void Dump::read_at(uint64_t offset, void *out, uint64_t size) {
    file_.seekg(std::streamoff(offset));
    file_.read(static_cast<char *>(out), std::streamsize(size));
    if (uint64_t(file_.gcount()) != size) fatal("dump is truncated (read at %#llx)", (unsigned long long)offset);
}

uint64_t Dump::read_romfs(uint64_t offset, void *out, uint64_t size) {
    if (offset >= romfs_size_) return 0;
    size = std::min(size, romfs_size_ - offset);
    std::lock_guard lk(read_lock_);
    if (replacements_.empty()) { // the common case: just the dump
        read_at(romfs_offset_ + offset, out, size);
        return size;
    }
    // in pieces: the patched file table, the dump, or replaced files (zeros between them)
    auto *dst = static_cast<uint8_t *>(out);
    uint64_t at = offset, end = offset + size;
    const uint64_t meta_end = file_meta_offset_ + file_meta_.size();
    while (at < end) {
        uint64_t n;
        if (at >= file_meta_offset_ && at < meta_end) {
            n = std::min(end, meta_end) - at;
            std::memcpy(dst, file_meta_.data() + (at - file_meta_offset_), n);
        } else if (at < original_size_) {
            uint64_t stop = std::min(end, original_size_);
            if (at < file_meta_offset_) stop = std::min(stop, file_meta_offset_);
            n = stop - at;
            read_at(romfs_offset_ + at, dst, n);
        } else {
            n = end - at;
            Replacement *in = nullptr;
            for (Replacement &r : replacements_) {
                if (at >= r.start && at < r.start + r.size) {
                    in = &r;
                    break;
                }
                if (r.start > at) n = std::min(n, r.start - at); // padding up to the next one
            }
            if (in) {
                n = std::min(n, in->start + in->size - at);
                read_replacement(*in, at - in->start, dst, n);
            } else {
                std::memset(dst, 0, n);
            }
        }
        dst += n, at += n;
    }
    return size;
}

void Dump::read_replacement(Replacement &r, uint64_t at, uint8_t *dst, uint64_t n) {
    uint64_t piece_start = 0;
    for (level5::Piece &p : r.pieces) {
        if (n == 0) break;
        if (at >= piece_start + p.size) {
            piece_start += p.size;
            continue;
        }
        uint64_t from = at - piece_start, len = std::min(n, p.size - from);
        if (!p.bytes.empty()) {
            std::memcpy(dst, p.bytes.data() + from, len);
        } else if (p.original_offset != ~0ull) {
            read_at(romfs_offset_ + p.original_offset + from, dst, len);
        } else {
            auto &f = r.open[p.file];
            if (!f) f = std::make_unique<std::ifstream>(p.file, std::ios::binary);
            f->clear();
            f->seekg(std::streamoff(from));
            if (!f->read(reinterpret_cast<char *>(dst), std::streamsize(len))) std::memset(dst, 0, len);
        }
        dst += len, at += len, n -= len;
        piece_start += p.size;
    }
    if (n) std::memset(dst, 0, n);
}

bool Dump::index_romfs() {
    if (!file_index_.empty()) return true;
    uint8_t h[0x28];
    read_at(romfs_offset_, h, sizeof h);
    uint32_t dir_meta_off = u32(h + 0x0C), dir_meta_size = u32(h + 0x10);
    uint32_t file_meta_off = u32(h + 0x1C), file_meta_size = u32(h + 0x20);
    std::vector<uint8_t> dirs(dir_meta_size);
    file_meta_.resize(file_meta_size);
    read_at(romfs_offset_ + dir_meta_off, dirs.data(), dirs.size());
    read_at(romfs_offset_ + file_meta_off, file_meta_.data(), file_meta_.size());
    file_meta_offset_ = file_meta_off;
    file_data_offset_ = u32(h + 0x24);
    original_size_ = romfs_size_;

    constexpr uint32_t NONE = 0xFFFFFFFF;
    auto name = [](const std::vector<uint8_t> &t, uint32_t at, uint32_t len) {
        std::string s; // UTF-16 names; the games' are ASCII
        for (uint32_t i = 0; i + 1 < len && at + i + 1 < t.size(); i += 2) {
            uint16_t c = uint16_t(t[at + i] | (t[at + i + 1] << 8));
            s += c < 0x80 ? char(c) : '?';
        }
        return s;
    };
    std::vector<std::pair<uint32_t, std::string>> todo{{0, ""}};
    for (size_t guard = 0; !todo.empty() && guard < 100000; guard++) {
        auto [d, prefix] = todo.back();
        todo.pop_back();
        if (size_t(d) + 0x18 > dirs.size()) continue;
        for (uint32_t f = u32(&dirs[d + 0x0C]); f != NONE && size_t(f) + 0x20 <= file_meta_.size();
             f = u32(&file_meta_[f + 0x04]))
            file_index_[prefix + name(file_meta_, f + 0x20, u32(&file_meta_[f + 0x1C]))] = f;
        for (uint32_t c = u32(&dirs[d + 0x08]); c != NONE && size_t(c) + 0x18 <= dirs.size(); c = u32(&dirs[c + 0x04]))
            todo.push_back({c, prefix + name(dirs, c + 0x18, u32(&dirs[c + 0x14])) + "/"});
    }
    return !file_index_.empty();
}

std::optional<Dump::RomfsFile> Dump::romfs_file(const std::string &path) {
    std::lock_guard lk(read_lock_);
    if (!index_romfs()) return std::nullopt;
    auto it = file_index_.find(path);
    if (it == file_index_.end()) return std::nullopt;
    uint64_t data, size;
    std::memcpy(&data, &file_meta_[it->second + 0x08], 8);
    std::memcpy(&size, &file_meta_[it->second + 0x10], 8);
    if (file_data_offset_ + data >= original_size_) return std::nullopt; // replaced already
    return RomfsFile{file_data_offset_ + data, size};
}

std::vector<std::string> Dump::romfs_paths() {
    std::lock_guard lk(read_lock_);
    index_romfs();
    std::vector<std::string> out;
    for (auto &[path, entry] : file_index_) out.push_back(path);
    return out;
}

bool Dump::replace_romfs_file(const std::string &path, std::vector<level5::Piece> pieces) {
    std::lock_guard lk(read_lock_);
    if (!index_romfs()) return false;
    auto it = file_index_.find(path);
    if (it == file_index_.end()) return false;
    uint64_t data, size = 0;
    std::memcpy(&data, &file_meta_[it->second + 0x08], 8);
    uint64_t original = file_data_offset_ + data; // the file as the dump has it
    for (level5::Piece &p : pieces) {
        if (p.bytes.empty() && p.original_offset != ~0ull) p.original_offset += original;
        size += p.size;
    }
    uint64_t start = (romfs_size_ + 15) & ~uint64_t(15);
    uint64_t data_off = start - file_data_offset_;
    std::memcpy(&file_meta_[it->second + 0x08], &data_off, 8);
    std::memcpy(&file_meta_[it->second + 0x10], &size, 8);
    replacements_.push_back({start, size, std::move(pieces), {}});
    romfs_size_ = start + size;
    return true;
}

int Dump::apply_romfs_mod(const std::filesystem::path &dir) {
    int replaced = 0, skipped = 0;
    std::error_code ec;
    for (auto &e : std::filesystem::recursive_directory_iterator(dir, ec)) {
        if (!e.is_regular_file(ec)) continue;
        std::string rel = std::filesystem::relative(e.path(), dir, ec).generic_string();
        uint64_t size = e.file_size(ec);
        if (replace_romfs_file(rel, {level5::Piece{size, {}, ~0ull, e.path().string()}})) replaced++;
        else if (skipped++ < 10) LOG("mods: %s isn't in the game's RomFS; adding files isn't supported", rel.c_str());
    }
    return replaced;
}

Dump &dump() { return *instance; }

void open_dump(const std::filesystem::path &path) { instance = std::make_unique<Dump>(path); }
