// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
// One channel of the RIO EBus DMA engine (the parallel port, the two audio
// directions and the floppy each have one): a control/status register, an
// address and a byte count, all little-endian. The engine moves data
// between its device and memory as a PCI master and is also the path for
// the device's interrupt: the CSR shows it as INT_PEND, together with
// terminal count, and the channel interrupts when INT_EN is set. Bit
// layout as used by the Linux ebus DMA code.
#pragma once

#include <cstddef>
#include <functional>

#include "mem/bus.h"

namespace uv {

class EbusDma final : public Device {
public:
    static constexpr u32 kSize = 0x10;
    // CSR bits
    static constexpr u32 kIntPend = 1u << 0, kErrPend = 1u << 1, kDrain = 1u << 2, kIntEn = 1u << 4,
                         kReset = 1u << 7, kWrite = 1u << 8, kEnDma = 1u << 9, kCycPend = 1u << 10,
                         kEnCnt = 1u << 13, kTc = 1u << 14, kTciDis = 1u << 23, kEnNext = 1u << 24,
                         kDmaOn = 1u << 25, kALoaded = 1u << 26, kNaLoaded = 1u << 27;
    // DEV_ID (bits 31:28): all four channels on a real Ultra 10 read 0xC.
    static constexpr u32 kDevId = 0xCu << 28;

    explicit EbusDma(const char* name) : name_(name) {}

    const char* name() const override { return name_; }
    bool read(u64 offset, unsigned size, u64& value) override;
    bool write(u64 offset, unsigned size, u64 value) override;

    void reset();
    // The channel's interrupt to the system (to the Sabre).
    void set_irq_sink(std::function<void(bool)> sink) { irq_ = std::move(sink); }
    // PCI master access to memory (through the Sabre's IOMMU).
    void set_dma_port(std::function<bool(u32, u8*, std::size_t)> read, std::function<bool(u32, const u8*, std::size_t)> write) {
        mem_read_ = std::move(read);
        mem_write_ = std::move(write);
    }

    // The device's interrupt request line.
    void set_device_irq(bool asserted);
    // Device side of a transfer: moves up to `length` bytes to memory (when
    // the CSR selects writes to memory) or from it, and returns how many
    // moved. Reaching the end of the count sets TC.
    std::size_t to_memory(const u8* data, std::size_t length);
    std::size_t from_memory(u8* data, std::size_t length);

    u32 csr() const;

private:
    std::size_t grant(std::size_t length, bool to_memory) const;  // bytes the channel will take now
    std::size_t complete(std::size_t n, bool ok);
    void update_irq();

    const char* name_;
    std::function<void(bool)> irq_;
    std::function<bool(u32, u8*, std::size_t)> mem_read_;
    std::function<bool(u32, const u8*, std::size_t)> mem_write_;
    u32 control_ = 0;  // the writable CSR bits
    u32 addr_ = 0, count_ = 0;
    bool loaded_ = false;  // an address and count are waiting to be used
    bool tc_ = false, err_ = false, device_irq_ = false;
    bool irq_out_ = false;
};

}  // namespace uv
