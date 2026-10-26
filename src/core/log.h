// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
// Minimal levelled logging. Formatting cost is only paid when the level is enabled.
#pragma once

#include "core/types.h"

namespace uv::log {

enum class Level : int { Trace = 0, Debug, Info, Warn, Error, Off };

void set_level(Level level);
Level level();
void write(Level level, const char* tag, const char* fmt, ...) UV_PRINTF(3, 4);

}  // namespace uv::log

#define UV_LOG(lvl, tag, ...)                                                   \
    do {                                                                        \
        if (::uv::log::Level::lvl >= ::uv::log::level())                        \
            ::uv::log::write(::uv::log::Level::lvl, tag, __VA_ARGS__);          \
    } while (0)
