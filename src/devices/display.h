// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
// A monitor's picture, whichever graphics device drives it: the window
// shows one, a screenshot saves one.
#pragma once

#include <memory>
#include <vector>

#include "core/types.h"

namespace uv {

class Display {
public:
    // A picture kept up to date by render(Frame&): each call remakes only
    // what changed since the last (everything the first time, or when
    // another device made it). `changed_top` to `changed_bottom`
    // (exclusive): the rows the last call remade (none if equal).
    struct Frame {
        std::vector<u32> pixels;  // width x height of 0x00RRGGBB
        unsigned width = 0, height = 0;
        unsigned changed_top = 0, changed_bottom = 0;

        // What the device made the picture from, for the next call.
        struct State {
            virtual ~State() = default;
        };
        std::unique_ptr<State> state;
        const Display* source = nullptr;  // the device that made it
    };

    virtual ~Display() = default;

    // The picture as the monitor would show it. False if there is none
    // (no mode set, or the video is off).
    virtual bool render(Frame& frame) const = 0;
    bool render(std::vector<u32>& pixels, unsigned& width, unsigned& height) const {
        Frame f;
        if (!render(f)) return false;
        pixels = std::move(f.pixels);
        width = f.width;
        height = f.height;
        return true;
    }
    // Where the hardware cursor's image starts on the picture, if it is
    // shown.
    virtual bool cursor_position(int& x, int& y) const = 0;
    // Changes with anything that may change the picture, so a display need
    // not be remade while it stays put.
    virtual u64 display_version() const = 0;

protected:
    // The device's state in `f`, made new if `f` was made by another.
    template <typename S>
    S& frame_state(Frame& f) const {
        if (!f.state || f.source != this || !dynamic_cast<S*>(f.state.get())) {
            f.state = std::make_unique<S>();
            f.source = this;
        }
        return static_cast<S&>(*f.state);
    }
};

}  // namespace uv
