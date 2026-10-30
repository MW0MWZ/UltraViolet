// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
#include "app/settings.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "core/log.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#endif

namespace uv::app {

std::vector<unsigned> ram_sizes() {
    // As the machine places them: banks of up to 256 MiB, each a power of
    // two, the largest first; a size is offered if it all finds a place.
    std::vector<unsigned> sizes;
    for (unsigned mb = 32; mb <= 1024; mb += 32) {
        unsigned left = mb;
        for (unsigned bank = 0; bank < Ultra5_10::kBanks && left; ++bank) {
            unsigned size = std::min(left, 256u);
            while (size & (size - 1)) size &= size - 1;
            left -= size;
        }
        if (!left) sizes.push_back(mb);
    }
    return sizes;
}

const std::vector<double>& cpu_clocks() {
    static const std::vector<double> clocks = {270, 300, 333, 360, 400, 440};
    return clocks;
}

const std::vector<unsigned>& ecache_sizes() {
    static const std::vector<unsigned> sizes = {256, 512, 1024, 2048};
    return sizes;
}

bool check_hardware(const Settings& s, std::string& error) {
    const std::vector<unsigned> sizes = ram_sizes();
    if (std::find(sizes.begin(), sizes.end(), s.ram_mb) == sizes.end()) {
        std::string list;
        for (unsigned mb : sizes) list += (list.empty() ? "" : ", ") + std::to_string(mb);
        error = "the Ultra 5/10 cannot hold " + std::to_string(s.ram_mb) +
                " MiB (four banks of 32 to 256 MiB, each a power of two); it takes " + list;
        return false;
    }
    const auto& caches = ecache_sizes();
    if (std::find(caches.begin(), caches.end(), s.ecache_kb) == caches.end()) {
        error = "the E-cache is 256, 512, 1024 or 2048 KiB";
        return false;
    }
    return true;
}

u8 keyboard_layout(const std::string& name) {
    if (name == "uk") return SunKeyboard::kLayoutUk;
    if (name == "us" || name.empty()) return SunKeyboard::kLayoutUs;
    return static_cast<u8>(std::strtoul(name.c_str(), nullptr, 0) & 0x3F);
}

Ultra5_10::Options Settings::options() const {
    Ultra5_10::Options o;
    o.ram_mb = ram_mb;
    o.cpu_hz = cpu_mhz * 1e6;
    o.ecache_bytes = ecache_kb << 10;
    o.nvram_path = nvram;
    o.disk_path = disk;
    o.cdrom_path = cdroms.empty() ? std::string() : cdroms.front();
    o.mac = mac;
    o.timing = timing;
    o.fast_disk = fast_disk;
    o.keyboard = console == "screen";
    o.keyboard_layout = keyboard_layout(keyboard);
    return o;
}

const char* timing_name(Pacer::Mode mode) {
    switch (mode) {
        case Pacer::Mode::Cycle: return "cycle";
        case Pacer::Mode::Max: return "max";
        case Pacer::Mode::Free: return "free";
        default: return "realtime";
    }
}

bool parse_timing(const std::string& text, Pacer::Mode& mode) {
    if (text == "realtime") mode = Pacer::Mode::Realtime;
    else if (text == "cycle") mode = Pacer::Mode::Cycle;
    else if (text == "max") mode = Pacer::Mode::Max;
    else if (text == "free") mode = Pacer::Mode::Free;
    else return false;
    return true;
}

std::string mac_text(const std::array<u8, 6>& mac) {
    char buf[18];
    std::snprintf(buf, sizeof buf, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return buf;
}

bool parse_mac(const std::string& text, std::array<u8, 6>& mac) {
    unsigned b[6];
    char extra;
    if (std::sscanf(text.c_str(), "%x:%x:%x:%x:%x:%x%c", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &extra) != 6)
        return false;
    for (unsigned k = 0; k < 6; ++k) {
        if (b[k] > 0xFF) return false;
        mac[k] = static_cast<u8>(b[k]);
    }
    return !(mac[0] & 1);  // (not a multicast address)
}

std::string forward_text(const net::PortForward& f) {
    return std::string(f.udp ? "udp" : "tcp") + ":" + std::to_string(f.host_port) + ":" + std::to_string(f.guest_port);
}

bool parse_forward(const std::string& text, net::PortForward& f) {
    char proto[4] = {};
    unsigned host = 0, guest = 0;
    if (std::sscanf(text.c_str(), "%3[a-z]:%u:%u", proto, &host, &guest) != 3) return false;
    const std::string p = proto;
    if ((p != "tcp" && p != "udp") || !host || host > 65535 || !guest || guest > 65535) return false;
    f = {p == "udp", static_cast<u16>(host), static_cast<u16>(guest)};
    return true;
}

void apply_log_level(const std::string& l) {
    using log::Level;
    log::set_level(l == "trace"   ? Level::Trace
                   : l == "debug" ? Level::Debug
                   : l == "info"  ? Level::Info
                   : l == "error" ? Level::Error
                   : l == "off"   ? Level::Off
                                  : Level::Warn);
}

namespace {

bool parse_bool(const std::string& v, bool& out) {
    if (v == "1" || v == "yes" || v == "true" || v == "on") out = true;
    else if (v == "0" || v == "no" || v == "false" || v == "off") out = false;
    else return false;
    return true;
}

bool parse_number(const std::string& v, double& out) {
    char* end = nullptr;
    out = std::strtod(v.c_str(), &end);
    return !v.empty() && end && *end == '\0';
}

}  // namespace

bool set_option(Settings& s, const std::string& key, const std::string& v, std::string& error) {
    double n = 0;
    bool ok = true;
    if (key == "prom_dir") s.prom_dir = v;
    else if (key == "nvram") s.nvram = v;
    else if (key == "disk") s.disk = v;
    else if (key == "cdrom") s.cdroms.push_back(v);
    else if (key == "ram_mb") ok = parse_number(v, n) && n >= 32 && n <= 1024 && n == static_cast<unsigned>(n) &&
                                  (s.ram_mb = static_cast<unsigned>(n), true);  // checked at power-on
    else if (key == "cpu_mhz") ok = parse_number(v, n) && n >= 10 && n <= 2000 && (s.cpu_mhz = n, true);
    else if (key == "ecache_kb") ok = parse_number(v, n) && n == static_cast<unsigned>(n) &&
                                     (s.ecache_kb = static_cast<unsigned>(n), true);
    else if (key == "console") ok = (v == "screen" || v == "ttya") && (s.console = v, true);
    else if (key == "keyboard") {
        char* end = nullptr;
        const unsigned long id = std::strtoul(v.c_str(), &end, 0);
        ok = (v == "us" || v == "uk" || (!v.empty() && end && *end == '\0' && id <= 0x3F)) && (s.keyboard = v, true);
    } else if (key == "mac") {
        std::array<u8, 6> mac{};
        if (v.empty()) s.mac.reset();
        else ok = parse_mac(v, mac) && (s.mac = mac, true);
    } else if (key == "net") ok = (v == "user" || v == "none" || (v.rfind("tap:", 0) == 0 && v.size() > 4)) && (s.net = v, true);
    else if (key == "hostfwd") {
        net::PortForward f;
        ok = parse_forward(v, f) && (s.forwards.push_back(f), true);
    } else if (key == "timing") ok = parse_timing(v, s.timing);
    else if (key == "block_cache") ok = parse_bool(v, s.block_cache);
    else if (key == "jit") ok = parse_bool(v, s.jit);
    else if (key == "idle_skip") ok = parse_bool(v, s.idle_skip);
    else if (key == "fast_disk") ok = parse_bool(v, s.fast_disk);
    else if (key == "mouse_auto_capture") ok = parse_bool(v, s.mouse_auto_capture);
    else if (key == "sound") ok = parse_bool(v, s.sound);
    else if (key == "volume") ok = parse_number(v, n) && n >= 0 && n <= 100 && n == static_cast<unsigned>(n) &&
                                  (s.volume = static_cast<unsigned>(n), true);
    else if (key == "wheel_keys") ok = (v == "off" || v == "arrows" || v == "pages") && (s.wheel_keys = v, true);
    else if (key == "log") ok = (v == "trace" || v == "debug" || v == "info" || v == "warn" || v == "error" || v == "off") &&
                               (s.log = v, true);
    else {
        error = "unknown setting \"" + key + "\"";
        return false;
    }
    if (!ok) error = "bad value for " + key + ": \"" + v + "\"";
    return ok;
}

std::string user_data_folder() {
#if defined(_WIN32)
    PWSTR documents = nullptr;
    std::string folder;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &documents)))
        folder = (std::filesystem::path(documents) / "UltraViolet").string();
    CoTaskMemFree(documents);
    return folder;
#else
    const char* home = std::getenv("HOME");
    if (!home || !*home) return {};
    return (std::filesystem::path(home) / "Documents" / "UltraViolet").string();
#endif
}

void make_user_data_folders() {
    const std::string base = user_data_folder();
    if (base.empty()) return;
    for (const char* sub : {"roms", "diskImages", "nvram"}) {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(base) / sub, ec);
        if (ec) UV_LOG(Warn, "settings", "cannot make %s/%s: %s", base.c_str(), sub, ec.message().c_str());
    }
}

std::string user_file(const std::string& name) {
    const std::string base = user_data_folder();
    if (base.empty()) return name;
    make_user_data_folders();
    return (std::filesystem::path(base) / name).string();
}

bool load_settings(const std::string& path, Settings& s, std::string& error) {
    std::ifstream f(path);
    if (!f) {
        error = "cannot open " + path;
        return false;
    }
    Settings loaded;
    std::string line;
    unsigned number = 0;
    while (std::getline(f, line)) {
        ++number;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const auto eq = line.find('=');
        std::string why;
        if (eq == std::string::npos || !set_option(loaded, line.substr(0, eq), line.substr(eq + 1), why)) {
            // An unknown or bad line is skipped, so a file from another
            // version still loads.
            UV_LOG(Warn, "settings", "%s:%u: %s", path.c_str(), number, why.empty() ? "not key=value" : why.c_str());
        }
    }
    s = std::move(loaded);
    return true;
}

bool save_settings(const std::string& path, const Settings& s, std::string& error) {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        f << "# UltraViolet settings\n";
        f << "prom_dir=" << s.prom_dir << "\nnvram=" << s.nvram << "\ndisk=" << s.disk << "\n";
        for (const auto& c : s.cdroms) f << "cdrom=" << c << "\n";
        f << "ram_mb=" << s.ram_mb << "\ncpu_mhz=" << s.cpu_mhz << "\necache_kb=" << s.ecache_kb
          << "\nconsole=" << s.console << "\nkeyboard=" << s.keyboard << "\nmac=" << (s.mac ? mac_text(*s.mac) : "")
          << "\nnet=" << s.net << "\n";
        for (const auto& fw : s.forwards) f << "hostfwd=" << forward_text(fw) << "\n";
        f << "timing=" << timing_name(s.timing) << "\nblock_cache=" << (s.block_cache ? 1 : 0)
          << "\njit=" << (s.jit ? 1 : 0) << "\nidle_skip=" << (s.idle_skip ? 1 : 0) << "\nfast_disk=" << (s.fast_disk ? 1 : 0) << "\nlog=" << s.log << "\nmouse_auto_capture=" << (s.mouse_auto_capture ? 1 : 0)
          << "\nwheel_keys=" << s.wheel_keys << "\nsound=" << (s.sound ? 1 : 0) << "\nvolume=" << s.volume << "\n";
        if (!f) {
            error = "cannot write " + tmp;
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        error = "cannot replace " + path + ": " + ec.message();
        return false;
    }
    return true;
}

}  // namespace uv::app
