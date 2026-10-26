// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
#include "devices/rio_ebus.h"

#include <algorithm>

#include "core/endian.h"
#include "core/log.h"

namespace uv {

RioEbus::RioEbus() : pci::Function(0x108E, 0x1000, 0x01, 0x068000) {
    define_bar(0, kBarSize, false, kResetBar0);
    define_bar(1, kBarSize, false, kResetBar1);
    set_cfg16(pci::kCommand, pci::kCmdMemory);
    save_reset_state();
}

void RioEbus::set_prom(Device* prom, u32 size) {
    prom_ = prom;
    prom_size_ = size;
}

void RioEbus::add(u32 offset, u32 size, Device* device) { children_.push_back({offset, size, device}); }

// The PCI side carries values in little-endian significance; EBus devices
// see bytes in address order, as the processor's big-endian accesses do.
u64 RioEbus::to_bus(u64 v, unsigned size) {
    switch (size) {
        case 2: return byteswap(static_cast<u16>(v));
        case 4: return byteswap(static_cast<u32>(v));
        case 8: return byteswap(v);
        default: return v;
    }
}

bool RioEbus::bar_read(unsigned bar, u32 offset, unsigned size, u64& value) {
    if (bar == 0) {
        u64 v = ~u64{0};
        if (prom_ && prom_size_) prom_->read(offset % prom_size_, size, v);
        value = to_bus(v, size);
        return true;
    }
    for (const Child& c : children_) {
        if (offset - c.offset < c.size) {
            u64 v = 0;
            c.device->read(offset - c.offset, size, v);
            value = to_bus(v, size);
            return true;
        }
    }
    if (std::find(warned_.begin(), warned_.end(), offset) == warned_.end()) {
        warned_.push_back(offset);
        UV_LOG(Debug, "ebus", "read%u of empty EBus space +%06x", size * 8, offset);
    }
    value = ~u64{0} >> (64 - 8 * size);
    return true;
}

bool RioEbus::bar_write(unsigned bar, u32 offset, unsigned size, u64 value) {
    if (bar == 0) {
        if (prom_ && prom_size_) prom_->write(offset % prom_size_, size, to_bus(value, size));
        return true;
    }
    for (const Child& c : children_) {
        if (offset - c.offset < c.size) {
            c.device->write(offset - c.offset, size, to_bus(value, size));
            return true;
        }
    }
    if (std::find(warned_.begin(), warned_.end(), offset) == warned_.end()) {
        warned_.push_back(offset);
        UV_LOG(Debug, "ebus", "write%u of empty EBus space +%06x", size * 8, offset);
    }
    return true;
}

}  // namespace uv
