#pragma once
// Level-5's archive format (ARC0, the games' .fa files) and the compression its tables use, for
// mods that replace files inside an archive (GameBanana's "include/" mods). Read-only: a modded
// archive is described as pieces (see arc0_patch) that the RomFS overlay serves on the fly.
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace level5 {

// Level-5 compression: a u32 header (decompressed size << 3 | method), then the data. Methods:
// 0 none, 1 LZ10, 2 Huffman (4-bit), 3 Huffman (8-bit), 4 RLE. nullopt if it doesn't decode.
std::optional<std::vector<uint8_t>> decompress(const uint8_t *data, size_t size);

// reads `size` bytes at `offset` of an archive (from wherever it lives)
using Reader = std::function<bool(uint64_t offset, void *out, uint64_t size)>;

struct Arc0 {
    struct File {
        uint32_t entry;        // index in the file table
        uint64_t offset, size; // data, relative to the archive
    };
    uint8_t header[0x48];
    uint32_t file_table_offset, name_table_offset, data_offset;
    uint64_t size;                       // of the whole archive
    std::vector<uint8_t> file_table;     // decompressed: 16 bytes per file
    std::map<std::string, File> files;   // by path ("data/res/character/x.cfg.bin")
};

// reads an archive's tables; nullopt if it isn't an ARC0 archive (or a table doesn't decode)
std::optional<Arc0> read_arc0(const Reader &read, uint64_t size);

// A modded archive, as pieces in order: bytes held here, ranges of the original archive, and
// replacement files (host paths).
struct Piece {
    uint64_t size;
    std::vector<uint8_t> bytes;    // if not empty: these
    uint64_t original_offset = ~0ull; // else if set: the original archive from here
    std::string file;              // else: this host file
};
// the archive with `replacements` (archive path -> host file of `size` bytes) swapped in: the file
// table is rewritten (uncompressed) and the new files placed after the original data
std::vector<Piece> arc0_patch(const Arc0 &arc, const std::map<std::string, std::pair<std::string, uint64_t>> &replacements);

} // namespace level5
