#include "level5.h"

#include <algorithm>
#include <cstring>

namespace level5 {
namespace {

uint32_t u32(const uint8_t *p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}
uint16_t u16(const uint8_t *p) { return uint16_t(p[0] | p[1] << 8); }

// GBA-style LZ77: a flag byte per 8 blocks, MSB first; a set flag is a back-reference
bool lz10(const uint8_t *in, size_t n, std::vector<uint8_t> &out, size_t want) {
    size_t i = 0;
    while (out.size() < want && i < n) {
        uint8_t flags = in[i++];
        for (int b = 0; b < 8 && out.size() < want; b++, flags <<= 1) {
            if (!(flags & 0x80)) {
                if (i >= n) return false;
                out.push_back(in[i++]);
                continue;
            }
            if (i + 1 >= n) return false;
            size_t len = (in[i] >> 4) + 3, disp = (size_t(in[i] & 0xF) << 8 | in[i + 1]) + 1;
            i += 2;
            if (disp > out.size()) return false;
            for (size_t k = 0; k < len && out.size() < want; k++) out.push_back(out[out.size() - disp]);
        }
    }
    return out.size() == want;
}

// GBA-style Huffman: a tree (size byte, then nodes), then 32-bit little-endian words read from
// the top bit down. Leaves are bytes (8-bit) or nibbles (4-bit, low first).
bool huffman(const uint8_t *in, size_t n, std::vector<uint8_t> &out, size_t want, int bits) {
    if (n < 1) return false;
    size_t tree_end = (size_t(in[0]) + 1) * 2;
    if (tree_end > n) return false;
    size_t node = 1; // the root
    size_t pos = tree_end;
    bool low = true;
    uint8_t pending = 0;
    while (out.size() < want) {
        if (pos + 4 > n) return false;
        uint32_t word = u32(in + pos);
        pos += 4;
        for (int b = 31; b >= 0 && out.size() < want; b--) {
            int bit = (word >> b) & 1;
            uint8_t v = in[node];
            size_t child = (node & ~size_t(1)) + (size_t(v & 0x3F) << 1) + 2 + bit;
            bool leaf = v & (bit ? 0x40 : 0x80);
            if (child >= tree_end) return false;
            if (!leaf) {
                node = child;
                continue;
            }
            uint8_t sym = in[child];
            node = 1;
            if (bits == 8) {
                out.push_back(sym);
            } else if (low) {
                pending = sym & 0xF, low = false;
            } else {
                out.push_back(uint8_t(pending | (sym & 0xF) << 4)), low = true;
            }
        }
    }
    return true;
}

// runs of a byte, and literals
bool rle(const uint8_t *in, size_t n, std::vector<uint8_t> &out, size_t want) {
    size_t i = 0;
    while (out.size() < want && i < n) {
        uint8_t f = in[i++];
        if (f & 0x80) {
            if (i >= n) return false;
            out.insert(out.end(), std::min<size_t>((f & 0x7F) + 3, want - out.size()), in[i++]);
        } else {
            size_t len = (f & 0x7F) + 1;
            if (i + len > n) return false;
            out.insert(out.end(), in + i, in + i + std::min(len, want - out.size()));
            i += len;
        }
    }
    return out.size() == want;
}

} // namespace

std::optional<std::vector<uint8_t>> decompress(const uint8_t *data, size_t size) {
    if (size < 4) return std::nullopt;
    uint32_t h = u32(data), method = h & 7;
    size_t want = h >> 3;
    const uint8_t *in = data + 4;
    size_t n = size - 4;
    std::vector<uint8_t> out;
    out.reserve(want);
    bool ok = false;
    switch (method) {
    case 0:
        if (n >= want) out.assign(in, in + want), ok = true;
        break;
    case 1: ok = lz10(in, n, out, want); break;
    case 2: ok = huffman(in, n, out, want, 4); break;
    case 3: ok = huffman(in, n, out, want, 8); break;
    case 4: ok = rle(in, n, out, want); break;
    }
    if (!ok) return std::nullopt;
    return out;
}

std::optional<Arc0> read_arc0(const Reader &read, uint64_t size) {
    Arc0 a;
    a.size = size;
    if (size < sizeof a.header || !read(0, a.header, sizeof a.header) || std::memcmp(a.header, "ARC0", 4) != 0)
        return std::nullopt;
    const uint8_t *h = a.header;
    uint32_t dirs_off = u32(h + 0x04), files_off = u32(h + 0x0C), names_off = u32(h + 0x10), data_off = u32(h + 0x14);
    uint32_t dir_count = u16(h + 0x18), file_count = u32(h + 0x1C);
    if (!(dirs_off < u32(h + 0x08) && u32(h + 0x08) <= files_off && files_off < names_off && names_off < data_off &&
          data_off <= size))
        return std::nullopt;
    a.file_table_offset = files_off, a.name_table_offset = names_off, a.data_offset = data_off;

    auto table = [&](uint32_t from, uint32_t to) -> std::optional<std::vector<uint8_t>> {
        std::vector<uint8_t> raw(to - from);
        if (!read(from, raw.data(), raw.size())) return std::nullopt;
        return decompress(raw.data(), raw.size());
    };
    auto dirs = table(dirs_off, u32(h + 0x08));
    auto files = table(files_off, names_off);
    auto names = table(names_off, data_off);
    if (!dirs || !files || !names || dirs->size() < size_t(dir_count) * 20 || files->size() < size_t(file_count) * 16)
        return std::nullopt;
    a.file_table = std::move(*files);

    // a directory: name hash, first child directory, child count, first file, file count, where
    // its files' names start, where its own name is (in the name table)
    auto cstr = [&](uint32_t at) {
        std::string s;
        for (; at < names->size() && (*names)[at]; at++) s += char((*names)[at]);
        return s;
    };
    for (uint32_t d = 0; d < dir_count; d++) {
        const uint8_t *e = dirs->data() + d * 20;
        uint32_t first_file = u16(e + 8), count = u16(e + 10), file_names = u32(e + 12), dir_name = u32(e + 16);
        std::string dir = cstr(dir_name);
        for (uint32_t f = first_file; f < first_file + count && f < file_count; f++) {
            const uint8_t *fe = a.file_table.data() + f * 16; // name hash, name, data offset, size
            a.files[dir + cstr(file_names + u32(fe + 4))] = {f, data_off + uint64_t(u32(fe + 8)), u32(fe + 12)};
        }
    }
    return a;
}

std::vector<Piece> arc0_patch(const Arc0 &arc,
                              const std::map<std::string, std::pair<std::string, uint64_t>> &replacements) {
    // New layout: the header; the directory tables as they were; the file table, now uncompressed
    // (method 0, which the games read: the directory hash table is stored that way); the name table
    // as it was; the original data; then the new files. File offsets are relative to the data
    // start, so the original files keep theirs.
    std::vector<uint8_t> table = arc.file_table;
    uint64_t data_len = arc.size - arc.data_offset;
    uint64_t rel = (data_len + 3) & ~uint64_t(3); // where the next new file goes, from the data start
    struct Added {
        uint64_t rel, size;
        std::string file;
    };
    std::vector<Added> added;
    for (auto &[path, src] : replacements) {
        auto it = arc.files.find(path);
        if (it == arc.files.end() || rel + src.second > 0xFFFFFFFFu) continue;
        uint8_t *fe = table.data() + it->second.entry * 16; // name hash, name, data offset, size
        uint32_t off = uint32_t(rel), size = uint32_t(src.second);
        std::memcpy(fe + 8, &off, 4);
        std::memcpy(fe + 12, &size, 4);
        added.push_back({rel, src.second, src.first});
        rel = (rel + src.second + 3) & ~uint64_t(3);
    }

    uint64_t names_at = (arc.file_table_offset + 4 + table.size() + 3) & ~uint64_t(3);
    uint64_t names_len = arc.data_offset - arc.name_table_offset;
    uint64_t data_at = (names_at + names_len + 0x7F) & ~uint64_t(0x7F);
    std::vector<uint8_t> header(arc.header, arc.header + sizeof arc.header);
    uint32_t v = uint32_t(names_at);
    std::memcpy(header.data() + 0x10, &v, 4);
    v = uint32_t(data_at);
    std::memcpy(header.data() + 0x14, &v, 4);

    std::vector<Piece> pieces;
    auto zeros = [&](uint64_t n) {
        if (n) pieces.push_back({n, std::vector<uint8_t>(n, 0)});
    };
    pieces.push_back({header.size(), header});
    pieces.push_back({arc.file_table_offset - sizeof arc.header, {}, sizeof arc.header});
    std::vector<uint8_t> files(names_at - arc.file_table_offset, 0);
    uint32_t stored = uint32_t(table.size() << 3); // size << 3 | method 0
    std::memcpy(files.data(), &stored, 4);
    std::memcpy(files.data() + 4, table.data(), table.size());
    pieces.push_back({files.size(), std::move(files)});
    pieces.push_back({names_len, {}, arc.name_table_offset});
    zeros(data_at - (names_at + names_len));
    pieces.push_back({data_len, {}, arc.data_offset});
    uint64_t at = data_len;
    for (const Added &a : added) {
        zeros(a.rel - at);
        pieces.push_back({a.size, {}, ~0ull, a.file});
        at = a.rel + a.size;
    }
    return pieces;
}

} // namespace level5
