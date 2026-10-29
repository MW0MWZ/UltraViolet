// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
// UltraSPARC IIi memory access: address translation through the I- and
// D-MMU, the memory ASIs (including block, partial-store and short
// floating-point ASIs), and the internal registers reached through ASIs.
#include "core/log.h"
#include "cpu/ultrasparc/ultrasparc.h"

namespace uv::usparc {

namespace {

constexpr u64 kLsuIm = 1u << 2, kLsuDm = 1u << 3;  // LSU control: I- and D-MMU enables

// Fault type bits in the synchronous fault status registers.
constexpr unsigned kFtPrivilege = 0x01, kFtSideEffect = 0x02, kFtAtomicUncacheable = 0x04, kFtIllegalAsi = 0x08,
                   kFtNfo = 0x10, kFtVaRange = 0x20;

// Context types in the SFSR.
constexpr unsigned kCtPrimary = 0, kCtSecondary = 1, kCtNucleus = 2;

// Virtual addresses are 44 bits, sign-extended: VA[63:43] must all be equal.
inline bool va_in_range(u64 va) {
    const u64 top = va >> 43;
    return top == 0 || top == (u64{1} << 21) - 1;
}

u64 swap_size(u64 v, unsigned size) {
    switch (size) {
        case 2: return byteswap(static_cast<u16>(v));
        case 4: return byteswap(static_cast<u32>(v));
        case 8: return byteswap(v);
        default: return v;
    }
}

}  // namespace

bool UltraSparc::asi_is_memory(u8 a) {
    switch (a) {
        case asi::N:
        case asi::NL:
        case asi::AIUP:
        case asi::AIUS:
        case asi::PHYS_USE_EC:
        case asi::PHYS_BYPASS_EC_E:
        case asi::AIUPL:
        case asi::AIUSL:
        case asi::PHYS_USE_EC_L:
        case asi::PHYS_BYPASS_EC_E_L:
        case asi::NQUAD_LDD:
        case asi::NQUAD_LDD_L:
        case asi::P:
        case asi::S:
        case asi::PNF:
        case asi::SNF:
        case asi::PL:
        case asi::SL:
        case asi::PNFL:
        case asi::SNFL: return true;
        default: return false;
    }
}

bool UltraSparc::asi_is_little(u8 a) {
    switch (a) {
        case asi::NL:
        case asi::AIUPL:
        case asi::AIUSL:
        case asi::PHYS_USE_EC_L:
        case asi::PHYS_BYPASS_EC_E_L:
        case asi::NQUAD_LDD_L:
        case asi::PL:
        case asi::SL:
        case asi::PNFL:
        case asi::SNFL:
        case asi::BLK_AIUPL:
        case asi::BLK_AIUSL:
        case asi::BLK_PL:
        case asi::BLK_SL: return true;
        default: return (a & 0xF8) == 0xC8 || (a & 0xF8) == 0xD8;  // little partial stores and short loads
    }
}

u8 UltraSparc::base_memory_asi(u8 a) {
    switch (a) {
        case asi::BLK_AIUP: return asi::AIUP;
        case asi::BLK_AIUS: return asi::AIUS;
        case asi::BLK_AIUPL: return asi::AIUPL;
        case asi::BLK_AIUSL: return asi::AIUSL;
        case asi::BLK_COMMIT_P: return asi::P;
        case asi::BLK_COMMIT_S: return asi::S;
        case asi::BLK_P: return asi::P;
        case asi::BLK_S: return asi::S;
        case asi::BLK_PL: return asi::PL;
        case asi::BLK_SL: return asi::SL;
        default:
            // Partial stores (0xC0-0xCD) and short loads (0xD0-0xDB): bit 0
            // selects secondary, bit 3 little-endian.
            return static_cast<u8>(((a & 8) ? asi::PL : asi::P) | (a & 1));
    }
}

bool UltraSparc::asi_restricted_check(u8 a) {
    if (a < 0x80 && !(pstate_ & pstate::PRIV)) {
        take_trap(tt::kPrivilegedAction);
        return false;
    }
    return true;
}

TlbEntry* UltraSparc::tlb_lookup(MmuRegs& mmu, u64 va, unsigned context) {
    const u64 page = va >> 13;
    SoftTlbEntry& s = soft_tlb_[&mmu == &dmmu_][page & (kSoftTlbSize - 1)];
    if (s.gen == soft_gen_ && s.va_page == page && s.context == context) return s.entry;
    for (TlbEntry& e : mmu.tlb) {
        if (!(e.data & tte::V)) continue;
        const unsigned shift = tte::page_shift(tte::size(e.data));
        if (((e.tag ^ va) >> shift) != 0) continue;
        if (!(e.data & tte::G) && (e.tag & 0x1FFF) != context) continue;
        e.used = true;
        s = {page, context, soft_gen_, &e};
        return &e;
    }
    return nullptr;
}

bool UltraSparc::debug_translate(u64 va, bool data, u64& pa) const {
    const bool enabled = data ? (lsu_control_ & kLsuDm) : (!(pstate_ & pstate::RED) && (lsu_control_ & kLsuIm));
    if (!enabled) {
        pa = va & Bus::kAddressMask;
        return true;
    }
    const unsigned context = tl_ > 0 ? 0 : static_cast<unsigned>(primary_context_);
    for (const TlbEntry& e : (data ? dmmu_ : immu_).tlb) {
        if (!(e.data & tte::V)) continue;
        const unsigned shift = tte::page_shift(tte::size(e.data));
        if (((e.tag ^ va) >> shift) != 0) continue;
        if (!(e.data & tte::G) && (e.tag & 0x1FFF) != context) continue;
        const u64 mask = (u64{1} << shift) - 1;
        pa = ((e.data & tte::kPaMask) & ~mask) | (va & mask);
        return true;
    }
    return false;
}

void UltraSparc::fetch_fault(unsigned trap, u64 va, unsigned context, unsigned ft) {
    if (trap == tt::kFastInstructionAccessMmuMiss) {
        immu_.tag_access = (va & ~u64{0x1FFF}) | context;
    } else {
        const bool overwrite = immu_.sfsr & 1;
        const unsigned ct = context == 0 && tl_ > 0 ? kCtNucleus : kCtPrimary;
        immu_.sfsr = (u64{ft} << 7) | (ct << 4) | ((pstate_ & pstate::PRIV) ? 8u : 0u) | (overwrite ? 2u : 0u) | 1u;
        immu_.tag_access = (va & ~u64{0x1FFF}) | context;
    }
    take_trap(trap);
}

void UltraSparc::data_fault(unsigned trap, u64 va, u8 a, unsigned context_type, unsigned context, unsigned ft,
                            bool write, bool side_effect) {
    dmmu_.tag_access = (va & ~u64{0x1FFF}) | context;
    if (trap != tt::kFastDataAccessMmuMiss) {
        const bool overwrite = dmmu_.sfsr & 1;
        dmmu_.sfsr = (u64{a} << 16) | (u64{ft} << 7) | (side_effect ? 0x40u : 0u) | (context_type << 4) |
                     ((pstate_ & pstate::PRIV) ? 8u : 0u) | (write ? 4u : 0u) | (overwrite ? 2u : 0u) | 1u;
        sfar_ = va;
    }
    take_trap(trap);
}

bool UltraSparc::translate_fetch(u64 va, u64& pa) {
    if ((pstate_ & pstate::RED) || !(lsu_control_ & kLsuIm)) {
        pa = va & Bus::kAddressMask;
        return true;
    }
    const unsigned context = tl_ > 0 ? 0 : static_cast<unsigned>(primary_context_);
    if (!va_in_range(va)) {
        fetch_fault(tt::kInstructionAccessException, va, context, kFtVaRange);
        return false;
    }
    TlbEntry* e = tlb_lookup(immu_, va, context);
    if (!e) {
        fetch_fault(tt::kFastInstructionAccessMmuMiss, va, context, 0);
        return false;
    }
    if ((e->data & tte::P) && !(pstate_ & pstate::PRIV)) {
        fetch_fault(tt::kInstructionAccessException, va, context, kFtPrivilege);
        return false;
    }
    const u64 offset_mask = (u64{1} << tte::page_shift(tte::size(e->data))) - 1;
    pa = ((e->data & tte::kPaMask) & ~offset_mask) | (va & offset_mask);
    return true;
}

bool UltraSparc::translate_data(u64 va, u8 a, AccessKind kind, unsigned size, Access& out) {
    (void)size;
    out.little = asi_is_little(a);
    const bool write = kind != AccessKind::Load;
    if (a == asi::PHYS_USE_EC || a == asi::PHYS_BYPASS_EC_E || a == asi::PHYS_USE_EC_L ||
        a == asi::PHYS_BYPASS_EC_E_L) {
        out.pa = va & Bus::kAddressMask;
        out.side_effect = a == asi::PHYS_BYPASS_EC_E || a == asi::PHYS_BYPASS_EC_E_L;
        return true;
    }
    unsigned context, ct;
    bool priv = pstate_ & pstate::PRIV;
    switch (a) {
        case asi::N:
        case asi::NL:
        case asi::NQUAD_LDD:
        case asi::NQUAD_LDD_L:
            context = 0;
            ct = kCtNucleus;
            break;
        case asi::S:
        case asi::SL:
        case asi::SNF:
        case asi::SNFL:
            context = static_cast<unsigned>(secondary_context_);
            ct = kCtSecondary;
            break;
        case asi::AIUP:
        case asi::AIUPL:
            context = static_cast<unsigned>(primary_context_);
            ct = kCtPrimary;
            priv = false;
            break;
        case asi::AIUS:
        case asi::AIUSL:
            context = static_cast<unsigned>(secondary_context_);
            ct = kCtSecondary;
            priv = false;
            break;
        default:
            context = static_cast<unsigned>(primary_context_);
            ct = kCtPrimary;
            break;
    }
    const bool nonfaulting = a == asi::PNF || a == asi::SNF || a == asi::PNFL || a == asi::SNFL;
    if (nonfaulting && write) {
        data_fault(tt::kDataAccessException, va, a, ct, context, kFtIllegalAsi, write);
        return false;
    }
    if (!(lsu_control_ & kLsuDm)) {
        out.pa = va & Bus::kAddressMask;
        out.side_effect = (out.pa >> 40) & 1;  // I/O space is uncached, with side effects
        return true;
    }
    if (!va_in_range(va)) {
        data_fault(tt::kDataAccessException, va, a, ct, context, kFtVaRange, write);
        return false;
    }
    TlbEntry* e = tlb_lookup(dmmu_, va, context);
    if (!e) {
        data_fault(tt::kFastDataAccessMmuMiss, va, a, ct, context, 0, write);
        return false;
    }
    const u64 d = e->data;
    if ((d & tte::P) && !priv) {
        data_fault(tt::kDataAccessException, va, a, ct, context, kFtPrivilege, write, d & tte::E);
        return false;
    }
    if (nonfaulting && (d & tte::E)) {
        data_fault(tt::kDataAccessException, va, a, ct, context, kFtSideEffect, write, true);
        return false;
    }
    if (!nonfaulting && (d & tte::NFO)) {
        data_fault(tt::kDataAccessException, va, a, ct, context, kFtNfo, write, d & tte::E);
        return false;
    }
    if (kind == AccessKind::Atomic && !(d & tte::CP)) {
        data_fault(tt::kDataAccessException, va, a, ct, context, kFtAtomicUncacheable, write, d & tte::E);
        return false;
    }
    if (write && !(d & tte::W)) {
        direct_64k_ = tte::size(d) == 1;
        data_fault(tt::kFastDataAccessProtection, va, a, ct, context, 0, write, d & tte::E);
        return false;
    }
    const u64 offset_mask = (u64{1} << tte::page_shift(tte::size(d))) - 1;
    out.pa = ((d & tte::kPaMask) & ~offset_mask) | (va & offset_mask);
    out.side_effect = d & tte::E;
    if (d & tte::IE) out.little = !out.little;
    return true;
}

bool UltraSparc::phys_read(const Access& acc, unsigned size, u64& value) {
    bool ok;
    switch (size) {
        case 1: {
            u8 v = 0;
            ok = bus_.read(acc.pa, v);
            value = v;
            break;
        }
        case 2: {
            u16 v = 0;
            ok = bus_.read(acc.pa, v);
            value = v;
            break;
        }
        case 4: {
            u32 v = 0;
            ok = bus_.read(acc.pa, v);
            value = v;
            break;
        }
        default: {
            u64 v = 0;
            ok = bus_.read(acc.pa, v);
            value = v;
            break;
        }
    }
    if (!ok) {
        afar_ = acc.pa;
        afsr_ |= u64{1} << 21;  // TO: time-out
        take_trap(tt::kDataAccessError);
        return false;
    }
    if (acc.little) value = swap_size(value, size);
    return true;
}

bool UltraSparc::phys_write(const Access& acc, unsigned size, u64 value) {
    if (acc.little) value = swap_size(value, size);
    switch (size) {
        case 1: return bus_.write(acc.pa, static_cast<u8>(value));
        case 2: return bus_.write(acc.pa, static_cast<u16>(value));
        case 4: return bus_.write(acc.pa, static_cast<u32>(value));
        default: return bus_.write(acc.pa, value);
    }
}

bool UltraSparc::load(u64 va, u8 a, unsigned size, u64& value, AccessKind kind) {
    if (!asi_is_memory(a)) {
        if (asi_is_block(a) || asi_is_partial(a) || asi_is_short_fp(a)) {
            data_fault(tt::kDataAccessException, va, a, kCtPrimary, static_cast<unsigned>(primary_context_), kFtIllegalAsi,
                       kind != AccessKind::Load);
            return false;
        }
        if (kind == AccessKind::Atomic) {
            data_fault(tt::kDataAccessException, va, a, kCtPrimary, static_cast<unsigned>(primary_context_), kFtIllegalAsi,
                       true);
            return false;
        }
        return internal_asi_read(a, va, size, value);
    }
    Access acc;
    if (fast_device(va, a, kind, acc)) return phys_read(acc, size, value);
    if (!translate_data(va, a, kind, size, acc)) return false;
    const u64 slow = bus_.slow_accesses();
    if (!phys_read(acc, size, value)) return false;
    if (bus_.slow_accesses() == slow) {
        if (kind != AccessKind::Fetch) fast_fill(va, a, acc, kind);
    } else {
        fast_fill_device(va, a, acc, kind);
    }
    return true;
}

bool UltraSparc::store(u64 va, u8 a, unsigned size, u64 value) {
    if (!asi_is_memory(a)) {
        if (asi_is_block(a) || asi_is_partial(a) || asi_is_short_fp(a) || a == asi::PNF || a == asi::SNF) {
            data_fault(tt::kDataAccessException, va, a, kCtPrimary, static_cast<unsigned>(primary_context_), kFtIllegalAsi,
                       true);
            return false;
        }
        return internal_asi_write(a, va, size, value);
    }
    Access acc;
    if (fast_device(va, a, AccessKind::Store, acc)) return phys_write(acc, size, value);
    if (!translate_data(va, a, AccessKind::Store, size, acc)) return false;
    const u64 slow = bus_.slow_accesses();
    if (!phys_write(acc, size, value)) return false;
    if (bus_.slow_accesses() == slow) fast_fill(va, a, acc, AccessKind::Store);
    else fast_fill_device(va, a, acc, AccessKind::Store);
    return true;
}

// An atomic access that translated also passed the store checks, on a
// cacheable page.
void UltraSparc::fast_fill(u64 va, u8 a, const Access& acc, AccessKind kind) {
    const int mode = fast_class(a);
    if (mode < 0 || acc.little != asi_is_little(a)) return;  // (TTE.IE)
    constexpr u64 kPage = 0x1FFF;
    const bool write = kind != AccessKind::Load;
    u8* const host = write ? bus_.host_write_ptr(acc.pa & ~kPage) : bus_.host_read_ptr(acc.pa & ~kPage);
    if (!host) return;
    const u8* const code = write ? bus_.code_flag(host) : nullptr;
    if (write && !code) return;
    FastEntry& f = fast_[mode][(va >> 13) & (kFastEntries - 1)];
    const u64 key = (va >> 13) | fast_key_;
    const s64 addend = static_cast<s64>(reinterpret_cast<uintptr_t>(host) - (va & ~kPage));
    if (f.key != key || f.addend != addend || (f.perms & kFastDev)) f = FastEntry{key, addend, nullptr, 0};
    if (write) f.code = code;
    f.perms |= kind == AccessKind::Load ? kFastRead : kind == AccessKind::Store ? kFastWrite : kFastRead | kFastWrite | kFastAtomic;
}

bool UltraSparc::fast_device(u64 va, u8 a, AccessKind kind, Access& acc) const {
    if (kind != AccessKind::Load && kind != AccessKind::Store) return false;
    const int mode = fast_class(a);  // (none for no-fault loads, which fault on side effects)
    if (mode < 0) return false;
    const FastEntry& f = fast_[mode][(va >> 13) & (kFastEntries - 1)];
    if (f.key != ((va >> 13) | fast_key_) || !(f.perms & (kind == AccessKind::Load ? kFastDevRead : kFastDevWrite))) return false;
    acc.pa = static_cast<u64>(static_cast<s64>(va) + f.addend);
    acc.little = asi_is_little(a);
    acc.side_effect = f.perms & kFastDevSide;
    return true;
}

void UltraSparc::fast_fill_device(u64 va, u8 a, const Access& acc, AccessKind kind) {
    if (kind != AccessKind::Load && kind != AccessKind::Store) return;
    const int mode = fast_class(a);
    if (mode < 0 || acc.little != asi_is_little(a)) return;  // (TTE.IE)
    constexpr u64 kPage = 0x1FFF;
    FastEntry& f = fast_[mode][(va >> 13) & (kFastEntries - 1)];
    const u64 key = (va >> 13) | fast_key_;
    const s64 addend = static_cast<s64>((acc.pa & ~kPage) - (va & ~kPage));
    if (f.key != key || f.addend != addend || !(f.perms & kFastDev)) f = FastEntry{key, addend, nullptr, 0};
    f.perms |= static_cast<u8>((kind == AccessKind::Load ? kFastDevRead : kFastDevWrite) | (acc.side_effect ? kFastDevSide : 0));
}

// Block load: eight doublewords into an aligned group of eight double registers.
bool UltraSparc::block_load(u64 va, u8 a, unsigned rd) {
    if (rd & 0x0E) {
        take_trap(tt::kIllegalInstruction);
        return false;
    }
    if (va & 63) {
        take_trap(tt::kMemAddressNotAligned);
        return false;
    }
    if (a == asi::BLK_COMMIT_P || a == asi::BLK_COMMIT_S) {  // the commit ASIs are store-only
        data_fault(tt::kDataAccessException, va, a, kCtPrimary, static_cast<unsigned>(primary_context_), kFtIllegalAsi,
                   false);
        return false;
    }
    const u8 base = base_memory_asi(a);
    std::array<u64, 8> v{};
    for (unsigned i = 0; i < 8; ++i) {
        Access acc;
        if (!translate_data(va + 8 * i, base, AccessKind::Load, 8, acc) || !phys_read(acc, 8, v[i])) return false;
        if (i == 0) fast_fill(va, base, acc, AccessKind::Load);  // (one page)
    }
    for (unsigned i = 0; i < 8; ++i) set_f64(rd + 2 * i, v[i]);
    return true;
}

bool UltraSparc::block_store(u64 va, u8 a, unsigned rd) {
    if (rd & 0x0E) {
        take_trap(tt::kIllegalInstruction);
        return false;
    }
    if (va & 63) {
        take_trap(tt::kMemAddressNotAligned);
        return false;
    }
    const u8 base = base_memory_asi(a);
    std::array<Access, 8> acc{};
    for (unsigned i = 0; i < 8; ++i)
        if (!translate_data(va + 8 * i, base, AccessKind::Store, 8, acc[i])) return false;
    for (unsigned i = 0; i < 8; ++i) phys_write(acc[i], 8, freg64(rd + 2 * i));
    fast_fill(va, base, acc[0], AccessKind::Store);  // (one page)
    return true;
}

// Short floating-point loads: a byte (FL8) or halfword (FL16), zero-extended
// into a double register.
bool UltraSparc::short_fp_load(u64 va, u8 a, unsigned rd) {
    const unsigned size = (a & 2) ? 2 : 1;
    if (va & (size - 1)) {
        take_trap(tt::kMemAddressNotAligned);
        return false;
    }
    Access acc;
    u64 v;
    if (!translate_data(va, base_memory_asi(a), AccessKind::Load, size, acc)) return false;
    fast_fill(va, base_memory_asi(a), acc, AccessKind::Load);
    acc.little = asi_is_little(a);
    if (!phys_read(acc, size, v)) return false;
    set_f64(rd, v);
    return true;
}

bool UltraSparc::short_fp_store(u64 va, u8 a, unsigned rd) {
    const unsigned size = (a & 2) ? 2 : 1;
    if (va & (size - 1)) {
        take_trap(tt::kMemAddressNotAligned);
        return false;
    }
    Access acc;
    if (!translate_data(va, base_memory_asi(a), AccessKind::Store, size, acc)) return false;
    fast_fill(va, base_memory_asi(a), acc, AccessKind::Store);
    acc.little = asi_is_little(a);
    return phys_write(acc, size, freg64(rd));
}

// Partial store: the bytes, halfwords or words of a double register whose
// bits are set in the mask (r[rs2]); the most significant element is
// selected by the mask's highest bit.
bool UltraSparc::partial_store(u64 va, u8 a, unsigned rd, u64 mask) {
    if (va & 7) {
        take_trap(tt::kMemAddressNotAligned);
        return false;
    }
    const unsigned esize = 1u << ((a >> 1) & 3);  // 0xC0/1: 8-bit, 0xC2/3: 16-bit, 0xC4/5: 32-bit
    const unsigned count = 8 / esize;
    Access acc;
    if (!translate_data(va, base_memory_asi(a), AccessKind::Store, 8, acc)) return false;
    fast_fill(va, base_memory_asi(a), acc, AccessKind::Store);
    const u64 v = freg64(rd);
    const bool little = asi_is_little(a);
    for (unsigned i = 0; i < count; ++i) {
        if (!((mask >> (count - 1 - i)) & 1)) continue;
        const unsigned shift = (count - 1 - i) * esize * 8;
        u64 elem = (v >> shift) & ((esize == 8 ? ~u64{0} : (u64{1} << (esize * 8)) - 1));
        Access e = acc;
        e.pa = little ? acc.pa + (count - 1 - i) * esize : acc.pa + i * esize;
        e.little = little;
        phys_write(e, esize, elem);
    }
    return true;
}

u64& UltraSparc::diag(u8 a, u64 va) { return diag_[(u64{a} << 56) | (va & 0x00FFFFFFFFFFFFFFull)]; }

u64 UltraSparc::tsb_pointer(const MmuRegs& mmu, bool size_64k) const {
    const unsigned n = static_cast<unsigned>(mmu.tsb) & 7;
    const bool split = (mmu.tsb >> 12) & 1;
    const u64 entries = u64{512} << n;
    const u64 index = (mmu.tag_access >> (size_64k ? 16 : 13)) & (entries - 1);
    const u64 table = entries * 16;  // bytes in one TSB
    if (!split) return (mmu.tsb & ~(table - 1)) | (index << 4);
    return (mmu.tsb & ~(2 * table - 1)) | (size_64k ? table : 0) | (index << 4);
}

void UltraSparc::tlb_data_in(MmuRegs& mmu, u64 data) {
    TlbEntry* slot = nullptr;
    for (TlbEntry& e : mmu.tlb)
        if (!(e.data & tte::V)) {
            slot = &e;
            break;
        }
    if (!slot) {
        for (TlbEntry& e : mmu.tlb)
            if (!(e.data & tte::L) && !e.used) {
                slot = &e;
                break;
            }
    }
    if (!slot) {
        // Every entry used: clear the used bits, and the translations made
        // from the entries (hits through those do not set the bits again).
        for (TlbEntry& e : mmu.tlb)
            if (!(e.data & tte::L)) e.used = false;
        flush_soft_tlb();
        for (TlbEntry& e : mmu.tlb)
            if (!(e.data & tte::L)) {
                slot = &e;
                break;
            }
    }
    if (!slot) slot = &mmu.tlb[kTlbEntries - 1];  // every entry locked: replace the last
    if (slot->data & tte::V) forget_translations(mmu, *slot);
    slot->tag = mmu.tag_access;
    slot->data = data;
    slot->used = false;
    attention_ = true;
}

void UltraSparc::forget_translations(MmuRegs& mmu, const TlbEntry& e) {
    const bool data_side = &mmu == &dmmu_;
    const unsigned shift = tte::page_shift(tte::size(e.data));
    const u64 first = (e.tag >> shift) << (shift - 13), count = u64{1} << (shift - 13);  // in 8 KiB pages
    auto in_range = [&](u64 page) { return page - first < count; };
    for (SoftTlbEntry& s : soft_tlb_[data_side])
        if (s.entry == &e) s.gen = 0;
    if (data_side) {
        for (unsigned c = 0; c < kFastClasses; ++c) {
            if (c == 5) continue;  // (physical: no TLB)
            for (u64 k = 0; k < std::min<u64>(count, kFastEntries); ++k) {
                FastEntry& f = fast_[c][(first + k) & (kFastEntries - 1)];
                if (in_range(f.key & ((u64{1} << 51) - 1))) f = FastEntry{};
            }
        }
    } else {
        for (FetchEntry& f : fetch_cache_)
            if (in_range(f.va_page)) f.gen = 0;
    }
    attention_ = true;
}

void UltraSparc::tlb_demap(MmuRegs& mmu, u64 va) {
    const bool context_demap = (va >> 6) & 1;
    const unsigned sel = (va >> 4) & 3;
    unsigned context;
    switch (sel) {
        case 0: context = static_cast<unsigned>(primary_context_); break;
        case 1: context = static_cast<unsigned>(secondary_context_); break;
        case 2: context = 0; break;
        default: return;
    }
    for (TlbEntry& e : mmu.tlb) {
        if (!(e.data & tte::V)) continue;
        bool demap;
        if (context_demap) {
            demap = !(e.data & tte::G) && (e.tag & 0x1FFF) == context;
        } else {
            const unsigned shift = tte::page_shift(tte::size(e.data));
            demap = ((e.tag ^ va) >> shift) == 0 && ((e.data & tte::G) || (e.tag & 0x1FFF) == context);
        }
        if (demap) {
            forget_translations(mmu, e);
            e.data &= ~tte::V;
        }
    }
}

u64 UltraSparc::mmu_read(bool data_side, u64 va) const {
    const MmuRegs& m = data_side ? dmmu_ : immu_;
    switch (va & 0xFF) {
        case 0x00: return ((m.tag_access & 0x1FFF) << 48) | (m.tag_access >> 22);  // tag target
        case 0x08: return data_side ? primary_context_ : 0;
        case 0x10: return data_side ? secondary_context_ : 0;
        case 0x18: return m.sfsr;
        case 0x20: return data_side ? sfar_ : 0;
        case 0x28: return m.tsb;
        case 0x30: return m.tag_access;
        case 0x38: return data_side ? va_watch_ : 0;
        case 0x40: return data_side ? pa_watch_ : 0;
        default: return 0;
    }
}

void UltraSparc::mmu_write(bool data_side, u64 va, u64 value) {
    MmuRegs& m = data_side ? dmmu_ : immu_;
    switch (va & 0xFF) {
        case 0x08:
            if (data_side) {
                primary_context_ = value & 0x1FFF;
                flush_soft_tlb();
            }
            break;
        case 0x10:
            if (data_side) {
                secondary_context_ = value & 0x1FFF;
                flush_soft_tlb();
            }
            break;
        case 0x18: m.sfsr = value & 0xFF7FFF; break;
        case 0x28: m.tsb = value & ~u64{0xFF8}; break;
        case 0x30: m.tag_access = value; break;
        case 0x38:
            if (data_side) va_watch_ = value & ~u64{7};
            break;
        case 0x40:
            if (data_side) pa_watch_ = value & 0x1FFFFFFFFF8ull;
            break;
        default: break;
    }
}

bool UltraSparc::internal_read_is_plain(u8 a) {
    switch (a) {
        case asi::LSU_CONTROL:
        case asi::ESTATE_ERROR_EN:
        case asi::AFSR:
        case asi::AFAR:
        case asi::UPA_CONFIG:
        case asi::INTR_DISPATCH_STATUS:
        case asi::INTR_RECEIVE:
        case asi::IMMU:
        case asi::DMMU:
        case asi::IMMU_TSB_8K_PTR:
        case asi::IMMU_TSB_64K_PTR:
        case asi::DMMU_TSB_8K_PTR:
        case asi::DMMU_TSB_64K_PTR:
        case asi::DMMU_TSB_DIRECT_PTR:
        case asi::ITLB_DATA_ACCESS:
        case asi::DTLB_DATA_ACCESS:
        case asi::ITLB_TAG_READ:
        case asi::DTLB_TAG_READ:
        case asi::ECACHE_TAG_DATA: return true;
        default: return false;
    }
}

u64 UltraSparc::jit_internal_read(UltraSparc* cpu, u64 a, u64 va) {
    u64 value = 0;
    cpu->internal_asi_read(static_cast<u8>(a), va, 8, value);
    return value;
}

bool UltraSparc::internal_asi_read(u8 a, u64 va, unsigned size, u64& value) {
    switch (a) {
        case asi::LSU_CONTROL: value = lsu_control_; return true;
        case asi::ESTATE_ERROR_EN: value = estate_error_en_; return true;
        case asi::AFSR: value = afsr_; return true;
        case asi::AFAR: value = afar_; return true;
        case asi::UPA_CONFIG: value = upa_config_; return true;
        case asi::INTR_DISPATCH_STATUS: value = 0; return true;
        case asi::INTR_RECEIVE: value = intr_receive_; return true;
        case asi::UDB_INTR_R:
            switch (va & 0xFF) {
                case 0x00: value = udb_error_[0]; return true;
                case 0x18: value = udb_error_[1]; return true;
                case 0x20: value = udb_control_[0]; return true;
                case 0x38: value = udb_control_[1]; return true;
                case 0x40: value = intr_data_[0]; return true;
                case 0x50: value = intr_data_[1]; return true;
                case 0x60: value = intr_data_[2]; return true;
                default: break;
            }
            break;
        case asi::IMMU: value = mmu_read(false, va); return true;
        case asi::DMMU: value = mmu_read(true, va); return true;
        case asi::IMMU_TSB_8K_PTR: value = tsb_pointer(immu_, false); return true;
        case asi::IMMU_TSB_64K_PTR: value = tsb_pointer(immu_, true); return true;
        case asi::DMMU_TSB_8K_PTR: value = tsb_pointer(dmmu_, false); return true;
        case asi::DMMU_TSB_64K_PTR: value = tsb_pointer(dmmu_, true); return true;
        case asi::DMMU_TSB_DIRECT_PTR: value = tsb_pointer(dmmu_, direct_64k_); return true;
        case asi::ITLB_DATA_ACCESS: value = immu_.tlb[(va >> 3) & 63].data; return true;
        case asi::DTLB_DATA_ACCESS: value = dmmu_.tlb[(va >> 3) & 63].data; return true;
        case asi::ITLB_TAG_READ: value = immu_.tlb[(va >> 3) & 63].tag; return true;
        case asi::DTLB_TAG_READ: value = dmmu_.tlb[(va >> 3) & 63].tag; return true;
        case asi::ECACHE_TAG_DATA: value = ecache_tag_data_; return true;
        case asi::ECACHE_R: {
            // VA[40:39]: 1 selects the data array, 2 the tags (through the staging register).
            const u64 index = va & (opt_.ecache_bytes - 1);
            switch ((va >> 39) & 3) {
                case 1: value = ecache_data_[index >> 3]; return true;
                case 2:
                    ecache_tag_data_ = ecache_tag_[index >> 6];
                    value = ecache_tag_data_;
                    return true;
                default: break;
            }
            break;
        }
        case asi::DCACHE_DATA:
        case asi::DCACHE_TAG:
        case asi::ICACHE_INSTR:
        case asi::ICACHE_TAG:
        case asi::ICACHE_PRE_DECODE:
        case asi::ICACHE_NEXT_FIELD: value = diag(a, va); return true;
        default: break;
    }
    if (asi_hook && asi_hook(a, va, size, false, value)) return true;
    UV_LOG(Warn, "cpu", "read of unimplemented ASI %02x va %016llx at pc %016llx", a, static_cast<unsigned long long>(va),
           static_cast<unsigned long long>(pc_));
    data_fault(tt::kDataAccessException, va, a, kCtPrimary, static_cast<unsigned>(primary_context_), kFtIllegalAsi, false);
    return false;
}

bool UltraSparc::internal_asi_write(u8 a, u64 va, unsigned size, u64 value) {
    attention_ = true;
    switch (a) {
        case asi::LSU_CONTROL:
            lsu_control_ = value;
            flush_soft_tlb();
            return true;
        case asi::ESTATE_ERROR_EN: estate_error_en_ = value & 7; return true;
        case asi::AFSR: afsr_ &= ~value; return true;  // write one to clear
        case asi::AFAR: return true;
        case asi::UPA_CONFIG: upa_config_ = value; return true;
        case asi::INTR_RECEIVE: intr_receive_ = value & 0x20; return true;
        case asi::UDB_INTR_W:
            switch (va & 0xFF) {
                case 0x00: udb_error_[0] &= ~value; return true;
                case 0x18: udb_error_[1] &= ~value; return true;
                case 0x20: udb_control_[0] = value & 0x3FF; return true;
                case 0x38: udb_control_[1] = value & 0x3FF; return true;
                case 0x40: intr_dispatch_data_[0] = value; return true;
                case 0x50: intr_dispatch_data_[1] = value; return true;
                case 0x60: intr_dispatch_data_[2] = value; return true;
                case 0x70: return true;  // dispatch: there is no other processor to receive it
                default: break;
            }
            break;
        case asi::IMMU: mmu_write(false, va, value); return true;
        case asi::DMMU: mmu_write(true, va, value); return true;
        case asi::ITLB_DATA_IN: tlb_data_in(immu_, value); return true;
        case asi::DTLB_DATA_IN: tlb_data_in(dmmu_, value); return true;
        case asi::ITLB_DATA_ACCESS:
        case asi::DTLB_DATA_ACCESS: {
            MmuRegs& m = a == asi::ITLB_DATA_ACCESS ? immu_ : dmmu_;
            TlbEntry& e = m.tlb[(va >> 3) & 63];
            e.tag = m.tag_access;
            e.data = value;
            e.used = false;
            flush_soft_tlb();
            return true;
        }
        case asi::IMMU_DEMAP: tlb_demap(immu_, va); return true;
        case asi::DMMU_DEMAP: tlb_demap(dmmu_, va); return true;
        case asi::ECACHE_TAG_DATA: ecache_tag_data_ = value; return true;
        case asi::ECACHE_W: {
            const u64 index = va & (opt_.ecache_bytes - 1);
            switch ((va >> 39) & 3) {
                case 1: ecache_data_[index >> 3] = value; return true;
                case 2: ecache_tag_[index >> 6] = ecache_tag_data_; return true;
                default: break;
            }
            break;
        }
        case asi::DCACHE_DATA:
        case asi::DCACHE_TAG:
        case asi::ICACHE_INSTR:
        case asi::ICACHE_TAG:
        case asi::ICACHE_PRE_DECODE:
        case asi::ICACHE_NEXT_FIELD: diag(a, va) = value; return true;
        default: break;
    }
    if (asi_hook && asi_hook(a, va, size, true, value)) return true;
    UV_LOG(Warn, "cpu", "write of unimplemented ASI %02x va %016llx = %016llx at pc %016llx", a,
           static_cast<unsigned long long>(va), static_cast<unsigned long long>(value),
           static_cast<unsigned long long>(pc_));
    data_fault(tt::kDataAccessException, va, a, kCtPrimary, static_cast<unsigned>(primary_context_), kFtIllegalAsi, true);
    return false;
}

}  // namespace uv::usparc
