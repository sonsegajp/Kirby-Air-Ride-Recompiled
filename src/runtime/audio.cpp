// Host audio: every block the AI DMA "plays" (32 kHz, 16-bit big-endian right/left pairs, mixed
// by src/runtime/ax.cpp) goes to an SDL audio stream. Silent with --headless, --mute or SA2_MUTE;
// --volume <percent> (or SA2_VOLUME) scales it.
#include "runtime/runtime.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace rt {

namespace {
SDL_AudioStream* s_stream = nullptr;
float s_baseGain = 1.f;  // --volume / SA2_VOLUME
}

void audioInit(bool enabled, float volume) {
    if (!enabled) {
        log("[audio] muted\n");
        return;
    }
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        log("[audio] no audio: %s\n", SDL_GetError());
        return;
    }
    const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, 32000};
    s_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!s_stream) {
        log("[audio] no audio device: %s\n", SDL_GetError());
        return;
    }
    s_baseGain = std::max(0.f, volume);
    SDL_SetAudioStreamGain(s_stream, s_baseGain);
    SDL_ResumeAudioStreamDevice(s_stream);
    log("[audio] 32 kHz stereo output, volume %.0f%%\n", volume * 100);
}

// The menu's volume (0..1), on top of --volume.
void audioSetGain(float gain) {
    if (s_stream) SDL_SetAudioStreamGain(s_stream, std::max(0.f, gain) * s_baseGain);
}

void audioPushAi(u32 addr, u32 bytes) {
    // SA2_AUDIO_DUMP=<file>: also write everything played as raw 32 kHz 16-bit stereo (even muted).
    static std::FILE* s_dump = std::getenv("SA2_AUDIO_DUMP") ? std::fopen(std::getenv("SA2_AUDIO_DUMP"), "wb") : nullptr;
    if (!s_stream && !s_dump) return;
    static std::vector<s16> buf;
    const u32 frames = bytes / 4;
    buf.resize(frames * 2);
    for (u32 i = 0; i < frames; i++) {  // (address 0: silence)
        buf[i * 2] = addr ? s16(rd16(addr + i * 4 + 2)) : 0;  // left
        buf[i * 2 + 1] = addr ? s16(rd16(addr + i * 4)) : 0;  // right
    }
    if (s_dump) std::fwrite(buf.data(), 4, frames, s_dump);
    if (!s_stream) return;
    // The AI is paced by the host clock and the device by its own, and the AI thread wakes a little
    // early or late: a cushion of ~60 ms stays queued. The two clocks drift apart slowly, so the
    // device plays a fraction of a percent faster or slower to hold the cushion (inaudible) instead
    // of blocks being dropped or the queue running dry (clicks, crackle). Should it run dry anyway
    // (the host stalled), silence refills the cushion at once: one gap, not a crackle per block.
    constexpr int kBytesPerMs = 32 * 4, kTarget = 60 * kBytesPerMs;
    const int queued = SDL_GetAudioStreamQueued(s_stream);
    static u32 s_refills = 0, s_drops = 0, s_pushes = 0;
    if (queued < 15 * kBytesPerMs) {
        static const std::vector<u8> s_silence(size_t(kTarget), 0);
        SDL_PutAudioStreamData(s_stream, s_silence.data(), kTarget - queued - int(frames * 4) > 0 ? kTarget - queued - int(frames * 4) : 0);
        if (s_pushes) s_refills++;
    }
    if (queued > 4 * kTarget) s_drops++;  // far behind (a long stall): let this block go
    else SDL_PutAudioStreamData(s_stream, buf.data(), int(frames * 4));
    const float off = float(queued - kTarget) / float(kTarget);  // -1 (dry) .. +1 (twice the cushion)
    SDL_SetAudioStreamFrequencyRatio(s_stream, 1.f + std::clamp(off, -1.f, 1.f) * 0.005f);
    if (++s_pushes % 6000 == 0 && (s_refills || s_drops)) {
        log("[audio] queue ran dry %u times, %u blocks dropped (last %u blocks)\n", s_refills, s_drops, 6000u);
        s_refills = s_drops = 0;
    }
}

}  // namespace rt
