// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
// A raw disk or CD image file, read and written in place at byte offsets.
// Reads beyond the end of the file return zeros, so an image need not be a
// whole number of blocks.
#pragma once

#include <fstream>
#include <string>

#include "core/types.h"

namespace uv {

class DiskImage {
public:
    // Opens an existing image; `read_only` also applies if the file cannot
    // be opened for writing.
    bool open(const std::string& path, bool read_only);
    // Creates (or extends) an image of `size` bytes, sparse where the host
    // allows, and opens it for writing.
    bool create(const std::string& path, u64 size);
    void close();

    bool is_open() const { return f_.is_open(); }
    bool read_only() const { return read_only_; }
    u64 size() const { return size_; }
    const std::string& path() const { return path_; }

    bool read(u64 offset, u8* data, std::size_t length);
    bool write(u64 offset, const u8* data, std::size_t length);
    void flush();
    // Reads and writes so far (for an activity light).
    u64 accesses() const { return accesses_; }

private:
    std::fstream f_;
    std::string path_;
    u64 size_ = 0;
    bool read_only_ = true;
    u64 accesses_ = 0;
};

}  // namespace uv
