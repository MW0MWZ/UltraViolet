// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
#include "devices/hme.h"

#include <algorithm>
#include <cstring>

#include "core/log.h"

namespace uv {

namespace {

// Register blocks in BAR0.
constexpr u32 kGlobal = 0x0000, kEtx = 0x2000, kErx = 0x4000, kBmac = 0x6000, kMif = 0x7000;
// Global.
constexpr u32 kSwReset = 0x000, kGCfg = 0x004, kStat = 0x100, kIMask = 0x104;
// ETX.
constexpr u32 kTxPend = 0x00, kEtxCfg = 0x04, kEtxRing = 0x08, kEtxSm = 0x28, kEtxRSize = 0x2C;
// ERX.
constexpr u32 kErxCfg = 0x00, kErxRing = 0x04, kErxSm = 0x1C;
// BigMAC (offsets in the block).
constexpr u32 kXif = 0x000, kTxReset = 0x208, kTxCfg = 0x20C, kTxMax = 0x230, kRxReset = 0x308, kRxCfg = 0x30C,
              kRxMax = 0x310, kMadd2 = 0x318, kMadd1 = 0x31C, kMadd0 = 0x320, kLeCnt = 0x328, kHash3 = 0x340;
constexpr u32 kXifOe = 1u << 0, kXifLoopback = 1u << 1, kXifMiiLoopback = 1u << 2;
constexpr u32 kTxEnable = 1u << 0;
constexpr u32 kRxEnable = 1u << 0, kRxPromisc = 1u << 6, kRxKeepFcs = 1u << 8, kRxMyOwn = 1u << 9,
              kRxAllMulti = 1u << 10, kRxHash = 1u << 11;
// MIF.
constexpr u32 kBbClk = 0x00, kBbData = 0x04, kBbOe = 0x08, kFrame = 0x0C, kMifCfg = 0x10, kMifIMask = 0x14,
              kMifBsts = 0x18, kMifSm = 0x1C;
constexpr u32 kMifPs = 1u << 0, kMifPoll = 1u << 1, kMifBitBang = 1u << 2, kMifMdio0 = 1u << 8, kMifMdio1 = 1u << 9;
constexpr u32 kFrameDone = 1u << 16;
// Descriptors.
constexpr u32 kOwn = 1u << 31, kTxEop = 1u << 29, kTxCsum = 1u << 28, kRxOverflow = 1u << 30;
// The internal PHY's address, and its registers.
constexpr unsigned kPhyAddress = 1;
constexpr unsigned kBmcr = 0, kBmsr = 1, kId1 = 2, kId2 = 3, kAnar = 4, kAnlpar = 5, kAner = 6, kPar = 25;
constexpr u16 kBmcrReset = 0x8000, kBmcr100 = 0x2000, kBmcrAn = 0x1000, kBmcrRestart = 0x0200, kBmcrFull = 0x0100;
constexpr u16 kBmsrBase = 0x7809, kBmsrAnDone = 0x0020, kBmsrLink = 0x0004;

// The widths of the BigMAC's registers (masks), by word; 0: none there.
constexpr u32 bmac_mask(u32 off) {
    switch (off) {
        case 0x000: return 0x3FF;     // XIF configuration
        case 0x20C: return 0x7FF;     // TX configuration
        case 0x210: case 0x214: case 0x218: case 0x21C: case 0x220: case 0x224: case 0x228: case 0x22C:
        case 0x234: case 0x238: case 0x248: case 0x24C: case 0x314: case 0x328: case 0x32C: case 0x330:
        case 0x338: return 0xFF;
        case 0x230: case 0x310: return 0x1FFF;  // maximum frame sizes
        case 0x23C: case 0x240: case 0x244: case 0x318: case 0x31C: case 0x320: case 0x324: case 0x340:
        case 0x344: case 0x348: case 0x34C: case 0x350: case 0x354: case 0x358: case 0x35C: return 0xFFFF;
        case 0x250: return 0x3FF;     // random seed
        case 0x30C: return 0x1FFF;    // RX configuration
        default: return 0;
    }
}

u32 le32(const u8* p) { return u32{p[0]} | u32{p[1]} << 8 | u32{p[2]} << 16 | u32{p[3]} << 24; }
void put_le32(u8* p, u32 v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<u8>(v >> (8 * i));
}

// The ones'-complement sum of big-endian halfwords (an odd last byte
// padded with zero).
u32 ones_sum(const u8* p, std::size_t n) {
    u32 s = 0;
    for (std::size_t i = 0; i + 1 < n; i += 2) s += u32{p[i]} << 8 | p[i + 1];
    if (n & 1) s += u32{p[n - 1]} << 8;
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return s;
}

// CRC-32 (reflected, 0xEDB88320), as the hash filter and the FCS use it.
u32 crc32(const u8* p, std::size_t n, u32 crc = 0xFFFFFFFF) {
    for (std::size_t i = 0; i < n; ++i) {
        crc ^= p[i];
        for (int b = 0; b < 8; ++b) crc = (crc >> 1) ^ (crc & 1 ? 0xEDB88320u : 0);
    }
    return crc;
}

}  // namespace

Hme::Hme(Scheduler& scheduler, double cpu_hz)
    : Function(0x108E, 0x1001, 0x01, 0x020000, 0x80),  // revision 1 (hm-rev 0xC1: MII frames), function 1 of 2
      cpu_hz_(cpu_hz),
      tx_timer_(scheduler, [this] { tx_done(); }),
      rx_timer_(scheduler, [this] { poll(); }) {  // (the wire free again: the next frame waiting)
    // As a real Ultra 10's: medium DEVSEL, fast back-to-back; min-grant
    // 0x0A, max-latency 0x05; no interrupt pin (the RIO's interrupt is the
    // board's, from OBP's "interrupts").
    set_cfg16(pci::kStatus, 0x0280);
    set_cfg8(pci::kMinGrant, 0x0A);
    set_cfg8(pci::kMaxLatency, 0x05);
    define_bar(0, 0x8000, false);
    save_reset_state();
    reset();
}

void Hme::reset() {
    pci::Function::reset();
    tx_timer_.cancel();
    rx_timer_.cancel();
    gcfg_ = 0, stat_ = 0, imask_ = 0xFF7FFFFF;
    etx_cfg_ = 0x3FE, etx_ring_ = 0, etx_rsize_ = 0;
    erx_cfg_ = 0, erx_ring_ = 0;
    tx_busy_ = false;
    bmac_ = {};
    mif_frame_ = 0, mif_cfg_ = 0, mif_imask_ = 0xFFFF, mif_bsts_ = 0;
    bb_clk_ = 0, bb_data_ = 0, bb_oe_ = 0, bb_in_ = 0, bb_count_ = 0, bb_out_ = -1, bb_shift_ = 0, bb_out_left_ = 0;
    phy_reset();
    update_irq();
}

void Hme::set_network(std::shared_ptr<net::NetBackend> backend) {
    backend_ = std::move(backend);
    phy_autoneg();  // the link comes up (or goes down) as cables do
}

// ---- registers ----

bool Hme::bar_read(unsigned bar, u32 offset, unsigned size, u64& value) {
    if (bar != 0) return false;
    // Little-endian 32-bit registers; a narrower read takes its lanes.
    const u32 v = read_reg(offset & ~3u);
    const unsigned shift = 8 * (offset & 3);
    value = size >= 4 ? v : (v >> shift) & ((1u << (8 * size)) - 1);
    return true;
}

bool Hme::bar_write(unsigned bar, u32 offset, unsigned size, u64 value) {
    if (bar != 0) return false;
    u32 v = static_cast<u32>(value);
    if (size < 4) {  // (merged into the register as it reads, without its read side effects)
        const unsigned shift = 8 * (offset & 3);
        const u32 mask = ((1u << (8 * size)) - 1) << shift;
        const u32 base = offset - (offset & 3);
        const u32 old = base == kStat || base == kMif + kMifBsts ? 0 : read_reg(base);
        v = (old & ~mask) | ((v << shift) & mask);
    }
    write_reg(offset & ~3u, v);
    return true;
}

u32 Hme::read_reg(u32 off) {
    if (off < kEtx) {
        switch (off - kGlobal) {
            case kGCfg: return gcfg_;
            case kStat: {  // all but the MIF bit clear on reading
                const u32 v = stat_ | ((mif_bsts_ & 0xFFFF & ~mif_imask_) ? kStatMif : 0);
                stat_ = 0;
                update_irq();
                return v;
            }
            case kIMask: return imask_;
            default: return 0;  // (the reset bits: done at once)
        }
    }
    if (off < kErx) {
        switch (off - kEtx) {
            case kEtxCfg: return etx_cfg_;
            case kEtxRing: return etx_ring_;
            case kEtxSm: return tx_busy_ ? 0x02 : 0x01;  // idle: 1
            case kEtxRSize: return etx_rsize_;
            default: return 0;
        }
    }
    if (off < kBmac) {
        switch (off - kErx) {
            case kErxCfg: return erx_cfg_;
            case kErxRing: return erx_ring_;
            case kErxSm: return 0;  // idle
            default: return 0;
        }
    }
    if (off < kMif) {
        const u32 b = off - kBmac;
        if (b >= 0x360) return 0;
        return bmac_[b / 4] & bmac_mask(b);
    }
    switch (off - kMif) {
        case kBbClk: return bb_clk_;
        case kBbData: return bb_data_;
        case kBbOe: return bb_oe_;
        case kFrame: return mif_frame_;
        case kMifCfg: return mif_cfg_read();
        case kMifIMask: return mif_imask_;
        case kMifBsts: {  // the change bits clear on reading (and with them, global status bit 23)
            const u32 v = mif_bsts_;
            mif_bsts_ &= 0xFFFF0000;
            update_irq();
            return v;
        }
        case kMifSm: return 0;
        default: return 0;
    }
}

void Hme::write_reg(u32 off, u32 v) {
    if (off < kEtx) {
        switch (off - kGlobal) {
            case kSwReset: global_reset(v & 1, v & 2); break;
            case kGCfg: gcfg_ = v & 0xF; break;  // (bits 31:28, the channel version, read 0)
            case kIMask:
                imask_ = v;
                update_irq();
                break;
            default: break;  // (status: read-only)
        }
        return;
    }
    if (off < kErx) {
        switch (off - kEtx) {
            case kTxPend:
                if (v & 1) kick_tx();
                break;
            case kEtxCfg:
                etx_cfg_ = v & 0x7FF;
                kick_tx();
                break;
            case kEtxRing: etx_ring_ = v; break;
            case kEtxRSize: etx_rsize_ = v & 0xF; break;
            default: break;
        }
        return;
    }
    if (off < kBmac) {
        switch (off - kErx) {
            case kErxCfg: erx_cfg_ = v & 0x007F063F; break;
            case kErxRing: erx_ring_ = v; break;
            default: break;
        }
        return;
    }
    if (off < kMif) {
        const u32 b = off - kBmac;
        if (b == kTxReset) {  // any write: the TX MAC resets (and is done at once)
            tx_timer_.cancel();
            tx_busy_ = false;
            return;
        }
        if (b == kRxReset) return;
        if (b >= 0x360 || !bmac_mask(b)) return;
        bmac_[b / 4] = v & bmac_mask(b);
        if (b == kTxCfg) kick_tx();
        if (b == kRxCfg) poll();
        return;
    }
    switch (off - kMif) {
        case kBbClk: {
            const u32 old = bb_clk_;
            bb_clk_ = v & 1;
            if (!old && bb_clk_) bitbang_clock();  // the PHY samples and drives on the rising edge
            break;
        }
        case kBbData: bb_data_ = v & 1; break;
        case kBbOe: bb_oe_ = v & 1; break;
        case kFrame: {
            // An MII management frame: ST 01, OP (10 read, 01 write), PHY,
            // register, turnaround, data; done at once.
            const unsigned op = (v >> 28) & 3, phy = (v >> 23) & 31, reg = (v >> 18) & 31;
            if (op == 2) {
                mif_frame_ = (v & 0xFFFE0000) | kFrameDone | mii_read(phy, reg);
            } else {
                if (op == 1) mii_write(phy, reg, static_cast<u16>(v));
                mif_frame_ = v | kFrameDone;
            }
            break;
        }
        case kMifCfg: {
            const bool was_polling = mif_cfg_ & kMifPoll;
            mif_cfg_ = v & 0x7CFF & ~(kMifMdio0 | kMifMdio1);
            if (!was_polling && (mif_cfg_ & kMifPoll)) {  // the first poll latches the register as it is
                const unsigned phy = (mif_cfg_ >> 10) & 31, reg = (mif_cfg_ >> 3) & 31;
                mif_bsts_ = u32{mii_read(phy, reg)} << 16 | (mif_bsts_ & 0xFFFF);
            }
            break;
        }
        case kMifIMask:
            mif_imask_ = v & 0xFFFF;
            update_irq();
            break;
        default: break;
    }
}

void Hme::global_reset(bool tx, bool rx) {
    if (tx) {
        tx_timer_.cancel();
        tx_busy_ = false;
        etx_cfg_ = 0x3FE, etx_ring_ = 0, etx_rsize_ = 0;
    }
    if (rx) erx_cfg_ = 0, erx_ring_ = 0;
}

void Hme::raise(u32 bits) {
    stat_ |= bits;
    update_irq();
}

// The interrupt is a level: any status bit not masked (the MIF's by the
// MIF's own mask).
void Hme::update_irq() {
    const bool mif = (mif_bsts_ & 0xFFFF & ~mif_imask_) != 0;
    set_interrupt((stat_ & ~imask_ & ~kStatMif) != 0 || mif);
}

u64 Hme::wire_cycles(std::size_t bytes) const {
    // Preamble and start delimiter, the frame, its FCS and the gap after it.
    const double bits = 8.0 * static_cast<double>(8 + bytes + 4 + 12);
    const bool fast = (phy_[kPar] & 0x40) == 0;  // (SPEED_10 clear: 100 Mbit/s)
    return static_cast<u64>(bits / (fast ? 100e6 : 10e6) * cpu_hz_) + 1;
}

// ---- transmit ----

void Hme::kick_tx() {
    if (tx_busy_ || !(etx_cfg_ & 1) || !(bmac_[kTxCfg / 4] & kTxEnable)) return;
    const u32 base = etx_ring_ & ~0x7FFu, size = (etx_rsize_ + 1) * 16;
    const u32 first = ((etx_ring_ >> 3) & 0xFF) % size;
    // The frame: descriptors from SOP to EOP, all the device's.
    std::vector<u8> frame;
    u32 n = 0, sop_flags = 0;
    for (;; ++n) {
        if (n == size) {  // round the whole ring without an end of frame
            raise(kStatEopErr);
            return;
        }
        u8 d[8];
        const u32 at = base + ((first + n) % size) * 8;
        if (!master_read(at, d, 8)) {
            raise(kStatTxErrAck);
            return;
        }
        const u32 flags = le32(d), addr = le32(d + 4);
        if (!(flags & kOwn)) {
            if (n == 0) raise(kStatTxAll);  // nothing more to send
            return;                         // (else the rest of the frame is not handed over yet)
        }
        if (n == 0) sop_flags = flags;
        const std::size_t len = flags & 0x3FFF, at_byte = frame.size();
        frame.resize(at_byte + len);
        if (len && !master_read(addr, frame.data() + at_byte, len)) {
            raise(kStatTxErrAck);
            return;
        }
        if (flags & kTxEop) break;
    }
    // The checksum the driver asked for: the ones'-complement sum from the
    // start offset to the end, complemented, stored at the stuff offset.
    if (sop_flags & kTxCsum) {
        const std::size_t start = (sop_flags >> 14) & 0x3F, stuff = (sop_flags >> 20) & 0xFF;
        if (start < frame.size() && stuff + 2 <= frame.size()) {
            const u16 sum = static_cast<u16>(~ones_sum(frame.data() + start, frame.size() - start));
            frame[stuff] = static_cast<u8>(sum >> 8);
            frame[stuff + 1] = static_cast<u8>(sum);
        }
    }
    if (frame.size() < 60) frame.resize(60, 0);  // padded to the minimum
    tx_busy_ = true;
    tx_first_ = first;
    tx_count_ = n + 1;
    const u64 wire = wire_cycles(frame.size());
    if (frame.size() > (bmac_[kTxMax / 4] & 0x1FFF) && (bmac_[kTxMax / 4] & 0x1FFF)) {
        stat_ |= kStatMaxSize;  // too long: not sent
    } else {
        // Onto the wire through the output drivers; XIF loopback also hands
        // the frame to the receiver. (OBP 3.31 runs with XIF = OE | XIF
        // loopback: its loopback test takes its own frames back with RXCFG
        // MYOWN clear, and with MYOWN set the copy is rejected while the
        // frame still goes out, as a net boot needs.) MII loopback stays
        // inside the MAC.
        const u32 xif = bmac_[kXif / 4];
        if ((xif & kXifOe) && !(xif & kXifMiiLoopback) && backend_) backend_->send(frame.data(), frame.size()), ++frames_;
        if (xif & (kXifLoopback | kXifMiiLoopback)) receive(std::move(frame));
    }
    tx_timer_.schedule_in(wire);
}

void Hme::tx_done() {
    // The descriptors go back to the driver (OWN clear, the rest as they were).
    const u32 base = etx_ring_ & ~0x7FFu, size = (etx_rsize_ + 1) * 16;
    for (u32 i = 0; i < tx_count_; ++i) {
        u8 d[4];
        const u32 at = base + ((tx_first_ + i) % size) * 8;
        if (!master_read(at, d, 4)) {
            raise(kStatTxErrAck);
            break;
        }
        put_le32(d, le32(d) & ~kOwn);
        master_write(at, d, 4);
    }
    const u32 next = (tx_first_ + tx_count_) % size;
    etx_ring_ = (etx_ring_ & ~0x7F8u) | next << 3;
    tx_busy_ = false;
    raise(kStatTxToMac | kStatSent);
    kick_tx();  // the next frame, or TX_ALL
}

// ---- receive ----

void Hme::poll() {
    if (!backend_ || rx_timer_.active() || !backend_->pending()) return;
    std::vector<u8> frame;
    if (backend_->receive(frame)) ++frames_, receive(std::move(frame));
}

bool Hme::rx_accepts(const u8* f, std::size_t length) const {
    if (length < 12) return false;
    const u32 rxcfg = bmac_[kRxCfg / 4];
    const u8 me[6] = {static_cast<u8>(bmac_[kMadd0 / 4] >> 8), static_cast<u8>(bmac_[kMadd0 / 4]),
                      static_cast<u8>(bmac_[kMadd1 / 4] >> 8), static_cast<u8>(bmac_[kMadd1 / 4]),
                      static_cast<u8>(bmac_[kMadd2 / 4] >> 8), static_cast<u8>(bmac_[kMadd2 / 4])};
    if ((rxcfg & kRxMyOwn) && std::memcmp(f + 6, me, 6) == 0) return false;  // (one's own frames, looped back)
    if (rxcfg & kRxPromisc) return true;
    if (std::memcmp(f, me, 6) == 0) return true;
    static const u8 kBroadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    if (std::memcmp(f, kBroadcast, 6) == 0) return true;
    if (!(f[0] & 1)) return false;
    if (rxcfg & kRxAllMulti) return true;
    if (rxcfg & kRxHash) {  // the hash filter: the top six bits of the address's CRC
        const unsigned idx = crc32(f, 6) >> 26;
        return (bmac_[(kHash3 + 12 - 4 * (idx >> 4)) / 4] >> (idx & 15)) & 1;
    }
    return false;
}

void Hme::receive(std::vector<u8> frame) {
    // The wire is busy for this frame, taken or not: the next waits for it.
    rx_timer_.schedule_in(wire_cycles(std::max<std::size_t>(frame.size(), 60)));
    if (!(erx_cfg_ & 1) || !(bmac_[kRxCfg / 4] & kRxEnable)) return;  // not receiving: lost
    if (!rx_accepts(frame.data(), frame.size())) return;
    if (frame.size() < 60) frame.resize(60, 0);
    const u32 rxmax = bmac_[kRxMax / 4] & 0x1FFF;
    if (rxmax && frame.size() > rxmax) {  // too long: counted, not received
        bmac_[kLeCnt / 4] = (bmac_[kLeCnt / 4] + 1) & 0xFF;
        return;
    }
    const std::size_t length = frame.size();  // (without the FCS)
    if (bmac_[kRxCfg / 4] & kRxKeepFcs) {  // the FCS kept: appended, as it came off the wire
        const u32 fcs = ~crc32(frame.data(), frame.size());
        for (int i = 0; i < 4; ++i) frame.push_back(static_cast<u8>(fcs >> (8 * i)));
    }
    const u32 base = erx_ring_ & ~0x7FFu, size = 32u << ((erx_cfg_ >> 9) & 3);
    const u32 idx = ((erx_ring_ >> 3) & 0xFF) % size, at = base + idx * 8;
    u8 d[8];
    if (!master_read(at, d, 8)) {
        raise(kStatRxErrAck);
        return;
    }
    const u32 flags = le32(d), addr = le32(d + 4);
    if (!(flags & kOwn)) {  // no buffer: dropped
        raise(kStatNoRxd);
        return;
    }
    const std::size_t room = (flags >> 16) & 0x3FFF, offset = (erx_cfg_ >> 3) & 7;
    const std::size_t count = std::min(frame.size(), room);
    if (!master_write(addr + static_cast<u32>(offset), frame.data(), count)) {
        raise(kStatRxErrAck);
        return;
    }
    // The checksum: complemented ones'-complement sum from the start
    // offset (in halfwords) to the end of the frame, FCS excluded.
    const std::size_t start = 2 * ((erx_cfg_ >> 16) & 0x7F);
    const u32 sum = start < length ? ones_sum(frame.data() + start, length - start) : 0;
    put_le32(d, static_cast<u32>(length & 0x3FFF) << 16 | (frame.size() > room ? kRxOverflow : 0) | (~sum & 0xFFFF));
    master_write(at, d, 4);
    erx_ring_ = (erx_ring_ & ~0x7F8u) | ((idx + 1) % size) << 3;
    raise(kStatRxToHost | kStatRcvd);
}

// ---- MIF and the PHY ----

u32 Hme::mif_cfg_read() const {
    u32 v = mif_cfg_;
    if (mif_cfg_ & kMifBitBang) {
        // The lines as they are: the internal PHY's when it drives MDIO0,
        // pulled up otherwise; MDIO1 low, no external transceiver on it
        // (as in frame mode: drivers take MDIO1 high for one).
        v |= bb_out_ < 0 || bb_out_ ? kMifMdio0 : 0;
    } else {
        v |= kMifMdio0;  // an internal transceiver, no external one
    }
    return v;
}

u16 Hme::mii_read(unsigned phy, unsigned reg) {
    // Only the internal PHY, on MDIO0 (the PHY select clear), at its address.
    if ((mif_cfg_ & kMifPs) || phy != kPhyAddress) return 0xFFFF;
    return phy_[reg & 31];
}

void Hme::mii_write(unsigned phy, unsigned reg, u16 value) {
    if ((mif_cfg_ & kMifPs) || phy != kPhyAddress) return;
    reg &= 31;
    switch (reg) {
        case kBmcr:
            if (value & kBmcrReset) {
                phy_reset();
                break;
            }
            phy_[kBmcr] = value & ~kBmcrRestart;  // (a restart completes at once)
            phy_autoneg();
            break;
        case kBmsr: case kId1: case kId2: case kAnlpar: case kAner: break;  // read-only
        case kAnar: phy_[kAnar] = (value & 0x3FE0) | 0x0001; break;
        default: phy_[reg] = value; break;
    }
    mif_poll();
}

void Hme::phy_reset() {
    phy_ = {};
    phy_[kBmcr] = kBmcr100 | kBmcrAn;
    phy_[kId1] = 0x2000;  // National Semiconductor DP83840
    phy_[kId2] = 0x5C01;
    phy_[kAnar] = 0x01E1;
    phy_[kPar] = kPhyAddress;
    phy_autoneg();
}

// The link with what is at the other end: with a cable, a partner that can
// do everything; autonegotiation (or the forced setting) resolves the speed
// and duplex at once.
void Hme::phy_autoneg() {
    bool fast = true, full = true;
    if (!link_up()) {
        phy_[kBmsr] = kBmsrBase;
        phy_[kAnlpar] = 0, phy_[kAner] = 0;
    } else if (phy_[kBmcr] & kBmcrAn) {
        phy_[kAnlpar] = 0x41E1;
        phy_[kAner] = 0x0001;
        phy_[kBmsr] = kBmsrBase | kBmsrAnDone | kBmsrLink;
        const u16 common = phy_[kAnar] & phy_[kAnlpar];
        fast = common & 0x0180;
        full = (common & 0x0100) || (!fast && (common & 0x0040));
    } else {
        phy_[kAnlpar] = 0, phy_[kAner] = 0;
        phy_[kBmsr] = kBmsrBase | kBmsrLink;
        fast = phy_[kBmcr] & kBmcr100;
        full = phy_[kBmcr] & kBmcrFull;
    }
    phy_[kPar] = static_cast<u16>((phy_[kPar] & ~0xE0) | (fast ? 0 : 0x40) | (full ? 0x80 : 0) | (link_up() ? 0 : 0x20));
    mif_poll();
}

// MIF polling: the polled register's changes, latched as status bits.
void Hme::mif_poll() {
    if (!(mif_cfg_ & kMifPoll) || (mif_cfg_ & kMifBitBang)) return;
    const unsigned phy = (mif_cfg_ >> 10) & 31, reg = (mif_cfg_ >> 3) & 31;
    const u16 now = mii_read(phy, reg), before = static_cast<u16>(mif_bsts_ >> 16);
    mif_bsts_ = u32{now} << 16 | (mif_bsts_ & 0xFFFF) | static_cast<u16>(now ^ before);
    update_irq();
}

// A rising edge of the bit-banged MDC: the PHY samples MDIO while the MIF
// drives it, and shifts out a read's turnaround and data.
void Hme::bitbang_clock() {
    if (bb_out_left_) {  // a read: the next bit out
        --bb_out_left_;
        bb_out_ = static_cast<int>((bb_shift_ >> bb_out_left_) & 1);
        if (!bb_out_left_) bb_shift_ = 0;
        return;
    }
    bb_out_ = -1;
    if (!bb_oe_) return;
    // Preamble (ones), then ST 01, OP, PHY, register; a write goes on with
    // the turnaround and 16 data bits.
    const unsigned bit = bb_data_ & 1;
    if (bb_count_ == 0) {
        if (bit) {
            bb_in_ = 0;  // still preamble
            return;
        }
        bb_count_ = 1;  // the start's 0
        bb_in_ = 0;
        return;
    }
    bb_in_ = bb_in_ << 1 | bit;
    ++bb_count_;
    if (bb_count_ == 14) {  // 0, then 1 OP OP PHY*5 REG*5
        const unsigned st = (bb_in_ >> 12) & 1, op = (bb_in_ >> 10) & 3, phy = (bb_in_ >> 5) & 31, reg = bb_in_ & 31;
        if (!st) {
            bb_count_ = 0;
        } else if (op == 2) {  // a read: Z, then 0 and the data, one bit a clock
            bb_shift_ = mii_read(phy, reg);
            bb_out_left_ = 17;
            bb_count_ = 0;
        }
    } else if (bb_count_ == 14 + 2 + 16) {  // a write's turnaround and data
        const unsigned op = (bb_in_ >> 28) & 3, phy = (bb_in_ >> 23) & 31, reg = (bb_in_ >> 18) & 31;
        if (op == 1) mii_write(phy, reg, static_cast<u16>(bb_in_));
        bb_count_ = 0;
    }
}

}  // namespace uv
