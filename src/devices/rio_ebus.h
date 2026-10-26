// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
// The EBus of Sun's PCIO ("RIO") chip, function 0 (pci108e,1000): an
// 8-bit expansion bus behind two memory BARs. BAR0 is the boot PROM space
// (EBus space 0x10: the flash), BAR1 the device space (EBus space 0x14),
// where each device is decoded at its own offset. Both BARs are decoded
// from power-on at their reset addresses, so the processor can fetch the
// boot PROM and read the NVRAM before any configuration.
#pragma once

#include <string>
#include <vector>

#include "devices/pci.h"
#include "mem/bus.h"

namespace uv {

class RioEbus final : public pci::Function {
public:
    static constexpr u32 kBarSize = 16u << 20;
    static constexpr u32 kResetBar0 = 0xF0000000u, kResetBar1 = 0xF1000000u;

    RioEbus();
    const char* name() const override { return "ebus"; }

    // Devices in BAR0 (the PROM) and BAR1 (the rest), at `offset`.
    void set_prom(Device* prom, u32 size);
    void add(u32 offset, u32 size, Device* device);

protected:
    bool bar_read(unsigned bar, u32 offset, unsigned size, u64& value) override;
    bool bar_write(unsigned bar, u32 offset, unsigned size, u64 value) override;

private:
    struct Child {
        u32 offset, size;
        Device* device;
    };
    // PCI byte order to EBus (address order) and back.
    static u64 to_bus(u64 v, unsigned size);

    Device* prom_ = nullptr;
    u32 prom_size_ = 0;
    std::vector<Child> children_;
    std::vector<u32> warned_;
};

}  // namespace uv
