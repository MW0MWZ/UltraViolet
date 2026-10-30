// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

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
    s.ram_mb = 64;
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
    s.cpu_mhz = 10;                 // far slower than the host: mostly waiting
    app::Session session(s);
    std::string error;
    {
        app::Session::Hold hold(session);
        CHECK(session.power_on(error));
    }
    if (!session.on()) return;
    session.start_thread();
    wait_ms(30);
    // Each hold is had at once, not after the machine's waits.
    for (int i = 0; i < 20; ++i) {
        const auto start = std::chrono::steady_clock::now();
        { app::Session::Hold hold(session); }
        const auto took = std::chrono::steady_clock::now() - start;
        CHECK(took < std::chrono::milliseconds(20));
        wait_ms(3);
    }
}
