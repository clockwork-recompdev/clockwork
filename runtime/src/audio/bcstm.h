#pragma once
// BCSTM: the 3DS's streamed sound files (the games' music, in the RomFS as *.bcstm). Decoded whole
// to 16-bit stereo, for the launcher to play a game's own menu music from the player's dump.
#include <cstdint>
#include <optional>
#include <vector>

namespace audio {

struct Stream {
    uint32_t rate = 0;
    std::vector<int16_t> frames; // interleaved left, right
    bool loops = false;
    uint32_t loop_start = 0; // in frames; it plays on to the end, then from here
};

// nullopt if `file` isn't a BCSTM this can decode (PCM8, PCM16 or DSP-ADPCM)
std::optional<Stream> decode_bcstm(const std::vector<uint8_t> &file);

} // namespace audio
