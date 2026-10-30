// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
// The HME (Happy Meal) Ethernet: configuration, resets, the MIF and its
// PHY (frames and bit-banging), transmit and receive through descriptor
// rings by DMA, filtering, checksums, interrupts and loopback.
#include <cstring>
#include <memory>
#include <vector>

#include "devices/hme.h"
#include "test.h"

using namespace uv;

namespace {

// A host side that records what is sent and delivers what a test gives it.
struct Wire : net::NetBackend {
    std::vector<std::vector<u8>> sent;
    void send(const u8* frame, std::size_t length) override { sent.emplace_back(frame, frame + length); }
    void give(const std::vector<u8>& frame) { deliver(frame.data(), frame.size()); }
};

// An HME at 100 MHz (cycles), 64 KiB of memory for DMA at bus address 0,
// and a cable to `wire`.
struct Rig {
    u64 now = 0;
    Scheduler sched;
    std::vector<u8> mem = std::vector<u8>(64 * 1024);
    std::shared_ptr<Wire> wire = std::make_shared<Wire>();
    Hme hme{sched, 100e6};
    bool irq = false;

    Rig() {
        sched.set_clock([this] { return now; });
        hme.set_interrupt_sink([this](bool a) { irq = a; });
        hme.set_dma_port(
            [this](u32 a, u8* d, std::size_t n) {
                if (a + n > mem.size()) return false;
                std::memcpy(d, mem.data() + a, n);
                return true;
            },
            [this](u32 a, const u8* d, std::size_t n) {
                if (a + n > mem.size()) return false;
                std::memcpy(mem.data() + a, d, n);
                return true;
            });
        hme.config_write(pci::kCommand, 2, pci::kCmdMemory | pci::kCmdMaster);
        hme.config_write(pci::kBar0, 4, 0x10000000);
        hme.set_network(wire);
    }
    u32 rd(u32 off) {
        u64 v = 0;
        hme.mem_read(0x10000000 + off, 4, v);
        return static_cast<u32>(v);
    }
    void wr(u32 off, u32 v) { hme.mem_write(0x10000000 + off, 4, v); }
    void run(u64 cycles) {
        now += cycles;
        sched.run_due(now);
        hme.poll();
    }
    u32 le(u32 a) const { return u32{mem[a]} | u32{mem[a + 1]} << 8 | u32{mem[a + 2]} << 16 | u32{mem[a + 3]} << 24; }
    void put_le(u32 a, u32 v) {
        for (int i = 0; i < 4; ++i) mem[a + i] = static_cast<u8>(v >> (8 * i));
    }
    // MII management frames.
    u16 mii(unsigned phy, unsigned reg) {
        wr(0x700C, 0x60020000 | phy << 23 | reg << 18);
        const u32 f = rd(0x700C);
        CHECK(f & 0x10000);
        return static_cast<u16>(f);
    }
    void mii_write(unsigned phy, unsigned reg, u16 v) { wr(0x700C, 0x50020000 | phy << 23 | reg << 18 | v); }
    // The driver's set-up (as Solaris's): MAC, rings (TX at 0x0800, 64
    // entries; RX at 0x1000, 64 entries of 1728-byte buffers from 0x4000),
    // MACs enabled.
    void init(const u8 mac[6]) {
        wr(0x0000, 3);
        wr(0x6318, mac[4] << 8 | mac[5]);
        wr(0x631C, mac[2] << 8 | mac[3]);
        wr(0x6320, mac[0] << 8 | mac[1]);
        wr(0x6230, 1522);
        wr(0x6310, 1522);
        for (u32 i = 0; i < 64; ++i) {
            put_le(0x1000 + 8 * i + 4, 0x4000 + 2048 * i % 0x8000);
            put_le(0x1000 + 8 * i, 0x80000000u | 1728u << 16);
        }
        wr(0x2008, 0x0800);
        wr(0x4004, 0x1000);
        wr(0x0104, 0x01000103);
        wr(0x202C, 3);
        wr(0x2004, rd(0x2004) | 0x3FF);
        wr(0x4000, 0x00070211);  // 64 entries, first byte at +2, checksum from byte 14, DMA on
        wr(0x6000, 0x211);       // output drivers on
        wr(0x630C, 0xA01);       // hash, own, enabled
        wr(0x620C, 0x201);       // full duplex, enabled
    }
};

const u8 kMac[6] = {0x08, 0x00, 0x20, 0x55, 0x56, 0x01};

std::vector<u8> ip_frame(const u8 dst[6], std::size_t length) {
    std::vector<u8> f(length);
    std::memcpy(f.data(), dst, 6);
    const u8 src[6] = {0x52, 0x55, 0x0A, 0x00, 0x02, 0x02};
    std::memcpy(f.data() + 6, src, 6);
    f[12] = 0x08, f[13] = 0x00;
    for (std::size_t i = 14; i < length; ++i) f[i] = static_cast<u8>(i * 7);
    return f;
}

u16 ones_sum(const u8* p, std::size_t n) {
    u32 s = 0;
    for (std::size_t i = 0; i + 1 < n; i += 2) s += u32{p[i]} << 8 | p[i + 1];
    if (n & 1) s += u32{p[n - 1]} << 8;
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return static_cast<u16>(s);
}

}  // namespace

// Configuration as a real Ultra 10's HME, and a 32 KiB memory BAR.
TEST(hme_configuration_space_and_bar) {
    Rig r;
    CHECK_EQ(r.hme.config_read(0x00, 4), 0x1001108Eu);
    CHECK_EQ(r.hme.config_read(0x08, 4), 0x02000001u);  // class 02 00 00, revision 1
    CHECK_EQ(r.hme.config_read(0x0E, 1), 0x80u);        // one of several functions
    CHECK_EQ(r.hme.config_read(0x3E, 1), 0x0Au);
    CHECK_EQ(r.hme.config_read(0x3F, 1), 0x05u);
    r.hme.config_write(pci::kBar0, 4, 0xFFFFFFFF);
    CHECK_EQ(r.hme.config_read(pci::kBar0, 4), 0xFFFF8000u);  // 32 KiB of memory
    r.hme.config_write(0x0D, 1, 0xF0);                        // the latency timer Solaris sets under Simba
    CHECK_EQ(r.hme.config_read(0x0D, 1), 0xF0u);
}

// After a reset the DMA engines are idle, ETX configuration reads 0x3FE and
// ERX's 0; ring pointers and configuration read back as written.
TEST(hme_resets_and_registers_read_back) {
    Rig r;
    r.wr(0x0000, 3);
    CHECK_EQ(r.rd(0x0000), 0u);       // reset done at once
    CHECK_EQ(r.rd(0x2004), 0x3FEu);
    CHECK_EQ(r.rd(0x4000), 0u);
    CHECK_EQ(r.rd(0x2028) & 0x1F, 1u);  // ETX idle
    CHECK_EQ(r.rd(0x401C) & 0x3F, 0u);  // ERX idle
    CHECK_EQ(r.rd(0x0104), 0xFF7FFFFFu);
    r.wr(0x4004, 0x00012800);
    CHECK_EQ(r.rd(0x4004), 0x00012800u);
    r.wr(0x4000, 0x00070211);
    CHECK_EQ(r.rd(0x4000), 0x00070211u);
    r.wr(0x620C, 0x201);
    r.wr(0x620C, 0x200);
    CHECK_EQ(r.rd(0x620C), 0x200u);  // the enable reads back clear at once
    r.wr(0x202C, 0x13);
    CHECK_EQ(r.rd(0x202C), 3u);      // four bits
}

// The MIF finds an internal PHY (MDIO0) and no external one; frames reach
// the DP83840 at address 1, other addresses read all ones; reset and
// autonegotiation complete at once; the link follows the cable.
TEST(hme_mif_frames_reach_the_internal_phy) {
    Rig r;
    CHECK_EQ(r.rd(0x7010) & 0x300, 0x100u);
    CHECK_EQ(r.mii(1, 2), 0x2000u);
    CHECK_EQ(r.mii(1, 3) & 0xFFF0, 0x5C00u);
    CHECK_EQ(r.mii(0, 1), 0xFFFFu);  // no external PHY
    CHECK_EQ(r.mii(1, 1), 0x782Du);  // 100/10, full/half; autonegotiated; link
    CHECK_EQ(r.mii(1, 5), 0x41E1u);
    r.mii_write(1, 0, 0x8000);
    CHECK_EQ(r.mii(1, 0) & 0x8000, 0u);
    r.mii_write(1, 0, 0x1200);       // restart autonegotiation
    CHECK_EQ(r.mii(1, 0) & 0x0200, 0u);
    CHECK_EQ(r.mii(1, 1) & 0x0024, 0x0024u);
    r.wr(0x7010, 1);                 // the external PHY selected: nothing answers
    CHECK_EQ(r.mii(1, 2), 0xFFFFu);
    r.wr(0x7010, 0);
    r.mii_write(1, 4, 0x0021);       // advertise 10 half only
    r.mii_write(1, 0, 0x1200);
    CHECK_EQ(r.mii(1, 25) & 0x40, 0x40u);  // 10 Mbit/s
    r.hme.set_network(nullptr);      // the cable out
    CHECK_EQ(r.mii(1, 1) & 0x0024, 0u);
    CHECK_EQ(r.mii(1, 25) & 0x20, 0x20u);  // the PHY's own link-down bit
    r.hme.set_network(r.wire);
    CHECK_EQ(r.mii(1, 25) & 0x20, 0u);
}

// Bit-banged MII (as Solaris does with hm-rev 0xA0/0xC0): a read of the
// PHY's identifier and a write, clocked by hand.
TEST(hme_mif_bit_bang_reads_and_writes_the_phy) {
    Rig r;
    r.wr(0x7010, 4);  // bit-bang
    auto bit = [&](unsigned b) {
        r.wr(0x7004, b);
        r.wr(0x7000, 0);
        r.wr(0x7000, 1);
    };
    auto header = [&](unsigned op, unsigned phy, unsigned reg) {
        r.wr(0x7008, 1);
        for (int i = 0; i < 33; ++i) bit(1);
        bit(0), bit(1), bit(op >> 1), bit(op & 1);
        for (int i = 4; i >= 0; --i) bit((phy >> i) & 1);
        for (int i = 4; i >= 0; --i) bit((reg >> i) & 1);
    };
    auto read = [&](unsigned phy, unsigned reg) {
        header(2, phy, reg);
        r.wr(0x7008, 0);
        auto sample = [&] {
            r.wr(0x7000, 0);
            const unsigned b = (r.rd(0x7010) >> 8) & 1;
            r.wr(0x7000, 1);
            return b;
        };
        const unsigned ta1 = sample(), ta2 = sample();
        u16 v = 0;
        for (int i = 0; i < 16; ++i) v = static_cast<u16>(v << 1 | sample());
        for (int i = 0; i < 3; ++i) sample();
        CHECK_EQ(ta1, 1u);
        CHECK_EQ(ta2, 0u);
        return v;
    };
    CHECK_EQ(read(1, 2), 0x2000u);
    CHECK_EQ(read(1, 4), 0x01E1u);
    r.wr(0x7010, 4 | 1);  // MDIO1 selected: nothing there, the line low
    CHECK_EQ(r.rd(0x7010) & 0x200, 0u);
    r.wr(0x7010, 4);
    header(1, 1, 4);
    bit(1), bit(0);
    for (int i = 15; i >= 0; --i) bit((0x0161 >> i) & 1);
    r.wr(0x7008, 0);
    CHECK_EQ(read(1, 4), 0x0161u);
}

// A frame in two descriptors goes out once the driver kicks; after its wire
// time the descriptors come back (OWN clear, lengths kept), the ring moves
// on and TX status is raised: TINT and TX_ALL, the interrupt with TX_ALL
// unmasked. Short frames are padded; the checksum asked for is stuffed.
TEST(hme_transmits_from_the_ring) {
    Rig r;
    r.init(kMac);
    std::vector<u8> f = ip_frame(kMac, 100);
    f[50] = 0x12, f[51] = 0x34;  // (the pseudo-header's partial sum, where the checksum goes)
    std::memcpy(r.mem.data() + 0x8000, f.data(), 40);
    std::memcpy(r.mem.data() + 0x9000, f.data() + 40, 60);
    // SOP with the checksum from byte 34, stuffed at 50; then EOP.
    r.put_le(0x0804, 0x8000);
    r.put_le(0x080C, 0x9000);
    r.put_le(0x0808, 0x80000000u | 1u << 29 | 60);
    r.put_le(0x0800, 0x80000000u | 1u << 30 | 1u << 28 | 50u << 20 | 34u << 14 | 40);
    r.wr(0x2000, 1);
    CHECK_EQ(r.wire->sent.size(), 1u);
    CHECK_EQ(r.le(0x0800) & 0x80000000u, 0x80000000u);  // not yet back: on the wire
    r.run(2000);  // (100 bytes at 100 Mbit/s: ~10 us)
    CHECK_EQ(r.le(0x0800), 0x40000000u | 1u << 28 | 50u << 20 | 34u << 14 | 40);
    CHECK_EQ(r.le(0x0808), 0x20000000u | 60);
    CHECK_EQ((r.rd(0x2008) >> 3) & 0xFF, 2u);
    CHECK(r.irq);
    const u32 st = r.rd(0x0100);
    CHECK_EQ(st & 0x03000100, 0x03000100u);  // TX_ALL, TINT, sent
    CHECK(!r.irq);
    CHECK_EQ(r.rd(0x0100), 0u);  // (cleared by the read)
    const std::vector<u8>& out = r.wire->sent[0];
    const u16 expect = static_cast<u16>(~ones_sum(f.data() + 34, f.size() - 34));
    CHECK_EQ(u32{out[50]} << 8 | out[51], u32{expect});
    CHECK(std::equal(out.begin(), out.begin() + 50, f.begin()));
    // A 59-byte frame goes out padded to 60.
    std::memcpy(r.mem.data() + 0x8000, f.data(), 59);
    r.put_le(0x0814, 0x8000);
    r.put_le(0x0810, 0x80000000u | 3u << 29 | 59);
    r.wr(0x2000, 1);
    r.run(2000);
    CHECK_EQ(r.wire->sent[1].size(), 60u);
    CHECK_EQ(r.le(0x0810), 3u << 29 | 59);
    // With TX_ALL and TINT masked, completion raises status but no interrupt.
    r.rd(0x0100);
    r.wr(0x0104, 0xFFFFFFFF);
    r.put_le(0x081C, 0x8000);
    r.put_le(0x0818, 0x80000000u | 3u << 29 | 60);
    r.wr(0x2000, 1);
    r.run(2000);
    CHECK(!r.irq);
    CHECK_EQ(r.rd(0x0100) & 0x03000000, 0x03000000u);
}

// At the host's speed (the machine's Max timing) a frame's wire time is
// next to nothing: a 1500-byte frame is back within 2 us, where at 100
// Mbit/s it takes over 120.
TEST(hme_at_host_speed_sends_without_the_wire_time) {
    for (bool fast : {false, true}) {
        Rig r;
        r.hme.set_fast(fast);
        r.init(kMac);
        const std::vector<u8> f = ip_frame(kMac, 1500);
        std::memcpy(r.mem.data() + 0x8000, f.data(), f.size());
        r.put_le(0x0804, 0x8000);
        r.put_le(0x0800, 0x80000000u | 3u << 29 | 1500);
        r.wr(0x2000, 1);
        r.run(200);  // 2 us at 100 MHz
        CHECK_EQ((r.le(0x0800) & 0x80000000u) == 0, fast);
        r.run(20000);
        CHECK_EQ(r.le(0x0800) & 0x80000000u, 0u);
    }
}

// Frames for this address, broadcast and hashed multicast arrive in the
// next buffer (from the first-byte offset), padded to 60, with their length
// and the complemented checksum from the configured start; others are not
// taken; with no buffer the frame is dropped and RX_DROP raised. Frames
// arrive no faster than the wire carries them.
TEST(hme_receives_into_the_ring) {
    Rig r;
    r.init(kMac);
    r.wire->give(ip_frame(kMac, 200));
    r.hme.poll();
    CHECK_EQ(r.le(0x1000) >> 31, 0u);
    CHECK_EQ((r.le(0x1000) >> 16) & 0x3FFF, 200u);
    const std::vector<u8> f = ip_frame(kMac, 200);
    CHECK(std::equal(f.begin(), f.end(), r.mem.begin() + 0x4000 + 2));
    CHECK_EQ(r.le(0x1000) & 0xFFFF, u32{static_cast<u16>(~ones_sum(f.data() + 14, 186))});
    CHECK(r.irq);  // RINT
    CHECK_EQ(r.rd(0x0100) & 0x00010001, 0x00010001u);
    // Another station's unicast: not taken (and the wire is busy until the
    // first frame's time is up).
    const u8 other[6] = {0x08, 0x00, 0x20, 0x11, 0x22, 0x33};
    r.wire->give(ip_frame(other, 100));
    r.wire->give(ip_frame(other, 100));
    const u8 bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    r.wire->give(ip_frame(bcast, 42));
    r.hme.poll();
    CHECK_EQ(r.le(0x1008) >> 31, 1u);  // (the wire still busy)
    for (int i = 0; i < 4; ++i) r.run(5000);
    CHECK_EQ(r.le(0x1008) >> 31, 0u);
    CHECK_EQ((r.le(0x1008) >> 16) & 0x3FFF, 60u);  // the broadcast ARP, padded
    CHECK_EQ((r.rd(0x4004) >> 3) & 0xFF, 2u);
    // A multicast group in the hash filter.
    const u8 group[6] = {0x01, 0x00, 0x5E, 0x00, 0x00, 0x01};
    u32 crc = 0xFFFFFFFF;
    for (u8 b : group) {
        crc ^= b;
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (crc & 1 ? 0xEDB88320u : 0);
    }
    const unsigned idx = crc >> 26;
    r.wr(0x634C - 4 * (idx >> 4), 1u << (idx & 15));
    r.wire->give(ip_frame(group, 64));
    r.run(5000);
    CHECK_EQ((r.le(0x1010) >> 16) & 0x3FFF, 64u);
    // Promiscuous: another station's unicast taken too.
    r.wr(0x630C, 0xA41);
    r.wire->give(ip_frame(other, 70));
    r.run(5000);
    CHECK_EQ((r.le(0x1018) >> 16) & 0x3FFF, 70u);
    // No buffer: dropped, RX_DROP.
    for (u32 i = 4; i < 64; ++i) r.put_le(0x1000 + 8 * i, 0);
    r.rd(0x0100);
    r.wire->give(ip_frame(kMac, 64));
    r.run(5000);
    CHECK_EQ(r.rd(0x0100) & 0x00020000, 0x00020000u);
}

// XIF loopback hands each transmitted frame to the receiver as well as the
// wire (OBP runs this way: its own frames come back for its loopback test,
// and with MYOWN set the copy is rejected); MII loopback keeps it off the
// wire; with the output drivers off nothing goes out.
TEST(hme_loopback_returns_transmitted_frames) {
    Rig r;
    r.init(kMac);
    std::vector<u8> f = ip_frame(kMac, 80);
    std::memcpy(f.data() + 6, kMac, 6);  // from this station
    std::memcpy(r.mem.data() + 0x8000, f.data(), f.size());
    u32 slot = 0;
    auto send = [&] {
        r.put_le(0x0804 + 8 * slot, 0x8000);
        r.put_le(0x0800 + 8 * slot, 0x80000000u | 3u << 29 | 80);
        ++slot;
        r.wr(0x2000, 1);
        r.run(5000);
    };
    r.wr(0x6000, 0x211 | 2);  // XIF loopback, MYOWN set: out, and the copy rejected
    send();
    CHECK_EQ(r.wire->sent.size(), 1u);
    CHECK_EQ(r.le(0x1000) >> 31, 1u);
    r.wr(0x630C, 0x801);      // MYOWN clear: out, and back
    send();
    CHECK_EQ(r.wire->sent.size(), 2u);
    CHECK_EQ((r.le(0x1000) >> 16) & 0x3FFF, 80u);
    CHECK(std::equal(f.begin(), f.end(), r.mem.begin() + 0x4000 + 2));
    r.wr(0x6000, 0x211 | 4);  // MII loopback: back only
    send();
    CHECK_EQ(r.wire->sent.size(), 2u);
    CHECK_EQ((r.le(0x1008) >> 16) & 0x3FFF, 80u);
    r.wr(0x6000, 0x210);      // output drivers off: nowhere
    send();
    CHECK_EQ(r.wire->sent.size(), 2u);
    CHECK_EQ(r.le(0x1010) >> 31, 1u);
}

// MIF polling latches changes of the polled PHY register: status bit 23,
// readable in global status (and not cleared by reading it) until the MIF
// status is read.
TEST(hme_mif_polling_reports_link_changes) {
    Rig r;
    r.wr(0x7014, 0xFFFB);              // unmask the link bit's change
    r.wr(0x7010, 1u << 10 | 1u << 3 | 2);  // poll PHY 1 register 1
    CHECK(!r.irq);
    r.hme.set_network(nullptr);        // the link goes down
    CHECK(r.irq);
    CHECK_EQ(r.rd(0x0100) & (1u << 23), 1u << 23);
    CHECK_EQ(r.rd(0x0100) & (1u << 23), 1u << 23);  // (still: the MIF's own status clears it)
    CHECK_EQ(r.rd(0x7018) & 0x0004, 0x0004u);
    CHECK(!r.irq);
    CHECK_EQ(r.rd(0x0100) & (1u << 23), 0u);
}
