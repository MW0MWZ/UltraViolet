// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
// Command-line front end: runs an Ultra 5/10 with its first serial port
// (ttya) on the terminal. Ctrl-] then q quits.
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/log.h"
#include "cpu/ultrasparc/disasm.h"
#include "machine/ultra5_10/ultra5_10.h"

#if !defined(_WIN32)
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#endif

#ifndef UV_VERSION
#define UV_VERSION "dev"
#endif

namespace fs = std::filesystem;
using namespace uv;

namespace {

void usage() {
    std::printf(
        "UltraViolet %s - Sun Ultra 5/10 emulator\n"
        "usage: ultraviolet [options]\n"
        "  --prom-dir DIR   folder holding the flash PROM images (default: roms)\n"
        "  --nvram FILE     NVRAM contents, kept between runs\n"
        "  --ram MB         memory size (default 256)\n"
        "  --cpu-mhz MHZ    processor clock (default 440)\n"
        "  --max N          stop after N instructions\n"
        "  --trace          disassemble every instruction to stderr\n"
        "  --log LEVEL      trace, debug, info, warn, error or off\n"
        "At the console, Ctrl-] then q quits.\n",
        UV_VERSION);
}

// Version from a name such as ultra5_10-obp-3.31.bin, for ordering.
std::vector<int> version_of(const std::string& name) {
    std::vector<int> v;
    const auto p = name.find("-obp-");
    if (p == std::string::npos) return v;
    const char* s = name.c_str() + p + 5;
    while (*s >= '0' && *s <= '9') {
        v.push_back(std::atoi(s));
        while (*s >= '0' && *s <= '9') ++s;
        if (*s != '.') break;
        ++s;
    }
    return v;
}

// The newest Ultra 5/10 image in `dir`, preferring plain names over variants.
std::string find_prom(const std::string& dir) {
    std::string best;
    std::vector<int> best_v;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const std::string n = e.path().filename().string();
        if (n.rfind("ultra5_10-obp-", 0) != 0 || e.path().extension() != ".bin") continue;
        const std::vector<int> v = version_of(n);
        const bool plain = n.find('-', 14) == std::string::npos;
        if (best.empty() || v > best_v || (v == best_v && plain)) {
            best = e.path().string();
            best_v = v;
        }
    }
    return best;
}

#if !defined(_WIN32)
termios g_saved_termios;
bool g_raw = false;
void restore_terminal() {
    if (g_raw) tcsetattr(STDIN_FILENO, TCSANOW, &g_saved_termios);
    g_raw = false;
}
void raw_terminal() {
    if (!isatty(STDIN_FILENO) || tcgetattr(STDIN_FILENO, &g_saved_termios) != 0) return;
    termios t = g_saved_termios;
    cfmakeraw(&t);
    t.c_oflag |= OPOST | ONLCR;
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
    g_raw = true;
    std::atexit(restore_terminal);
}
#endif

}  // namespace

int main(int argc, char** argv) {
    std::string prom_dir = "roms", nvram;
    Ultra5_10::Options opt;
    u64 max = ~u64{0};
    bool trace = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", a.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--prom-dir") prom_dir = next();
        else if (a == "--nvram") nvram = next();
        else if (a == "--ram") opt.ram_mb = static_cast<u32>(std::strtoul(next().c_str(), nullptr, 0));
        else if (a == "--cpu-mhz") opt.cpu_hz = std::strtod(next().c_str(), nullptr) * 1e6;
        else if (a == "--max") max = std::strtoull(next().c_str(), nullptr, 0);
        else if (a == "--trace") trace = true;
        else if (a == "--log") {
            const std::string l = next();
            static const char* kNames[] = {"trace", "debug", "info", "warn", "error", "off"};
            for (int n = 0; n < 6; ++n)
                if (l == kNames[n]) log::set_level(static_cast<log::Level>(n));
        } else if (a == "--version") {
            std::printf("UltraViolet %s\n", UV_VERSION);
            return 0;
        } else if (a == "--help" || a == "-h") {
            usage();
            return 0;
        } else {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            usage();
            return 2;
        }
    }

    const std::string prom = find_prom(prom_dir);
    if (prom.empty()) {
        std::fprintf(stderr, "no Ultra 5/10 flash PROM image (ultra5_10-obp-*.bin) in %s\n", prom_dir.c_str());
        return 1;
    }
    std::ifstream f(prom, std::ios::binary);
    opt.prom.assign(std::istreambuf_iterator<char>(f), {});
    opt.nvram_path = nvram;

    Ultra5_10 m(opt);
    m.ttya().transmit = [](u8 b) {
        std::fputc(b, stdout);
        std::fflush(stdout);
    };
    if (trace) {
        m.cpu().trace_hook = [&m](usparc::UltraSparc& c) {
            u32 insn = 0;
            m.bus().read(c.pc() & Bus::kAddressMask, insn);
            std::fprintf(stderr, "%016llx: %08x  %s\n", static_cast<unsigned long long>(c.pc()), insn,
                         usparc::disassemble(insn, c.pc()).c_str());
        };
    }

    // Terminal input arrives on its own thread and is handed to ttya
    // between run slices.
    std::mutex in_mutex;
    std::deque<u8> input;
    std::atomic<bool> quit{false};
#if !defined(_WIN32)
    raw_terminal();
    std::thread reader([&] {
        bool escape = false;
        while (!quit) {
            pollfd p{STDIN_FILENO, POLLIN, 0};
            if (poll(&p, 1, 100) <= 0) continue;
            u8 b;
            if (::read(STDIN_FILENO, &b, 1) != 1) break;
            if (escape) {
                escape = false;
                if (b == 'q' || b == 'Q') {
                    quit = true;
                    break;
                }
            } else if (b == 0x1D) {  // Ctrl-]
                escape = true;
                continue;
            }
            std::lock_guard<std::mutex> lock(in_mutex);
            input.push_back(b);
        }
    });
#endif

    u64 done = 0;
    while (!quit && done < max && !m.cpu().halted()) {
        const u64 slice = std::min<u64>(max - done, 200000);
        m.run(slice);
        done += slice;
        {
            std::lock_guard<std::mutex> lock(in_mutex);
            bool any = false;
            while (!input.empty()) {
                m.ttya().receive(input.front());
                input.pop_front();
                any = true;
            }
            if (any) m.poll_serial();
        }
    }
    quit = true;
#if !defined(_WIN32)
    reader.join();
    restore_terminal();
#endif
    if (m.cpu().halted()) std::fprintf(stderr, "\r\nCPU stopped: %s at %016llx\r\n", m.cpu().halt_reason().c_str(),
                                       static_cast<unsigned long long>(m.cpu().pc()));
    m.save_nvram();
    return 0;
}
