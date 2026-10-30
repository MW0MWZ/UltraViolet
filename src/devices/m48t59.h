// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
// ST M48T59 timekeeper: 8 KiB of battery-backed SRAM (OpenBoot's NVRAM
// and the IDPROM) whose top 16 bytes are the clock: control (write and
// read freeze, calibration), BCD seconds to year, the alarm, the watchdog
// and the flags. The clock runs from the host's clock plus the offset the
// guest last set.
//
// With a scheduler attached, the alarm (once a second, minute, hour, day
// or month, as the repeat bits say) sets AF, and the watchdog (a multiple
// of 1/16, 1/4, 1 or 4 seconds from its last write) sets WDF; either
// drives the IRQ output (the alarm with AFE set, the watchdog with WDS
// clear) until the flags are read. A watchdog steered to the reset output
// (WDS set) pulses it and clears the watchdog, FT, AFE and ABE. On the
// Ultra 5/10 IRQ drives the power supply's POWERON_L (an alarm switches a
// system in standby on) and the reset output is not used.
#pragma once

#include <array>
#include <ctime>
#include <functional>
#include <memory>
#include <string>

#include "core/scheduler.h"
#include "mem/bus.h"

namespace uv {

class M48t59 final : public Device {
public:
    static constexpr std::size_t kSize = 8192;
    // The year register counts from this year (as Sun's software uses it).
    static constexpr int kYearBase = 1968;

    explicit M48t59(std::function<std::time_t()> host_clock = {});
    ~M48t59();
    // Guest time for the alarm and the watchdog (without, they do nothing).
    void attach(Scheduler& scheduler, double cpu_hz);
    // The IRQ output, and a watchdog's pulse on the reset output.
    std::function<void(bool)> on_irq;
    std::function<void()> on_watchdog_reset;
    bool irq() const { return irq_; }

    const char* name() const override { return "m48t59"; }
    bool read(u64 offset, unsigned size, u64& value) override;
    bool write(u64 offset, unsigned size, u64 value) override;
    u64 stable_until(u64 offset, unsigned size) const override;
    // The guest time (cycles) at which the host clock next moves to a new
    // second; without it, the running clock registers promise nothing.
    void set_next_second(std::function<u64()> f) { next_second_ = std::move(f); }

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
    void schedule_alarm();
    void alarm();
    void restart_watchdog();
    void watchdog();
    void set_irq(bool on);
    void latch_clock();  // copies the running time into the clock registers
    u8 read_byte(unsigned addr);
    void write_byte(unsigned addr, u8 v);

    std::array<u8, kSize> mem_{};
    std::function<std::time_t()> host_clock_;
    std::function<u64()> next_second_;
    long long offset_ = 0;   // guest time minus host time, seconds
    bool stopped_ = false;   // ST: oscillator stopped
    std::time_t stopped_at_ = 0;
    bool dirty_ = false;
    // The last time latch_clock() converted, and its register values
    // (seconds, minutes, hours, day of week, date, month, year), since the
    // clock is often read many times a second.
    std::time_t converted_ = -1;
    std::array<u8, 7> converted_regs_{};
    std::unique_ptr<Timer> alarm_timer_, watchdog_timer_;
    double cpu_hz_ = 0;
    bool irq_ = false;
};

}  // namespace uv
