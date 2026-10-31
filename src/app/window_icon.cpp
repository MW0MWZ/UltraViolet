// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
#include "app/window_icon.h"

#include <SDL3/SDL.h>

#include "window_icon_rgba.h"  // generated from graphics/UltraViolet_icon_128.rgba

namespace uv::app {

void set_window_icon(SDL_Window* window) {
    if (!window) return;
    static_assert(sizeof(kWindowIconRgba) == kWindowIconSize * kWindowIconSize * 4);
    SDL_Surface* icon = SDL_CreateSurfaceFrom(kWindowIconSize, kWindowIconSize, SDL_PIXELFORMAT_RGBA32,
                                              const_cast<unsigned char*>(kWindowIconRgba), kWindowIconSize * 4);
    if (!icon) return;
    SDL_SetWindowIcon(window, icon);
    SDL_DestroySurface(icon);
}

}  // namespace uv::app
