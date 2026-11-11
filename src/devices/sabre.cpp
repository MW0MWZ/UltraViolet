// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
#include "devices/sabre.h"

#include <algorithm>

#include "core/endian.h"
#include "core/log.h"
#include "core/text.h"

namespace uv {

namespace {

// Register offsets
constexpr u32 kUpaPortId = 0x0000;
constexpr u32 kUeAfsr = 0x0030, kUeceAfar = 0x0038, kCeAfsr = 0x0040, kUeceAfarAlias = 0x0048;
constexpr u32 kIommuCtl = 0x0200, kIommuTsb = 0x0208, kIommuFlush = 0x0210;
constexpr u32 kPciImap = 0x0C00, kObioImap = 0x1000, kPciIclr = 0x1400, kObioIclr = 0x1800, kIntStateDiag = 0xA800;
constexpr u32 kIommuVaDiag = 0xA400, kIommuTagCompare = 0xA408, kIommuTagDiag = 0xA580, kIommuDataDiag = 0xA600;
constexpr u32 kWriteSync = 0x1C20;
// Reserved in the IIi manual (where the Psycho has its interrupt retry timer
// and timer/counters): a real Ultra 10 reads 0x7C0 at each, and takes writes
// as nothing.
constexpr u32 kReservedAt1A00 = 0x1A00, kReservedTimers = 0x1C00;
constexpr u64 kReservedValue = 0x7C0;
// The delay before a vector the processor refused goes again (not a
// register on the IIi's bus module)
constexpr u64 kRetryCycles = 1000;
constexpr u32 kPciCtl = 0x2000, kPciAfsr = 0x2010, kPciAfar = 0x2018, kPciDiag = 0x2020, kPciTasr = 0x2028;
constexpr u32 kGfxImapAlias = 0x6000, kUpaImapAlias = 0x8000;
constexpr u32 kFfbConfig = 0xF000, kMcuF008 = 0xF008, kMemControl0 = 0xF010, kMemControl1 = 0xF018, kResetCtl = 0xF020;
// FFB_Config: the slave request queue size (27:24, 1 at reset; OpenBoot
// sets 7 for a Creator3D) and Oneread (14), always 1.
constexpr u64 kFfbSprqs = 0xFull << 24, kFfbOneread = 1ull << 14, kFfbConfigReset = 1ull << 24;
// Mem_Control0 and 1 at power-on (IIi manual tables 18-3 and 18-7: refresh
// every 0x30 x 32 clocks for all four DIMM pairs, off; the SME1430's
// default timings), and their writable bits.
constexpr u64 kMc0Reset = 0x00000F30, kMc0Writable = 0x9E001FFF, kMc1Reset = 0x004A2950, kMc1Writable = 0xFFFFFFFF;
// 0xF008, not in the IIi manual's list of MCU registers, reads this on a
// real Ultra 10; taken as read-only.
constexpr u64 kMcuF008Value = 0x4242424200001232ull;

// UPA port ID (read-only), as a real Ultra 10 reads it.
constexpr u64 kUpaPortIdValue = 0x8E10002000000000ull;

constexpr u64 kImapValid = 0x80000000ull, kImapTid = 0x7C000000ull;
// The graphics and UPA64S expansion maps (full maps): the whole interrupt
// number is software's.
constexpr u32 kGfxImap = 0x1098, kUpaImap = 0x10A0;
constexpr u64 kImapInr = 0x7FFull;
constexpr unsigned kStateIdle = 0, kStatePending = 3;

// Reset control
constexpr u64 kRstPor = 1ull << 31, kRstSoftPor = 1ull << 30, kRstSoftXir = 1ull << 29;

// PIO write error status (PCI AFSR): primary master abort, target abort,
// excessive retries, parity error; the secondary bits four below; the
// byte mask of the logged write (bit 32 byte 0).
constexpr u64 kAfsrPma = 1ull << 63, kAfsrPta = 1ull << 62, kAfsrPrimary = 0xFull << 60, kAfsrErrors = 0xFFull << 56,
              kAfsrByteMask = 0xFFull << 32;
// The module's PCI status: signalled target abort, received master abort.
constexpr u16 kStatusSta = 0x0800, kStatusRta = 0x1000, kStatusRma = 0x2000;
constexpr u64 kPciCtlErrEn = 1ull << 8;  // the PCI error interrupt enable

// DMA error status (UE AFSR): primary DMA read and write, secondary read
// and write, secondary and primary translation errors (bit layout as the
// Linux sabre driver has it); CE AFSR the same without translation errors.
constexpr u64 kUePdrd = 1ull << 62, kUePdwr = 1ull << 61, kUeSdrd = 1ull << 59, kUeSdwr = 1ull << 58,
              kUeSdte = 1ull << 57, kUePdte = 1ull << 56;
constexpr u64 kUeErrors = kUePdrd | kUePdwr | kUeSdrd | kUeSdwr | kUeSdte | kUePdte;
constexpr u64 kCeErrors = kUePdrd | kUePdwr | kUeSdrd | kUeSdwr;
constexpr u64 kUeLogged = (0xFFFFull << 32) | (7ull << 29) | (1ull << 23);  // byte mask, doubleword, block
// IOMMU control: a translation error (bit 24) and its kind (26:25:
// protection 0, invalid entry 1, time-out 2, ECC 3)
constexpr u64 kIommuErr = 1ull << 24, kIommuErrStatus = 3ull << 25;
constexpr unsigned kIommuProtection = 0, kIommuInvalid = 1;
// The bus module's own interrupts: PCI error, uncorrectable and correctable
// DMA errors (as a real Ultra 10's sabre node lists them)
constexpr unsigned kInoPciErr = 0x30, kInoUe = 0x2E, kInoCe = 0x2F;

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
    using Stable = u64 (Sabre::*)(u64, unsigned);
    Window(Sabre& s, const char* n, Read r, Write w, Stable st = nullptr) : s_(s), name_(n), r_(r), w_(w), st_(st) {}
    const char* name() const override { return name_; }
    bool read(u64 offset, unsigned size, u64& value) override { return (s_.*r_)(offset, size, value); }
    bool write(u64 offset, unsigned size, u64 value) override { return (s_.*w_)(offset, size, value); }
    u64 stable_until(u64 offset, unsigned size) const override { return st_ ? (s_.*st_)(offset, size) : 0; }

private:
    Sabre& s_;
    const char* name_;
    Read r_;
    Write w_;
    Stable st_;
};

// The module's own configuration header: bus 0, device 0.
class Sabre::HostFunction final : public pci::Function {
public:
    HostFunction() : pci::Function(0x108E, 0xA000, 0x00, 0x060000) {
        set_cfg16(pci::kCommand, pci::kCmdMemory | pci::kCmdMaster);  // (hardwired on)
        set_wmask(pci::kCommand, 2, 0x0140);                           // SERR_EN and PER only
        set_cfg16(pci::kStatus, 0x02A0);  // medium DEVSEL, fast back-to-back, 66 MHz capable
        set_wmask(0x40, 2, 0xFFFF);       // bus number and subordinate bus number
        save_reset_state();
    }
    const char* name() const override { return "sabre"; }
    // A master abort or target abort the module received as a PCI master.
    void received(u16 bit) { set_cfg16(pci::kStatus, cfg16(pci::kStatus) | bit); }
};

Sabre::Sabre(Bus& bus, Scheduler& scheduler, std::function<bool(u64)> send_vector)
    : bus_(bus), send_vector_(std::move(send_vector)),
      host_(std::make_unique<HostFunction>()),
      regs_win_(std::make_unique<Window>(*this, "sabre-regs", &Sabre::regs_read, &Sabre::regs_write)),
      config_win_(std::make_unique<Window>(*this, "pci-config", &Sabre::config_read, &Sabre::config_write,
                                           &Sabre::config_stable)),
      io_win_(std::make_unique<Window>(*this, "pci-io", &Sabre::io_read, &Sabre::io_write)),
      mem_win_(std::make_unique<Window>(*this, "pci-mem", &Sabre::mem_read, &Sabre::mem_write, &Sabre::mem_stable)),
      retry_timer_(scheduler, [this] { dispatch_all(); }) {
    root_.attach(0, 0, host_.get());
    reset(true);
}

Sabre::~Sabre() = default;

void Sabre::map() {
    bus_.map_device(kRegsBase, 0x10000, regs_win_.get());
    bus_.map_device(kConfigBase, 0x1000000, config_win_.get());
    bus_.map_device(kIoBase, 0x1000000, io_win_.get());
    bus_.map_device(kMemBase, 0x100000000ull, mem_win_.get());
}

void Sabre::map_mem_window(u64 pa, u64 size) { bus_.map_device(pa, size, mem_win_.get(), kMemBase); }

void Sabre::reset(bool power_on) {
    regs_.clear();
    for (unsigned i = 0; i < 16; ++i) tlb_lru_[i] = static_cast<u8>(i);
    // Only a power-on reset initialises Mem_Control0/1 (IIi manual 18.2:
    // SOFT_POR, B_POR and the XIRs leave refresh running as it was).
    if (power_on) {
        mem_control0_ = kMc0Reset, mem_control1_ = kMc1Reset;
        if (on_mem_control0) on_mem_control0(mem_control0_);
    }
    ffb_config_ = kFfbConfigReset;
    for (unsigned i = 0; i < 8; ++i) pci_imap_[i] = (u64{kIgn} << 6) | (i << 2);
    for (unsigned i = 0; i < 32; ++i) obio_imap_[i] = (u64{kIgn} << 6) | (0x20 + i);
    // On a real Ultra 10 the maps past the on-board devices' (0x1088-0x10C8)
    // read with no interrupt number, and the graphics and UPA expansion maps
    // (0x1098, 0x10A0) read 0.
    for (unsigned i = 0x11; i <= 0x19; ++i) obio_imap_[i] = u64{kIgn} << 6;
    obio_imap_[(kGfxImap - kObioImap) >> 3] = obio_imap_[(kUpaImap - kObioImap) >> 3] = 0;
    pulse_.fill(false);
    state_.fill(kStateIdle);
    iommu_ctl_ = iommu_tsb_ = 0;
    pci_ctl_ = pci_afsr_ = pci_afar_ = pci_diag_ = pci_tasr_ = 0;
    ue_afsr_ = ce_afsr_ = uece_afar_ = 0;
    update_error_interrupts();
    // The reason: POR at power-on; after a SOFT_POR its own bit, still set.
    reset_ctl_ = power_on ? kRstPor : (reset_ctl_ & ~kRstPor);
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
        retry_timer_.schedule_in(kRetryCycles);
    }
}

void Sabre::dispatch_all() {
    for (unsigned ino = 0; ino < kNumIno; ++ino)
        if (level_[ino]) dispatch(ino);
    for (unsigned i = 0; i < 2; ++i)
        if (pulse_[i]) dispatch_pulse(i);
}

void Sabre::pulse_upa_interrupt(unsigned which) {
    if (which > 1 || pulse_[which]) return;
    if (!(obio_imap_[((which ? kUpaImap : kGfxImap) - kObioImap) >> 3] & kImapValid)) return;
    pulse_[which] = true;
    dispatch_pulse(which);
}

// A pulse is sent with its map's interrupt number and then forgotten: it
// has no clear register.
void Sabre::dispatch_pulse(unsigned which) {
    const u64 map = obio_imap_[((which ? kUpaImap : kGfxImap) - kObioImap) >> 3];
    if (!(map & kImapValid)) return;
    if (send_vector_ && send_vector_(map & kImapInr)) {
        pulse_[which] = false;
    } else if (!retry_timer_.active()) {
        retry_timer_.schedule_in(kRetryCycles);
    }
}

// Whether IOMMU TLB entry n's tag (as its diagnostic access holds it)
// matches a VA<31:13> (VA<31:16> for a 64K entry).
bool Sabre::iommu_tag_matches(unsigned n, u64 vpn) const {
    const auto t = regs_.find(kIommuTagDiag + 8 * n);
    const u64 tag = t == regs_.end() ? 0 : t->second;
    const u64 mask = (tag & (1u << 19)) ? 0x7FFF8 : 0x7FFFF;
    return ((tag ^ vpn) & mask) == 0;
}

u64 Sabre::reg(u32 o) const {
    switch (o) {
        case kUpaPortId: return kUpaPortIdValue;
        case kIommuCtl: return iommu_ctl_;
        case kIommuTsb: return iommu_tsb_;
        case kPciCtl: return pci_ctl_;
        case kPciAfsr: return pci_afsr_;
        case kUeAfsr: return ue_afsr_;
        case kCeAfsr: return ce_afsr_;
        case kUeceAfar:
        case kUeceAfarAlias: return uece_afar_;
        case kPciAfar: return pci_afar_;
        case kPciDiag: return pci_diag_;
        case kPciTasr: return pci_tasr_;
        case kResetCtl: return reset_ctl_;
        case kFfbConfig: return ffb_config_ | kFfbOneread;
        case kMcuF008: return kMcuF008Value;
        case kMemControl0: return mem_control0_;
        case kMemControl1: return mem_control1_;
        default: break;
    }
    if (o >= kPciImap && o < kPciImap + 0x40) return pci_imap_[(o - kPciImap) >> 3];
    if (o >= kObioImap && o < kObioImap + 0x100) return obio_imap_[((o - kObioImap) >> 3) & 31];
    if (o == kGfxImapAlias) return obio_imap_[(kGfxImap - kObioImap) >> 3];
    if (o == kUpaImapAlias) return obio_imap_[(kUpaImap - kObioImap) >> 3];
    // The IOMMU's tag comparator (IIi manual 19.3.2.6-7): bit n set when
    // tag n (A580 + 8n) matches the virtual address diagnostic register's
    // VA<31:13> (VA<31:16> for a 64K entry).
    if (o == kIommuVaDiag) {
        const auto it = regs_.find(o);
        return it == regs_.end() ? 0 : it->second & 0xFFFFE000ull;
    }
    if (o == kIommuTagCompare) {
        const auto va = regs_.find(kIommuVaDiag);
        const u64 vpn = va == regs_.end() ? 0 : (va->second >> 13) & 0x7FFFF;
        u64 hits = 0;
        for (unsigned i = 0; i < 16; ++i)
            if (iommu_tag_matches(i, vpn)) hits |= u64{1} << i;
        return hits;
    }
    // The interrupt state diagnostic registers: each level interrupt's state
    // machine, two bits an INO (IIi manual 19.3.3.4): PCI's 00-1F at A800,
    // the others' 20-3F at A808.
    if (o == kIntStateDiag || o == kIntStateDiag + 8) {
        const unsigned first = o == kIntStateDiag ? 0 : 0x20;
        u64 v = 0;
        for (unsigned i = 0; i < 0x20; ++i) v |= u64{state_[first + i] & 3u} << (2 * i);
        return v;
    }
    if (o >= kPciIclr && o < kPciIclr + 0x100) return state_[((o - kPciIclr) >> 3) & 0x1F];
    if (o >= kObioIclr && o < kObioIclr + 0x100) return state_[0x20 + (((o - kObioIclr) >> 3) & 0x1F)];
    if (o == kReservedAt1A00 || (o >= kReservedTimers && o < kReservedTimers + 0x20)) return kReservedValue;
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
        // (ERR and ERRSTS, 26:24, cleared by writing 1; the rest as written)
        case kIommuCtl: iommu_ctl_ = (iommu_ctl_ & ~v & 0x07000000ull) | (v & 0x00FF0007ull); return true;
        case kIommuTsb: iommu_tsb_ = v & 0x1FFFFFFE000ull; return true;
        // A flush invalidates each TLB entry whose tag matches the address
        // (IIi manual 19.3.2.3).
        case kIommuFlush:
            for (unsigned i = 0; i < 16; ++i)
                if (iommu_tag_matches(i, (v >> 13) & 0x7FFFF)) {
                    const auto d = regs_.find(kIommuDataDiag + 8 * i);
                    if (d != regs_.end()) d->second &= ~(u64{1} << 30);  // V
                }
            return true;
        case kPciCtl:
            // (the fields table 19-2 gives: MRLM_EN 36, ARB_PARK, priorities 21:16,
            // ERRINT_EN 8, RETRY_WAIT_EN 7, ARB_EN 3:0; SERR, 34, W1C; the rest 0)
            pci_ctl_ = (v & ((1ull << 36) | 0x3F0000u | 0x18Fu)) | (pci_ctl_ & (1ull << 34) & ~v);
            update_error_interrupts();
            return true;
        case kPciAfsr:
            pci_afsr_ &= ~(v & kAfsrErrors);  // the error bits clear when written with 1
            update_error_interrupts();
            return true;
        case kUeAfsr:
            ue_afsr_ &= ~(v & kUeErrors);
            update_error_interrupts();
            return true;
        case kCeAfsr:
            ce_afsr_ &= ~(v & kCeErrors);
            update_error_interrupts();
            return true;
        case kUeceAfar:
        case kUeceAfarAlias: return true;
        case kPciAfar: return true;
        case kPciDiag: pci_diag_ = v; return true;
        case kPciTasr: pci_tasr_ = v & 0xFF; return true;
        case kWriteSync: return true;
        case kFfbConfig: ffb_config_ = v & kFfbSprqs; return true;
        case kMcuF008: return true;
        case kMemControl0:
            mem_control0_ = v & kMc0Writable;
            if (on_mem_control0) on_mem_control0(mem_control0_);
            return true;
        case kMemControl1: mem_control1_ = v & kMc1Writable; return true;
        case kResetCtl: {
            // POR, B_POR and B_XIR clear when written with 1; SOFT_POR and
            // SOFT_XIR hold what is written (IIi manual table 17-2). Setting
            // one resets: written with 1 again while still set it does not
            // (OpenBoot writes back what it read to clear the others).
            const u64 rising = v & ~reset_ctl_ & (kRstSoftPor | kRstSoftXir);
            reset_ctl_ = (reset_ctl_ & ~(v & 0x98000000ull) & ~0x60000000ull) | (v & 0x60000000ull);
            if ((rising & kRstSoftPor) && on_soft_reset) on_soft_reset(true);
            else if ((rising & kRstSoftXir) && on_soft_reset) on_soft_reset(false);
            return true;
        }
        default: break;
    }
    if (o >= kPciImap && o < kPciImap + 0x40) {
        u64& m = pci_imap_[(o - kPciImap) >> 3];
        m = (m & ~(kImapValid | kImapTid)) | (v & (kImapValid | kImapTid));
        dispatch_all();
        return true;
    }
    if ((o >= kObioImap && o < kObioImap + 0x100) || o == kGfxImapAlias || o == kUpaImapAlias) {
        const u32 at = o == kGfxImapAlias ? kGfxImap : o == kUpaImapAlias ? kUpaImap : o;
        const u64 writable = kImapValid | kImapTid | (at == kGfxImap || at == kUpaImap ? kImapInr : 0);
        u64& m = obio_imap_[((at - kObioImap) >> 3) & 31];
        m = (m & ~writable) | (v & writable);
        dispatch_all();
        return true;
    }
    if ((o >= kPciIclr && o < kPciIclr + 0x100) || (o >= kObioIclr && o < kObioIclr + 0x100)) {
        const unsigned ino = o >= kObioIclr ? 0x20 + (((o - kObioIclr) >> 3) & 0x1F) : ((o - kPciIclr) >> 3) & 0x1F;
        state_[ino] = static_cast<u8>(v & 3);
        if (state_[ino] == kStateIdle && level_[ino]) dispatch(ino);  // a level still held is sent again
        return true;
    }
    if (o == kReservedAt1A00 || (o >= kReservedTimers && o < kReservedTimers + 0x20)) return true;
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

// (Configuration space is little-endian: a function's clock is one read
// little-endian here.)
u64 Sabre::config_stable(u64 offset, unsigned size) {
    const unsigned busn = static_cast<unsigned>(offset >> 16) & 0xFF, dev = static_cast<unsigned>(offset >> 11) & 31,
                   fn = static_cast<unsigned>(offset >> 8) & 7;
    const u8 reg = static_cast<u8>(offset);
    u64 st;
    if (size != 8) {
        st = root_.config_stable(busn, dev, fn, reg, size);
    } else {
        // Two cycles, read together: a clock if both halves are (or one is
        // and the other never changes).
        const u64 lo = root_.config_stable(busn, dev, fn, reg, 4), hi = root_.config_stable(busn, dev, fn, static_cast<u8>(reg + 4), 4);
        if (lo == Device::kClock || hi == Device::kClock)
            st = (lo == Device::kClock || lo == Device::kForever) && (hi == Device::kClock || hi == Device::kForever) ? Device::kClock : 0;
        else
            st = std::min(lo, hi);
    }
    return st == Device::kClock ? Device::kClockLittle : st;
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

// A PIO write: while no primary error is held, the AFAR and the AFSR's
// byte mask follow it (a real Ultra 10 reads them holding its last PIO
// write: the SAB82532's command register at the ok prompt, the IDE
// bus-master status under Solaris, with byte 0's mask bit; the manual
// describes them only as logging the primary error, which they then keep).
void Sabre::pio_write(u64 pa, unsigned size) {
    if (pci_afsr_ & kAfsrPrimary) return;
    pci_afar_ = pa & 0x1FFFFFFFFFCull;
    const u64 mask = ((u64{1} << size) - 1) << (pa & 7);
    pci_afsr_ = (pci_afsr_ & ~kAfsrByteMask) | ((mask & 0xFF) << 32);
}

// A PIO write terminated by a master abort: the primary error bit (or the
// secondary one while a primary is held), the received master abort in
// the module's PCI status, and the PCI error interrupt.
void Sabre::pio_error(u64 bit) {
    if (!(pci_afsr_ & kAfsrPrimary)) pci_afsr_ |= bit;
    else pci_afsr_ |= bit >> 4;  // a second error sets the secondary bit
    if (bit == kAfsrPma) host_->received(kStatusRma);
    if (bit == kAfsrPta) host_->received(kStatusRta);
    update_error_interrupts();
}

// A DVMA access without a usable translation (an invalid entry, or a
// write to a page not writable): the uncorrectable error status records a
// translation error with its direction (the primary bits, or the secondary
// ones while a primary error is held), the AFAR the TTE's address, and the
// IOMMU control register the kind of error. The master's cycle is target-aborted.
void Sabre::dma_error(u32 dvma, bool write, unsigned kind, u64 tte_pa) {
    if (!(ue_afsr_ & (kUePdrd | kUePdwr | kUePdte))) {
        const u64 offset = (tte_pa >> 3) & 7;  // the doubleword in its 64 bytes
        ue_afsr_ = (ue_afsr_ & ~kUeLogged) | kUePdte | (write ? kUePdwr : kUePdrd) | (offset << 29) |
                   ((offset & 1) ? 0xFF00ull << 32 : 0x00FFull << 32);
        uece_afar_ = tte_pa & 0x1FFFFFFFFF8ull;
    } else {
        ue_afsr_ |= kUeSdte | (write ? kUeSdwr : kUeSdrd);
    }
    host_->received(kStatusSta);
    iommu_ctl_ = (iommu_ctl_ & ~kIommuErrStatus) | kIommuErr | (u64{kind} << 25);
    UV_LOG(Debug, "sabre", "DMA translation error: %s @ %08x", write ? "write" : "read", dvma);
    update_error_interrupts();
}

void Sabre::update_error_interrupts() {
    set_interrupt(kInoPciErr, (pci_ctl_ & kPciCtlErrEn) && (pci_afsr_ & kAfsrErrors));
    set_interrupt(kInoUe, ue_afsr_ & kUeErrors);
    set_interrupt(kInoCe, ce_afsr_ & kCeErrors);
}

bool Sabre::io_read(u64 offset, unsigned size, u64& value) {
    u32 v;
    pci::target_abort_signalled = false;
    if (size > 4 || !root_.io_read(static_cast<u32>(offset), size, v)) {
        read_failed();
        return false;
    }
    value = swap_size(v, size);
    return true;
}

bool Sabre::io_write(u64 offset, unsigned size, u64 value) {
    pio_write(kIoBase + offset, size);
    pci::target_abort_signalled = false;
    if (size > 4 || !root_.io_write(static_cast<u32>(offset), size, static_cast<u32>(swap_size(value, size))))
        pio_error(pci::target_abort_signalled ? kAfsrPta : kAfsrPma);
    return true;
}

// A PIO read that fails is not logged here: a master abort reaches the
// processor as a time-out, a target abort as a bus error (its AFSR's TO or
// BERR); the module's status records which it received.
void Sabre::read_failed() {
    if (pci::target_abort_signalled) {
        host_->received(kStatusRta);
        bus_.signal_bus_error();
    } else {
        host_->received(kStatusRma);
    }
}

bool Sabre::mem_read(u64 offset, unsigned size, u64& value) {
    u64 v;
    pci::target_abort_signalled = false;
    if (!root_.mem_read(static_cast<u32>(offset), size, v)) {
        read_failed();
        UV_LOG(Debug, "sabre", "PCI master abort: read%u @ %08llx", size * 8, static_cast<unsigned long long>(offset));
        return false;
    }
    value = swap_size(v, size);
    return true;
}

u64 Sabre::mem_stable(u64 offset, unsigned size) { return root_.mem_stable(static_cast<u32>(offset), size); }

bool Sabre::mem_write(u64 offset, unsigned size, u64 value) {
    pio_write(kMemBase + offset, size);
    pci::target_abort_signalled = false;
    if (!root_.mem_write(static_cast<u32>(offset), size, swap_size(value, size))) {
        pio_error(pci::target_abort_signalled ? kAfsrPta : kAfsrPma);
        UV_LOG(Debug, "sabre", "PCI master abort: write%u @ %08llx", size * 8, static_cast<unsigned long long>(offset));
    }
    return true;
}

// IOMMU: with translation enabled, a DVMA address indexes the TSB (8-byte
// entries, one per 8 KiB or 64 KiB page as the control register says) in
// memory; each entry holds valid (bit 63), page size (61), writable (1)
// and the physical page. With it disabled, DVMA addresses are physical.
bool Sabre::translate(u32 dvma, bool write, u64& pa) const {
    unsigned kind;
    u64 tte_pa;
    return translate(dvma, write, pa, kind, tte_pa);
}

bool Sabre::translate(u32 dvma, bool write, u64& pa, unsigned& kind, u64& tte_pa, u64* tte_out) const {
    if (!(iommu_ctl_ & 1)) {
        pa = dvma;
        return true;
    }
    kind = kIommuInvalid;
    const unsigned tsb_size = static_cast<unsigned>(iommu_ctl_ >> 16) & 7;
    const bool big = iommu_ctl_ & 4;
    const unsigned page_shift = big ? 16 : 13;
    const u64 entries = u64{1024} << tsb_size;
    const u64 index = (dvma >> page_shift) & (entries - 1);
    u64 tte;
    u8 buf[8];
    tte_pa = iommu_tsb_ + index * 8;
    if (!bus_.read_block(tte_pa, buf, 8)) return false;
    tte = load_be<u64>(buf);
    if (!(tte >> 63)) return false;
    if (write && !(tte & 2)) {
        kind = kIommuProtection;
        return false;
    }
    const bool tte64k = (tte >> 61) & 1;
    const u64 mask = tte64k ? 0xFFFF : 0x1FFF;
    pa = ((tte & 0x1FFFFFFE000ull) & ~mask) | (dvma & mask);
    if (tte_out) *tte_out = tte;
    return true;
}

// The IOMMU's TLB (IIi manual 19.3.2): 16 entries, held where their
// diagnostic accesses show them (tags at A580 + 8n: W 21, streaming 20,
// 64K 19, VA<31:13> 18:0; data at A600 + 8n: V 30, cacheable 28, PA<40:13>
// 27:0). A DMA address is looked up there first; a miss walks the TSB and
// loads the translation into the least recently used entry, or, with the
// control register's LRU_LCKEN (bit 23) set, into the entry LRU_LCKPTR
// (22:19) names, the only one then replaced (19.3.2.1, "IOMMU locking").
// Software flushes an entry when it changes the TSB under it.
void Sabre::tlb_used(unsigned n) {
    auto it = std::find(tlb_lru_.begin(), tlb_lru_.end(), static_cast<u8>(n));
    std::rotate(it, it + 1, tlb_lru_.end());
}

bool Sabre::iommu_map(u32 dvma, bool write, u64& pa, unsigned& kind, u64& tte_pa) {
    if (!(iommu_ctl_ & 1)) {
        pa = dvma;
        return true;
    }
    const u64 vpn = (dvma >> 13) & 0x7FFFF;
    for (unsigned n = 0; n < 16; ++n) {
        const auto d = regs_.find(kIommuDataDiag + 8 * n);
        if (d == regs_.end() || !(d->second & (u64{1} << 30)) || !iommu_tag_matches(n, vpn)) continue;
        const u64 tag = regs_[kIommuTagDiag + 8 * n];
        if (write && !(tag & (u64{1} << 21))) {
            kind = kIommuProtection;
            const unsigned tsb_size = static_cast<unsigned>(iommu_ctl_ >> 16) & 7;
            const unsigned page_shift = (iommu_ctl_ & 4) ? 16 : 13;
            tte_pa = iommu_tsb_ + ((dvma >> page_shift) & ((u64{1024} << tsb_size) - 1)) * 8;
            return false;
        }
        const u64 mask = (tag & (u64{1} << 19)) ? 0xFFFF : 0x1FFF;
        pa = (((d->second & 0xFFFFFFFull) << 13) & ~mask) | (dvma & mask);
        tlb_used(n);
        return true;
    }
    u64 tte = 0;
    if (!translate(dvma, write, pa, kind, tte_pa, &tte)) return false;
    const unsigned n = (iommu_ctl_ & (u64{1} << 23)) ? static_cast<unsigned>(iommu_ctl_ >> 19) & 0xF : tlb_lru_[0];
    const bool big = (tte >> 61) & 1;
    regs_[kIommuTagDiag + 8 * n] = (tte & 2 ? u64{1} << 21 : 0) | (((tte >> 60) & 1) << 20) | (u64{big} << 19) |
                                   (big ? vpn & 0x7FFF8 : vpn);
    regs_[kIommuDataDiag + 8 * n] = (u64{1} << 30) | (((tte >> 4) & 1) << 28) | ((tte >> 13) & 0xFFFFFFFull);
    tlb_used(n);
    return true;
}

// A 32-bit DMA address is the module's only in the 512 MiB regions the
// target address space register enables (OpenBoot enables 0xC000_0000 to
// 0xDFFF_FFFF); elsewhere it is left to other PCI devices, and with none
// there the master's cycle ends in a master abort.
bool Sabre::claims(u32 dvma) const { return (pci_tasr_ >> (dvma >> 29)) & 1; }

bool Sabre::dma_read(u32 dvma, u8* data, std::size_t length) {
    while (length) {
        if (!claims(dvma)) return false;
        u64 pa;
        unsigned kind;
        u64 tte_pa;
        if (!iommu_map(dvma, false, pa, kind, tte_pa)) {
            dma_error(dvma, false, kind, tte_pa);
            return false;
        }
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
        if (!claims(dvma)) return false;
        u64 pa;
        unsigned kind;
        u64 tte_pa;
        if (!iommu_map(dvma, true, pa, kind, tte_pa)) {
            dma_error(dvma, true, kind, tte_pa);
            return false;
        }
        const std::size_t chunk = std::min<std::size_t>(length, 0x2000 - (dvma & 0x1FFF));
        if (!bus_.write_block(pa, data, chunk)) return false;
        dvma += static_cast<u32>(chunk);
        data += chunk;
        length -= chunk;
    }
    return true;
}

void Sabre::describe(std::string& out) const {
    static const char* const kState[] = {"idle", "received", "?", "pending"};
    appendf(out, "sabre: interrupt sources asserted or not idle (ino: level, clear state, IMAP):");
    bool any = false;
    for (unsigned ino = 0; ino < kNumIno; ++ino) {
        if (!level_[ino] && state_[ino] == kStateIdle) continue;
        const u64 map = ino < 0x20 ? pci_imap_[ino >> 2] : obio_imap_[(ino - 0x20) & 31];
        appendf(out, " %02x: %s %s %llx;", ino, level_[ino] ? "high" : "low", kState[state_[ino] & 3],
                static_cast<unsigned long long>(map));
        any = true;
    }
    appendf(out, "%s%s\n", any ? "" : " none", retry_timer_.active() ? " (retry timer running)" : "");
}

}  // namespace uv
