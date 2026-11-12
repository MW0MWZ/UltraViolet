// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "app/proms.h"
#include "app/session.h"
#include "core/endian.h"
#include "test.h"

using namespace uv;

namespace {

// A folder holding a flash image the session takes for OpenBoot 3.31: the
// version string, and at the power-on reset vector (+0x20) ba . ; nop.
std::string prom_folder_with_loop() {
    const auto dir = std::filesystem::temp_directory_path() / "uv_session_tests";
    std::filesystem::create_directories(dir);
    std::vector<u8> prom(1u << 20, 0);
    store_be<u32>(prom.data() + 0x20, 0x10800000);
    store_be<u32>(prom.data() + 0x24, 0x01000000);
    const char version[] = "@(#) Sun Ultra 5/10 UPA/PCI 3.31 Version 0 created 2001/07/25 20:36";
    std::memcpy(prom.data() + 0x1000, version, sizeof version);
    std::ofstream(dir / "loop.bin", std::ios::binary).write(reinterpret_cast<const char*>(prom.data()),
                                                            static_cast<std::streamsize>(prom.size()));
    return dir.string();
}

app::Settings loop_settings() {
    app::Settings s;
    s.prom_dir = prom_folder_with_loop();
    s.ram_mb = 128;
    s.net = "none";
    s.console = "ttya";
    s.timing = Pacer::Mode::Free;
    s.log = "off";
    return s;
}

u64 instructions(app::Session& session) {
    app::Session::Hold hold(session);
    return session.machine()->cpu().instructions();
}

void wait_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

}  // namespace

TEST(settings_take_only_processor_modules_sun_shipped) {
    // A clock and E-cache together, as the modules came; a clock alone
    // names its module's E-cache (360 MHz came with both: 2 MiB).
    app::Settings s;
    std::string error;
    for (const app::CpuModule& m : app::cpu_modules()) {
        s.cpu_mhz = m.mhz, s.ecache_kb = m.ecache_kb;
        CHECK(app::check_hardware(s, error));
    }
    s.cpu_mhz = 440, s.ecache_kb = 256;  // (no 440 MHz module had 256 KiB)
    CHECK(!app::check_hardware(s, error));
    CHECK(error.find("270 MHz with 256 KiB") != std::string::npos);
    s.cpu_mhz = 500, s.ecache_kb = 2048;
    CHECK(!app::check_hardware(s, error));
    CHECK_EQ(app::default_ecache_kb(270), 256u);
    CHECK_EQ(app::default_ecache_kb(300), 512u);
    CHECK_EQ(app::default_ecache_kb(360), 2048u);
    CHECK_EQ(app::default_ecache_kb(440), 2048u);
    CHECK_EQ(app::default_ecache_kb(500), 0u);
}

TEST(session_thread_runs_the_machine_and_a_hold_stops_it) {
    app::Session session(loop_settings());
    std::string error;
    {
        app::Session::Hold hold(session);
        CHECK(session.power_on(error));
    }
    if (!session.on()) return;
    session.start_thread();
    const u64 first = instructions(session);
    wait_ms(50);
    const u64 later = instructions(session);
    CHECK(later > first);  // running without the window's help
    {
        app::Session::Hold hold(session);
        const u64 held = session.machine()->cpu().instructions();
        wait_ms(30);
        CHECK(session.machine()->cpu().instructions() == held);  // still while held
        {
            app::Session::Hold again(session);  // nested: no deadlock
            CHECK(session.machine()->cpu().instructions() == held);
        }
        session.power_off();  // while the thread waits
        CHECK(!session.on());
        CHECK(session.power_on(error));
    }
    wait_ms(20);
    CHECK(instructions(session) > 0);  // running again after a power cycle
    // The session ends with the thread running: it stops and joins.
}

TEST(session_hold_is_had_while_the_machine_waits_for_the_wall_clock) {
    app::Settings s = loop_settings();
    s.timing = Pacer::Mode::Cycle;  // a cycle an instruction, waiting when ahead
    s.cpu_mhz = 270;                // the slowest module
    s.ecache_kb = 256;
    app::Session session(s);
    std::string error;
    {
        app::Session::Hold hold(session);
        CHECK(session.power_on(error));
    }
    if (!session.on()) return;
    session.start_thread();
    wait_ms(30);
    // Each hold is had at once: not after the machine's waits when it is
    // ahead of the wall clock, nor after the rest of its slice when it is
    // behind (its loop is fetched from the PROM, which some hosts run at
    // less than 270 MHz: a millisecond's slice then takes 20 ms or more).
    // The median is checked, as the host can stall any one thread.
    std::vector<std::chrono::steady_clock::duration> took;
    for (int i = 0; i < 20; ++i) {
        const auto start = std::chrono::steady_clock::now();
        { app::Session::Hold hold(session); }
        took.push_back(std::chrono::steady_clock::now() - start);
        wait_ms(3);
    }
    std::sort(took.begin(), took.end());
    CHECK(took[took.size() / 2] < std::chrono::milliseconds(20));
    CHECK(took.back() < std::chrono::milliseconds(500));
}

TEST(creator3d_prom_prefers_the_whole_prom_to_its_fcode_alone) {
    // Two images of FCode 2.9: the FCode program as read, and the whole PROM
    // (the same program, then the card's logo): the whole one is taken.
    const auto dir = std::filesystem::temp_directory_path() / "uv_ffb_prom_tests";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const std::string body = "SUNW,ffb @(#)ffb2p.fth 2.9 98/07/14 ";
    std::vector<u8> fcode = {0xF3, 0x03, 0xC7, 0x71};
    fcode.insert(fcode.end(), body.begin(), body.end());
    std::vector<u8> whole = fcode;
    whole.resize(0x10000, 0xFF);
    for (auto [name, image] : {std::pair{"a-fcode.bin", fcode}, std::pair{"b-prom.bin", whole}})
        std::ofstream(dir / name, std::ios::binary).write(reinterpret_cast<const char*>(image.data()),
                                                          static_cast<std::streamsize>(image.size()));
    app::Settings s;
    s.prom_dir = dir.string();
    std::vector<u8> data;
    std::string path, error;
    CHECK(app::find_creator3d_fcode(s, data, path));
    CHECK_EQ(data.size(), 0x10000u);
    std::filesystem::remove_all(dir);
}

TEST(creator3d_fcode_is_built_in) {
    // UltraViolet's own, its logo after it at PROM byte 0x4000 (a Sun
    // raster file).
    const std::vector<u8> d = app::builtin_creator3d_fcode();
    CHECK(app::creator3d_fcode_name(d).rfind("UltraViolet's, ffb.fth ", 0) == 0);
    CHECK(d.size() > 0x4004);
    CHECK_EQ(d[0x4000], 0x59u);
    CHECK_EQ(d[0x4001], 0xA6u);
    app::Settings s;
    s.prom_dir = (std::filesystem::temp_directory_path() / "uv_no_such_folder").string();
    s.creator3d_fcode = "supplied";
    std::vector<u8> data;
    std::string path, error;
    CHECK(!app::select_creator3d_fcode(s, data, path, error));
    CHECK(!error.empty());
}

TEST(creator3d_prom_takes_the_free_fcode_only_without_the_cards) {
    const auto dir = std::filesystem::temp_directory_path() / "uv_ffb_free_tests";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    auto fcode = [](const std::string& body) {
        std::vector<u8> d = {0xF3, 0x08, 0x00, 0x00};
        d.insert(d.end(), body.begin(), body.end());
        return d;
    };
    auto put = [&](const char* name, const std::vector<u8>& d) {
        std::ofstream(dir / name, std::ios::binary).write(reinterpret_cast<const char*>(d.data()),
                                                          static_cast<std::streamsize>(d.size()));
    };
    put("free.fc", fcode("SUNW,ffb UltraViolet ffb.fth 0.1 "));
    app::Settings s;
    s.prom_dir = dir.string();
    std::vector<u8> data;
    std::string path, error;
    CHECK(app::find_creator3d_fcode(s, data, path));
    CHECK(path.find("free.fc") != std::string::npos);
    put("card.bin", fcode("SUNW,ffb @(#)ffb2p.fth 2.9 98/07/14 "));
    CHECK(app::find_creator3d_fcode(s, data, path));
    CHECK(path.find("card.bin") != std::string::npos);
    // Taken only when chosen: the built-in one otherwise.
    CHECK(app::select_creator3d_fcode(s, data, path, error));
    CHECK(path.empty());
    CHECK(data == app::builtin_creator3d_fcode());
    s.creator3d_fcode = "supplied";
    CHECK(app::select_creator3d_fcode(s, data, path, error));
    CHECK(path.find("card.bin") != std::string::npos);
    CHECK(app::creator3d_fcode_name(data) == "the card's own, ffb2p.fth 2.9");
    std::filesystem::remove_all(dir);
}
