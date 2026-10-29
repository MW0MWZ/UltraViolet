// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
// Everything that describes a machine and how to run it: filled from the
// command line, or kept in a settings file by the window's Settings dialog.
#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>

#include "core/pacer.h"
#include "core/types.h"
#include "machine/ultra5_10/ultra5_10.h"
#include "net/slirp_backend.h"

namespace uv::app {

struct Settings {
    std::string prom_dir;  // folder of flash PROM images; empty: see prom_folder()
    std::string nvram, disk;
    std::vector<std::string> cdroms;  // a set of discs: the first in the drive at power-on
    unsigned ram_mb = 256;
    double cpu_mhz = 440;
    unsigned ecache_kb = 2048;
    // Where OpenBoot and Solaris put their console: "screen" (the on-board
    // graphics, with a Sun keyboard and mouse) or "ttya" (no keyboard: the
    // first serial port).
    std::string console = "screen";
    std::string keyboard = "us";  // the keyboard's layout: us, uk or a number (the DIP switch value)
    std::optional<std::array<u8, 6>> mac;
    std::string net = "user";  // user | tap:NAME | none
    std::vector<net::PortForward> forwards;
    Pacer::Mode timing = Pacer::Mode::Realtime;
    bool block_cache = true, jit = true;
    bool mouse_auto_capture = true;  // the window captures the mouse when the pointer moves onto the screen
    // The wheel, which the Sun mouse lacks, as keys: "off", "arrows" (Up
    // and Down) or "pages" (Page Up and Down).
    std::string wheel_keys = "off";
    std::string log = "warn";

    // The machine these settings describe (the PROM image, the network and
    // anything made at power-on are added by the session).
    Ultra5_10::Options options() const;
};

// The memory sizes (MiB) the four banks hold (each a power of two, 256 MiB
// at most, filled largest first).
std::vector<unsigned> ram_sizes();
// Processor clocks the Ultra 5 and 10 shipped with, and E-cache sizes.
const std::vector<double>& cpu_clocks();
const std::vector<unsigned>& ecache_sizes();
// Whether the memory size is one the banks hold; false with a message.
bool check_hardware(const Settings& s, std::string& error);
u8 keyboard_layout(const std::string& name);  // us, uk or a number

// Text forms shared by the command line and the settings file.
const char* timing_name(Pacer::Mode mode);
bool parse_timing(const std::string& text, Pacer::Mode& mode);
std::string mac_text(const std::array<u8, 6>& mac);
bool parse_mac(const std::string& text, std::array<u8, 6>& mac);
std::string forward_text(const net::PortForward& f);
bool parse_forward(const std::string& text, net::PortForward& f);
void apply_log_level(const std::string& level);

// Sets one option from its settings-file key; false (with `error`) if the
// key is unknown or the value malformed.
bool set_option(Settings& s, const std::string& key, const std::string& value, std::string& error);

// The user's folder for UltraViolet: the settings, the log, and roms,
// diskImages and nvram folders for machine files: UltraViolet in the
// user's Documents folder. Empty if the home folder is unknown.
std::string user_data_folder();
// Makes the folder and its subfolders where they are missing.
void make_user_data_folders();
// A file in the user's UltraViolet folder (the folders made if need be).
std::string user_file(const std::string& name);

// The settings file: one "key=value" per line, repeated keys for lists.
bool load_settings(const std::string& path, Settings& s, std::string& error);
bool save_settings(const std::string& path, const Settings& s, std::string& error);

}  // namespace uv::app
