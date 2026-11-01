// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
#include "devices/simba.h"

namespace uv {

namespace {

// PCI control's ERRINT_EN (bit 8) is the bridge control's SERR# enable
// (bit 1); its PCI_SERR (bit 34) the secondary status's received system
// error (bit 14).
constexpr u16 kBcSerrEn = 0x0002, kSecRse = 0x4000;
constexpr u8 kErrIntEn = 0x01, kPciSerr = 0x04;  // in bytes 0xE1 and 0xE4

bool touches(u8 reg, unsigned size, u8 lo, u8 hi) { return reg <= hi && reg + size > lo; }

}  // namespace

Simba::Simba(bool bus_a, std::function<u64()> ticks)
    : pci::Bridge(0x108E, 0x5000, 0x13), bus_a_(bus_a), ticks_(std::move(ticks)) {
    set_cfg16(pci::kCommand, pci::kCmdMemory);       // memory decode is on at power-on
    set_cfg16(pci::kStatus, 0x02A0);                 // 66 MHz capable, fast back-to-back, medium DEVSEL
    set_cfg8(pci::kHeaderType, 0x81);                // a bridge, and one of two functions
    set_cfg8(pci::kCacheLine, 0x10);
    set_cfg8(pci::kLatency, 0x28);
    set_cfg8(pci::kSecLatency, 0x28);
    set_cfg16(pci::kSecStatus, 0x0280);              // as the primary, less 66 MHz
    // Implementation registers (APB User's Manual 805-1251, 4.1.3): the
    // primary master retry limit (0xC0); the DMA and PIO error status
    // (0xC8, 0xE8), write-one-to-clear, and their addresses (0xD0, 0xF0),
    // read-only; the retry limits, latency timers, secondary control and
    // address maps (0xD8-0xDF); PCI control (0xE0: arbitration parking and
    // priorities, ERRINT_EN, the slot enables; PCI_SERR write-one-to-clear,
    // PCI_SPEED read-only, ARBITER_EN from the x_Gnt[3]# pin, set on an
    // Ultra 10, which has 2:1 clocking); the parity diagnostics (0xF8).
    set_wmask(kRetryLimit, 1, 0xFF);
    set_wmask(0xD8, 4, 0xFFFFFFFF);
    set_wmask(0xDC, 4, 0xFFFFFFFF);
    set_wmask(kPciControl, 4, 0x003F010F);
    set_wmask(kPciControl + 4, 4, 0x00000001);
    set_w1c(kPciControl + 4, 4, 0x00000004);
    set_cfg32(kPciControl + 4, 0x00000001);
    set_wmask(0xF8, 4, 0x00000077);
    // Not implemented (APB User's Manual): the I/O and memory base and
    // limit registers (the address maps take their place), the upper I/O
    // halves, the expansion ROM and interrupt line: they read 0. Of the
    // bridge control, only secondary reset, master-abort mode, SERR# and
    // parity error response are writable.
    set_wmask(pci::kIoBase, 2, 0);
    set_wmask(pci::kMemBase, 4, 0);
    set_wmask(pci::kPrefBase, 4, 0);
    set_wmask(pci::kIoBaseUpper, 4, 0);
    set_wmask(0x38, 4, 0);
    set_wmask(pci::kInterruptLine, 1, 0);
    set_wmask(pci::kBridgeControl, 2, 0x0063);
    for (unsigned r : {0xC8u, 0xCCu, 0xE8u, 0xECu}) {
        set_wmask(static_cast<u8>(r), 4, 0);
        set_w1c(static_cast<u8>(r), 4, 0xFFFFFFFF);
    }
    // The bus with the boot flash (function 1, which Sun calls bus A and
    // the APB manual bus B) forwards all memory at reset, as the APB's BOOT
    // pin selects; the I/O maps and the other bus's start empty (APB
    // User's Manual 805-1251, chapter 4).
    if (bus_a) set_cfg8(kMemMap, 0xFF);
    save_reset_state();
}

void Simba::reset() {
    pci::Bridge::reset();
    tick_base_ = ticks_ ? ticks_() : 0;
}

// The tick register counts guest time (in PCI clocks): a clock. The rest
// of the configuration space promises nothing here.
u64 Simba::config_stable(u8 reg, unsigned) const { return reg >= kTick && reg < kTick + 8 ? Device::kClock : 0; }

u32 Simba::config_read(u8 reg, unsigned size) {
    // A read of INT ACK runs an interrupt acknowledge cycle on the secondary
    // bus: no interrupt controller is there to answer it, and the master
    // abort's all ones come back.
    if (reg >= kIntAck && reg < kIntAck + 4) return size >= 4 ? 0xFFFFFFFFu : (1u << (8 * size)) - 1;
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
        if (other_) other_->tick_base_ = tick_base_;
        return;
    }
    pci::Bridge::config_write(reg, size, value);
    after_write(reg, size);
}

void Simba::after_write(u8 reg, unsigned size) {
    if (touches(reg, size, pci::kBridgeControl, pci::kBridgeControl + 1))
        set_cfg8(kPciControl + 1, static_cast<u8>((cfg8(kPciControl + 1) & ~kErrIntEn) |
                                                  ((cfg16(pci::kBridgeControl) & kBcSerrEn) ? kErrIntEn : 0)));
    else if (touches(reg, size, kPciControl, kPciControl + 7))
        set_cfg16(pci::kBridgeControl, static_cast<u16>((cfg16(pci::kBridgeControl) & ~kBcSerrEn) |
                                                        ((cfg8(kPciControl + 1) & kErrIntEn) ? kBcSerrEn : 0)));
    // A received system error cleared through either register is cleared.
    const bool serr = (cfg16(pci::kSecStatus) & kSecRse) && (cfg8(kPciControl + 4) & kPciSerr);
    set_cfg16(pci::kSecStatus, static_cast<u16>((cfg16(pci::kSecStatus) & ~kSecRse) | (serr ? kSecRse : 0)));
    set_cfg8(kPciControl + 4, static_cast<u8>((cfg8(kPciControl + 4) & ~kPciSerr) | (serr ? kPciSerr : 0)));
    if (!other_) return;
    // The aliased registers: the primary header to 0x0F, the primary bus
    // number and the primary master retry limit.
    bool shared = false;
    for (u8 r : {u8{0x04}, u8{0x05}, u8{0x06}, u8{0x07}, u8{0x0C}, u8{0x0D}, u8{pci::kPrimaryBus}, kRetryLimit})
        if (touches(reg, size, r, r)) other_->set_cfg8(r, cfg8(r)), shared = true;
    if (shared && other_->on_config_write) other_->on_config_write();
}

bool Simba::forwards_mem(u32 addr) const {
    return (command() & pci::kCmdMemory) && ((cfg8(kMemMap) >> (addr >> 29)) & 1);
}

bool Simba::forwards_io(u32 addr) const {
    return (command() & pci::kCmdIo) && ((cfg8(kIoMap) >> ((addr >> 21) & 7)) & 1);
}

}  // namespace uv
