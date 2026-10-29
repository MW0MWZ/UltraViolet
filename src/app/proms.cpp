// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
#include "app/proms.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace uv::app {

namespace {

constexpr std::size_t kFlashSize = 1u << 20;

bool read_file(const std::string& path, std::vector<u8>& data) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    data.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

// "3.31" -> {3, 31}, for ordering.
std::vector<int> version_numbers(const std::string& v) {
    std::vector<int> n;
    int a = 0, b = 0, c = 0;
    const int got = std::sscanf(v.c_str(), "%d.%d.%d", &a, &b, &c);
    if (got >= 1) n.push_back(a);
    if (got >= 2) n.push_back(b);
    if (got >= 3) n.push_back(c);
    return n;
}

}  // namespace

PromImage identify_prom(const std::string& path) {
    PromImage p;
    p.path = path;
    std::error_code ec;
    if (std::filesystem::file_size(path, ec) != kFlashSize || ec) return p;
    std::vector<u8> data;
    if (!read_file(path, data)) return p;
    // "@(#) Sun Ultra 5/10 UPA/PCI 3.31 Version 0 created 2001/07/25 20:36"
    static const std::string kTag = "@(#) Sun Ultra 5/10 UPA/PCI ";
    auto it = std::search(data.begin(), data.end(), kTag.begin(), kTag.end());
    if (it == data.end()) return p;
    std::string text;
    for (auto q = it + static_cast<std::ptrdiff_t>(kTag.size()); q != data.end() && *q >= 0x20 && *q < 0x7F; ++q)
        text.push_back(static_cast<char>(*q));
    p.version = text.substr(0, text.find(' '));
    if (const auto at = text.find("created "); at != std::string::npos) p.date = text.substr(at + 8, 10);
    return p;
}

std::vector<PromImage> scan_proms(const std::string& folder) {
    std::vector<PromImage> found;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        PromImage p = identify_prom(entry.path().string());
        if (!p.version.empty()) found.push_back(std::move(p));
    }
    // Newest first; of the same version, the shortest name (a plain image
    // before variants such as one with OBDiag added).
    std::sort(found.begin(), found.end(), [](const PromImage& a, const PromImage& b) {
        const auto va = version_numbers(a.version), vb = version_numbers(b.version);
        if (va != vb) return va > vb;
        if (a.path.size() != b.path.size()) return a.path.size() < b.path.size();
        return a.path < b.path;
    });
    return found;
}

std::string prom_folder(const Settings& s) {
    if (!s.prom_dir.empty()) return s.prom_dir;
    std::error_code ec;
    if (std::filesystem::is_directory("roms", ec)) return "roms";
    const std::string base = user_data_folder();
    return base.empty() ? std::string("roms") : (std::filesystem::path(base) / "roms").string();
}

bool select_prom(const Settings& s, PromImage& out, std::string& error) {
    const std::string folder = prom_folder(s);
    const std::vector<PromImage> found = scan_proms(folder);
    if (!found.empty()) {
        out = found.front();
        return true;
    }
    error = "no Ultra 5/10 flash PROM image (1 MiB, OpenBoot 3.x) in " + folder +
            ": put one there, such as the image from Sun's flash update patch 106121";
    return false;
}

bool read_prom(const PromImage& p, std::vector<u8>& data, std::string& error) {
    if (!read_file(p.path, data) || data.size() != kFlashSize) {
        error = "cannot read the flash PROM image " + p.path;
        return false;
    }
    return true;
}

}  // namespace uv::app
