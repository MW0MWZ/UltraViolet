// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
#include "devices/simba.h"

namespace uv {

Simba::Simba(bool bus_a, std::function<u64()> ticks)
    : pci::Bridge(0x108E, 0x5000, 0x11), bus_a_(bus_a), ticks_(std::move(ticks)) {
    set_cfg16(pci::kCommand, pci::kCmdMemory);       // memory decode is on at power-on
    set_cfg16(pci::kStatus, 0x02A0);                 // 66 MHz capable, fast back-to-back, medium DEVSEL
    // Implementation registers 0xB8-0xFF: interrupt acknowledge, retry
    // limits, latency timers, secondary control, the address maps, PCI
    // control and diagnostics are writable; the DMA and PIO error status
    // registers (0xC8, 0xE8) are write-one-to-clear.
    for (unsigned r = 0xB8; r < 0x100; r += 4) set_wmask(static_cast<u8>(r), 4, 0xFFFFFFFF);
    for (unsigned r : {0xC8u, 0xCCu, 0xE8u, 0xECu}) {
        set_wmask(static_cast<u8>(r), 4, 0);
        set_w1c(static_cast<u8>(r), 4, 0xFFFFFFFF);
    }
    if (bus_a) {
        set_cfg8(kIoMap, 0xFF);
        set_cfg8(kMemMap, 0xFF);
    }
    save_reset_state();
}

void Simba::reset() {
    pci::Bridge::reset();
    tick_base_ = ticks_ ? ticks_() : 0;
}

u32 Simba::config_read(u8 reg, unsigned size) {
    if (reg >= kTick && reg < kTick + 8) {
        const u64 t = (ticks_ ? ticks_() : 0) - tick_base_;
        const unsigned shift = 8 * (reg - kTick);
        const u64 v = t >> shift;
        return size == 4 ? static_cast<u32>(v) : static_cast<u32>(v & ((1u << (8 * size)) - 1));
    }
    return pci::Bridge::config_read(reg, size);
}

void Simba::config_write(u8 reg, unsigned size, u32 value) {
    if (reg >= kTick && reg < kTick + 8) {
        // Writing the tick register sets the count (only the low word is
        // taken from a write to it; the high word restarts at zero).
        tick_base_ = (ticks_ ? ticks_() : 0) - (reg == kTick ? value : 0);
        return;
    }
    pci::Bridge::config_write(reg, size, value);
}

bool Simba::forwards_mem(u32 addr) const {
    return (command() & pci::kCmdMemory) && ((cfg8(kMemMap) >> (addr >> 29)) & 1);
}

bool Simba::forwards_io(u32 addr) const {
    return (command() & pci::kCmdIo) && ((cfg8(kIoMap) >> ((addr >> 21) & 7)) & 1);
}

}  // namespace uv
