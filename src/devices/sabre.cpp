// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
#include "devices/sabre.h"

#include <algorithm>

#include "core/endian.h"
#include "core/log.h"

namespace uv {

namespace {

// Register offsets
constexpr u32 kUpaPortId = 0x0000;
constexpr u32 kIommuCtl = 0x0200, kIommuTsb = 0x0208, kIommuFlush = 0x0210;
constexpr u32 kPciImap = 0x0C00, kObioImap = 0x1000, kPciIclr = 0x1400, kObioIclr = 0x1800, kRetryTimer = 0x1A00;
constexpr u32 kTimer = 0x1C00, kWriteSync = 0x1C20;
constexpr u32 kPciCtl = 0x2000, kPciAfsr = 0x2010, kPciAfar = 0x2018, kPciDiag = 0x2020, kPciTasr = 0x2028;
constexpr u32 kGfxImapAlias = 0x6000, kUpaImapAlias = 0x8000;
constexpr u32 kResetCtl = 0xF020;

// UPA port ID (read-only), as a real Ultra 10 reads it.
constexpr u64 kUpaPortIdValue = 0x8E10002000000000ull;

constexpr u64 kImapValid = 0x80000000ull, kImapTid = 0x7C000000ull;
constexpr unsigned kStateIdle = 0, kStatePending = 3;

// Reset control
constexpr u64 kRstPor = 1ull << 31, kRstSoftPor = 1ull << 30, kRstSoftXir = 1ull << 29;

// PIO error status (PCI AFSR)
constexpr u64 kAfsrPma = 1ull << 63;

// Timer limit fields
constexpr u64 kLimitIntEn = 1ull << 31, kLimitToZero = 1ull << 30, kLimitMask = 0x1FFFFFFF;
constexpr unsigned kTimerIno[2] = {0x2C, 0x2D};

u64 swap_size(u64 v, unsigned size) {
    switch (size) {
        case 2: return byteswap(static_cast<u16>(v));
        case 4: return byteswap(static_cast<u32>(v));
        case 8: return byteswap(v);
        default: return v;
    }
}

}  // namespace

// One of the module's address windows on the processor's bus.
class Sabre::Window final : public Device {
public:
    using Read = bool (Sabre::*)(u64, unsigned, u64&);
    using Write = bool (Sabre::*)(u64, unsigned, u64);
    Window(Sabre& s, const char* n, Read r, Write w) : s_(s), name_(n), r_(r), w_(w) {}
    const char* name() const override { return name_; }
    bool read(u64 offset, unsigned size, u64& value) override { return (s_.*r_)(offset, size, value); }
    bool write(u64 offset, unsigned size, u64 value) override { return (s_.*w_)(offset, size, value); }

private:
    Sabre& s_;
    const char* name_;
    Read r_;
    Write w_;
};

// The module's own configuration header: bus 0, device 0.
class Sabre::HostFunction final : public pci::Function {
public:
    HostFunction() : pci::Function(0x108E, 0xA000, 0x00, 0x060000) {
        set_cfg16(pci::kCommand, pci::kCmdMemory | pci::kCmdMaster);
        set_cfg16(pci::kStatus, 0x0280);  // fast back-to-back, medium DEVSEL
        set_wmask(0x40, 2, 0xFFFF);       // bus number and subordinate bus number
        save_reset_state();
    }
    const char* name() const override { return "sabre"; }
};

Sabre::Sabre(Bus& bus, Scheduler& scheduler, double cpu_hz, std::function<bool(u64)> send_vector)
    : bus_(bus), sched_(scheduler), cpu_hz_(cpu_hz), send_vector_(std::move(send_vector)),
      host_(std::make_unique<HostFunction>()),
      regs_win_(std::make_unique<Window>(*this, "sabre-regs", &Sabre::regs_read, &Sabre::regs_write)),
      config_win_(std::make_unique<Window>(*this, "pci-config", &Sabre::config_read, &Sabre::config_write)),
      io_win_(std::make_unique<Window>(*this, "pci-io", &Sabre::io_read, &Sabre::io_write)),
      mem_win_(std::make_unique<Window>(*this, "pci-mem", &Sabre::mem_read, &Sabre::mem_write)),
      retry_timer_(scheduler, [this] { dispatch_all(); }) {
    root_.attach(0, 0, host_.get());
    for (unsigned n = 0; n < 2; ++n)
        tc_timer_[n] = std::make_unique<Timer>(scheduler, [this, n] {
            TimerCounter& t = tc_[n];
            if (t.limit & kLimitToZero) t.base_cycle = sched_.now();
            if (t.limit & kLimitIntEn) {
                set_interrupt(kTimerIno[n], true);
                set_interrupt(kTimerIno[n], false);  // edge: latched by the clear state
            }
            timer_update(n);
        });
    reset(true);
}

Sabre::~Sabre() = default;

void Sabre::map() {
    bus_.map_device(kRegsBase, 0x10000, regs_win_.get());
    bus_.map_device(kConfigBase, 0x1000000, config_win_.get());
    bus_.map_device(kIoBase, 0x1000000, io_win_.get());
    bus_.map_device(kMemBase, 0x100000000ull, mem_win_.get());
}

void Sabre::map_mem_window(u64 pa, u64 size) { bus_.map_device(pa, size, mem_win_.get()); }

void Sabre::reset(bool power_on) {
    regs_.clear();
    for (unsigned i = 0; i < 8; ++i) pci_imap_[i] = (u64{kIgn} << 6) | (i << 2);
    for (unsigned i = 0; i < 32; ++i) obio_imap_[i] = (u64{kIgn} << 6) | (0x20 + i);
    // On a real Ultra 10 the maps past the on-board devices' (0x1088-0x10C8)
    // read with no interrupt number, and the graphics and UPA expansion maps
    // (0x1098, 0x10A0) read 0.
    for (unsigned i = 0x11; i <= 0x19; ++i) obio_imap_[i] = u64{kIgn} << 6;
    obio_imap_[(0x1098 - kObioImap) >> 3] = obio_imap_[(0x10A0 - kObioImap) >> 3] = 0;
    state_.fill(kStateIdle);
    iommu_ctl_ = iommu_tsb_ = 0;
    pci_ctl_ = pci_afsr_ = pci_afar_ = pci_diag_ = pci_tasr_ = 0;
    reset_ctl_ = power_on ? kRstPor : (reset_ctl_ & ~kRstPor);
    tc_ = {};
    for (auto& t : tc_timer_) t->cancel();
    retry_timer_.cancel();
    root_.reset();
}

u64& Sabre::imap(unsigned ino) { return ino < 0x20 ? pci_imap_[ino >> 2] : obio_imap_[(ino - 0x20) & 31]; }

void Sabre::set_interrupt(unsigned ino, bool asserted) {
    if (ino >= kNumIno) return;
    const bool rising = asserted && !level_[ino];
    level_[ino] = asserted;
    // Timers raise a pulse; latch it for dispatch.
    if (rising && state_[ino] == kStateIdle) dispatch(ino);
}

void Sabre::dispatch(unsigned ino) {
    if (state_[ino] != kStateIdle) return;
    if (!(imap(ino) & kImapValid)) return;
    const u64 inr = (u64{kIgn} << 6) | ino;
    if (send_vector_ && send_vector_(inr)) {
        state_[ino] = kStatePending;
    } else if (!retry_timer_.active()) {
        // The processor is busy with another vector: try again after the
        // interrupt retry timer.
        const u64 retry = regs_.count(kRetryTimer) ? (regs_[kRetryTimer] & 0xFFFFF) : 0;
        retry_timer_.schedule_in(retry ? retry : 1000);
    }
}

void Sabre::dispatch_all() {
    for (unsigned ino = 0; ino < kNumIno; ++ino)
        if (level_[ino]) dispatch(ino);
}

u64 Sabre::tc_count(unsigned n) const {
    const double us = static_cast<double>(sched_.now() - tc_[n].base_cycle) * 1e6 / cpu_hz_;
    return static_cast<u64>(us) & kLimitMask;
}

void Sabre::timer_update(unsigned n) {
    TimerCounter& t = tc_[n];
    tc_timer_[n]->cancel();
    const u64 limit = t.limit & kLimitMask;
    if (!(t.limit & (kLimitIntEn | kLimitToZero)) || limit == 0) return;
    const u64 count = tc_count(n);
    const u64 ticks = limit > count ? limit - count : (kLimitMask + 1 - count + limit);
    tc_timer_[n]->schedule_in(static_cast<u64>(static_cast<double>(ticks) * cpu_hz_ / 1e6) + 1);
}

u64 Sabre::reg(u32 o) const {
    switch (o) {
        case kUpaPortId: return kUpaPortIdValue;
        case kIommuCtl: return iommu_ctl_;
        case kIommuTsb: return iommu_tsb_;
        case kPciCtl: return pci_ctl_;
        case kPciAfsr: return pci_afsr_;
        case kPciAfar: return pci_afar_;
        case kPciDiag: return pci_diag_;
        case kPciTasr: return pci_tasr_;
        case kResetCtl: return reset_ctl_;
        default: break;
    }
    if (o >= kPciImap && o < kPciImap + 0x40) return pci_imap_[(o - kPciImap) >> 3];
    if (o >= kObioImap && o < kObioImap + 0x100) return obio_imap_[((o - kObioImap) >> 3) & 31];
    if (o == kGfxImapAlias) return obio_imap_[(0x1098 - kObioImap) >> 3];
    if (o == kUpaImapAlias) return obio_imap_[(0x10A0 - kObioImap) >> 3];
    if (o >= kPciIclr && o < kPciIclr + 0x100) return state_[((o - kPciIclr) >> 3) & 0x1F];
    if (o >= kObioIclr && o < kObioIclr + 0x100) return state_[0x20 + (((o - kObioIclr) >> 3) & 0x1F)];
    if (o >= kTimer && o < kTimer + 0x20) {
        const unsigned n = (o - kTimer) >> 4;
        return (o & 8) ? tc_[n].limit : tc_count(n);
    }
    const auto it = regs_.find(o);
    return it == regs_.end() ? 0 : it->second;
}

bool Sabre::regs_read(u64 offset, unsigned size, u64& value) {
    const u32 o = static_cast<u32>(offset & ~u64{7});
    const u64 v = reg(o);
    if (size == 8) value = v;
    else value = (v >> (8 * (8 - size - (offset & 7)))) & ((u64{1} << (8 * size)) - 1);
    return true;
}

bool Sabre::regs_write(u64 offset, unsigned size, u64 value) {
    const u32 o = static_cast<u32>(offset & ~u64{7});
    u64 v = value;
    if (size != 8) {
        const unsigned shift = 8 * (8 - size - static_cast<unsigned>(offset & 7));
        const u64 mask = ((u64{1} << (8 * size)) - 1) << shift;
        v = (reg(o) & ~mask) | ((value << shift) & mask);
    }
    switch (o) {
        case kUpaPortId: return true;
        case kIommuCtl: iommu_ctl_ = v & 0x07F70007ull; return true;
        case kIommuTsb: iommu_tsb_ = v & 0x1FFFFFFE000ull; return true;
        case kIommuFlush: return true;  // translations are not cached
        case kPciCtl: pci_ctl_ = (v & ~(1ull << 34)) | (pci_ctl_ & (1ull << 34) & ~v); return true;  // SERR is W1C
        case kPciAfsr: pci_afsr_ &= ~v; return true;
        case kPciAfar: return true;
        case kPciDiag: pci_diag_ = v; return true;
        case kPciTasr: pci_tasr_ = v & 0xFF; return true;
        case kWriteSync: return true;
        case kResetCtl:
            reset_ctl_ = (reset_ctl_ & ~(v & 0x98000000ull)) | (v & 0x60000000ull);
            if ((v & kRstSoftPor) && on_soft_reset) on_soft_reset(true);
            else if ((v & kRstSoftXir) && on_soft_reset) on_soft_reset(false);
            return true;
        default: break;
    }
    if (o >= kPciImap && o < kPciImap + 0x40) {
        u64& m = pci_imap_[(o - kPciImap) >> 3];
        m = (m & ~(kImapValid | kImapTid)) | (v & (kImapValid | kImapTid));
        dispatch_all();
        return true;
    }
    if ((o >= kObioImap && o < kObioImap + 0x100) || o == kGfxImapAlias || o == kUpaImapAlias) {
        const unsigned i = o == kGfxImapAlias ? (0x1098 - kObioImap) >> 3
                         : o == kUpaImapAlias ? (0x10A0 - kObioImap) >> 3
                                              : ((o - kObioImap) >> 3) & 31;
        u64& m = obio_imap_[i];
        m = (m & ~(kImapValid | kImapTid)) | (v & (kImapValid | kImapTid));
        dispatch_all();
        return true;
    }
    if ((o >= kPciIclr && o < kPciIclr + 0x100) || (o >= kObioIclr && o < kObioIclr + 0x100)) {
        const unsigned ino = o >= kObioIclr ? 0x20 + (((o - kObioIclr) >> 3) & 0x1F) : ((o - kPciIclr) >> 3) & 0x1F;
        state_[ino] = static_cast<u8>(v & 3);
        if (state_[ino] == kStateIdle && level_[ino]) dispatch(ino);  // a level still held is sent again
        return true;
    }
    if (o >= kTimer && o < kTimer + 0x20) {
        const unsigned n = (o - kTimer) >> 4;
        if (o & 8) {
            tc_[n].limit = v & (0xE0000000ull | kLimitMask);
        } else {
            const double cycles = static_cast<double>(v & kLimitMask) * cpu_hz_ / 1e6;
            tc_[n].base_cycle = sched_.now() - static_cast<u64>(cycles);
        }
        timer_update(n);
        return true;
    }
    regs_[o] = v;
    return true;
}

bool Sabre::config_read(u64 offset, unsigned size, u64& value) {
    const unsigned busn = static_cast<unsigned>(offset >> 16) & 0xFF, dev = static_cast<unsigned>(offset >> 11) & 31,
                   fn = static_cast<unsigned>(offset >> 8) & 7;
    const u8 reg = static_cast<u8>(offset);
    u64 v;
    if (size == 8) {
        // A doubleword is two configuration cycles, the lower address first.
        v = root_.config_read(busn, dev, fn, reg, 4) | (u64{root_.config_read(busn, dev, fn, static_cast<u8>(reg + 4), 4)} << 32);
    } else {
        v = root_.config_read(busn, dev, fn, reg, size);
    }
    value = swap_size(v, size);
    return true;
}

bool Sabre::config_write(u64 offset, unsigned size, u64 value) {
    const unsigned busn = static_cast<unsigned>(offset >> 16) & 0xFF, dev = static_cast<unsigned>(offset >> 11) & 31,
                   fn = static_cast<unsigned>(offset >> 8) & 7;
    const u8 reg = static_cast<u8>(offset);
    const u64 v = swap_size(value, size);
    if (size == 8) {
        root_.config_write(busn, dev, fn, reg, 4, static_cast<u32>(v));
        root_.config_write(busn, dev, fn, static_cast<u8>(reg + 4), 4, static_cast<u32>(v >> 32));
    } else {
        root_.config_write(busn, dev, fn, reg, size, static_cast<u32>(v));
    }
    return true;
}

void Sabre::pio_error(u64 bit) {
    if (!(pci_afsr_ & (kAfsrPma | (1ull << 62) | (1ull << 61) | (1ull << 60)))) pci_afsr_ |= bit;
    else pci_afsr_ |= bit >> 4;  // a second error sets the secondary bit
}

bool Sabre::io_read(u64 offset, unsigned size, u64& value) {
    u32 v;
    if (size > 4 || !root_.io_read(static_cast<u32>(offset), size, v)) {
        pio_error(kAfsrPma);
        pci_afar_ = kIoBase + offset;
        return false;
    }
    value = swap_size(v, size);
    return true;
}

bool Sabre::io_write(u64 offset, unsigned size, u64 value) {
    if (size > 4 || !root_.io_write(static_cast<u32>(offset), size, static_cast<u32>(swap_size(value, size)))) {
        pio_error(kAfsrPma);
        pci_afar_ = kIoBase + offset;
    }
    return true;
}

bool Sabre::mem_read(u64 offset, unsigned size, u64& value) {
    u64 v;
    if (!root_.mem_read(static_cast<u32>(offset), size, v)) {
        pio_error(kAfsrPma);
        pci_afar_ = kMemBase + offset;
        UV_LOG(Debug, "sabre", "PCI master abort: read%u @ %08llx", size * 8, static_cast<unsigned long long>(offset));
        return false;
    }
    value = swap_size(v, size);
    return true;
}

bool Sabre::mem_write(u64 offset, unsigned size, u64 value) {
    if (!root_.mem_write(static_cast<u32>(offset), size, swap_size(value, size))) {
        pio_error(kAfsrPma);
        pci_afar_ = kMemBase + offset;
        UV_LOG(Debug, "sabre", "PCI master abort: write%u @ %08llx", size * 8, static_cast<unsigned long long>(offset));
    }
    return true;
}

// IOMMU: with translation enabled, a DVMA address indexes the TSB (8-byte
// entries, one per 8 KiB or 64 KiB page as the control register says) in
// memory; each entry holds valid (bit 63), page size (61), writable (1)
// and the physical page. With it disabled, DVMA addresses are physical.
bool Sabre::translate(u32 dvma, bool write, u64& pa) const {
    if (!(iommu_ctl_ & 1)) {
        pa = dvma;
        return true;
    }
    const unsigned tsb_size = static_cast<unsigned>(iommu_ctl_ >> 16) & 7;
    const bool big = iommu_ctl_ & 4;
    const unsigned page_shift = big ? 16 : 13;
    const u64 entries = u64{1024} << tsb_size;
    const u64 index = (dvma >> page_shift) & (entries - 1);
    u64 tte;
    u8 buf[8];
    if (!bus_.read_block(iommu_tsb_ + index * 8, buf, 8)) return false;
    tte = load_be<u64>(buf);
    if (!(tte >> 63)) return false;
    if (write && !(tte & 2)) return false;
    const bool tte64k = (tte >> 61) & 1;
    const u64 mask = tte64k ? 0xFFFF : 0x1FFF;
    pa = ((tte & 0x1FFFFFFE000ull) & ~mask) | (dvma & mask);
    return true;
}

bool Sabre::dma_read(u32 dvma, u8* data, std::size_t length) {
    while (length) {
        u64 pa;
        if (!translate(dvma, false, pa)) return false;
        const std::size_t chunk = std::min<std::size_t>(length, 0x2000 - (dvma & 0x1FFF));
        if (!bus_.read_block(pa, data, chunk)) return false;
        dvma += static_cast<u32>(chunk);
        data += chunk;
        length -= chunk;
    }
    return true;
}

bool Sabre::dma_write(u32 dvma, const u8* data, std::size_t length) {
    while (length) {
        u64 pa;
        if (!translate(dvma, true, pa)) return false;
        const std::size_t chunk = std::min<std::size_t>(length, 0x2000 - (dvma & 0x1FFF));
        if (!bus_.write_block(pa, data, chunk)) return false;
        dvma += static_cast<u32>(chunk);
        data += chunk;
        length -= chunk;
    }
    return true;
}

}  // namespace uv
