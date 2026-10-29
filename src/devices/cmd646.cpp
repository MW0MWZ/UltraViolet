// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
#include "devices/cmd646.h"

namespace uv {

namespace {
constexpr u32 kLegacyCommand[2] = {0x1F0, 0x170}, kLegacyControl[2] = {0x3F4, 0x374};
constexpr u8 kRevision = 0x03;    // PCI0646U2
constexpr u8 kProgIf = 0x8A;      // bus master; both channels in compatibility mode, either may be switched
}  // namespace

Cmd646::Cmd646(Scheduler& scheduler, double cpu_hz)
    : pci::Function(0x1095, 0x0646, kRevision, 0x010100 | kProgIf), ch0_(scheduler, cpu_hz), ch1_(scheduler, cpu_hz) {
    define_bar(0, 8, true);
    define_bar(1, 4, true);
    define_bar(2, 8, true);
    define_bar(3, 4, true);
    define_bar(4, 16, true);
    set_cfg16(pci::kStatus, 0x0280);  // medium DEVSEL, fast back-to-back
    set_cfg16(pci::kSubsystemVendor, 0x1095);
    set_cfg16(pci::kSubsystemId, 0x0646);
    set_cfg8(pci::kInterruptPin, 1);
    set_cfg8(pci::kMinGrant, 0x02);
    set_cfg8(pci::kMaxLatency, 0x04);
    // Power-on values as a real Ultra 10 reads them at the ok prompt (OBP
    // does not write them); CNTRL has both channels enabled.
    set_cfg8(kCfr, 0x40);
    set_cfg8(kCntrl, 0xEC);
    set_cfg8(0x53, 0xC0);  // ARTTIM0
    set_cfg8(0x55, 0xC0);  // ARTTIM1
    set_cfg8(kArttim23, 0xCC);
    set_cfg8(0x59, 0x40);  // BRST
    set_wmask(pci::kClassProg, 1, 0x05);
    set_wmask(kCfr, 1, 0xF8);
    set_wmask(kCntrl, 1, 0xFF);
    set_wmask(0x52, 4, 0xFFFFFFFF);  // CMDTIM, ARTTIM0, DRWTIM0, ARTTIM1
    set_wmask(0x56, 1, 0xFF);        // DRWTIM1
    set_wmask(kArttim23, 1, 0xEF);
    set_wmask(0x58, 4, 0xFFFFFFFF);  // DRWTIM2, BRST, DRWTIM3
    save_reset_state();
    for (unsigned ch = 0; ch < 2; ++ch) {
        AtaChannel& c = channel(ch);
        c.set_irq_sink([this, ch](bool level) { channel_irq(ch, level); });
        c.set_dma([this](u32 a, u8* d, std::size_t n) { return master_read(a, d, n); },
                  [this](u32 a, const u8* d, std::size_t n) { return master_write(a, d, n); });
    }
}

void Cmd646::reset() {
    pci::Function::reset();
    intr_ = mrdmode_ = bmidecsr_ = 0;
    udidetcr_ = {0xF0, 0xF0};
    level_ = {};
    ch0_.reset();
    ch1_.reset();
    update_line();
}

void Cmd646::channel_irq(unsigned ch, bool level) {
    if (level && !level_[ch]) intr_ |= static_cast<u8>(1u << ch);
    level_[ch] = level;
    update_line();
}

void Cmd646::update_line() {
    const bool line = (level_[0] && !(mrdmode_ & 0x10)) || (level_[1] && !(mrdmode_ & 0x20));
    set_interrupt(line);
}

u8 Cmd646::bm_read(unsigned offset) {
    switch (offset & 15) {
        case 0x1: return static_cast<u8>(mrdmode_ | ((intr_ & 3) << 2));
        case 0x3: return udidetcr_[0];
        case 0x9: return bmidecsr_;
        case 0xB: return udidetcr_[1];
        default: return channel((offset >> 3) & 1).bm_read(offset & 7);
    }
}

void Cmd646::bm_write(unsigned offset, u8 value) {
    switch (offset & 15) {
        case 0x1:
            intr_ &= static_cast<u8>(~((value >> 2) & 3));
            mrdmode_ = value & 0xF3;
            update_line();
            break;
        case 0x3: udidetcr_[0] = value; break;
        case 0x9: bmidecsr_ = value; break;
        case 0xB: udidetcr_[1] = value; break;
        default: channel((offset >> 3) & 1).bm_write(offset & 7, value); break;
    }
}

u32 Cmd646::config_read(u8 reg, unsigned size) {
    u32 v = 0;
    for (unsigned i = 0; i < size; ++i) {
        const unsigned r = reg + i;
        u8 b;
        if (r >= kBmide && r < kBmide + 16) b = bm_read(r - kBmide);
        else if (r == kCfr) b = static_cast<u8>(cfg8(kCfr) | ((intr_ & 1) << 2));
        else if (r == kArttim23) b = static_cast<u8>(cfg8(kArttim23) | ((intr_ & 2) << 3));
        else b = cfg8(static_cast<u8>(r));
        v |= u32{b} << (8 * i);
    }
    return v;
}

void Cmd646::config_write(u8 reg, unsigned size, u32 value) {
    for (unsigned i = 0; i < size; ++i) {
        const unsigned r = reg + i;
        const u8 b = static_cast<u8>(value >> (8 * i));
        if (r >= kBmide && r < kBmide + 16) {
            bm_write(r - kBmide, b);
            continue;
        }
        if (r == kCfr && (b & 0x04)) intr_ &= static_cast<u8>(~1);
        if (r == kArttim23 && (b & 0x10)) intr_ &= static_cast<u8>(~2);
        pci::Function::config_write(static_cast<u8>(r), 1, b);
    }
    update_line();
}

u32 Cmd646::command_block_read(unsigned ch, u32 offset, unsigned size) {
    AtaChannel& c = channel(ch);
    if (offset == 0) return c.read_command(0, size);
    u32 v = 0;
    for (unsigned i = 0; i < size; ++i) v |= (c.read_command((offset + i) & 7, 1) & 0xFF) << (8 * i);
    return v;
}

void Cmd646::command_block_write(unsigned ch, u32 offset, unsigned size, u32 value) {
    AtaChannel& c = channel(ch);
    if (offset == 0) return c.write_command(0, size, value);
    for (unsigned i = 0; i < size; ++i) c.write_command((offset + i) & 7, 1, (value >> (8 * i)) & 0xFF);
}

u32 Cmd646::control_block_read(unsigned ch, u32 offset, unsigned size) {
    u32 v = 0;
    for (unsigned i = 0; i < size; ++i) {
        const u32 r = (offset + i) & 3;
        const u8 b = r >= 2 ? channel(ch).read_control(r) : 0xFF;
        v |= u32{b} << (8 * i);
    }
    return v;
}

void Cmd646::control_block_write(unsigned ch, u32 offset, unsigned size, u32 value) {
    for (unsigned i = 0; i < size; ++i) {
        const u32 r = (offset + i) & 3;
        if (r >= 2) channel(ch).write_control(r, static_cast<u8>(value >> (8 * i)));
    }
}

bool Cmd646::bar_read(unsigned bar, u32 offset, unsigned size, u64& value) {
    if (bar == 4) {
        u32 v = 0;
        for (unsigned i = 0; i < size; ++i) v |= u32{bm_read(offset + i)} << (8 * i);
        value = v;
        return true;
    }
    const unsigned ch = bar >> 1;
    if (!enabled(ch) || !native(ch)) return false;
    value = (bar & 1) ? control_block_read(ch, offset, size) : command_block_read(ch, offset, size);
    return true;
}

bool Cmd646::bar_write(unsigned bar, u32 offset, unsigned size, u64 value) {
    if (bar == 4) {
        for (unsigned i = 0; i < size; ++i) bm_write(offset + i, static_cast<u8>(value >> (8 * i)));
        return true;
    }
    const unsigned ch = bar >> 1;
    if (!enabled(ch) || !native(ch)) return false;
    if (bar & 1) control_block_write(ch, offset, size, static_cast<u32>(value));
    else command_block_write(ch, offset, size, static_cast<u32>(value));
    return true;
}

// A channel in compatibility mode answers at the legacy ports.
pci::Function::Claim Cmd646::claim(u32 addr, bool io, Decoded& d) const {
    if (io && (command() & pci::kCmdIo)) {  // the legacy ports of a channel in compatibility mode
        for (unsigned ch = 0; ch < 2; ++ch) {
            if (native(ch) || !enabled(ch)) continue;
            if (addr - kLegacyCommand[ch] < 8 || addr - kLegacyControl[ch] < 4)
                return Claim::Special;
        }
    }
    return pci::Function::claim(addr, io, d);
}

bool Cmd646::io_read(u32 addr, unsigned size, u32& value) {
    if (command() & pci::kCmdIo) {
        for (unsigned ch = 0; ch < 2; ++ch) {
            if (native(ch) || !enabled(ch)) continue;
            if (addr - kLegacyCommand[ch] < 8) {
                value = command_block_read(ch, addr - kLegacyCommand[ch], size);
                return true;
            }
            if (addr - kLegacyControl[ch] < 4 && addr - kLegacyControl[ch] >= 2) {
                value = control_block_read(ch, addr - kLegacyControl[ch], size);
                return true;
            }
        }
    }
    return pci::Function::io_read(addr, size, value);
}

bool Cmd646::io_write(u32 addr, unsigned size, u32 value) {
    if (command() & pci::kCmdIo) {
        for (unsigned ch = 0; ch < 2; ++ch) {
            if (native(ch) || !enabled(ch)) continue;
            if (addr - kLegacyCommand[ch] < 8) {
                command_block_write(ch, addr - kLegacyCommand[ch], size, value);
                return true;
            }
            if (addr - kLegacyControl[ch] < 4 && addr - kLegacyControl[ch] >= 2) {
                control_block_write(ch, addr - kLegacyControl[ch], size, value);
                return true;
            }
        }
    }
    return pci::Function::io_write(addr, size, value);
}

}  // namespace uv
