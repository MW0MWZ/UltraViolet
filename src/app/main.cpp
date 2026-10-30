// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
// UltraViolet front end. Started with no arguments it opens the window with
// the machine powered off and the saved settings; with arguments it powers
// on at once. With --headless (or without SDL3) the first serial port
// (ttya) is this terminal and the console; Ctrl-] then q quits, and ~# at
// the start of a line sends a BREAK.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "app/detach.h"
#include "app/proms.h"
#include "app/session.h"
#include "app/settings.h"
#include "core/log.h"
#include "cpu/ultrasparc/disasm.h"
#include "net/slirp_backend.h"
#include "net/tap_backend.h"

#if defined(UV_HAVE_SDL3)
#include "app/display_sdl.h"
#endif

#if !defined(_WIN32)
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#else
#include <windows.h>
#endif

#ifndef UV_VERSION
#define UV_VERSION "dev"
#endif

using namespace uv;

namespace {

volatile std::sig_atomic_t g_interrupted = 0;
volatile std::sig_atomic_t g_next_disc = 0;

void on_stop_signal(int) { g_interrupted = 1; }

#if defined(SIGUSR1)
void on_disc_signal(int) { g_next_disc = 1; }
#endif

void usage() {
    std::printf(
        "UltraViolet %s - Sun Ultra 5/10 emulator\n"
        "usage: ultraviolet [options]\n"
        "With no options the window opens with the machine off and the last settings.\n"
        "  --version        print the version and exit\n"
        "  --settings FILE  start from a settings file saved by the window (options after it\n"
        "                   override it)\n"
        "  --prom-dir DIR   folder of flash PROM images; the newest Ultra 5/10 one is used\n"
        "                   (default: ./roms, else roms in the UltraViolet folder)\n"
        "  --nvram FILE     M48T59 NVRAM image, loaded at start and saved when it changes\n"
        "  --disk FILE      ATA disk image (primary master)\n"
        "  --disk-size MB   create the disk image at this size if it does not exist\n"
        "  --cdrom FILE     CD image in the drive (secondary master); give it more than once\n"
        "                   for a set of discs (Ctrl+Alt+E in the window, or SIGUSR1, moves on)\n"
        "  --ram MB         memory size (default 256)\n"
        "  --cpu-mhz MHZ    processor clock (default 440)\n"
        "  --ecache-kb KB   E-cache size: 256, 512, 1024 or 2048 (default 2048)\n"
        "  --console WHERE  screen (the on-board graphics, a Sun keyboard and mouse, in the\n"
        "                   window; the default) or ttya (no keyboard: OpenBoot and Solaris\n"
        "                   take the first serial port; the default with --headless)\n"
        "  --keyboard ID    the keyboard's layout: us (default), uk or a number\n"
        "  --mac ADDR       Ethernet address for a new NVRAM (default: a random Sun address)\n",
        UV_VERSION);
    // Only the network connections this build and host can make.
    const bool user = net::slirp_available(), tap = net::tap_available();
    std::fputs("  --net MODE       ", stdout);
    if (user)
        std::fputs("user:     NAT through the host, no privileges needed (the default;\n"
                   "                             the guest is 10.0.2.15, the host and gateway 10.0.2.2,\n"
                   "                             DNS 10.0.2.3)\n                   ",
                   stdout);
    if (tap) std::fputs("tap:NAME: an existing TAP interface\n                   ", stdout);
    std::fputs("none:     no cable\n", stdout);
    if (user)
        std::fputs("  --hostfwd tcp|udp:HOSTPORT:GUESTPORT\n"
                   "                   with --net user, forward a port on the host's loopback\n"
                   "                   interface to the guest (may be repeated)\n",
                   stdout);
    std::fputs("  --timing MODE    realtime (default: guest time keeps to the wall clock,\n"
               "                   the processor as fast as the host allows), cycle (one\n"
               "                   cycle an instruction, waiting for the wall clock when\n"
               "                   ahead), max (realtime without a speed limit) or free (one\n"
               "                   cycle an instruction, no waiting)\n"
               "  --no-jit         run decoded blocks without translating them to host code\n"
               "  --no-block-cache interpret every instruction individually (for comparison)\n"
               "  --no-idle-skip   run the guest's idle loops instruction by instruction\n"
               "                   (default: at once, to the next event, the host asleep)\n"
               "  --no-fast-disk   the IDE drives with a real drive's delays and DMA at the\n"
               "                   controller's rate (default: at once, at host speed)\n"
               "  --headless       no window: ttya is this terminal\n"
               "  --foreground     keep the window's messages on this terminal (the window otherwise\n"
               "                   runs in the background, writing them to ultraviolet.log)\n"
               "  --screenshot F   on exit, save the display as a binary PPM image\n"
               "  --max N          stop after N instructions; implies --headless\n"
               "  --trace          disassemble every instruction to stderr; implies --headless\n"
               "  --log LEVEL      trace, debug, info, warn, error or off (default warn)\n"
               "At the terminal console, Ctrl-] then q quits; ~# at the start of a line sends a BREAK.\n",
               stdout);
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
#else
// The Windows console: keys as they are typed (VT sequences for the arrow
// and other keys), and VT sequences understood on output.
DWORD g_saved_in_mode = 0, g_saved_out_mode = 0;
bool g_console_in = false, g_console_out = false;
void restore_terminal() {
    if (g_console_in) SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), g_saved_in_mode);
    if (g_console_out) SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), g_saved_out_mode);
    g_console_in = g_console_out = false;
}
void raw_terminal() {
    const HANDLE in = GetStdHandle(STD_INPUT_HANDLE), out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (GetConsoleMode(in, &g_saved_in_mode)) {
        g_console_in = SetConsoleMode(in, ENABLE_VIRTUAL_TERMINAL_INPUT);
    }
    if (GetConsoleMode(out, &g_saved_out_mode)) {
        g_console_out = SetConsoleMode(out, g_saved_out_mode | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
    std::atexit(restore_terminal);
}
#endif

// ttya on this terminal: what the machine sends is printed; what is typed
// goes to it (read on a thread of its own, handed over between slices).
// Ctrl-] then q quits; ~# at the start of a line sends a BREAK (as Sun's
// tip does; ~~ sends a ~).
int run_headless(app::Session& session, u64 max, bool trace) {
    Ultra5_10& m = *session.machine();
    session.set_serial_output([](unsigned port, u8 b) {
        if (port != 0) return;
        std::fputc(b, stdout);
        std::fflush(stdout);
    });
    if (trace) {
        m.cpu().trace_hook = [&m](usparc::UltraSparc& c) {
            u32 insn = 0;
            m.bus().read(c.pc() & Bus::kAddressMask, insn);
            std::fprintf(stderr, "%016llx: %08x  %s\n", static_cast<unsigned long long>(c.pc()), insn,
                         usparc::disassemble(insn, c.pc()).c_str());
        };
    }
    std::mutex in_mutex;
    std::vector<int> input;  // bytes, and kBreak
    constexpr int kBreak = -1;
    std::atomic<bool> quit{false};
    // A byte typed: Ctrl-] then q quits (false); ~# at the start of a line
    // is a BREAK.
    bool escape = false, line_start = true, tilde = false;
    auto take = [&](u8 b) {
        if (escape) {
            escape = false;
            if (b == 'q' || b == 'Q') {
                quit = true;
                return false;
            }
        } else if (b == 0x1D) {  // Ctrl-]
            escape = true;
            return true;
        }
        std::lock_guard<std::mutex> lock(in_mutex);
        if (tilde) {
            tilde = false;
            if (b == '#') {
                input.push_back(kBreak);
                return true;
            }
            if (b != '~') input.push_back('~');
        } else if (line_start && b == '~') {
            tilde = true;
            return true;
        }
        line_start = b == '\r' || b == '\n';
        input.push_back(b);
        return true;
    };
    raw_terminal();
#if !defined(_WIN32)
    std::thread reader([&] {
        while (!quit) {
            pollfd p{STDIN_FILENO, POLLIN, 0};
            if (poll(&p, 1, 100) <= 0) continue;
            u8 b;
            if (::read(STDIN_FILENO, &b, 1) != 1 || !take(b)) break;
        }
    });
#else
    std::thread reader([&] {
        const HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
        while (!quit) {
            if (!g_console_in) {  // a pipe or file: read as it comes
                u8 b;
                DWORD n = 0;
                if (!ReadFile(in, &b, 1, &n, nullptr) || n != 1 || !take(b)) break;
                continue;
            }
            if (WaitForSingleObject(in, 100) != WAIT_OBJECT_0) continue;
            INPUT_RECORD rec[16];
            DWORD n = 0;
            if (!ReadConsoleInputW(in, rec, 16, &n)) break;
            for (DWORD i = 0; i < n; ++i) {
                const KEY_EVENT_RECORD& k = rec[i].Event.KeyEvent;
                if (rec[i].EventType != KEY_EVENT || !k.bKeyDown || k.uChar.UnicodeChar == 0) continue;
                for (WORD r = 0; r < std::max<WORD>(k.wRepeatCount, 1); ++r)
                    if (k.uChar.UnicodeChar < 0x80 && !take(static_cast<u8>(k.uChar.UnicodeChar))) return;
            }
        }
    });
#endif
    u64 done = 0;
    auto next_periodic = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!quit && !g_interrupted && done < max && session.stopped().empty() && !session.switched_off()) {
        if (g_next_disc) {
            g_next_disc = 0;
            session.next_disc();
        }
        const u64 slice = std::min<u64>(max - done, 200000);
        session.run(slice);
        done += slice;
        {
            std::lock_guard<std::mutex> lock(in_mutex);
            std::string bytes;
            for (int c : input) {
                if (c != kBreak) {
                    bytes.push_back(static_cast<char>(c));
                    continue;
                }
                session.serial_input(0, bytes);
                bytes.clear();
                session.serial_break(0);
            }
            session.serial_input(0, bytes);
            input.clear();
        }
        if (const auto now = std::chrono::steady_clock::now(); now >= next_periodic) {
            next_periodic = now + std::chrono::seconds(1);
            session.periodic();
        }
    }
    quit = true;
#if defined(_WIN32)
    if (!g_console_in) reader.detach();  // (blocked reading a pipe)
    else reader.join();
#else
    reader.join();
#endif
    restore_terminal();
    if (session.switched_off()) std::fprintf(stderr, "\r\nThe system switched itself off.\r\n");
    if (const std::string why = session.stopped(); !why.empty())
        std::fprintf(stderr, "\r\nCPU stopped: %s at %016llx\r\n", why.c_str(),
                     static_cast<unsigned long long>(m.cpu().pc()));
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    app::Settings settings;
    std::string screenshot, settings_file;
    u64 max = ~u64{0}, disk_mb = 0;
    bool trace = false, headless = false, foreground = false, log_given = false, console_given = false;
#if defined(SIGPIPE)
    // A write to a connection the far end has closed (a forwarded port's
    // client going away) is an error to handle, not the end of the machine.
    std::signal(SIGPIPE, SIG_IGN);
#endif

    // Options that describe a machine. Finder on older macOS passes a process
    // serial number, which is not one, nor is --foreground.
    int real_args = 0;
    for (int i = 1; i < argc; ++i)
        if (std::strncmp(argv[i], "-psn_", 5) != 0 && std::strcmp(argv[i], "--foreground") != 0) ++real_args;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (!std::strncmp(a, "-psn_", 5)) continue;
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", a);
                std::exit(2);
            }
            return argv[++i];
        };
        // Options that map onto a setting.
        static const struct {
            const char* flag;
            const char* key;
        } kValued[] = {{"--prom-dir", "prom_dir"},   {"--nvram", "nvram"},     {"--disk", "disk"},
                       {"--cdrom", "cdrom"},         {"--mac", "mac"},         {"--net", "net"},
                       {"--hostfwd", "hostfwd"},     {"--ram", "ram_mb"},      {"--cpu-mhz", "cpu_mhz"},
                       {"--ecache-kb", "ecache_kb"}, {"--console", "console"}, {"--keyboard", "keyboard"},
                       {"--timing", "timing"},       {"--log", "log"}};
        static const struct {
            const char* flag;
            const char* key;
            const char* value;
        } kSwitches[] = {{"--no-jit", "jit", "0"}, {"--no-block-cache", "block_cache", "0"},
                         {"--no-idle-skip", "idle_skip", "0"}, {"--no-fast-disk", "fast_disk", "0"}};
        std::string key, value, error;
        for (const auto& v : kValued)
            if (!std::strcmp(a, v.flag)) key = v.key, value = next();
        for (const auto& v : kSwitches)
            if (!std::strcmp(a, v.flag)) key = v.key, value = v.value;
        if (!key.empty()) {
            if (!app::set_option(settings, key, value, error)) {
                std::fprintf(stderr, "error: %s %s\n", a, error.c_str());
                return 2;
            }
            if (key == "log") app::apply_log_level(value), log_given = true;
            if (key == "console") console_given = true;
        } else if (!std::strcmp(a, "--settings")) {
            settings_file = next();
            if (!app::load_settings(settings_file, settings, error)) {
                std::fprintf(stderr, "error: %s\n", error.c_str());
                return 2;
            }
        } else if (!std::strcmp(a, "--disk-size")) {
            disk_mb = std::strtoull(next().c_str(), nullptr, 0);
        } else if (!std::strcmp(a, "--screenshot")) {
            screenshot = next();
        } else if (!std::strcmp(a, "--max")) {
            max = std::strtoull(next().c_str(), nullptr, 0);
            headless = true;
        } else if (!std::strcmp(a, "--trace")) {
            trace = headless = true;
        } else if (!std::strcmp(a, "--headless")) {
            headless = true;
        } else if (!std::strcmp(a, "--foreground")) {
            foreground = true;
        } else if (!std::strcmp(a, "--version")) {
            std::printf("UltraViolet %s\n", UV_VERSION);
            return 0;
        } else if (!std::strcmp(a, "--help") || !std::strcmp(a, "-h")) {
            usage();
            return 0;
        } else {
            std::fprintf(stderr, "unknown option %s\n", a);
            usage();
            return 2;
        }
    }
#if !defined(UV_HAVE_SDL3)
    if (!headless) {
        std::fprintf(stderr, "note: this build has no window (SDL3 was not found when it was built); "
                             "using the terminal console\n");
        headless = true;
    }
#endif
    // Without a window, the console is ttya unless asked otherwise.
    if (headless && !console_given) settings.console = "ttya";
    {
        std::string error;
        if (!app::check_hardware(settings, error)) {
            std::fprintf(stderr, "error: %s\n", error.c_str());
            return 2;
        }
        // A machine described here needs its PROM: say so before anything starts.
        app::PromImage prom;
        if ((real_args > 0 || headless) && !app::select_prom(settings, prom, error)) {
            std::fprintf(stderr, "error: %s\n", error.c_str());
            return 2;
        }
        if (disk_mb && !settings.disk.empty() && !std::filesystem::exists(settings.disk) &&
            !app::create_blank_image(settings.disk, disk_mb << 20, error)) {
            std::fprintf(stderr, "error: %s\n", error.c_str());
            return 2;
        }
    }
    app::apply_log_level(settings.log);

#if defined(UV_HAVE_SDL3)
    // The window runs in the background, leaving the terminal free, unless
    // its messages were asked for (--log, --foreground).
    std::string log_file;
    if (!headless && !foreground && !log_given) {
        log_file = app::user_file("ultraviolet.log");
        if (app::detach_from_terminal(argc, argv, log_file) == app::Detach::Parent) return 0;
    }
#else
    (void)foreground;
    (void)log_given;
#endif

    std::signal(SIGINT, on_stop_signal);
    std::signal(SIGTERM, on_stop_signal);  // stop cleanly (the NVRAM is saved) when asked to terminate
#if defined(SIGUSR1)
    std::signal(SIGUSR1, on_disc_signal);  // next CD-ROM disc
#endif

#if defined(UV_HAVE_SDL3)
    if (!headless) {
        app::GuiOptions options;
        options.settings_file = settings_file;
        options.power_on = real_args > 0;  // started with a machine described: power it on
        options.screenshot = screenshot;
        options.log_file = log_file;
        const int rc = app::run_gui(settings, options, g_interrupted, g_next_disc);
        if (rc >= 0) return rc;
        std::fprintf(stderr, "note: no window could be opened; using the terminal console\n");
        if (!console_given) settings.console = "ttya";
    }
#endif

    app::Session session(settings);
    std::string error;
    if (!session.power_on(error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }
    const int rc = run_headless(session, max, trace);
    if (!screenshot.empty() && !session.save_screenshot(screenshot)) std::fprintf(stderr, "screenshot: no video signal\n");
    session.power_off();
    return rc;
}
