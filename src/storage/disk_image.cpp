// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
#include "storage/disk_image.h"

#include <algorithm>
#include <cstring>
#include <filesystem>

#include "core/log.h"

namespace uv {

namespace fs = std::filesystem;

bool DiskImage::open(const std::string& path, bool read_only) {
    close();
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec) return false;
    if (!read_only) f_.open(path, std::ios::in | std::ios::out | std::ios::binary);
    if (!f_.is_open()) {
        f_.clear();
        f_.open(path, std::ios::in | std::ios::binary);
        if (!f_.is_open()) return false;
        if (!read_only) UV_LOG(Warn, "disk", "%s is read-only", path.c_str());
        read_only = true;
    }
    path_ = path;
    size_ = size;
    read_only_ = read_only;
    return true;
}

bool DiskImage::create(const std::string& path, u64 size) {
    close();
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        std::ofstream(path, std::ios::binary);
    }
    if (fs::file_size(path, ec) < size) fs::resize_file(path, size, ec);
    if (ec) return false;
    return open(path, false);
}

void DiskImage::close() {
    if (f_.is_open()) f_.close();
    f_.clear();
    path_.clear();
    size_ = 0;
    read_only_ = true;
}

bool DiskImage::read(u64 offset, u8* data, std::size_t length) {
    if (!f_.is_open()) return false;
    ++accesses_;
    const u64 avail = offset < size_ ? std::min<u64>(length, size_ - offset) : 0;
    if (avail) {
        f_.clear();
        f_.seekg(static_cast<std::streamoff>(offset));
        f_.read(reinterpret_cast<char*>(data), static_cast<std::streamsize>(avail));
        if (!f_) {
            f_.clear();
            return false;
        }
    }
    std::memset(data + avail, 0, length - avail);
    return true;
}

bool DiskImage::write(u64 offset, const u8* data, std::size_t length) {
    if (!f_.is_open() || read_only_) return false;
    ++accesses_;
    f_.clear();
    f_.seekp(static_cast<std::streamoff>(offset));
    f_.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(length));
    if (!f_) {
        f_.clear();
        return false;
    }
    size_ = std::max<u64>(size_, offset + length);
    return true;
}

void DiskImage::flush() {
    if (f_.is_open()) f_.flush();
}

}  // namespace uv
