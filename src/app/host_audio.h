// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
// The window's sound: the machine's audio output, and the keyboard's bell
// and click, on the host's default playback device (SDL3).
#pragma once

#include <atomic>
#include <mutex>

#include <SDL3/SDL.h>

#include "core/types.h"

namespace uv::app {

class HostAudio {
public:
    HostAudio() = default;
    ~HostAudio() { close(); }
    HostAudio(const HostAudio&) = delete;
    HostAudio& operator=(const HostAudio&) = delete;

    // False if there is no sound device (everything else then does nothing).
    bool open();
    void close();

    // 16-bit stereo frames at a rate, from the machine's thread. More than a
    // quarter of a second waiting to be heard (a guest running faster than
    // real time) is dropped.
    void play(const s16* frames, std::size_t count, unsigned rate);

    void set_enabled(bool on) { enabled_ = on; }
    void set_volume(float volume);  // 0 to 1

    // The keyboard: its bell on or off, a ring begun (heard for at least a
    // moment however short), a key's click.
    void set_bell(bool on) { bell_ = on; }
    void ring() { ring_ = true; }
    void click() { click_ = true; }

private:
    static void SDLCALL tones(void* self, SDL_AudioStream* stream, int additional, int total);
    void make_tones(SDL_AudioStream* stream, int bytes);

    SDL_AudioStream* guest_ = nullptr;  // the machine's audio
    SDL_AudioStream* keys_ = nullptr;   // bell and click, made as they are wanted
    std::mutex mutex_;
    unsigned rate_ = 0;
    std::atomic<bool> enabled_{true}, bell_{false}, ring_{false}, click_{false};
    // Made on SDL's audio thread
    unsigned ring_left_ = 0, click_left_ = 0, phase_ = 0;
};

}  // namespace uv::app
