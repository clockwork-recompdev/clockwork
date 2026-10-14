// DSP firmware HLE. The game and the DSP share two copies ("regions") of a block of DSP RAM;
// the game fills the one with the newer frame counter, the DSP reads it and writes results to
// the other. Layout and behavior follow 3dbrew and Citra's audio_core, which documented the
// firmware's structures.
//
// Within a region, multi-word integers are "middle-endian" (two 16-bit halves, high first)
// unless noted; floats and the dirty-flag words are plain little-endian.
#include "dsp_hle.h"

#include "frontend/frontend.h"
#include "log.h"
#include "memory.h"
#include "video/pica.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

namespace audio {
namespace {

constexpr uint32_t REGION0 = 0x1FF50000, REGION1 = 0x1FF70000;
constexpr int SOURCES = 24;

// byte offsets inside a region (DSP word address - 0x8000, times two)
constexpr uint32_t FRAME_COUNTER = 0x7FFE;
constexpr uint32_t SOURCE_CONFIG = 0x3D24, SOURCE_CONFIG_SIZE = 192;
constexpr uint32_t SOURCE_STATUS = 0x0D00, SOURCE_STATUS_SIZE = 12;
constexpr uint32_t ADPCM_COEFFS = 0x4F24; // 16 x s16 per source
constexpr uint32_t DSP_CONFIG = 0x2860;
constexpr uint32_t DSP_STATUS = 0x0800;
constexpr uint32_t FINAL_SAMPLES = 0x0A80;
constexpr uint32_t INTERMEDIATE = 0x2924; // two aux mixes of 4 channels x 160 s32

// source configuration fields
enum : uint32_t {
    C_DIRTY = 0, C_GAIN = 4, C_RATE = 52, C_INTERP = 56, C_FILTERS = 58, C_SIMPLE = 60, C_BIQUAD = 64,
    C_BUFFERS_DIRTY = 74, C_BUFFERS = 76, C_LOOP_RELATED = 156, C_ENABLE = 160, C_SYNC = 162, C_PLAY_POS = 164,
    C_ADDR = 172, C_LENGTH = 176, C_FLAGS1 = 180, C_ADPCM_PS = 182, C_ADPCM_YN = 184, C_FLAGS2 = 188,
    C_BUFFER_ID = 190,
};
// dirty bits
enum : uint32_t {
    D_FORMAT = 1u << 0, D_MONO_STEREO = 1u << 1, D_ADPCM_COEFFS = 1u << 2, D_PARTIAL_EMBEDDED = 1u << 3,
    D_PARTIAL_RESET = 1u << 4, D_ENABLE = 1u << 16, D_INTERP = 1u << 17, D_RATE = 1u << 18, D_QUEUE = 1u << 19,
    D_PLAY_POS = 1u << 21, D_FILTERS = 1u << 22, D_SIMPLE = 1u << 23, D_BIQUAD = 1u << 24, D_GAIN0 = 1u << 25,
    D_SYNC = 1u << 28, D_RESET = 1u << 29, D_EMBEDDED = 1u << 30,
};
enum Format : uint8_t { PCM8 = 0, PCM16 = 1, ADPCM = 2 };

using Sample = std::array<int16_t, 2>;
using Quad = std::array<int32_t, 4>;

// ---- region access ----

struct Region {
    uint8_t *base;
    uint8_t *at(uint32_t off) const { return base + off; }
    uint16_t u16(uint32_t off) const { return uint16_t(base[off] | (base[off + 1] << 8)); }
    int16_t s16(uint32_t off) const { return int16_t(u16(off)); }
    uint32_t u32le(uint32_t off) const { return u16(off) | (uint32_t(u16(off + 2)) << 16); }
    uint32_t u32dsp(uint32_t off) const { return (uint32_t(u16(off)) << 16) | u16(off + 2); }
    float f32(uint32_t off) const {
        float f;
        std::memcpy(&f, base + off, 4);
        return f;
    }
    void put16(uint32_t off, uint32_t v) const {
        base[off] = uint8_t(v);
        base[off + 1] = uint8_t(v >> 8);
    }
    void put32le(uint32_t off, uint32_t v) const {
        put16(off, v);
        put16(off + 2, v >> 16);
    }
    void put32dsp(uint32_t off, uint32_t v) const {
        put16(off, v >> 16);
        put16(off + 2, v);
    }
};

// ---- per-voice filters ----

struct Filters {
    bool simple_on = false, biquad_on = false;
    int32_t s_b0 = 1 << 15, s_a1 = 0;
    Sample s_y1{};
    int32_t b_a1 = 0, b_a2 = 0, b_b0 = 1 << 14, b_b1 = 0, b_b2 = 0;
    Sample b_x1{}, b_x2{}, b_y1{}, b_y2{};

    void enable(bool simple, bool biquad) {
        simple_on = simple, biquad_on = biquad;
        if (!simple) s_b0 = 1 << 15, s_a1 = 0, s_y1 = {};
        if (!biquad) b_a1 = b_a2 = b_b1 = b_b2 = 0, b_b0 = 1 << 14, b_x1 = b_x2 = b_y1 = b_y2 = {};
    }

    void process(std::array<Sample, SAMPLES_PER_FRAME> &frame) {
        for (Sample &x : frame) {
            if (simple_on) {
                for (int c = 0; c < 2; c++) x[c] = int16_t(std::clamp((s_b0 * x[c] + s_a1 * s_y1[c]) >> 15, -32768, 32767));
                s_y1 = x;
            }
            if (biquad_on) {
                Sample y;
                for (int c = 0; c < 2; c++)
                    y[c] = int16_t(std::clamp(
                        (b_b0 * x[c] + b_b1 * b_x1[c] + b_b2 * b_x2[c] + b_a1 * b_y1[c] + b_a2 * b_y2[c]) >> 14, -32768,
                        32767));
                b_x2 = b_x1, b_x1 = x, b_y2 = b_y1, b_y1 = y;
                x = y;
            }
        }
    }
};

// ---- voices ----

struct Buffer {
    uint32_t addr, length;
    uint8_t adpcm_ps;
    int16_t yn1, yn2;
    bool adpcm_dirty, looping;
    uint16_t id;
    bool stereo;
    uint8_t format;
    bool from_queue;
    uint32_t play_position;
    bool has_played;
};

std::vector<Sample> decode(uint8_t format, bool stereo, uint32_t addr, uint32_t count, const int16_t coeffs[16],
                           int16_t &yn1, int16_t &yn2) {
    std::vector<Sample> out;
    uint32_t channels = stereo ? 2 : 1;
    uint32_t bytes = format == PCM8 ? count * channels : format == PCM16 ? count * channels * 2 : (count + 13) / 14 * 8;
    const uint8_t *p = pica::phys(addr & ~3u, bytes);
    if (!p) return out;
    out.resize(count);
    if (format == PCM8) {
        for (uint32_t i = 0; i < count; i++) {
            int16_t l = int16_t(int8_t(p[i * channels]) * 256);
            out[i] = {l, stereo ? int16_t(int8_t(p[i * 2 + 1]) * 256) : l};
        }
    } else if (format == PCM16) {
        for (uint32_t i = 0; i < count; i++) {
            int16_t l, r;
            std::memcpy(&l, p + i * channels * 2, 2);
            if (stereo)
                std::memcpy(&r, p + i * 4 + 2, 2);
            else
                r = l;
            out[i] = {l, r};
        }
    } else { // DSP-ADPCM: 8-byte frames of a scale/predictor byte and 14 4-bit samples
        int y1 = yn1, y2 = yn2;
        for (uint32_t i = 0; i < count; i++) {
            const uint8_t *frame = p + (i / 14) * 8;
            int scale = 1 << (frame[0] & 0xF), idx = (frame[0] >> 4) & 7;
            uint32_t k = i % 14;
            int nibble = (k & 1) ? frame[1 + k / 2] & 0xF : frame[1 + k / 2] >> 4;
            if (nibble >= 8) nibble -= 16;
            int v = (((nibble * scale) << 11) + 0x400 + coeffs[idx * 2] * y1 + coeffs[idx * 2 + 1] * y2) >> 11;
            v = std::clamp(v, -32768, 32767);
            y2 = y1, y1 = v;
            out[i] = {int16_t(v), int16_t(v)};
        }
        yn1 = int16_t(y1), yn2 = int16_t(y2);
    }
    return out;
}

struct Voice {
    bool enabled = false;
    uint16_t sync_count = 0;
    float rate = 1.0f;
    uint8_t interp = 0;
    std::array<std::array<float, 4>, 3> gain{}, ramp_start{};
    std::array<bool, 3> ramp_active{};
    Filters filters;
    int16_t coeffs[16] = {};
    int16_t yn1 = 0, yn2 = 0;
    uint8_t format = PCM16;
    bool stereo = false;

    std::vector<Buffer> queue; // ordered by buffer id, lowest first

    // the buffer being played; `pos` indexes `samples`
    std::vector<Sample> samples;
    size_t pos = 0;
    uint32_t sample_number = 0;
    uint32_t cur_addr = 0, cur_length = 0;
    uint8_t cur_format = PCM16;
    bool cur_stereo = false, cur_looping = false;
    uint16_t cur_id = 0, last_id = 0;
    bool buffer_update = false;

    // interpolation
    uint64_t fpos = 0;
    Sample xn1{}, xn2{};

    std::array<Sample, SAMPLES_PER_FRAME> frame{};

    bool empty() const { return pos >= samples.size(); }

    void enqueue(const Buffer &b) {
        auto it = std::upper_bound(queue.begin(), queue.end(), b.id, [](uint16_t id, const Buffer &x) { return id < x.id; });
        queue.insert(it, b);
    }

    void parse(const Region &r, uint32_t c, uint32_t coeff_off) {
        uint32_t dirty = r.u32le(c + C_DIRTY);
        if (!dirty) return;
        LOG_DSP("voice %u dirty %08x enable %u format %u flags1 %04x rate %g gain0 %g %g addr %08x len %u queue %04x",
                (c - SOURCE_CONFIG) / SOURCE_CONFIG_SIZE, dirty, r.at(c + C_ENABLE)[0], (r.u16(c + C_FLAGS1) >> 2) & 3,
                r.u16(c + C_FLAGS1), double(r.f32(c + C_RATE)), double(r.f32(c + C_GAIN)), double(r.f32(c + C_GAIN + 4)),
                r.u32dsp(c + C_ADDR), r.u32dsp(c + C_LENGTH), r.u16(c + C_BUFFERS_DIRTY));
        if (dirty & D_RESET) *this = Voice{};
        if (dirty & D_PARTIAL_RESET) queue.clear();
        if (dirty & D_ENABLE) enabled = r.at(c + C_ENABLE)[0] != 0;
        if (dirty & D_SYNC) sync_count = r.u16(c + C_SYNC);
        if (dirty & D_RATE) {
            rate = r.f32(c + C_RATE);
            if (!(rate > 0.0f)) rate = 1.0f;
        }
        if (dirty & D_ADPCM_COEFFS)
            for (int i = 0; i < 16; i++) coeffs[i] = r.s16(coeff_off + 2 * i);
        for (int m = 0; m < 3; m++) {
            if (!(dirty & (D_GAIN0 << m))) continue;
            ramp_start[m] = gain[m];
            ramp_active[m] = true;
            for (int ch = 0; ch < 4; ch++) gain[m][ch] = r.f32(c + C_GAIN + 16 * m + 4 * ch);
        }
        if (dirty & D_FILTERS) {
            uint16_t f = r.u16(c + C_FILTERS);
            filters.enable(f & 1, f & 2);
        }
        if (dirty & D_SIMPLE) filters.s_b0 = r.s16(c + C_SIMPLE), filters.s_a1 = r.s16(c + C_SIMPLE + 2);
        if (dirty & D_BIQUAD) {
            filters.b_a2 = r.s16(c + C_BIQUAD), filters.b_a1 = r.s16(c + C_BIQUAD + 2);
            filters.b_b2 = r.s16(c + C_BIQUAD + 4), filters.b_b1 = r.s16(c + C_BIQUAD + 6);
            filters.b_b0 = r.s16(c + C_BIQUAD + 8);
        }
        if (dirty & D_INTERP) interp = r.at(c + C_INTERP)[0];
        uint16_t flags1 = r.u16(c + C_FLAGS1), flags2 = r.u16(c + C_FLAGS2);
        if (dirty & (D_FORMAT | D_EMBEDDED)) format = uint8_t((flags1 >> 2) & 3);
        if (dirty & (D_MONO_STEREO | D_EMBEDDED)) stereo = (flags1 & 3) == 2;
        uint32_t play_position = dirty & D_PLAY_POS ? r.u32dsp(c + C_PLAY_POS) : 0;

        // new data written into the playing PCM16 buffer
        if ((dirty & D_PARTIAL_EMBEDDED) && format == PCM16) {
            int16_t a = 0, b = 0;
            samples = decode(PCM16, stereo, cur_addr, r.u32dsp(c + C_LENGTH), coeffs, a, b);
            cur_length = r.u32dsp(c + C_LENGTH), cur_stereo = stereo, cur_format = format;
            cur_looping = flags2 & 2;
            pos = std::min<size_t>(sample_number, samples.size());
            if (samples.size() < sample_number) sample_number = 0, pos = 0;
        }
        if (dirty & D_EMBEDDED) {
            int32_t length = int32_t(r.u32dsp(c + C_LENGTH));
            if (length >= 0) {
                uint16_t ps = r.u16(c + C_ADPCM_PS);
                enqueue({r.u32dsp(c + C_ADDR), uint32_t(length), uint8_t(ps), r.s16(c + C_ADPCM_YN),
                         r.s16(c + C_ADPCM_YN + 2), bool(flags2 & 1), bool(flags2 & 2), r.u16(c + C_BUFFER_ID), stereo,
                         format, false, play_position, false});
            }
        }
        if (dirty & D_QUEUE) {
            uint16_t which = r.u16(c + C_BUFFERS_DIRTY);
            for (int i = 0; i < 4; i++) {
                if (!(which & (1 << i))) continue;
                uint32_t b = c + C_BUFFERS + 20 * i;
                int32_t length = int32_t(r.u32dsp(b + 4));
                if (length < 0) continue;
                enqueue({r.u32dsp(b), uint32_t(length), uint8_t(r.u16(b + 8)), r.s16(b + 10), r.s16(b + 12),
                         r.at(b + 14)[0] != 0, r.at(b + 15)[0] != 0, r.u16(b + 16), stereo, format, true, 0, false});
            }
            r.put16(c + C_BUFFERS_DIRTY, 0);
        }
        r.put32le(c + C_DIRTY, 0);
    }

    bool dequeue() {
        if (queue.empty()) return false;
        Buffer b = queue.front();
        queue.erase(queue.begin());
        if (b.adpcm_dirty) yn1 = b.yn1, yn2 = b.yn2;
        samples = decode(b.format, b.stereo, b.addr, b.length, coeffs, yn1, yn2);
        pos = 0;
        if (samples.empty() && b.length) {
            static bool warned;
            if (!warned) LOG("dsp: voice buffer at %08x is not in GPU-visible memory", b.addr);
            warned = true;
            return true;
        }
        sample_number = b.has_played ? 0 : b.play_position;
        cur_addr = b.addr, cur_length = b.length, cur_stereo = b.stereo, cur_format = b.format;
        cur_looping = b.looping, cur_id = b.id, last_id = 0;
        buffer_update = b.from_queue && !b.has_played;
        if (b.looping) {
            b.has_played = true;
            enqueue(b);
        }
        pos = std::min<size_t>(sample_number, samples.size());
        return true;
    }

    // linear (or nearest) resampling from the current buffer into frame[out...]
    void resample(size_t &out) {
        if (empty()) return;
        size_t avail = samples.size() - pos + 2; // with xn2, xn1 in front
        auto at = [&](size_t k) -> const Sample & { return k == 0 ? xn2 : k == 1 ? xn1 : samples[pos + k - 2]; };
        constexpr uint64_t ONE = 1ull << 24;
        uint64_t step = uint64_t(double(rate) * double(ONE));
        size_t i = 0;
        while (out < frame.size()) {
            i = size_t(fpos >> 24);
            if (i + 2 >= avail) {
                i = avail - 2;
                break;
            }
            const Sample &x0 = at(i), &x1 = at(i + 1);
            uint64_t frac = fpos & (ONE - 1);
            if (interp == 2) {
                frame[out++] = x0;
            } else {
                Sample s;
                for (int ch = 0; ch < 2; ch++) {
                    int64_t d = std::clamp<int64_t>(int64_t(x1[ch]) - x0[ch], -32768, 32767);
                    s[ch] = int16_t(x0[ch] + int64_t(frac) * d / int64_t(ONE));
                }
                frame[out++] = s;
            }
            fpos += step;
        }
        Sample n2 = at(i), n1 = at(i + 1);
        xn2 = n2, xn1 = n1;
        fpos -= uint64_t(i) << 24;
        pos += i;
        sample_number += uint32_t(i);
    }

    void generate() {
        frame.fill({});
        // a looping PCM16 buffer may be refilled while it plays (streamed music)
        if (cur_looping && cur_format == PCM16 && cur_addr && sample_number < cur_length && !samples.empty()) {
            uint32_t channels = cur_stereo ? 2 : 1;
            int16_t a = 0, b = 0;
            auto fresh = decode(PCM16, cur_stereo, (cur_addr & ~3u) + sample_number * channels * 2,
                                cur_length - sample_number, coeffs, a, b);
            if (!fresh.empty()) samples = std::move(fresh), pos = 0;
        }
        if (empty()) {
            if (dequeue()) return;
            enabled = false;
            buffer_update = true;
            last_id = cur_id;
            cur_id = 0;
            return;
        }
        size_t out = 0;
        while (out < frame.size()) {
            if (empty() && !dequeue()) break;
            resample(out);
            if (empty() && out < frame.size() && queue.empty()) break;
        }
        filters.process(frame);
    }

    void mix_into(std::array<Quad, SAMPLES_PER_FRAME> &dest, int m) {
        if (!enabled) {
            ramp_start[m] = gain[m];
            ramp_active[m] = false;
            return;
        }
        const auto &g = gain[m], &s = ramp_start[m];
        for (int i = 0; i < SAMPLES_PER_FRAME; i++) {
            float t = float(i) / float(SAMPLES_PER_FRAME - 1);
            float k[4];
            for (int ch = 0; ch < 4; ch++) k[ch] = ramp_active[m] ? s[ch] + (g[ch] - s[ch]) * t : g[ch];
            dest[i][0] += int32_t(k[0] * frame[i][0]);
            dest[i][1] += int32_t(k[1] * frame[i][1]);
            dest[i][2] += int32_t(k[2] * frame[i][0]);
            dest[i][3] += int32_t(k[3] * frame[i][1]);
        }
        if (ramp_active[m]) ramp_start[m] = g, ramp_active[m] = false;
    }

    void status(const Region &w, uint32_t off) {
        w.at(off)[0] = enabled;
        w.at(off)[1] = buffer_update;
        buffer_update = false;
        w.put16(off + 2, sync_count);
        w.put32dsp(off + 4, sample_number);
        w.put16(off + 8, cur_id);
        w.put16(off + 10, last_id);
    }
};

struct Mixer {
    float volume[3] = {0, 0, 0}; // master, aux return 0, aux return 1
    bool aux_enabled[2] = {false, false};
    uint16_t output_format = 1;
    std::array<std::array<Quad, SAMPLES_PER_FRAME>, 3> buffers{};

    void parse(const Region &r) {
        uint32_t dirty = r.u32le(DSP_CONFIG);
        if (!dirty) return;
        LOG_DSP("mixer dirty %08x master %g format %u aux %u %u", dirty, double(r.f32(DSP_CONFIG + 4)), r.u16(DSP_CONFIG + 22),
                r.u16(DSP_CONFIG + 40), r.u16(DSP_CONFIG + 42));
        if (dirty & (1u << 8)) aux_enabled[0] = r.u16(DSP_CONFIG + 40) != 0;
        if (dirty & (1u << 9)) aux_enabled[1] = r.u16(DSP_CONFIG + 42) != 0;
        if (dirty & (1u << 16)) volume[0] = r.f32(DSP_CONFIG + 4);
        if (dirty & (1u << 24)) volume[1] = r.f32(DSP_CONFIG + 8);
        if (dirty & (1u << 25)) volume[2] = r.f32(DSP_CONFIG + 12);
        if (dirty & (1u << 26)) output_format = r.u16(DSP_CONFIG + 22);
        r.put32le(DSP_CONFIG, 0);
    }
};

Voice voices[SOURCES];
Mixer mixer;

// the region the game wrote most recently (larger frame counter, with wraparound)
bool region1_is_current() {
    uint16_t f0 = Region{mem::ptr(REGION0)}.u16(FRAME_COUNTER), f1 = Region{mem::ptr(REGION1)}.u16(FRAME_COUNTER);
    if (f0 == 0xFFFF && f1 != 0xFFFE) return true;
    if (f1 == 0xFFFF && f0 != 0xFFFE) return false;
    return f0 <= f1;
}

int16_t clamp16(int32_t v) { return int16_t(std::clamp(v, -32768, 32767)); }

} // namespace

void reset() {
    for (Voice &v : voices) v = Voice{};
    mixer = Mixer{};
}

void tick() {
    bool one = region1_is_current();
    Region read{mem::ptr(one ? REGION1 : REGION0)}, write{mem::ptr(one ? REGION0 : REGION1)};

    std::array<std::array<Quad, SAMPLES_PER_FRAME>, 3> mixes{};
    for (int i = 0; i < SOURCES; i++) {
        Voice &v = voices[i];
        v.parse(read, SOURCE_CONFIG + i * SOURCE_CONFIG_SIZE, ADPCM_COEFFS + i * 32);
        if (v.enabled) v.generate();
        v.status(write, SOURCE_STATUS + i * SOURCE_STATUS_SIZE);
        for (int m = 0; m < 3; m++) v.mix_into(mixes[m], m);
    }

    // aux buses: with an aux bus enabled the game processes that mix on the CPU, reading it
    // from the write region and returning it through the read region one frame later
    mixer.parse(read);
    for (int a = 0; a < 2; a++) {
        uint32_t base = INTERMEDIATE + a * 4 * SAMPLES_PER_FRAME * 4;
        if (mixer.aux_enabled[a]) {
            for (int s = 0; s < SAMPLES_PER_FRAME; s++)
                for (int ch = 0; ch < 4; ch++) {
                    uint32_t off = base + (ch * SAMPLES_PER_FRAME + s) * 4;
                    mixer.buffers[a + 1][s][ch] = int32_t(read.u32le(off));
                    write.put32le(off, uint32_t(mixes[a + 1][s][ch]));
                }
        } else {
            mixer.buffers[a + 1] = mixes[a + 1];
        }
    }
    mixer.buffers[0] = mixes[0];

    int16_t out[SAMPLES_PER_FRAME * 2] = {};
    for (int m = 0; m < 3; m++) {
        float g = mixer.volume[m];
        for (int s = 0; s < SAMPLES_PER_FRAME; s++) {
            const Quad &q = mixer.buffers[m][s];
            int16_t l, r;
            if (mixer.output_format == 0) {
                l = r = clamp16(int32_t((g * q[0] + g * q[1] + g * q[2] + g * q[3]) / 2));
            } else {
                l = clamp16(int32_t(g * q[0] + g * q[2]));
                r = clamp16(int32_t(g * q[1] + g * q[3]));
            }
            out[s * 2] = clamp16(out[s * 2] + l);
            out[s * 2 + 1] = clamp16(out[s * 2 + 1] + r);
        }
    }
    for (int s = 0; s < SAMPLES_PER_FRAME * 2; s++) write.put16(FINAL_SAMPLES + s * 2, uint16_t(out[s]));
    write.put16(DSP_STATUS, 0);
    write.put16(DSP_STATUS + 2, 0);
    frontend::play_audio(out, SAMPLES_PER_FRAME);
}

} // namespace audio
