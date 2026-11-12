// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
#include "app/host_audio.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "core/log.h"

namespace uv::app {

namespace {

constexpr int kToneRate = 48000;
constexpr unsigned kBellHz = 2000;            // the keyboard's buzzer
constexpr unsigned kRingFrames = kToneRate / 20;  // a ring is heard for at least 50 ms
constexpr unsigned kClickFrames = kToneRate / 500;  // a click: 2 ms
constexpr s16 kBellLevel = 6000, kClickLevel = 8000;

}  // namespace

bool HostAudio::open() {
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        UV_LOG(Warn, "audio", "no sound: %s", SDL_GetError());
        return false;
    }
    const SDL_AudioSpec guest{SDL_AUDIO_S16, 2, 8000};
    guest_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &guest, nullptr, nullptr);
    const SDL_AudioSpec keys{SDL_AUDIO_S16, 1, kToneRate};
    keys_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &keys, &HostAudio::tones, this);
    if (!guest_ || !keys_) {
        UV_LOG(Warn, "audio", "no sound device: %s", SDL_GetError());
        close();
        return false;
    }
    rate_ = 8000;
    SDL_ResumeAudioStreamDevice(guest_);
    SDL_ResumeAudioStreamDevice(keys_);
    return true;
}

void HostAudio::close() {
    close_recording();
    std::lock_guard<std::mutex> lock(mutex_);
    if (guest_) SDL_DestroyAudioStream(guest_);
    if (keys_) SDL_DestroyAudioStream(keys_);
    guest_ = keys_ = nullptr;
}

void HostAudio::set_volume(float volume) {
    std::lock_guard<std::mutex> lock(mutex_);
    volume = std::clamp(volume, 0.0f, 1.0f);
    if (guest_) SDL_SetAudioStreamGain(guest_, volume);
    if (keys_) SDL_SetAudioStreamGain(keys_, volume);
}

void HostAudio::play(const s16* frames, std::size_t count, unsigned rate) {
    if (!enabled_ || count == 0) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!guest_) return;
    if (rate != rate_) {
        const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, static_cast<int>(rate)};
        SDL_SetAudioStreamFormat(guest_, &spec, nullptr);
        rate_ = rate;
    }
    const int queued = SDL_GetAudioStreamQueued(guest_);
    if (queued > static_cast<int>(rate) * 4 / 4) return;  // a quarter of a second ahead
    if (queued == 0) {
        // Starting (or caught up): a little silence first, so the next
        // slice of the machine arrives before this runs out.
        std::vector<s16> lead(static_cast<std::size_t>(rate) * 2 * 30 / 1000);
        SDL_PutAudioStreamData(guest_, lead.data(), static_cast<int>(lead.size() * sizeof(s16)));
    }
    SDL_PutAudioStreamData(guest_, frames, static_cast<int>(count * 2 * sizeof(s16)));
}

bool HostAudio::open_recording() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (recording_) return true;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        UV_LOG(Warn, "audio", "no sound: %s", SDL_GetError());
        return false;
    }
    const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, 44100};
    recording_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_RECORDING, &spec, nullptr, nullptr);
    if (!recording_) {
        UV_LOG(Warn, "audio", "no recording device: %s", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return false;
    }
    recording_rate_ = 44100;
    SDL_ResumeAudioStreamDevice(recording_);
    return true;
}

void HostAudio::close_recording() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!recording_) return;
    SDL_DestroyAudioStream(recording_);
    recording_ = nullptr;
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

void HostAudio::record(s16* frames, std::size_t count, double rate) {
    std::fill(frames, frames + count * 2, s16{0});
    std::lock_guard<std::mutex> lock(mutex_);
    if (!recording_ || count == 0) return;
    const unsigned r = static_cast<unsigned>(std::lround(rate));
    if (r != recording_rate_) {
        // (the stream's output side: what is taken from it)
        const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, static_cast<int>(r)};
        SDL_SetAudioStreamFormat(recording_, nullptr, &spec);
        recording_rate_ = r;
    }
    const int frame_bytes = 2 * static_cast<int>(sizeof(s16));
    const int want = static_cast<int>(count) * frame_bytes;
    // Keep up with the device: what is more than a tenth of a second behind
    // this block is dropped (a guest running slower than real time).
    int excess = SDL_GetAudioStreamAvailable(recording_) - want - static_cast<int>(r / 10) * frame_bytes;
    excess -= excess % frame_bytes;  // whole frames
    if (excess > 0) {
        std::vector<u8> drop(static_cast<std::size_t>(excess));
        SDL_GetAudioStreamData(recording_, drop.data(), excess);
    }
    SDL_GetAudioStreamData(recording_, frames, want);
}

void SDLCALL HostAudio::tones(void* self, SDL_AudioStream* stream, int additional, int) {
    static_cast<HostAudio*>(self)->make_tones(stream, additional);
}

// The bell is a square wave while it is on (and for a moment after a ring
// begins); a click a short burst.
void HostAudio::make_tones(SDL_AudioStream* stream, int bytes) {
    if (ring_.exchange(false)) ring_left_ = kRingFrames;
    if (click_.exchange(false)) click_left_ = kClickFrames;
    std::vector<s16> out(static_cast<std::size_t>(bytes) / sizeof(s16));
    for (s16& s : out) {
        int v = 0;
        if (bell_ || ring_left_) {
            v += (phase_ < kToneRate / kBellHz / 2) ? kBellLevel : -kBellLevel;
            phase_ = (phase_ + 1) % (kToneRate / kBellHz);
            if (ring_left_) --ring_left_;
        }
        if (click_left_) {
            v += (click_left_ & 8) ? kClickLevel : -kClickLevel;
            --click_left_;
        }
        s = static_cast<s16>(enabled_ ? std::clamp(v, -32768, 32767) : 0);
    }
    SDL_PutAudioStreamData(stream, out.data(), static_cast<int>(out.size() * sizeof(s16)));
}

}  // namespace uv::app
