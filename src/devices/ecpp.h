// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
// The SuperIO's parallel port (PC87332 family): the standard port's data,
// status and control registers at its base, and, when the configuration's
// PCR bit 2 enables the ECP, the extended registers at base + 0x400: the
// 16-byte FIFO (CFIFO, DFIFO or TFIFO, by the mode), CNFGA, CNFGB and the
// extended control register (ECR). With ECR's DMA enable set and its
// service bit clear, the FIFO is filled from (forward) or emptied to
// (backward) memory through the port's EBus DMA channel; reaching the
// channel's terminal count sets the service bit. Nothing is connected to
// the port: the status lines read as an Ultra 10's with no cable, and no
// peripheral cycles are run (the FIFO modes 010 and 011 fill the FIFO but
// never drain it).
#pragma once

#include <deque>

#include "mem/bus.h"

namespace uv {

class EbusDma;
class SuperIoConfig;

class Ecpp final : public Device {
public:
    static constexpr u32 kSize = 8, kExtendedOffset = 0x400, kExtendedSize = 3;

    Ecpp(const SuperIoConfig& config, EbusDma& dma);
    const char* name() const override { return "ecpp"; }
    bool read(u64 offset, unsigned size, u64& value) override;
    bool write(u64 offset, unsigned size, u64 value) override;
    void reset();

    // The extended registers' window (base + 0x400).
    Device& extended() { return extended_; }
    // Moves FIFO data through the DMA channel as far as it will go now
    // (after a change to the channel or to the port).
    void service();

private:
    class Extended final : public Device {
    public:
        explicit Extended(Ecpp& port) : port_(port) {}
        const char* name() const override { return "ecpp-ecr"; }
        bool read(u64 offset, unsigned size, u64& value) override;
        bool write(u64 offset, unsigned size, u64 value) override;

    private:
        Ecpp& port_;
    };

    bool ecp_enabled() const;
    unsigned mode() const { return ecr_ >> 5; }
    bool backward() const;
    u8 read_byte(unsigned offset);
    void write_byte(unsigned offset, u8 v);
    u8 read_extended(unsigned offset);
    void write_extended(unsigned offset, u8 v);

    static constexpr std::size_t kFifo = 16;
    const SuperIoConfig& config_;
    EbusDma& dma_;
    Extended extended_{*this};
    u8 data_ = 0, control_ = 0;
    u8 epp_[kSize - 3]{};  // the EPP registers' addresses: kept, not modelled
    u8 ecr_ = 0x15;
    std::deque<u8> fifo_;
};

}  // namespace uv
