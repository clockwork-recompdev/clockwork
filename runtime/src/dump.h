#pragma once
// Reads the player's own decrypted dump (.3ds/.cci or .cxi): exheader, decompressed .code,
// and the RomFS, which the game's file system code reads as one image.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <mutex>
#include <optional>
#include <vector>

#include "level5.h"

class Dump {
public:
    explicit Dump(const std::filesystem::path &path);

    const std::vector<uint8_t> &exheader() const { return exheader_; }
    const std::vector<uint8_t> &code() const { return code_; }
    uint64_t program_id() const { return program_id_; }

    // the RomFS as the game sees it (IVFC level 3: header, tables, file data), with mods applied
    uint64_t romfs_size() const { return romfs_size_; }
    // returns bytes read (short at the end)
    uint64_t read_romfs(uint64_t offset, void *out, uint64_t size);

    // Mods. A replaced file's table entry points past the end of the original RomFS, where reads
    // come from its pieces; the dump itself is never changed.
    //
    // a file in the original RomFS ("yw2_a.fa"): where its data is, and its size
    struct RomfsFile {
        uint64_t offset, size;
    };
    std::optional<RomfsFile> romfs_file(const std::string &path);
    // replaces a RomFS file with `pieces` (level5::Piece::original_offset: within the original
    // file); false if the game has no such file
    bool replace_romfs_file(const std::string &path, std::vector<level5::Piece> pieces);
    // replaces RomFS files with the same-named files under `dir` (a LayeredFS-style romfs/ folder,
    // as fan translations ship); files the game doesn't have are skipped. Returns how many.
    int apply_romfs_mod(const std::filesystem::path &dir);
    // every RomFS file, by path
    std::vector<std::string> romfs_paths();

private:
    std::ifstream file_;
    std::mutex read_lock_;
    std::vector<uint8_t> exheader_, code_;
    uint64_t program_id_ = 0;
    uint64_t romfs_offset_ = 0, romfs_size_ = 0;
    // mods: the RomFS's file table (patched) and index, and what replaced files read as
    uint64_t original_size_ = 0, file_meta_offset_ = 0, file_data_offset_ = 0;
    std::vector<uint8_t> file_meta_;
    std::map<std::string, uint32_t> file_index_; // path -> entry in file_meta_
    struct Replacement {
        uint64_t start, size; // in the RomFS
        std::vector<level5::Piece> pieces; // original_offset already in the RomFS
        std::map<std::string, std::unique_ptr<std::ifstream>> open; // host files, opened on first read
    };
    std::vector<Replacement> replacements_;
    bool index_romfs(); // fills the above; false if the RomFS tables don't read
    void read_replacement(Replacement &r, uint64_t at, uint8_t *dst, uint64_t n);

    void read_at(uint64_t offset, void *out, uint64_t size);
};

// the program id of a dump, read from its header without loading it; nullopt if it isn't a
// readable, decrypted .3ds/.cci or .cxi
std::optional<uint64_t> peek_program_id(const std::filesystem::path &path);

Dump &dump();
void open_dump(const std::filesystem::path &path);
