#pragma once
// High-level emulation of the 3DS audio DSP firmware: every audio frame (160 samples at
// ~32.7 kHz) it reads the voice and mixer configuration the game wrote to DSP RAM, plays the
// queued sample buffers of its 24 voices, mixes them, reports voice status back, and sends
// the stereo result to the host's audio output.
#include <cstdint>

namespace audio {

constexpr int SAMPLES_PER_FRAME = 160;
constexpr int SAMPLE_RATE = 32728;

// clears all voices and mixer state (the game (re)initialized the DSP)
void reset();
// produces one audio frame; called with the kernel lock held
void tick();

} // namespace audio
