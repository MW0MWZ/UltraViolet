// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
// PCI: functions with a standard configuration header and base address
// registers, buses that route memory, I/O and configuration cycles to them,
// and PCI-to-PCI bridges. Values here are in PCI (little-endian)
// significance: a 32-bit register reads as its value, byte 0 at its lowest
// address. The host bridge converts to and from the CPU's byte order.
#pragma once

#include <array>
#include <functional>
#include <string>
#include <vector>

#include "core/types.h"

namespace uv::pci {

// Configuration header offsets
constexpr u8 kVendorId = 0x00, kDeviceId = 0x02, kCommand = 0x04, kStatus = 0x06, kRevision = 0x08,
             kClassProg = 0x09, kCacheLine = 0x0C, kLatency = 0x0D, kHeaderType = 0x0E, kBar0 = 0x10,
             kSubsystemVendor = 0x2C, kRomBase = 0x30, kInterruptLine = 0x3C, kInterruptPin = 0x3D;
// Type 1 (bridge) header
constexpr u8 kPrimaryBus = 0x18, kSecondaryBus = 0x19, kSubordinateBus = 0x1A, kSecLatency = 0x1B,
             kIoBase = 0x1C, kIoLimit = 0x1D, kSecStatus = 0x1E, kMemBase = 0x20, kMemLimit = 0x22,
             kPrefBase = 0x24, kPrefLimit = 0x26, kIoBaseUpper = 0x30, kIoLimitUpper = 0x32,
             kBridgeControl = 0x3E;

constexpr u16 kCmdIo = 1u << 0, kCmdMemory = 1u << 1, kCmdMaster = 1u << 2;

class Bus;

// One function of a PCI device.
class Function {
public:
    Function(u16 vendor, u16 device, u8 revision, u32 class_code, u8 header_type = 0);
    virtual ~Function() = default;
    Function(const Function&) = delete;
    Function& operator=(const Function&) = delete;

    virtual const char* name() const = 0;
    // Restores the power-on configuration.
    virtual void reset();

    // Configuration space; `reg` is aligned to `size` (1, 2 or 4).
    virtual u32 config_read(u8 reg, unsigned size);
    virtual void config_write(u8 reg, unsigned size, u32 value);

    // Memory and I/O cycles addressed to this function. Return false if it
    // does not claim the address (the bus then tries others, or aborts).
    virtual bool mem_read(u32 addr, unsigned size, u64& value);
    virtual bool mem_write(u32 addr, unsigned size, u64 value);
    virtual bool io_read(u32 addr, unsigned size, u32& value);
    virtual bool io_write(u32 addr, unsigned size, u32 value);

    // Interrupt output (INTx#), wired by the platform.
    void set_interrupt_sink(std::function<void(bool)> sink) { irq_ = std::move(sink); }
    void set_interrupt(bool asserted) {
        if (irq_) irq_(asserted);
    }

    u16 command() const { return static_cast<u16>(cfg_[kCommand] | (cfg_[kCommand + 1] << 8)); }
    // Called after every configuration write (for platforms that keep fast
    // paths through this function's decode).
    std::function<void()> on_config_write;
    u32 bar_address(unsigned bar) const;

protected:
    // Declares BAR `bar` (0-5): `size` a power of two, I/O or memory.
    // `reset_address` is its value at power-on (normally 0).
    void define_bar(unsigned bar, u32 size, bool io, u32 reset_address = 0, bool prefetchable = false);
    // Accesses that fall in an enabled BAR, at `offset` into it.
    virtual bool bar_read(unsigned bar, u32 offset, unsigned size, u64& value);
    virtual bool bar_write(unsigned bar, u32 offset, unsigned size, u64 value);

    // Raw configuration bytes, the bits software may write, and the
    // write-one-to-clear bits.
    u8 cfg8(u8 reg) const { return cfg_[reg]; }
    u16 cfg16(u8 reg) const { return static_cast<u16>(cfg_[reg] | (cfg_[reg + 1] << 8)); }
    u32 cfg32(u8 reg) const { return cfg16(reg) | (u32{cfg16(reg + 2)} << 16); }
    void set_cfg8(u8 reg, u8 v) { cfg_[reg] = v; }
    void set_cfg16(u8 reg, u16 v) { cfg_[reg] = static_cast<u8>(v), cfg_[reg + 1] = static_cast<u8>(v >> 8); }
    void set_cfg32(u8 reg, u32 v) { set_cfg16(reg, static_cast<u16>(v)), set_cfg16(reg + 2, static_cast<u16>(v >> 16)); }
    void set_wmask(u8 reg, unsigned size, u32 mask);
    void set_w1c(u8 reg, unsigned size, u32 mask);
    // The power-on configuration image restored by reset().
    void save_reset_state() { reset_cfg_ = cfg_; }

    std::array<u8, 256> cfg_{}, wmask_{}, w1c_{};

private:
    struct Bar {
        u32 size = 0;  // 0: not implemented
        bool io = false;
    };
    // The BAR containing `addr` of the given kind, or -1.
    int decode(u32 addr, bool io) const;

    std::array<Bar, 6> bars_{};
    std::array<u8, 256> reset_cfg_{};
    std::function<void(bool)> irq_;
};

// A PCI bus segment: up to 32 devices of 8 functions.
class Bus {
public:
    void attach(unsigned device, unsigned function, Function* f);
    Function* at(unsigned device, unsigned function) const { return slots_[device & 31][function & 7]; }
    void reset();

    // Configuration cycle for this bus (type 0) or one below it (forwarded
    // through a bridge whose secondary..subordinate range holds `bus`).
    // Reads with no device answer all ones.
    u32 config_read(unsigned bus, unsigned device, unsigned function, u8 reg, unsigned size);
    void config_write(unsigned bus, unsigned device, unsigned function, u8 reg, unsigned size, u32 value);
    // Memory and I/O cycles: false if no function claims them (master abort).
    bool mem_read(u32 addr, unsigned size, u64& value);
    bool mem_write(u32 addr, unsigned size, u64 value);
    bool io_read(u32 addr, unsigned size, u32& value);
    bool io_write(u32 addr, unsigned size, u32 value);

    unsigned number = 0;  // this segment's bus number (set by its bridge)

private:
    friend class Bridge;
    std::array<std::array<Function*, 8>, 32> slots_{};
    std::vector<Function*> functions_;
    std::vector<class Bridge*> bridges_;
};

// A PCI-to-PCI bridge (type 1 header) with a secondary bus. It forwards
// configuration cycles for its bus range, and memory and I/O cycles in its
// windows; forwarding_mem/io may be overridden for bridges with other
// decode rules.
class Bridge : public Function {
public:
    Bridge(u16 vendor, u16 device, u8 revision);
    Bus& secondary() { return secondary_; }

    void reset() override;
    void config_write(u8 reg, unsigned size, u32 value) override;
    bool mem_read(u32 addr, unsigned size, u64& value) override;
    bool mem_write(u32 addr, unsigned size, u64 value) override;
    bool io_read(u32 addr, unsigned size, u32& value) override;
    bool io_write(u32 addr, unsigned size, u32 value) override;

    bool claims_bus(unsigned bus) const {
        return bus >= cfg8(kSecondaryBus) && bus <= cfg8(kSubordinateBus) && bus != 0;
    }

protected:
    virtual bool forwards_mem(u32 addr) const;
    virtual bool forwards_io(u32 addr) const;

private:
    Bus secondary_;
};

}  // namespace uv::pci
