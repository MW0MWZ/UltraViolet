// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
// The SDL3 front end: a window showing the on-board graphics, with menus for
// the machine and its media, a settings dialog, a status bar, serial
// terminal windows, and the host's keyboard and mouse as the Sun keyboard
// and mouse.
#pragma once

#include <csignal>
#include <string>

#include "app/settings.h"

namespace uv::app {

struct GuiOptions {
    // Where the Settings dialog saves; empty: the user's settings file, which
    // is also loaded when `power_on` is false.
    std::string settings_file;
    bool power_on = false;   // power the machine on at once
    std::string screenshot;  // saved when the window closes
    std::string log_file;    // where messages go when not on a terminal (shown in About)
};

// Runs the window until it is closed or `stop` becomes set. `next_disc` is
// a request (from a signal) to change to the next CD-ROM disc. Returns the
// exit status, or -1 if no window could be opened.
int run_gui(Settings settings, const GuiOptions& options, volatile std::sig_atomic_t& stop,
            volatile std::sig_atomic_t& next_disc);

}  // namespace uv::app
