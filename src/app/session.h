// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
// A machine built from Settings that can be powered on and off: owns the
// Ultra 5/10, its network connection and its set of CD-ROM discs. Used by
// the window and by the terminal console alike.
//
// The terminal console runs the machine itself (run()). The window gives it
// a thread of its own (start_thread()) and holds it (Hold) whenever it uses
// the machine or the session.
#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "app/settings.h"
#include "machine/ultra5_10/ultra5_10.h"

namespace uv::app {

// Writes a file of `bytes` zero bytes (sparse where the file system allows),
// for a blank disk or NVRAM image. Refuses to replace an existing file.
bool create_blank_image(const std::string& path, u64 bytes, std::string& error);
constexpr u64 kNvramBytes = 8192;  // M48T59

class Session {
public:
    explicit Session(Settings settings) : settings_(std::move(settings)) {}
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // From now until the session ends, a thread of its own runs the machine
    // whenever it is on and not stopped, a millisecond of guest time at a
    // go (run() is then not used).
    void start_thread();
    // While one is held the machine's thread waits between slices (a wait
    // for the wall clock is cut short), and the session and its machine
    // may be used. A hold taken while this thread already holds one (a
    // system menu filled from within) adds nothing.
    class Hold {
    public:
        explicit Hold(Session& session);
        ~Hold();
        Hold(const Hold&) = delete;
        Hold& operator=(const Hold&) = delete;

    private:
        Session& s_;
        bool nested_;
    };

    const Settings& settings() const { return settings_; }
    // Takes effect at the next power-on (the disc set is also kept in step
    // by the drive's own controls while running).
    void set_settings(Settings s) { settings_ = std::move(s); }

    bool power_on(std::string& error);
    // At once, as pulling the plug: the NVRAM is saved first.
    void power_off();
    bool on() const { return machine_ != nullptr; }
    Ultra5_10* machine() { return machine_.get(); }
    // The processor has stopped (a guest error state, or the system halted
    // itself): why, or empty.
    std::string stopped() const;
    // The system has switched itself off (OpenBoot's power-off, Solaris's
    // poweroff): the machine is to be powered off.
    bool switched_off() const { return machine_ && machine_->switched_off(); }

    // Turbo: Max timing while on, the configured timing when off. Offered
    // when the configured timing is Realtime or Cycle; kept across power
    // cycles, not a setting.
    bool turbo_available() const {
        return settings_.timing == Pacer::Mode::Realtime || settings_.timing == Pacer::Mode::Cycle;
    }
    bool turbo() const { return turbo_; }
    void set_turbo(bool on);

    // Serial ports A (ttya) and B (ttyb): what the machine sends (set before
    // or after power-on), and typing into them.
    void set_serial_output(std::function<void(unsigned port, u8 byte)> out);
    void serial_input(unsigned port, const std::string& bytes);
    // A BREAK on a port (on the console, OpenBoot's and Solaris's abort).
    void serial_break(unsigned port);
    // Where the machine's audio goes (from its thread; set before or after
    // power-on).
    void set_audio_output(Cs4231::AudioSink out);

    // The CD-ROM drive and the set of discs: the one in the drive (-1:
    // empty), changing it, ejecting, inserting another (added to the set).
    const std::vector<std::string>& discs() const { return settings_.cdroms; }
    int disc_in_drive() const { return machine_ && !machine_->disc().empty() ? disc_ : -1; }
    // Changing or ejecting the disc fails while the system has locked it in.
    bool disc_locked() const;
    bool select_disc(int index);
    bool eject();
    bool next_disc();
    bool insert_disc(const std::string& path);

    // Runs `instructions` (the machine keeps guest time to the host's clock
    // as its timing says).
    void run(u64 instructions);
    // About once a second: saves the NVRAM if the guest changed it.
    void periodic();
    // Saves the display as a binary PPM; false if there is no picture.
    bool save_screenshot(const std::string& path);

private:
    void wire_serial();
    void machine_thread();
    void host_sleep(u64 ns);

    Settings settings_;
    std::unique_ptr<Ultra5_10> machine_;
    u64 slice_ = 0;  // instructions in about a millisecond of guest time

    std::thread thread_;
    std::mutex mutex_;  // held by the machine's thread while it runs a slice, and by Hold
    std::condition_variable resume_;
    std::atomic<unsigned> holds_{0};  // holds wanted or held
    bool quit_ = false;
    std::mutex sleep_mutex_;  // the pacer's waits for the wall clock
    std::condition_variable wake_;
    std::function<void(unsigned, u8)> serial_out_;
    Cs4231::AudioSink audio_out_;
    bool turbo_ = false;
    int disc_ = -1;
};

}  // namespace uv::app
