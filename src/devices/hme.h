// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
// Sun "Happy Meal" Ethernet (STP2002, the network function of the RIO
// PCIO: pci108e,1001, function 1 beside the EBus on an Ultra 5/10). One
// 32 KiB memory BAR holds the global (SEB) registers at 0, the transmit
// DMA (ETX) at 0x2000, the receive DMA (ERX) at 0x4000, the BigMAC at
// 0x6000 and the MII interface (MIF) at 0x7000, all 32-bit little-endian
// PCI registers. Transmit and receive descriptor rings of two
// little-endian words (flags, address) in memory, 2 KiB aligned; frames
// move by bus-master DMA. The MIF reaches an internal DP83840 PHY at
// address 1 by MII management frames or bit-banging.
//
// Frames take the wire time of the link's speed: a transmitted frame
// completes (its descriptor written back, TX interrupts raised) once it
// would have been sent, and received frames are taken no faster than the
// wire delivers them.
#pragma once

#include <array>
#include <memory>
#include <vector>

#include "core/scheduler.h"
#include "devices/pci.h"
#include "net/net_backend.h"

namespace uv {

class Hme final : public pci::Function {
public:
    // Global status bits.
    static constexpr u32 kStatRcvd = 1u << 0, kStatSent = 1u << 8, kStatRxToHost = 1u << 16, kStatNoRxd = 1u << 17,
                         kStatRxErrAck = 1u << 18, kStatEopErr = 1u << 22, kStatMif = 1u << 23,
                         kStatTxToMac = 1u << 24, kStatTxAll = 1u << 25, kStatTxErrAck = 1u << 26,
                         kStatMaxSize = 1u << 10;

    Hme(Scheduler& scheduler, double cpu_hz);
    const char* name() const override { return "hme"; }
    void reset() override;

    // The host side; null: no cable (the link is down).
    void set_network(std::shared_ptr<net::NetBackend> backend);
    // Takes frames the host has for the guest (between run slices).
    void poll();

    // Frames sent and received so far (for an activity light).
    u64 frames() const { return frames_; }

    // For tests: the PHY's registers (internal, address 1).
    u16 phy(unsigned reg) const { return phy_[reg & 31]; }

protected:
    bool bar_read(unsigned bar, u32 offset, unsigned size, u64& value) override;
    bool bar_write(unsigned bar, u32 offset, unsigned size, u64 value) override;

private:
    u32 read_reg(u32 offset);
    void write_reg(u32 offset, u32 value);
    void global_reset(bool tx, bool rx);
    void update_irq();
    void raise(u32 bits);

    // Transmit: the next frame from the ring, and its completion.
    void kick_tx();
    void tx_done();
    // Receive: one frame into the ring.
    bool rx_accepts(const u8* frame, std::size_t length) const;
    void receive(std::vector<u8> frame);

    // MII management: a frame (MIF FRAME register), and bit-banging.
    u16 mii_read(unsigned phy, unsigned reg);
    void mii_write(unsigned phy, unsigned reg, u16 value);
    void phy_reset();
    void phy_autoneg();
    void bitbang_clock();
    u32 mif_cfg_read() const;
    void mif_poll();

    u64 wire_cycles(std::size_t bytes) const;
    bool link_up() const { return backend_ != nullptr; }

    double cpu_hz_;
    std::shared_ptr<net::NetBackend> backend_;
    Timer tx_timer_, rx_timer_;

    // Global.
    u32 gcfg_ = 0, stat_ = 0, imask_ = 0xFF7FFFFF;
    // ETX and ERX.
    u32 etx_cfg_ = 0x3FE, etx_ring_ = 0, etx_rsize_ = 0;
    u32 erx_cfg_ = 0, erx_ring_ = 0;
    bool tx_busy_ = false;
    u32 tx_first_ = 0, tx_count_ = 0;  // the frame on the wire: its first descriptor and how many
    // BigMAC: storage by word (0x6000-0x635C), with the fields' widths.
    std::array<u32, 0xD8> bmac_{};
    // MIF.
    u32 mif_frame_ = 0, mif_cfg_ = 0, mif_imask_ = 0xFFFF, mif_bsts_ = 0;
    u32 bb_clk_ = 0, bb_data_ = 0, bb_oe_ = 0;
    // Bit-banged MII: the bits shifted in, and those being shifted out.
    u64 bb_in_ = 0;
    unsigned bb_count_ = 0;
    int bb_out_ = -1;  // the bit the PHY drives (-1: not driving)
    u32 bb_shift_ = 0;
    unsigned bb_out_left_ = 0;
    // PHY (DP83840, internal, address 1).
    std::array<u16, 32> phy_{};
    u64 frames_ = 0;
};

}  // namespace uv
