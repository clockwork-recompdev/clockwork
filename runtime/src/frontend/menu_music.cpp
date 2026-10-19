#include "menu_music.h"

#include "audio/bcstm.h"
#include "dump.h"
#include "games.h"
#include "log.h"
#include "platform.h"
#include "settings.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

namespace frontend::menu_music {
namespace {

// the version select's music (the menu theme that also plays under the update's notices)
constexpr const char *BLASTERS_MENU = "snd/product/stream/bgm_B02_menu_a3.dspadpcm.bcstm";

struct Player {
    std::mutex lock; // stream, stopping
    std::thread loader;
    SDL_AudioStream *stream = nullptr;
    bool stopping = false, running = false;
    std::atomic<float> volume{1.0f};
    // the music, once loaded: read by the audio thread only after the stream is opened
    std::vector<int16_t> frames; // stereo
    size_t loop_start = 0, pos = 0;
} player;

// fills the device's request from the music, looping
void SDLCALL feed(void *, SDL_AudioStream *stream, int additional, int) {
    Player &p = player;
    size_t n = p.frames.size() / 2;
    if (!n) return;
    int16_t chunk[2048];
    while (additional > 0) {
        size_t want = std::min<size_t>(std::size(chunk) / 2, size_t(additional) / 4);
        if (!want) break;
        for (size_t i = 0; i < want; i++) {
            if (p.pos >= n) p.pos = p.loop_start;
            chunk[i * 2] = p.frames[p.pos * 2], chunk[i * 2 + 1] = p.frames[p.pos * 2 + 1];
            p.pos++;
        }
        SDL_PutAudioStreamData(stream, chunk, int(want * 4));
        additional -= int(want * 4);
    }
}

// the player's own file, as 16-bit stereo at its rate (looping from the start)
bool load_wav(const std::filesystem::path &path, audio::Stream &out) {
    SDL_AudioSpec spec;
    Uint8 *buf = nullptr;
    Uint32 len = 0;
    if (!SDL_LoadWAV(path.string().c_str(), &spec, &buf, &len)) {
        LOG("menu music: %s: %s", path.string().c_str(), SDL_GetError());
        return false;
    }
    SDL_AudioSpec want{SDL_AUDIO_S16, 2, spec.freq};
    Uint8 *conv = nullptr;
    int conv_len = 0;
    bool ok = SDL_ConvertAudioSamples(&spec, buf, int(len), &want, &conv, &conv_len);
    SDL_free(buf);
    if (!ok) return false;
    out.rate = uint32_t(spec.freq);
    out.frames.assign(reinterpret_cast<int16_t *>(conv), reinterpret_cast<int16_t *>(conv) + conv_len / 2);
    out.loops = true, out.loop_start = 0;
    SDL_free(conv);
    return true;
}

// Blasters' theme from a Blasters dump the launcher knows (any version or region)
bool load_from_dump(audio::Stream &out) {
    const Settings &s = settings();
    std::error_code ec;
    for (const KnownDump &d : KNOWN_DUMPS) {
        if (std::strcmp(d.game, "blasters") != 0) continue;
        auto it = s.dumps.find(d.program_id);
        if (it == s.dumps.end() || !std::filesystem::is_regular_file(it->second, ec)) continue;
        Dump dump(it->second);
        auto file = dump.romfs_file(BLASTERS_MENU);
        if (!file) continue;
        std::vector<uint8_t> data(file->size);
        if (dump.read_romfs(file->offset, data.data(), data.size()) != data.size()) continue;
        if (auto st = audio::decode_bcstm(data)) {
            out = std::move(*st);
            return true;
        }
        LOG("menu music: couldn't decode %s", BLASTERS_MENU);
    }
    return false;
}

void load() {
    audio::Stream music;
    std::filesystem::path own = platform::data_dir() / "menu-music.wav";
    std::error_code ec;
    bool found = (std::filesystem::is_regular_file(own, ec) && load_wav(own, music)) || load_from_dump(music);
    if (!found || music.frames.empty()) {
        LOG("menu music: none (no Blasters dump, and no menu-music.wav in the data folder)");
        return;
    }
    std::lock_guard lk(player.lock);
    if (player.stopping) return;
    player.frames = std::move(music.frames);
    player.loop_start = music.loop_start, player.pos = 0;
    SDL_AudioSpec spec{SDL_AUDIO_S16, 2, int(music.rate)};
    player.stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, feed, nullptr);
    if (!player.stream) {
        LOG("menu music: no audio device: %s", SDL_GetError());
        return;
    }
    SDL_SetAudioStreamGain(player.stream, player.volume.load());
    SDL_ResumeAudioStreamDevice(player.stream);
    LOG("menu music: playing (%.0f s, looping from %.1f s)", double(player.frames.size() / 2) / music.rate,
        double(player.loop_start) / music.rate);
}

} // namespace

void start() {
    std::lock_guard lk(player.lock);
    if (player.running) return;
    player.running = true, player.stopping = false;
    player.loader = std::thread(load);
}

bool started() {
    std::lock_guard lk(player.lock);
    return player.running;
}

void set_volume(float volume) {
    volume = std::clamp(volume, 0.0f, 1.0f);
    if (player.volume.exchange(volume) == volume) return;
    std::lock_guard lk(player.lock);
    if (player.stream) SDL_SetAudioStreamGain(player.stream, volume);
}

void stop() {
    {
        std::lock_guard lk(player.lock);
        if (!player.running) return;
        player.stopping = true;
    }
    if (player.loader.joinable()) player.loader.join();
    std::lock_guard lk(player.lock);
    if (player.stream) SDL_DestroyAudioStream(player.stream), player.stream = nullptr;
    player.frames.clear(), player.frames.shrink_to_fit();
    player.running = false;
}

} // namespace frontend::menu_music
