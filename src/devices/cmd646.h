// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
// CMD (Silicon Image) PCI0646U2 IDE controller (pci1095,646): two ATA
// channels, each in native-PCI mode (command block at BAR0/BAR2, control
// block at BAR1/BAR3 + 2) or compatibility mode (the legacy 1F0/3F6 and
// 170/376 ports), and the bus-master DMA engines at BAR4, whose registers
// also appear in configuration space at 0x70. The controller's own
// registers: channel enables (CNTRL), timing, the per-channel interrupt
// status bits (CFR bit 2, ARTTIM23 bit 4, MRDMODE bits 2 and 3, cleared by
// writing one) and the interrupt blocks (MRDMODE bits 4 and 5).
#pragma once

#include <array>

#include "devices/ata.h"
#include "devices/pci.h"

namespace uv {

class Cmd646 final : public pci::Function {
public:
    static constexpr u8 kCfr = 0x50, kCntrl = 0x51, kArttim23 = 0x57, kBmide = 0x70;

    Cmd646(Scheduler& scheduler, double cpu_hz);
    const char* name() const override { return "cmd646"; }

    void reset() override;
    AtaChannel& channel(unsigned n) { return n ? ch1_ : ch0_; }

    u32 config_read(u8 reg, unsigned size) override;
    void config_write(u8 reg, unsigned size, u32 value) override;
    bool io_read(u32 addr, unsigned size, u32& value) override;
    Claim claim(u32 addr, bool io, Decoded& d) const override;
    bool io_write(u32 addr, unsigned size, u32 value) override;

protected:
    bool bar_read(unsigned bar, u32 offset, unsigned size, u64& value) override;
    bool bar_write(unsigned bar, u32 offset, unsigned size, u64 value) override;

private:
    bool enabled(unsigned ch) const { return cfg8(kCntrl) & (ch ? 0x08 : 0x04); }
    bool native(unsigned ch) const { return cfg8(pci::kClassProg) & (ch ? 0x04 : 0x01); }
    u32 command_block_read(unsigned ch, u32 offset, unsigned size);
    void command_block_write(unsigned ch, u32 offset, unsigned size, u32 value);
    u32 control_block_read(unsigned ch, u32 offset, unsigned size);
    void control_block_write(unsigned ch, u32 offset, unsigned size, u32 value);
    u8 bm_read(unsigned offset);
    void bm_write(unsigned offset, u8 value);
    void channel_irq(unsigned ch, bool level);
    void update_line();

    AtaChannel ch0_, ch1_;
    std::array<bool, 2> level_{};
    u8 intr_ = 0;     // latched interrupt status: bit 0 primary, bit 1 secondary
    u8 mrdmode_ = 0;  // MRDMODE without its status bits
    u8 bmidecsr_ = 0;
    std::array<u8, 2> udidetcr_{0xF0, 0xF0};  // as a real Ultra 10 reads them
};

}  // namespace uv
