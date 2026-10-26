// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
// ST M48T59 timekeeper: 8 KiB of battery-backed SRAM (OpenBoot's NVRAM
// and the IDPROM) whose top 16 bytes are the clock: control (write and
// read freeze, calibration), BCD seconds to year, the alarm, the watchdog
// and the flags. The clock runs from the host's clock plus the offset the
// guest last set.
#pragma once

#include <array>
#include <ctime>
#include <functional>
#include <string>

#include "mem/bus.h"

namespace uv {

class M48t59 final : public Device {
public:
    static constexpr std::size_t kSize = 8192;
    // The year register counts from this year (as Sun's software uses it).
    static constexpr int kYearBase = 1968;

    explicit M48t59(std::function<std::time_t()> host_clock = {});

    const char* name() const override { return "m48t59"; }
    bool read(u64 offset, unsigned size, u64& value) override;
    bool write(u64 offset, unsigned size, u64 value) override;

    u8* data() { return mem_.data(); }
    const u8* data() const { return mem_.data(); }
    bool load(const std::string& path);
    bool save(const std::string& path) const;
    bool dirty() const { return dirty_; }

    // A valid IDPROM (format 1, the machine type, the Ethernet address, a
    // serial number and the checksum) at 0x1FD8, for an empty NVRAM.
    void write_idprom(u8 machine_type, const u8 mac[6], u32 serial);

private:
    std::time_t now() const;
    void latch_clock();  // copies the running time into the clock registers
    u8 read_byte(unsigned addr);
    void write_byte(unsigned addr, u8 v);

    std::array<u8, kSize> mem_{};
    std::function<std::time_t()> host_clock_;
    long long offset_ = 0;   // guest time minus host time, seconds
    bool stopped_ = false;   // ST: oscillator stopped
    std::time_t stopped_at_ = 0;
    bool dirty_ = false;
};

}  // namespace uv
