// Endless Keeper launcher: the Soul Container's beat (see heart_sound.h).
#include "heart_sound.h"
#include "engine_view.h"
#include "fx.h"
#include <SDL3/SDL.h>
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <thread>
#include <vector>

namespace {

/* the samples: sound.dat's 150 (beat1.wav, HEART_BEAT_DOWN) and 151 (beat2a.wav, HEART_BEAT_UP); each is a whole
   beat - its loud thump peaks about 60 ms in, a second one about 0.37 s later at 60%, as the glows' two pulses */
const int SAMPLE_IDS[2] = {150, 151};
const float SAMPLE_PEAK = 0.06f;
const float BASE_GAIN = 0.55f; /* the launcher's beat under the game's own loudness: a menu, not a heart in the room */

struct Sample {
    SDL_AudioSpec spec;
    std::vector<unsigned char> pcm;
};

Sample g_samples[2];
std::thread g_worker;
std::atomic<SDL_AudioStream *> g_stream{nullptr};
std::atomic<bool> g_stop{false};
bool g_audio_up = false, g_on = false;
float g_volume = 1.0f, g_queued_beat = -1.0f;
int g_next = 0;
bool g_test_log = false;

uint32_t u32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/** A plain PCM WAV (as all of sound.dat's are) into its format and samples. */
bool parse_wav(const std::vector<unsigned char> &w, Sample &out)
{
    if ((w.size() < 12) || (memcmp(&w[0], "RIFF", 4) != 0) || (memcmp(&w[8], "WAVE", 4) != 0))
        return false;
    bool have_fmt = false;
    for (size_t p = 12; p + 8 <= w.size();)
    {
        const uint32_t size = u32(&w[p + 4]);
        if (p + 8 + size > w.size())
            return false;
        if ((memcmp(&w[p], "fmt ", 4) == 0) && (size >= 16))
        {
            const unsigned tag = w[p + 8] | (w[p + 9] << 8), channels = w[p + 10] | (w[p + 11] << 8);
            const unsigned bits = w[p + 22] | (w[p + 23] << 8);
            if ((tag != 1) || (channels < 1) || (channels > 2) || ((bits != 8) && (bits != 16)))
                return false;
            out.spec.format = (bits == 8) ? SDL_AUDIO_U8 : SDL_AUDIO_S16LE;
            out.spec.channels = (int)channels;
            out.spec.freq = (int)u32(&w[p + 12]);
            have_fmt = true;
        } else if ((memcmp(&w[p], "data", 4) == 0) && have_fmt)
        {
            out.pcm.assign(w.begin() + p + 8, w.begin() + p + 8 + size);
            return !out.pcm.empty();
        }
        p += 8 + size + (size & 1);
    }
    return false;
}

/** sound.dat's layout as the engine reads it (bflib_sndlib.cpp, load_sound_bank): its last 4 bytes say where its head
    is; the head's third entry is the directory of 32-byte sample records (name, data offset, rate, size ...). */
bool load_samples(const std::wstring &game_dir)
{
    std::ifstream f((game_dir + L"\\sound\\sound.dat").c_str(), std::ios::binary);
    if (!f)
        return false;
    unsigned char b[64];
    f.seekg(-4, std::ios::end);
    if (!f.read((char *)b, 4))
        return false;
    f.seekg(u32(b) + 18 + 2 * 16, std::ios::beg);
    if (!f.read((char *)b, 16))
        return false;
    const uint32_t first_sample = u32(b), first_data = u32(b + 4), count = u32(b + 8) / 32;
    for (int i = 0; i < 2; i++)
    {
        if ((uint32_t)SAMPLE_IDS[i] >= count)
            return false;
        f.seekg(first_sample + 32 * SAMPLE_IDS[i], std::ios::beg);
        if (!f.read((char *)b, 32))
            return false;
        const uint32_t offset = u32(b + 18), size = u32(b + 26);
        if ((size < 44) || (size > (4u << 20)))
            return false;
        std::vector<unsigned char> wav(size);
        f.seekg(first_data + offset, std::ios::beg);
        if (!f.read((char *)wav.data(), size) || !parse_wav(wav, g_samples[i]))
            return false;
    }
    return (g_samples[0].spec.format == g_samples[1].spec.format) && (g_samples[0].spec.freq == g_samples[1].spec.freq)
        && (g_samples[0].spec.channels == g_samples[1].spec.channels);
}

void work(std::wstring game_dir)
{
    if (!load_samples(game_dir) || g_stop)
        return;
    SDL_AudioStream *s = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &g_samples[0].spec, nullptr, nullptr);
    if (s == nullptr)
        return;
    SDL_ResumeAudioStreamDevice(s);
    g_stream = s;
}

} // namespace

float heart_sound_game_volume(const std::wstring &game_dir)
{
    std::ifstream f((game_dir + L"\\save\\settings.toml").c_str());
    std::string line;
    bool audio = false;
    while (std::getline(f, line))
    {
        const size_t a = line.find_first_not_of(" \t");
        if ((a == std::string::npos) || (line[a] == '#'))
            continue;
        if (line[a] == '[')
            audio = (line.compare(a, 7, "[audio]") == 0);
        else if (audio && (line.compare(a, 12, "sound_volume") == 0))
        {
            const size_t eq = line.find('=', a);
            if (eq != std::string::npos)
                return std::max(0, std::min(127, atoi(line.c_str() + eq + 1))) / 127.0f; /* the game's 0 .. 127 */
        }
    }
    return 1.0f;
}

void heart_sound_start(const std::wstring &game_dir)
{
    if (g_audio_up)
        return;
    g_audio_up = true;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
        return;
    g_worker = std::thread(work, game_dir);
}

void heart_sound_settings(bool on, float volume)
{
    g_on = on;
    g_volume = std::max(0.0f, std::min(1.0f, volume));
    SDL_AudioStream *s = g_stream;
    if ((s != nullptr) && !g_test_log)
        SDL_SetAudioStreamGain(s, BASE_GAIN * g_volume); /* (the slider is heard while it moves) */
}

void heart_sound_frame(bool audible, float dt)
{
    SDL_AudioStream *s = g_stream;
    if (s == nullptr)
        return;
    if (!audible || !g_on || !fx_motion || (g_volume <= 0.0f))
    {
        /* still (no beat to keep), off, or not to be heard now: nothing queued, and the next beat starts afresh */
        if (g_queued_beat >= 0.0f)
            SDL_ClearAudioStream(s);
        g_queued_beat = -1.0f;
        return;
    }
    /* the next beat's sample starts SAMPLE_PEAK before the glow's first pulse; queued in the frame before it, with
       silence ahead for the rest of the way, so the thump lands on the pulse and not on a frame's edge */
    const float t = fx_time(), ahead = std::min(std::max(dt, 0.008f), 0.1f), start_at = FX_BEAT_FIRST - SAMPLE_PEAK;
    const float beat = floorf((t + ahead - start_at) / FX_BEAT_PERIOD) * FX_BEAT_PERIOD + start_at;
    if ((beat <= g_queued_beat + 0.5f * FX_BEAT_PERIOD) || (beat < t - 0.05f))
        return; /* queued already, or missed (a slow frame): a late thump is worse than none */
    g_queued_beat = beat;
    if (SDL_GetAudioStreamQueued(s) <= 0)
    {
        const Sample &smp = g_samples[g_next];
        const int frame_bytes = SDL_AUDIO_FRAMESIZE(smp.spec);
        const int pad = (int)(std::max(0.0f, beat - t) * smp.spec.freq) * frame_bytes;
        if (pad > 0)
        {
            static std::vector<unsigned char> silence;
            silence.assign((size_t)pad, (smp.spec.format == SDL_AUDIO_U8) ? 0x80 : 0);
            SDL_PutAudioStreamData(s, silence.data(), pad);
        }
        SDL_SetAudioStreamGain(s, g_test_log ? 0.0f : BASE_GAIN * g_volume);
        SDL_PutAudioStreamData(s, smp.pcm.data(), (int)smp.pcm.size());
        SDL_FlushAudioStream(s); /* (else the resampler keeps the last few bytes back, and the stream never runs dry) */
        if (g_test_log)
        {
            char line[200];
            snprintf(line, sizeof(line), "Heartbeat: sample %d queued at fx time %.3f after %.1f ms of silence: thump at %.3f, the glow's peak at %.3f (gain %.2f)",
                SAMPLE_IDS[g_next], t, std::max(0.0f, beat - t) * 1000.0f, beat + SAMPLE_PEAK, beat - start_at + FX_BEAT_FIRST, BASE_GAIN * g_volume);
            engine_view_note(line);
        }
    }
    g_next ^= 1; /* the engine's heart plays the two by turns */
}

void heart_sound_test_log(bool on)
{
    g_test_log = on;
}

void heart_sound_shutdown()
{
    g_stop = true;
    if (g_worker.joinable())
        g_worker.join();
    SDL_AudioStream *s = g_stream.exchange(nullptr);
    if (s != nullptr)
        SDL_DestroyAudioStream(s);
}
