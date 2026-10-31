// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
// The application's icon on its windows (title bar, taskbar, window list).
#pragma once

struct SDL_Window;

namespace uv::app {

void set_window_icon(SDL_Window* window);

}  // namespace uv::app
