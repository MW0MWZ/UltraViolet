// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
#include "app/proms.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "creator3d_fcode.h"

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

namespace {

// What an image is: a Creator3D FCode program (its start token first, and
// naming itself SUNW,ffb), and whose: 1 the card's own, 0 UltraViolet's
// open source one, -1 neither; its version ("2.9").
int creator3d_kind(const std::vector<u8>& d, std::string& version) {
    static const std::string kName = "SUNW,ffb", kTags[] = {"UltraViolet ffb.fth ", "ffb2p.fth "};
    if (d.size() < 8 || d[0] < 0xF0 || d[0] > 0xF3 || std::search(d.begin(), d.end(), kName.begin(), kName.end()) == d.end())
        return -1;
    for (int kind = 1; kind >= 0; --kind) {
        const std::string& t = kTags[kind];
        const auto tag = std::search(d.begin(), d.end(), t.begin(), t.end());
        if (tag == d.end()) continue;
        version.clear();
        for (auto q = tag + static_cast<std::ptrdiff_t>(t.size()); q != d.end() && *q > 0x20 && *q < 0x7F; ++q)
            version.push_back(static_cast<char>(*q));
        return kind;
    }
    return -1;
}

}  // namespace

std::vector<u8> builtin_creator3d_fcode() {
    return std::vector<u8>(std::begin(kCreator3dFcode), std::end(kCreator3dFcode));
}

std::string creator3d_fcode_name(const std::vector<u8>& data) {
    std::string v;
    switch (creator3d_kind(data, v)) {
        case 1: return "the card's own, ffb2p.fth " + v;
        case 0: return "UltraViolet's, ffb.fth " + v;
        default: return "";
    }
}

bool find_creator3d_fcode(const Settings& s, std::vector<u8>& data, std::string& path) {
    const std::string folder = prom_folder(s);
    // The card's over the open source one; the newest version; of the same
    // version, the longer image (a whole PROM, which also holds the card's
    // own logo, over its FCode alone).
    int best_kind = -1;
    std::vector<int> best;
    std::error_code ec;
    path.clear();
    for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
        std::vector<u8> d;
        if (!entry.is_regular_file(ec) || entry.file_size(ec) > (256u << 10) || ec || !read_file(entry.path().string(), d))
            continue;
        std::string v;
        const int kind = creator3d_kind(d, v);
        if (kind < 0) continue;
        const std::vector<int> n = version_numbers(v);
        if (kind > best_kind || (kind == best_kind && (n > best || (n == best && d.size() > data.size()))))
            best_kind = kind, best = n, path = entry.path().string(), data = std::move(d);
    }
    return !path.empty();
}

bool select_creator3d_fcode(const Settings& s, std::vector<u8>& data, std::string& path, std::string& error) {
    if (s.creator3d_fcode != "supplied") {
        data = builtin_creator3d_fcode();
        path.clear();
        return true;
    }
    if (find_creator3d_fcode(s, data, path)) return true;
    error = "no Creator3D FCode PROM image (the card's ffb2p.fth, or a build of UltraViolet's ffb.fc) in " +
            prom_folder(s) + "; or choose UltraViolet's built-in FCode in Settings";
    return false;
}

}  // namespace uv::app
