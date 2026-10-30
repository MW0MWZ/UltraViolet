// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
// Smaller EBus devices: the SuperIO's configuration registers, the flash
// PROM, the power control register, and a plain register block for devices not yet modelled in detail.
#pragma once

#include <algorithm>
#include <array>
#include <functional>
#include <string>
#include <vector>

#include "mem/bus.h"

namespace uv {

// National PC87332-family SuperIO configuration: an index port and a data
// port. The first read of the index port after reset returns 0x88, which
// is how firmware finds the chip; a register takes the second of two
// consecutive writes to the data port (an index write in between starts
// again).
class SuperIoConfig final : public Device {
public:
    explicit SuperIoConfig(u8 sid);
    const char* name() const override { return "superio-config"; }
    bool read(u64 offset, unsigned size, u64& value) override;
    bool write(u64 offset, unsigned size, u64 value) override;
    void reset();
    u8 reg(unsigned index) const { return regs_[index & 0xFF]; }

private:
    u8 regs_[256]{};
    u8 index_ = 0;
    u8 sid_;
    bool first_read_ = true;
    bool armed_ = false;  // one data write made: the next one updates the register
};

// AMD-style 8-bit flash: reads return the array; the unlock sequence
// (0xAA, 0x55) leads to byte program (0xA0), sector and chip erase (0x80,
// then 0x30 or 0x10) and autoselect (0x90: manufacturer and device codes).
// Programming and erasing complete at once.
class FlashProm final : public Device {
public:
    FlashProm(std::vector<u8> image, u8 manufacturer, u8 device, u32 sector_size);
    const char* name() const override { return "flashprom"; }
    bool read(u64 offset, unsigned size, u64& value) override;
    bool write(u64 offset, unsigned size, u64 value) override;
    // Whether reads return the array (false in autoselect mode), and a
    // callback when that changes.
    bool array_mode() const { return state_ != State::Autoselect; }
    std::function<void()> on_mode_change;
    const std::vector<u8>& image() const { return image_; }
    std::vector<u8>& image() { return image_; }
    bool dirty() const { return dirty_; }

private:
    u8 read_byte(u32 addr) const;
    void write_byte(u32 addr, u8 v);
    void write_command(u32 addr, u8 v);

    std::vector<u8> image_;
    u8 manufacturer_, device_;
    u32 sector_size_;
    enum class State { Read, Unlock1, Unlock2, Program, Erase1, Erase2, Erase3, Autoselect } state_ = State::Read;
    bool dirty_ = false;
};

// The power control register (the EBus "power" node): bit 0 of its first
// byte switches the system off, as OpenBoot's power-off and Solaris write
// it. Every byte reads back what was written.
class PowerControl final : public Device {
public:
    static constexpr u32 kSize = 0x100;
    static constexpr u8 kSystemOff = 0x01;
    const char* name() const override { return "power"; }
    bool read(u64 offset, unsigned size, u64& value) override;
    bool write(u64 offset, unsigned size, u64 value) override;
    void reset() { bytes_.fill(0); }
    std::function<void()> on_power_off;

private:
    std::array<u8, kSize> bytes_{};
};

// Bytes that read back what was written: for devices whose behaviour is
// not modelled yet. Accesses are logged (once per register) at debug level.
class RegisterBlock final : public Device {
public:
    RegisterBlock(std::string name, std::size_t size, u8 fill = 0);
    const char* name() const override { return name_.c_str(); }
    bool read(u64 offset, unsigned size, u64& value) override;
    bool write(u64 offset, unsigned size, u64 value) override;
    u8* data() { return bytes_.data(); }
    const u8* data() const { return bytes_.data(); }
    void reset() { std::fill(bytes_.begin(), bytes_.end(), fill_); }

private:
    std::string name_;
    u8 fill_;
    std::vector<u8> bytes_;
    std::vector<bool> seen_;
};

}  // namespace uv
