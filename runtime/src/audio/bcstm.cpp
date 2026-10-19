#include "bcstm.h"

#include <algorithm>
#include <cstring>

// Layout (3dbrew, "BCSTM"): a header listing blocks (INFO, SEEK, DATA). INFO holds the stream's
// format and loop, and per channel its DSP-ADPCM coefficients. DATA holds the samples in blocks,
// each block one run per channel in turn. Offsets inside INFO are relative to the reference table
// they're in.
namespace audio {
namespace {

constexpr uint16_t INFO_BLOCK = 0x4000, DATA_BLOCK = 0x4002;
enum Codec : uint8_t { PCM8 = 0, PCM16 = 1, DSP_ADPCM = 2 };

struct Reader {
    const std::vector<uint8_t> &d;
    bool ok = true;
    template <typename T> T get(size_t at) {
        T v{};
        if (at + sizeof(T) > d.size()) {
            ok = false;
            return v;
        }
        std::memcpy(&v, &d[at], sizeof(T));
        return v;
    }
};

struct Channel {
    int16_t coeffs[16] = {};
    int16_t y1 = 0, y2 = 0;
};

} // namespace

std::optional<Stream> decode_bcstm(const std::vector<uint8_t> &file) {
    Reader r{file};
    if (file.size() < 0x40 || std::memcmp(file.data(), "CSTM", 4) != 0 || r.get<uint16_t>(4) != 0xFEFF)
        return std::nullopt;
    uint32_t info = 0, data = 0;
    for (uint16_t i = 0, n = r.get<uint16_t>(0x10); i < n && i < 8; i++) {
        size_t ref = 0x14 + i * 12;
        uint16_t type = r.get<uint16_t>(ref);
        uint32_t off = r.get<uint32_t>(ref + 4);
        if (type == INFO_BLOCK) info = off;
        if (type == DATA_BLOCK) data = off;
    }
    if (!info || !data || std::memcmp(&file[std::min<size_t>(info, file.size() - 4)], "INFO", 4) != 0) return std::nullopt;

    // INFO: references to the stream info, the track table and the channel table
    size_t base = info + 8;
    size_t stream = base + r.get<int32_t>(base + 4), channels_table = base + r.get<int32_t>(base + 0x14);
    uint8_t codec = r.get<uint8_t>(stream), loop = r.get<uint8_t>(stream + 1), count = r.get<uint8_t>(stream + 2);
    uint32_t rate = r.get<uint32_t>(stream + 4), loop_start = r.get<uint32_t>(stream + 8), total = r.get<uint32_t>(stream + 12);
    uint32_t blocks = r.get<uint32_t>(stream + 16), block_size = r.get<uint32_t>(stream + 20);
    uint32_t block_samples = r.get<uint32_t>(stream + 24), last_size = r.get<uint32_t>(stream + 28);
    uint32_t last_samples = r.get<uint32_t>(stream + 32), last_padded = r.get<uint32_t>(stream + 36);
    uint32_t data_start = data + 8 + r.get<uint32_t>(stream + 0x34);
    (void)last_size;
    if (!r.ok || !count || !rate || !blocks || codec > DSP_ADPCM) return std::nullopt;

    std::vector<Channel> ch(count);
    if (codec == DSP_ADPCM) {
        uint32_t listed = r.get<uint32_t>(channels_table);
        for (uint32_t c = 0; c < count && c < listed; c++) {
            size_t entry = channels_table + r.get<int32_t>(channels_table + 4 + c * 8 + 4);
            size_t adpcm = entry + r.get<int32_t>(entry + 4);
            for (int k = 0; k < 16; k++) ch[c].coeffs[k] = r.get<int16_t>(adpcm + k * 2);
            ch[c].y1 = r.get<int16_t>(adpcm + 0x22), ch[c].y2 = r.get<int16_t>(adpcm + 0x24);
        }
    }
    if (!r.ok) return std::nullopt;

    // decode each channel, block by block (each channel's run is block_size bytes, the last padded)
    std::vector<std::vector<int16_t>> pcm(count);
    for (auto &p : pcm) p.reserve(total);
    for (uint32_t b = 0; b < blocks; b++) {
        bool last = b + 1 == blocks;
        uint32_t stride = last ? last_padded : block_size, samples = last ? last_samples : block_samples;
        for (uint32_t c = 0; c < count; c++) {
            size_t at = data_start + size_t(b) * block_size * count + size_t(c) * stride;
            if (at + (codec == DSP_ADPCM ? (samples + 13) / 14 * 8 : samples * (codec == PCM16 ? 2 : 1)) > file.size())
                return std::nullopt;
            const uint8_t *p = &file[at];
            auto &out = pcm[c];
            if (codec == PCM8) {
                for (uint32_t i = 0; i < samples; i++) out.push_back(int16_t(int8_t(p[i]) * 256));
            } else if (codec == PCM16) {
                for (uint32_t i = 0; i < samples; i++) {
                    int16_t v;
                    std::memcpy(&v, p + i * 2, 2);
                    out.push_back(v);
                }
            } else { // DSP-ADPCM: 8-byte frames of a scale/predictor byte and 14 4-bit samples
                Channel &s = ch[c];
                int y1 = s.y1, y2 = s.y2;
                for (uint32_t i = 0; i < samples; i++) {
                    const uint8_t *frame = p + (i / 14) * 8;
                    int scale = 1 << (frame[0] & 0xF), idx = (frame[0] >> 4) & 7;
                    uint32_t k = i % 14;
                    int nibble = (k & 1) ? frame[1 + k / 2] & 0xF : frame[1 + k / 2] >> 4;
                    if (nibble >= 8) nibble -= 16;
                    int v = (((nibble * scale) << 11) + 0x400 + s.coeffs[idx * 2] * y1 + s.coeffs[idx * 2 + 1] * y2) >> 11;
                    v = std::clamp(v, -32768, 32767);
                    y2 = y1, y1 = v;
                    out.push_back(int16_t(v));
                }
                s.y1 = int16_t(y1), s.y2 = int16_t(y2);
            }
        }
    }

    // stereo: the first two channels (the first track), or the one twice
    Stream st;
    st.rate = rate, st.loops = loop != 0;
    size_t n = std::min<size_t>(pcm[0].size(), total);
    st.loop_start = st.loops && loop_start < n ? loop_start : 0;
    st.frames.resize(n * 2);
    const auto &left = pcm[0], &right = pcm[count > 1 ? 1 : 0];
    for (size_t i = 0; i < n; i++) st.frames[i * 2] = left[i], st.frames[i * 2 + 1] = i < right.size() ? right[i] : left[i];
    return st;
}

} // namespace audio
