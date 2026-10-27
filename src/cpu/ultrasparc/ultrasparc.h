// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
// UltraSPARC IIi processor: SPARC V9 integer unit with eight register
// windows, four trap levels and the four global register sets (normal,
// alternate, MMU, interrupt); the UltraSPARC I/II MMU (64-entry I- and D-TLBs,
// TSB pointer generation); the internal ASIs; TICK, TICK_COMPARE and SOFTINT;
// and the interrupt vector receive registers. References: SPARC V9
// Architecture Manual; UltraSPARC IIi User's Manual (805-0087).
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/types.h"
#include "mem/bus.h"

namespace uv::usparc {

constexpr unsigned kWindows = 8;
constexpr unsigned kMaxTl = 4;
constexpr unsigned kTlbEntries = 64;

// Reset trap vector: virtual 0xFFFF_FFFF_F000_0000, physical 0x1FF_F000_0000.
constexpr u64 kRstvAddr = 0xFFFFFFFFF0000000ull;

namespace tt {  // trap types
constexpr unsigned kPowerOnReset = 0x001, kWatchdogReset = 0x002, kExternallyInitiatedReset = 0x003,
                   kSoftwareInitiatedReset = 0x004, kRedState = 0x005;
constexpr unsigned kInstructionAccessException = 0x008, kInstructionAccessError = 0x00A;
constexpr unsigned kIllegalInstruction = 0x010, kPrivilegedOpcode = 0x011;
constexpr unsigned kFpDisabled = 0x020, kFpExceptionIeee754 = 0x021, kFpExceptionOther = 0x022,
                   kTagOverflow = 0x023, kCleanWindow = 0x024;
constexpr unsigned kDivisionByZero = 0x028;
constexpr unsigned kDataAccessException = 0x030, kDataAccessError = 0x032, kMemAddressNotAligned = 0x034,
                   kLddfMemAddressNotAligned = 0x035, kStdfMemAddressNotAligned = 0x036,
                   kPrivilegedAction = 0x037;
constexpr unsigned kInterruptLevel1 = 0x041;
constexpr unsigned kInterruptVector = 0x060, kPaWatchpoint = 0x061, kVaWatchpoint = 0x062;
constexpr unsigned kFastInstructionAccessMmuMiss = 0x064, kFastDataAccessMmuMiss = 0x068,
                   kFastDataAccessProtection = 0x06C;
constexpr unsigned kSpillNormal = 0x080, kSpillOther = 0x0A0, kFillNormal = 0x0C0, kFillOther = 0x0E0;
constexpr unsigned kTrapInstruction = 0x100;
}  // namespace tt

namespace pstate {
constexpr u16 AG = 1u << 0, IE = 1u << 1, PRIV = 1u << 2, AM = 1u << 3, PEF = 1u << 4, RED = 1u << 5,
              MM = 3u << 6, TLE = 1u << 8, CLE = 1u << 9, MG = 1u << 10, IG = 1u << 11;
constexpr u16 kWritable = 0x0FFF;
}  // namespace pstate

namespace asi {
constexpr u8 N = 0x04, NL = 0x0C, AIUP = 0x10, AIUS = 0x11, PHYS_USE_EC = 0x14, PHYS_BYPASS_EC_E = 0x15,
             AIUPL = 0x18, AIUSL = 0x19, PHYS_USE_EC_L = 0x1C, PHYS_BYPASS_EC_E_L = 0x1D, NQUAD_LDD = 0x24,
             NQUAD_LDD_L = 0x2C;
constexpr u8 LSU_CONTROL = 0x45, DCACHE_DATA = 0x46, DCACHE_TAG = 0x47, INTR_DISPATCH_STATUS = 0x48,
             INTR_RECEIVE = 0x49, UPA_CONFIG = 0x4A, ESTATE_ERROR_EN = 0x4B, AFSR = 0x4C, AFAR = 0x4D,
             ECACHE_TAG_DATA = 0x4E;
constexpr u8 IMMU = 0x50, IMMU_TSB_8K_PTR = 0x51, IMMU_TSB_64K_PTR = 0x52, ITLB_DATA_IN = 0x54,
             ITLB_DATA_ACCESS = 0x55, ITLB_TAG_READ = 0x56, IMMU_DEMAP = 0x57;
constexpr u8 DMMU = 0x58, DMMU_TSB_8K_PTR = 0x59, DMMU_TSB_64K_PTR = 0x5A, DMMU_TSB_DIRECT_PTR = 0x5B,
             DTLB_DATA_IN = 0x5C, DTLB_DATA_ACCESS = 0x5D, DTLB_TAG_READ = 0x5E, DMMU_DEMAP = 0x5F;
constexpr u8 ICACHE_INSTR = 0x66, ICACHE_TAG = 0x67, ICACHE_PRE_DECODE = 0x6E, ICACHE_NEXT_FIELD = 0x6F;
constexpr u8 BLK_AIUP = 0x70, BLK_AIUS = 0x71, ECACHE_W = 0x76, UDB_INTR_W = 0x77, BLK_AIUPL = 0x78,
             BLK_AIUSL = 0x79, ECACHE_R = 0x7E, UDB_INTR_R = 0x7F;
constexpr u8 P = 0x80, S = 0x81, PNF = 0x82, SNF = 0x83, PL = 0x88, SL = 0x89, PNFL = 0x8A, SNFL = 0x8B;
constexpr u8 BLK_COMMIT_P = 0xE0, BLK_COMMIT_S = 0xE1, BLK_P = 0xF0, BLK_S = 0xF1, BLK_PL = 0xF8, BLK_SL = 0xF9;
}  // namespace asi

// One TLB entry: the tag (VA[63:13] and context) and the TTE data.
struct TlbEntry {
    u64 tag = 0;   // VA[63:13] << 13 | context
    u64 data = 0;  // TTE data (bit 63 valid)
    bool used = false;
};

namespace tte {
constexpr u64 V = u64{1} << 63, NFO = u64{1} << 60, IE = u64{1} << 59, L = 1u << 6, CP = 1u << 5, CV = 1u << 4,
              E = 1u << 3, P = 1u << 2, W = 1u << 1, G = 1u << 0;
constexpr u64 kPaMask = 0x000001FFFFFFE000ull;  // PA[40:13]
inline unsigned size(u64 data) { return static_cast<unsigned>(data >> 61) & 3; }
// Page offset bits for a size code: 8 KiB, 64 KiB, 512 KiB, 4 MiB.
inline unsigned page_shift(unsigned size) { return 13 + 3 * size; }
}  // namespace tte

// The MMU's software-visible registers for one side (instruction or data).
struct MmuRegs {
    u64 tsb = 0;         // TSB base, split and size
    u64 tag_access = 0;  // VA[63:13] | context of the last miss or exception
    u64 sfsr = 0;        // synchronous fault status
    std::array<TlbEntry, kTlbEntries> tlb{};
};

class UltraSparc {
public:
    struct Options {
        u64 version = 0x0017001291000407ull;  // VER: manufacturer 0x17, impl 0x12 (IIi), mask, maxtl 4, maxwin 7
        double clock_hz = 440e6;
        u32 ecache_bytes = 2u << 20;  // external (E-) cache: 256 KiB to 2 MiB, a power of two
    };

    explicit UltraSparc(Bus& bus) : UltraSparc(bus, Options{}) {}
    UltraSparc(Bus& bus, const Options& options);

    // Power-on reset: RED_state at the reset vector + 0x20, TL = MAXTL.
    void reset();
    // Externally initiated reset (XIR): RED_state at the reset vector + 0x60.
    void externally_initiated_reset() { take_trap(tt::kExternallyInitiatedReset); }
    // Executes up to `instructions` instructions (fewer if halted).
    void run(u64 instructions);
    void step();

    // --- architectural state, for machines, debuggers and tests ----------------
    u64 pc() const { return pc_; }
    u64 npc() const { return npc_; }
    void set_pc(u64 pc) {
        pc_ = pc;
        npc_ = pc + 4;
    }
    void set_pc_npc(u64 pc, u64 npc) {
        pc_ = pc;
        npc_ = npc;
    }
    // Integer register in the current window and global set.
    u64 reg(unsigned r) const { return r == 0 ? 0 : *r_[r]; }
    void set_reg(unsigned r, u64 v) {
        if (r != 0) *r_[r] = v;
    }
    u8 ccr() const { return ccr_; }
    void set_ccr(u8 v) { ccr_ = v; }
    u64 y() const { return y_; }
    u16 pstate_reg() const { return pstate_; }
    void set_pstate(u16 v);
    unsigned tl() const { return tl_; }
    void set_tl(unsigned v) { tl_ = v > kMaxTl ? kMaxTl : v; }
    unsigned cwp() const { return cwp_; }
    void set_cwp(unsigned v);
    unsigned cansave() const { return cansave_; }
    unsigned canrestore() const { return canrestore_; }
    unsigned cleanwin() const { return cleanwin_; }
    unsigned otherwin() const { return otherwin_; }
    void set_window_state(unsigned cansave, unsigned canrestore, unsigned cleanwin, unsigned otherwin,
                          unsigned wstate);
    unsigned pil() const { return pil_; }
    u64 tba() const { return tba_; }
    u64 tpc(unsigned level) const { return trap_[level].tpc; }
    u64 tnpc(unsigned level) const { return trap_[level].tnpc; }
    u64 tstate(unsigned level) const { return trap_[level].tstate; }
    unsigned trap_type(unsigned level) const { return trap_[level].tt; }
    u64 tick() const;
    u64 softint() const { return softint_; }
    u64 lsu_control() const { return lsu_control_; }
    u8 asi_reg() const { return asi_; }
    u8 fprs() const { return fprs_; }
    u64 fsr() const { return fsr_; }
    void set_fsr(u64 v) { fsr_ = v; }
    u32 freg32(unsigned r) const { return f_[r]; }
    void set_freg32(unsigned r, u32 v) { f_[r] = v; }
    u64 freg64(unsigned r) const;  // r is the 5-bit encoded double register number
    void set_freg64(unsigned r, u64 v);

    const MmuRegs& immu() const { return immu_; }
    // Debugger translation of an instruction (or data) address in the
    // current context: no traps, no TLB state changes.
    bool debug_translate(u64 va, bool data, u64& pa) const;
    const MmuRegs& dmmu() const { return dmmu_; }

    // Interrupt vector delivery (from the PCI bus module): sets the receive
    // busy bit and the three data words, if the receive register is free.
    // Returns false if it is busy (the sender must retry).
    bool deliver_interrupt_vector(u64 d0, u64 d1, u64 d2);

    u64 cycles() const { return cycles_; }
    u64 instructions() const { return instret_; }

    // Stops run() (the machine wants control, or the CPU entered error_state).
    bool halted() const { return halted_; }
    void set_halted(bool h) { halted_ = h; }
    const std::string& halt_reason() const { return halt_reason_; }

    // Debugger hook: called before each instruction executes.
    std::function<void(UltraSparc&)> trace_hook;
    // Called on every trap entry (after the state is saved), for tracing.
    std::function<void(UltraSparc&, unsigned tt)> trap_hook;
    // Device for an implementation ASI the CPU does not model itself (for
    // example the machine's diagnostics). Returns false if it does not
    // answer, which raises data_access_exception.
    std::function<bool(u8 asi, u64 va, unsigned size, bool is_write, u64& value)> asi_hook;

private:
    struct TrapLevel {
        u64 tpc = 0, tnpc = 0, tstate = 0;
        unsigned tt = 0;
    };

    // Memory access attributes for one translation.
    struct Access {
        u64 pa = 0;
        bool little = false;  // the access is little-endian (ASI, CLE or TTE.IE)
        bool side_effect = false;
    };
    enum class AccessKind : u8 { Load, Store, Atomic, Fetch };

    // execution
    void execute(u32 insn);
    void take_trap(unsigned type);
    void enter_error_state(const char* why);
    void check_interrupts();
    void update_globals();
    void update_tick_interrupt();
    unsigned window(int delta) const { return static_cast<unsigned>(static_cast<int>(cwp_) + delta + kWindows) % kWindows; }

    bool cond_icc(unsigned cond, unsigned cc4) const;
    bool cond_fcc(unsigned cond, unsigned fcc) const;
    unsigned fcc(unsigned n) const;

    // memory
    u64 mask_address(u64 va) const { return (pstate_ & pstate::AM) ? (va & 0xFFFFFFFFull) : va; }
    bool translate_fetch(u64 va, u64& pa);
    bool translate_data(u64 va, u8 asi, AccessKind kind, unsigned size, Access& out);
    TlbEntry* tlb_lookup(MmuRegs& mmu, u64 va, unsigned context);
    void data_fault(unsigned trap, u64 va, u8 asi, unsigned context_type, unsigned context, unsigned ft, bool write,
                    bool side_effect = false);
    void fetch_fault(unsigned trap, u64 va, unsigned context, unsigned ft);
    bool asi_restricted_check(u8 asi);
    bool load(u64 va, u8 asi, unsigned size, u64& value, AccessKind kind = AccessKind::Load);
    bool store(u64 va, u8 asi, unsigned size, u64 value);
    bool phys_read(const Access& a, unsigned size, u64& value);
    bool phys_write(const Access& a, unsigned size, u64 value);
    bool internal_asi_read(u8 asi, u64 va, unsigned size, u64& value);
    bool internal_asi_write(u8 asi, u64 va, unsigned size, u64 value);
    static bool asi_is_memory(u8 asi);
    static bool asi_is_little(u8 asi);
    static bool asi_is_block(u8 asi) { return (asi & 0xF6) == 0x70 || (asi & 0xF6) == 0xF0 || asi == 0xE0 || asi == 0xE1; }
    static bool asi_is_short_fp(u8 asi) { return (asi & 0xF4) == 0xD0; }
    static bool asi_is_partial(u8 asi) { return (asi & 0xF0) == 0xC0 && (asi & 7) <= 5; }
    // The plain memory ASI a block, short or partial ASI translates as.
    static u8 base_memory_asi(u8 asi);
    bool block_load(u64 va, u8 asi, unsigned rd);
    bool block_store(u64 va, u8 asi, unsigned rd);
    bool short_fp_load(u64 va, u8 asi, unsigned rd);
    bool short_fp_store(u64 va, u8 asi, unsigned rd);
    bool partial_store(u64 va, u8 asi, unsigned rd, u64 mask);
    u8 implicit_asi() const { return tl_ > 0 ? (pstate_ & pstate::CLE ? asi::NL : asi::N) : (pstate_ & pstate::CLE ? asi::PL : asi::P); }

    // MMU registers and TLBs
    u64 mmu_read(bool data_side, u64 va) const;
    void mmu_write(bool data_side, u64 va, u64 value);
    u64 tsb_pointer(const MmuRegs& mmu, bool size_64k) const;
    void tlb_data_in(MmuRegs& mmu, u64 data);
    void tlb_demap(MmuRegs& mmu, u64 va);
    void flush_soft_tlb() { ++soft_gen_; }

    // privileged and ancillary registers
    bool read_pr(unsigned r, u64& v);
    bool write_pr(unsigned r, u64 v);
    bool read_asr(unsigned r, u64& v);
    bool write_asr(unsigned r, u64 v);
    bool fp_enabled() const { return (pstate_ & pstate::PEF) && (fprs_ & 4); }

    // floating point and VIS (fpu.cpp)
    void fpop1(u32 insn);
    void fpop2(u32 insn);
    void impdep1(u32 insn);
    void fp_unimplemented();  // fp_exception_other, unimplemented_FPop (quad operations, unknown opf)
    // Ends an arithmetic FPop with the exceptions it raised: traps if any is
    // enabled in TEM (returns false: the result must not be written), else
    // accrues them.
    bool fp_finish(unsigned cexc);
    void set_f32(unsigned r, u32 v) {
        f_[r] = v;
        fprs_ |= r < 32 ? 1 : 2;
    }
    void set_f64(unsigned r, u64 v) {
        set_freg64(r, v);
        fprs_ |= (r & 1) ? 2 : 1;  // encoded bit 0 is register bit 5: f32-f62
    }

    Bus& bus_;
    Options opt_;

    u64 pc_ = 0, npc_ = 0;
    std::array<std::array<u64, 8>, 4> globals_{};      // normal, alternate, MMU, interrupt
    std::array<u64, kWindows * 16> windows_{};         // ins and locals of each window
    std::array<u64*, 32> r_{};                         // current mapping of r0..r31
    u64 zero_sink_ = 0;

    u8 ccr_ = 0, asi_ = 0, fprs_ = 0;
    u64 y_ = 0;
    u16 pstate_ = 0;
    unsigned tl_ = 0, pil_ = 0, cwp_ = 0;
    unsigned cansave_ = 0, canrestore_ = 0, cleanwin_ = 0, otherwin_ = 0, wstate_ = 0;
    std::array<TrapLevel, kMaxTl + 1> trap_{};
    u64 tba_ = 0;

    u64 tick_offset_ = 0;  // TICK = cycles_ + tick_offset_ (bits 62:0)
    bool tick_npt_ = true;
    u64 tick_cmpr_ = u64{1} << 63;
    u64 tick_match_ = ~u64{0};  // cycles_ value at which TICK reaches TICK_CMPR
    u64 softint_ = 0;
    u64 dcr_ = 0, pcr_ = 0, pic_ = 0, gsr_ = 0;

    u64 lsu_control_ = 0, estate_error_en_ = 0, afsr_ = 0, afar_ = 0, upa_config_ = 0;
    u64 intr_receive_ = 0;            // bit 5 busy
    std::array<u64, 3> intr_data_{};  // UDB interrupt receive data 0..2
    std::array<u64, 3> intr_dispatch_data_{};
    // UDB error (VA 0x00 high, 0x18 low; write one to clear) and control
    // (VA 0x20 high, 0x38 low) registers, through ASI 0x7F (read) and 0x77 (write).
    u64 udb_error_[2]{}, udb_control_[2]{};

    MmuRegs immu_, dmmu_;
    // Soft TLB: recent hits of each TLB by 8 KiB virtual page and context,
    // so most translations skip the 64-entry search. Any change to the TLBs,
    // the contexts or the LSU control register starts a new generation.
    struct SoftTlbEntry {
        u64 va_page = ~u64{0};
        unsigned context = 0;
        u32 gen = 0;
        TlbEntry* entry = nullptr;
    };
    static constexpr unsigned kSoftTlbSize = 1024;
    std::array<std::array<SoftTlbEntry, kSoftTlbSize>, 2> soft_tlb_{};
    u32 soft_gen_ = 1;
    u64 primary_context_ = 0, secondary_context_ = 0;
    u64 sfar_ = 0, va_watch_ = 0, pa_watch_ = 0;
    bool direct_64k_ = false;  // the last fast_data_access_protection was on a 64 KiB page
    std::unordered_map<u64, u64> diag_;  // L1 cache tag/data arrays seen through the diagnostic ASIs, by ASI and address
    u64& diag(u8 asi, u64 va);
    // The E-cache as the diagnostic ASIs see it: data by doubleword and a tag
    // per 64-byte line, indexed modulo its size (so larger offsets alias, as
    // on the real cache), and the tag staging register.
    std::vector<u64> ecache_data_, ecache_tag_;
    u64 ecache_tag_data_ = 0;

    std::array<u32, 64> f_{};  // f0..f63 as 32-bit halves
    u64 fsr_ = 0;

    u64 cycles_ = 0, instret_ = 0;
    bool halted_ = false;
    bool trapped_ = false;  // the current instruction trapped (its effects are discarded)
    std::string halt_reason_;
};

}  // namespace uv::usparc
