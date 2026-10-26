// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
// Sun Advanced PCI Bridge ("Simba", pci108e,5000): two PCI-to-PCI bridges
// in one device (function 0 is bus B, the slots; function 1 is bus A, the
// on-board devices). Instead of base/limit windows each forwards the
// 2 MiB blocks of I/O space and 512 MiB blocks of memory space whose bits
// are set in its I/O and memory address map registers. Its implementation
// registers include a tick counter that software uses for timeouts.
#pragma once

#include <functional>

#include "devices/pci.h"

namespace uv {

class Simba final : public pci::Bridge {
public:
    static constexpr u8 kTick = 0xB0, kIoMap = 0xDE, kMemMap = 0xDF;

    // `bus_a`: this is function 1 (on-board devices), which forwards
    // everything at power-on so the boot PROM is reachable. `ticks` returns
    // the tick counter's free-running count.
    Simba(bool bus_a, std::function<u64()> ticks);

    const char* name() const override { return bus_a_ ? "simba-a" : "simba-b"; }
    void reset() override;
    u32 config_read(u8 reg, unsigned size) override;
    void config_write(u8 reg, unsigned size, u32 value) override;

protected:
    bool forwards_mem(u32 addr) const override;
    bool forwards_io(u32 addr) const override;

private:
    bool bus_a_;
    std::function<u64()> ticks_;
    u64 tick_base_ = 0;  // count at the last write of the tick register
};

}  // namespace uv
