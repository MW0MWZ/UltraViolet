// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
// printf-style appending to a std::string (diagnostic reports).
#pragma once

#include <cstdarg>
#include <cstdio>
#include <string>

#include "core/types.h"

namespace uv {

inline void appendf(std::string& out, const char* fmt, ...) UV_PRINTF(2, 3);

inline void appendf(std::string& out, const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > 0) out.append(buf, static_cast<std::size_t>(n) < sizeof buf ? static_cast<std::size_t>(n) : sizeof buf - 1);
}

}  // namespace uv
