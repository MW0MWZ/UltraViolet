// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
// A machine built from Settings that can be powered on and off: owns the
// Ultra 5/10, its network connection and its set of CD-ROM discs. Used by
// the window and by the terminal console alike.
#pragma once

#include <functional>
#include <memory>
#include <string>

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
    ~Session() { power_off(); }
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

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

    // The CD-ROM drive and the set of discs: the one in the drive (-1:
    // empty), changing it, ejecting, inserting another (added to the set).
    const std::vector<std::string>& discs() const { return settings_.cdroms; }
    int disc_in_drive() const { return machine_ && !machine_->disc().empty() ? disc_ : -1; }
    bool select_disc(int index);
    void eject();
    void next_disc();
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

    Settings settings_;
    std::unique_ptr<Ultra5_10> machine_;
    std::function<void(unsigned, u8)> serial_out_;
    bool turbo_ = false;
    int disc_ = -1;
};

}  // namespace uv::app
