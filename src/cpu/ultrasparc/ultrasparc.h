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
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/types.h"
#include "mem/bus.h"

namespace uv::jit {
class ExecBuffer;
}

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
    ~UltraSparc();
    UltraSparc(const UltraSparc&) = delete;
    UltraSparc& operator=(const UltraSparc&) = delete;

    // Power-on reset: RED_state at the reset vector + 0x20, TL = MAXTL.
    void reset();
    // Externally initiated reset (XIR): RED_state at the reset vector + 0x60.
    void externally_initiated_reset() { take_trap(tt::kExternallyInitiatedReset); }
    // Executes up to `instructions` instructions (fewer if halted).
    void run(u64 instructions);
    void step();
    // Decoded-block cache: run() executes straight-line runs of instructions
    // fetched and translated once per block instead of once per instruction.
    // Results are identical to step(); blocks are dropped when their memory
    // is written or remapped. On by default; off while trace_hook is set.
    void set_block_cache(bool enabled) {
        block_cache_ = enabled;
        flush_blocks();
    }
    // Dynamic recompiler: blocks run often are translated to host code
    // (x86-64), with the same results as the decoded-block cache. On by
    // default where the host has one; needs the block cache.
    static bool jit_available() { return jit_compile_supported(); }
    // (`buffer`: the space for translations; when it fills, they are all
    // discarded and translation starts again.)
    static constexpr std::size_t kJitBuffer = std::size_t{64} << 20;
    void set_jit(bool enabled, std::size_t buffer = kJitBuffer);
    bool jit_enabled() const { return jit_buf_ != nullptr; }
    u64 jit_compiles() const { return jit_compiles_; }
    // Validation: every translated block that does all its work itself is
    // also run by execute() from the same state, and the CPU halts on the
    // first difference (halt_reason() says where).
    void set_jit_check(bool enabled) { jit_check_ = enabled; }

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
    u64 reg(unsigned r) const { return r == 0 ? 0 : base_[r >> 3][r & 7]; }
    void set_reg(unsigned r, u64 v) {
        if (r != 0) base_[r >> 3][r & 7] = v;
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

    // Guest time, in cycles of the processor's clock: what TICK counts and
    // every other clock of the machine follows. Each step of the processor
    // (an instruction started, or a trap taken) is charged cycles_per_step
    // of it, which the machine sets to keep guest time to the wall clock (a
    // faster host is then a faster processor, not a faster clock).
    u64 cycles() const { return time_base_ + (((cycles_ - step_base_) * cpi_fx_) >> 16); }
    u64 instructions() const { return instret_; }
    u64 steps() const { return cycles_; }
    // From now on, `cpi` cycles a step (at least 1/65536).
    void set_cycles_per_step(double cpi);
    double cycles_per_step() const { return static_cast<double>(cpi_fx_) / 65536.0; }
    // Steps until guest time reaches `cycles` (at least 1).
    u64 steps_until(u64 cycles) const;
    // Delay loops run at once (see jit_loops.h) are charged the time they
    // take the real CPU, `cpi` being its charge for an instruction at one a
    // cycle (0, the default: they are charged as any other steps). The IIi
    // runs an iteration, a branch to itself and its delay slot, a cycle
    // (OBP counts clock-frequency / 1000 of them for a millisecond), so a
    // step costs half that. With a charge other than the usual one, run()
    // ends after such a loop, since its count of steps was worked out at
    // the usual rate.
    void set_delay_cycles_per_step(double cpi);
    // The steps of delay loops run at once (counted and timed), and the
    // guest time charged for them (the pacer leaves them out of the host's
    // speed).
    u64 delay_steps() const { return delay_steps_; }
    u64 delay_cycles() const { return delay_time_fx_ >> 16; }

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

    // Decoded blocks, by the host address of their first instruction. A
    // block lies within one 4 KiB code page (and so within one I-MMU page).
    static constexpr unsigned kBlockBits = 16;
    static constexpr unsigned kMaxBlockLength = 32;
    // Translated block: runs the block from its first instruction at `pc`,
    // and may go on into other translated blocks of the same page while
    // their instructions fit in `budget`; returns how many instructions it
    // started (see jit_x64.cpp).
    using JitFn = u32 (*)(UltraSparc* cpu, u64 pc, u64 budget);
    struct Block {
        const u8* host = nullptr;
        unsigned length = 0;
        JitFn code = nullptr;
        u16 runs = 0;
        u8 jit_mode = 0;        // jit_mode() the code was translated for
        bool jit_self = false;  // the code never calls execute()
        u32 loop = 0;           // a delay loop (CountedLoop::code(), TimedLoop::code(), or the last
                                // block of a polling loop: 3 << 30 | how far back the loop starts
                                // in instructions << 16 | its block index; 0: none)
        u16 page_off = 0;       // where it starts in its 4 KiB code page, in instructions
        std::array<u32, kMaxBlockLength> insns{};
    };
    static constexpr u16 kJitThreshold = 16;  // runs before a block is translated
    bool jit_compile(Block& b);
    static bool jit_compile_supported();
    // An instruction the translated code does not do itself; true if the
    // block must end after it (trap, attention or halt).
    static bool jit_execute(UltraSparc* cpu, u32 insn);
    // The back edge of a delay loop (Block::loop; see jit_loops.h), with
    // `room` instructions left that may start and `next` the step the next
    // would be: runs all but the last of the iterations that fit at once,
    // and returns how many instructions that was (0 if none). A translation
    // leaves after one charged other than the usual rate (delay_charged_).
    u64 loop_at_once(u32 loop, u64 room, u64 next);
    u64 count_down(u32 loop, u64 room);
    u64 wait_for_tick(u32 loop, u64 room, u64 next);
    u64 cycle_at_once(const Block& b, unsigned back, u64 room, u64 next);
    // TICK as an instruction at `step` reads it.
    u64 tick_at_step(u64 step) const;
    // A polling loop's registers and condition codes at its last back edge
    // (`next`: the step after it), to see whether the next iteration
    // changed them.
    struct PollSnapshot {
        const u8* host = nullptr;
        u64 next = 0;
        u8 ccr = 0;
        unsigned count = 0;
        std::array<u64, 16> regs{};
    };
    PollSnapshot poll_snap_;
    static u64 jit_loop_at_once(UltraSparc* cpu, u64 loop, u64 room, u64 next) {
        return cpu->loop_at_once(static_cast<u32>(loop), room, next);
    }
    std::unique_ptr<jit::ExecBuffer> jit_buf_;
    std::size_t jit_link_offset_ = 0;  // where blocks going on from another enter every translation
    // The code translations call to run an instruction through execute()
    // (made again whenever the buffer is emptied; jit_x64.cpp): with
    // npc = pc + 4, and with npc in a register.
    const void* jit_stub_seq_ = nullptr;
    const void* jit_stub_dyn_ = nullptr;
    bool jit_make_stubs();
    u64 jit_compiles_ = 0;
    bool jit_check_ = false;
    u32 jit_run_checked(Block& b);
    void run_blocks(u64 instructions);
    void decode_block(Block& b, const u8* host, u64 pa);
    // Recent fetch translations, by 8 KiB virtual page: valid while the soft
    // TLB generation and the state that selects the translation (RED_state,
    // privilege, TL > 0 for the nucleus context) are unchanged. `host` is
    // the page's host memory, or null if it is not host-backed.
    struct FetchEntry {
        u64 va_page = ~u64{0};
        u32 gen = 0;
        u16 mode = 0;
        const u8* host = nullptr;
        u64 pa = 0;
    };
    static constexpr unsigned kFetchEntries = 256;
    std::array<FetchEntry, kFetchEntries> fetch_cache_{};
    u16 fetch_mode() const { return static_cast<u16>((pstate_ & (pstate::RED | pstate::PRIV)) | (tl_ > 0 ? 0x8000 : 0)); }
    static unsigned block_index(const u8* host) {
        return static_cast<unsigned>(reinterpret_cast<uintptr_t>(host) >> 2) & ((1u << kBlockBits) - 1);
    }
    void code_written(const u8* page);
    void flush_blocks();

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
    // Internal registers whose reads never fault and change nothing (the
    // MMU and LSU registers, TSB pointers, TLB reads, error and interrupt
    // status): translated code reads them through jit_internal_read().
    static bool internal_read_is_plain(u8 asi);
    static u64 jit_internal_read(UltraSparc* cpu, u64 asi, u64 va);
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
    // Before a valid TLB entry is replaced or demapped: drops what was made
    // from it (its soft TLB entries, and the fast data translations or
    // fetch cache entries in its virtual range).
    void forget_translations(MmuRegs& mmu, const TlbEntry& e);
    void flush_soft_tlb() {
        // (The fast tables' keys hold the low 13 bits of the generation,
        // never 0: when those wrap, the tables are emptied.)
        if ((++soft_gen_ & 0x1FFF) == 0) {
            ++soft_gen_;
            for (auto& table : fast_) table.fill(FastEntry{});
        }
        fast_key_ = u64{soft_gen_ & 0x1FFF} << 51;
        attention_ = true;
    }
    // Fast data translations for translated code's loads and stores, by
    // 8 KiB virtual page, one table for each way a memory ASI translates
    // (fast_class()). An entry is made by a load or store execute()
    // completed to host memory, so it allows only what translate_data()
    // allowed; pages whose TTE.IE reverses the byte order get none.
    //
    // An entry for a page that is not host memory (a device) instead holds
    // its translation for load() and store(), which then need not
    // translate again: addend is the physical address - va, and the perms
    // are kFastDevRead and kFastDevWrite (kFastDevSide: side effects),
    // which translated code does not take for its own.
    struct FastEntry {
        u64 key = 0;               // (va >> 13) | fast_key_
        s64 addend = 0;            // host address - va
        const u8* code = nullptr;  // the code flags of the page's two 4 KiB halves
        u8 perms = 0;              // kFastRead, kFastWrite, kFastAtomic: what execute() has done there
    };
    static constexpr u8 kFastRead = 1, kFastWrite = 2, kFastAtomic = 4;
    static constexpr u8 kFastDevRead = 8, kFastDevWrite = 16, kFastDevSide = 32, kFastDev = kFastDevRead | kFastDevWrite;
    static constexpr unsigned kFastEntries = 512, kFastClasses = 6;
    std::array<std::array<FastEntry, kFastEntries>, kFastClasses> fast_{};
    u64 fast_key_ = u64{1} << 51;
    // The table for a memory ASI used now: 0 primary context with user
    // permissions (P, PL in user code; AIUP, AIUPL), 1 primary with
    // privileged (P, PL in privileged code), 2 nucleus (N, NL), 3 secondary
    // privileged (S, SL), 4 secondary user (AIUS, AIUSL; S, SL in user
    // code), 5 physical (the PHYS_* ASIs: virtual address = physical); -1
    // none (other ASIs, and the restricted ones in user code). (No-fault
    // loads translate as P or S but make no entries: on a no-fault-only
    // page they succeed where P and S fault.)
    int fast_class(u8 asi) const {
        const bool priv = pstate_ & pstate::PRIV;
        switch (asi) {
            case asi::P:
            case asi::PL: return priv ? 1 : 0;
            case asi::N:
            case asi::NL: return priv ? 2 : -1;
            case asi::AIUP:
            case asi::AIUPL: return priv ? 0 : -1;
            case asi::S:
            case asi::SL: return priv ? 3 : 4;
            case asi::AIUS:
            case asi::AIUSL: return priv ? 4 : -1;
            case asi::PHYS_USE_EC:
            case asi::PHYS_BYPASS_EC_E:
            case asi::PHYS_USE_EC_L:
            case asi::PHYS_BYPASS_EC_E_L: return priv ? 5 : -1;
            case asi::NQUAD_LDD:
            case asi::NQUAD_LDD_L: return priv ? 2 : -1;  // (the quad loads, as N)
            default: return -1;
        }
    }
    // 0 user, 1 privileged, 2 nucleus (TL > 0); 3: no fast path (little-endian
    // accesses with PSTATE.CLE, or TL > 0 without privilege).
    unsigned fast_mode() const {
        if (pstate_ & pstate::CLE) return 3;
        if (tl_ > 0) return (pstate_ & pstate::PRIV) ? 2 : 3;
        return (pstate_ & pstate::PRIV) ? 1 : 0;
    }
    void fast_fill(u64 va, u8 asi, const Access& acc, AccessKind kind);
    // The device entries (see FastEntry): a translation already made for a
    // load or store, and the entry made after one.
    bool fast_device(u64 va, u8 asi, AccessKind kind, Access& acc) const;
    void fast_fill_device(u64 va, u8 asi, const Access& acc, AccessKind kind);
    // What a translation depends on: PSTATE.AM, fast_mode() and fetch_mode().
    unsigned jit_mode() const {
        return ((pstate_ & pstate::AM) ? 1 : 0) | fast_mode() << 1 | ((pstate_ & pstate::RED) ? 8 : 0) |
               ((pstate_ & pstate::PRIV) ? 16 : 0) | (tl_ > 0 ? 32 : 0);
    }

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
    // Where r0-r31 are, eight at a time: the current global set, then the
    // current window's outs, locals and ins (the locals follow the ins).
    std::array<u64*, 4> base_{};

    u8 ccr_ = 0, asi_ = 0, fprs_ = 0;
    u64 y_ = 0;
    u16 pstate_ = 0;
    unsigned tl_ = 0, pil_ = 0, cwp_ = 0;
    unsigned cansave_ = 0, canrestore_ = 0, cleanwin_ = 0, otherwin_ = 0, wstate_ = 0;
    std::array<TrapLevel, kMaxTl + 1> trap_{};
    u64 tba_ = 0;

    u64 tick_offset_ = 0;  // TICK = cycles() + tick_offset_ (bits 62:0)
    bool tick_npt_ = true;
    u64 tick_cmpr_ = u64{1} << 63;
    u64 tick_match_ = ~u64{0};  // the step (cycles_ value) at which TICK reaches TICK_CMPR
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
    // so most translations skip the 64-entry search. A TLB entry replaced or
    // demapped drops what was made from it (forget_translations()); other
    // changes to the TLBs, the contexts or the LSU control register start a
    // new generation.
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

    u64 cycles_ = 0, instret_ = 0;  // steps, and instructions retired
    // Guest time = time_base_ + (cycles_ - step_base_) * cpi_fx_ / 65536.
    u64 time_base_ = 0, step_base_ = 0, cpi_fx_ = 65536;
    // Delay loops run at once: the charge for a step (16.16; 0: the usual
    // one), the part of a cycle charged apart from the usual charge and not
    // yet in time_base_, the steps run and the time charged for them (16.16).
    u64 delay_fx_ = 0, delay_frac_ = 0, delay_steps_ = 0, delay_time_fx_ = 0;
    bool delay_charged_ = false;  // a delay loop was charged other than the usual rate
    bool halted_ = false;
    bool trapped_ = false;  // the current instruction trapped (its effects are discarded)
    // The current instruction changed state that run_blocks() would
    // otherwise only look at between blocks: the interrupt conditions, the
    // fetch translation, or the code being run.
    bool attention_ = false;
    bool block_cache_ = true;
    std::vector<Block> blocks_;
    std::string halt_reason_;
};

}  // namespace uv::usparc
