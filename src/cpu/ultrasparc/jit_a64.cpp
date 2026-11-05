// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
// UltraSPARC IIi dynamic recompiler, AArch64 back end: the same translation
// as the x86-64 one (jit_x64.cpp, which describes it), for AArch64 hosts.
//
// Guest registers are reached through three host registers holding the
// current global set, the current window's outs, and its ins (its locals
// follow them); they are loaded again after every call to execute(), which
// may change the window or the global set. The guest registers a block uses
// most are kept in host registers through its translation, condition codes
// are left lazy, and translations go straight into the next one they found
// before, all as in jit_x64.cpp.
#include "cpu/ultrasparc/ultrasparc.h"

#if defined(__aarch64__) || defined(_M_ARM64)

#include <array>
#include <cstddef>
#include <optional>
#include <vector>

#include "cpu/ultrasparc/vis.h"
#include "jit/a64_emitter.h"
#include "jit/exec_buffer.h"

namespace uv::usparc {

namespace {

using namespace uv::jit::a64;
using Site = Emitter::Site;

// Live across the whole function (callee-saved).
constexpr Reg kCpu = x19;  // UltraSparc*
constexpr Reg kPc = x20;   // pc of the block's first instruction
constexpr Reg kNpc = x21;  // npc chosen by a control transfer
constexpr Reg kG = x22;    // the current global set: %g0-%g7
constexpr Reg kO = x23;    // the current window's outs: %o0-%o7
constexpr Reg kI = x24;    // the current window's ins, then its locals

// Guest registers kept in host registers: x25-x28 (callee-saved, so kept
// across calls to C) and x12-x15 (loaded again after them).
constexpr Reg kCache[] = {x25, x26, x27, x28, x12, x13, x14, x15};
constexpr unsigned kCacheRegs = sizeof(kCache) / sizeof(kCache[0]), kCacheSaved = 4;

// What is left of the budget, as in the frame's kLeft (below) and changed
// with it: loaded on entering a translation and again after calls to C.
constexpr Reg kLeftReg = x11;

constexpr s64 sext(u64 v, unsigned bits) {
    const u64 m = u64{1} << (bits - 1);
    v &= (m << 1) - 1;
    return static_cast<s64>((v ^ m) - m);
}

// A delayed control transfer: the next instruction runs before the target.
bool delayed_cti(u32 insn) {
    const unsigned op = insn >> 30, op2 = (insn >> 22) & 7, op3 = (insn >> 19) & 0x3F;
    return op == 1 || (op == 0 && op2 != 0 && op2 != 4 && op2 != 7) || (op == 2 && (op3 == 0x38 || op3 == 0x39));
}

// The frame of a translation (from sp): x29 and x30, x19-x24, then cycles_
// and instret_ as they were on entry, what is left of the budget once the
// blocks before the current one (when one went on into another) are taken
// from it, the budget given on entry, the host's FPCR around an FPop, and
// x25-x28.
constexpr s32 kCycles0 = 64, kInstret0 = 72, kLeft = 80, kBudget = 88, kFpcr = 96;
constexpr s32 kFrame = 144;

void prologue(Emitter& e) {
    e.stp_pre(x29, x30, sp, -kFrame);
    e.stp(x19, x20, sp, 16);
    e.stp(x21, x22, sp, 32);
    e.stp(x23, x24, sp, 48);
    e.stp(x25, x26, sp, 112);
    e.stp(x27, x28, sp, 128);
}
void epilogue(Emitter& e) {  // (sp at the frame)
    e.ldp(x27, x28, sp, 128);
    e.ldp(x25, x26, sp, 112);
    e.ldp(x23, x24, sp, 48);
    e.ldp(x21, x22, sp, 32);
    e.ldp(x19, x20, sp, 16);
    e.ldp_post(x29, x30, sp, kFrame);
    e.ret();
}

// FPSR's cumulative exception flags (IOC, DZC, OFC, UFC, IXC: bits 0-4) as
// SPARC's cexc bits (NV 0x10, DZ 0x02, OF 0x08, UF 0x04, NX 0x01).
struct ExcTable {
    u8 v[32];
};
constexpr ExcTable make_exc_table() {
    ExcTable t{};
    for (unsigned f = 0; f < 32; ++f)
        t.v[f] = static_cast<u8>(((f & 1) ? 0x10 : 0) | ((f & 2) ? 0x02 : 0) | ((f & 4) ? 0x08 : 0) | ((f & 8) ? 0x04 : 0) |
                                 ((f & 16) ? 0x01 : 0));
    return t;
}
constexpr ExcTable kFpsrExc = make_exc_table();

// AArch64 condition for BPr/MOVr's rcond after `cmp r, #0` (0 and 4 are reserved).
constexpr Cond kRcond[8] = {Cond::Al, Cond::Eq, Cond::Le, Cond::Lt, Cond::Al, Cond::Ne, Cond::Gt, Cond::Ge};
bool rcond_valid(unsigned rcond) { return (rcond & 3) != 0; }

// CCR from the flags (NZCV >> 28) of a 32-bit operation in bits 3:0 of the
// index and of a 64-bit one in bits 7:4. SPARC's C after a subtraction is a
// borrow, the inverse of AArch64's.
constexpr std::array<u8, 256> make_ccr(bool sub) {
    std::array<u8, 256> t{};
    for (unsigned i = 0; i < 256; ++i) {
        auto nib = [sub](unsigned f) {  // N Z C V -> N Z V C
            const unsigned c = ((f >> 1) & 1) ^ (sub ? 1 : 0);
            return (f & 0xC) | ((f & 1) << 1) | c;
        };
        t[i] = static_cast<u8>(nib(i & 15) | (nib(i >> 4) << 4));
    }
    return t;
}
constexpr std::array<u8, 256> kCcrAdd = make_ccr(false), kCcrSub = make_ccr(true);

// Each icc/xcc condition as the host's after a subtraction (SPARC's C the
// inverse of AArch64's) and after an addition or a logical operation (the
// same C); Al where there is none (leu and gu after an addition).
constexpr Cond kIccSub[16] = {Cond::Al, Cond::Eq, Cond::Le, Cond::Lt, Cond::Ls, Cond::Lo, Cond::Mi, Cond::Vs,
                              Cond::Al, Cond::Ne, Cond::Gt, Cond::Ge, Cond::Hi, Cond::Hs, Cond::Pl, Cond::Vc};
constexpr Cond kIccAdd[16] = {Cond::Al, Cond::Eq, Cond::Le, Cond::Lt, Cond::Al, Cond::Hs, Cond::Mi, Cond::Vs,
                              Cond::Al, Cond::Ne, Cond::Gt, Cond::Ge, Cond::Al, Cond::Lo, Cond::Pl, Cond::Vc};

}  // namespace

bool UltraSparc::jit_compile_supported() { return true; }

// The code a translation calls (x9 = the instruction's index in the block,
// w10 = the instruction) to run an instruction through execute() with pc
// and npc in place and the counts as run_blocks() would have them. It
// returns to the translation, with the window and global set loaded again,
// unless run_blocks() would end the block there (a trap, attention_,
// halted_, or, outside a delay slot, a pc other than the next
// instruction's; after a delay slot, the translation chains to the next pc
// itself); then it leaves the translation itself, returning the
// instructions started.
bool UltraSparc::jit_make_stubs() {
    Emitter e;
    auto off = [this](const auto& member) {
        return static_cast<s64>(reinterpret_cast<const u8*>(&member) - reinterpret_cast<const u8*>(this));
    };
    constexpr s32 kUp = 16;  // to the translation's frame, past the saved x30 and index
    const std::size_t ccr = 0;  // (the code for the condition codes, below, comes first)
    // After execute(): back into the translation, unless the block ends
    // there. After a delay slot (`sequential` false) the translation itself
    // goes on to the next pc (a delay slot ends its block); after any other
    // instruction it goes on to the next one, so the pc must be that.
    auto body = [&](bool sequential) {
        e.stp_pre(x30, x9, sp, -16);
        e.bl_to(ccr);  // the condition codes, for execute()
        e.ldr(x1, sp, kUp + kBudget);  // (the instructions started before)
        e.ldr(x0, sp, kUp + kLeft);
        e.sub(x1, x1, x0);
        e.add(x1, x1, x9);
        e.ldr(x0, sp, kUp + kCycles0);
        e.add(x0, x0, x1);
        e.str(x0, kCpu, off(cycles_));
        e.ldr(x0, sp, kUp + kInstret0);
        e.add(x0, x0, x1);
        e.str(x0, kCpu, off(instret_));
        e.mov(x0, kCpu);
        e.mov(x1, x10, false);
        e.load_constant(x16, reinterpret_cast<u64>(&UltraSparc::jit_execute));
        e.blr(x16);
        e.and_imm(x0, x0, 0xFF, false);  // (a bool return value is only the low byte)
        std::vector<Site> out;
        out.push_back(e.cbnz(x0, false));
        if (sequential) {
            e.ldr(x9, sp, 8);
            e.add(x0, kPc, x9, true, Shift::Lsl, 2);
            e.add_imm(x0, x0, 4);
            e.ldr(x1, kCpu, off(pc_));
            e.cmp(x0, x1);
            out.push_back(e.b(Cond::Ne));
        }
        e.ldr(kG, kCpu, off(base_[0]));
        e.ldr(kO, kCpu, off(base_[1]));
        e.ldr(kI, kCpu, off(base_[3]));
        e.ldr(kLeftReg, sp, kUp + kLeft);
        e.ldp_post(x30, x9, sp, 16);
        e.ret();
        for (const Site& site : out) e.bind(site);
        e.ldr(x9, sp, 8);
        e.ldr(x0, sp, kUp + kBudget);
        e.ldr(x1, sp, kUp + kLeft);
        e.sub(x0, x0, x1);
        e.add(x0, x0, x9);
        e.add_imm(x0, x0, 1);
        e.add_imm(sp, sp, kUp);
        epilogue(e);
    };
    // ccr_ from the lazy condition codes, if any (see cc_op_), with only
    // x16, x17 and the flags changed (at ccr).
    {
        e.ldr(x16, kCpu, off(cc_op_), 1);
        const Site none = e.cbz(x16, false);
        e.stp_pre(x0, x1, sp, -32);
        e.stp(x2, x3, sp, 16);
        e.ldr(x0, kCpu, off(cc_a_));
        e.ldr(x1, kCpu, off(cc_b_));
        e.cmp_imm(x16, kCcLogic, false);
        const Site logic = e.b(Cond::Eq);
        e.cmp_imm(x16, kCcSub, false);
        const Site add = e.b(Cond::Ne);
        std::vector<Site> to_store;
        // (as ccr_into in jit_compile)
        auto codes = [&](bool sub) {
            e.lsr(x2, x2, 28, false);
            e.lsr(x3, x3, 28, false);
            e.orr(x2, x2, x3, false, Shift::Lsl, 4);
            e.load_constant(x16, reinterpret_cast<u64>(sub ? kCcrSub.data() : kCcrAdd.data()));
            e.ldr_index(x3, x16, x2, 1);
            to_store.push_back(e.b());
        };
        e.subs(x2, x0, x1, false);
        e.mrs_nzcv(x2);
        e.subs(x3, x0, x1);
        e.mrs_nzcv(x3);
        codes(true);
        e.bind(add);
        e.adds(x2, x0, x1, false);
        e.mrs_nzcv(x2);
        e.adds(x3, x0, x1);
        e.mrs_nzcv(x3);
        codes(false);
        e.bind(logic);
        e.tst(x0, x0, false);
        e.mrs_nzcv(x2);
        e.tst(x0, x0);
        e.mrs_nzcv(x3);
        codes(false);
        for (const Site& site : to_store) e.bind(site);
        e.str(x3, kCpu, off(ccr_), 1);
        e.str(xzr, kCpu, off(cc_op_), 1);
        e.ldp(x2, x3, sp, 16);
        e.ldp_post(x0, x1, sp, 32);
        e.bind(none);
        e.ret();
    }
    const std::size_t dyn = e.size();  // npc in kNpc
    e.add(x0, kPc, x9, true, Shift::Lsl, 2);
    e.str(x0, kCpu, off(pc_));
    e.str(kNpc, kCpu, off(npc_));
    body(false);
    const std::size_t seq = e.size();  // npc = pc + 4
    e.add(x0, kPc, x9, true, Shift::Lsl, 2);
    e.str(x0, kCpu, off(pc_));
    e.add_imm(x0, x0, 4);
    e.str(x0, kCpu, off(npc_));
    body(true);
    // Out of the translation with pc_ = x2 and npc_ = pc_ + 4 (at out), or
    // first (at link) on into another translation, as in jit_x64.cpp: x3 =
    // the block's host address, x5 = the first slot of its set, x4 = the
    // mode, x9 = where to note it (or 0).
    static_assert(sizeof(JitLink) == 16 && offsetof(JitLink, gen) == 0 && offsetof(JitLink, target) == 8);
    const std::size_t link = e.size();
    {
        std::vector<Site> hit, no;
        for (unsigned w = 0; w < kBlockWays; ++w) {
            const s64 d = static_cast<s64>(w * sizeof(Block));
            e.ldr(x6, x5, d + static_cast<s64>(offsetof(Block, host)));
            e.cmp(x6, x3);
            const Site next = e.b(Cond::Ne);
            if (d) e.add_imm(x5, x5, d);
            hit.push_back(e.b());
            e.bind(next);
        }
        no.push_back(e.b());
        for (const Site& site : hit) e.bind(site);
        e.ldr(x8, x5, static_cast<s64>(offsetof(Block, code)));
        no.push_back(e.cbz(x8));
        e.ldr(x6, x5, static_cast<s64>(offsetof(Block, jit_mode)), 1);
        e.cmp(x6, x4, false);
        no.push_back(e.b(Cond::Ne));
        e.ldr(x6, kCpu, off(jit_link_offset_));
        e.add(x8, x8, x6);
        const Site no_link = e.cbz(x9);
        e.ldr(x6, x5, static_cast<s64>(offsetof(Block, loop)), 4);
        const Site loop = e.cbnz(x6, false);  // (not into a delay loop, which looks up its block from C++)
        e.ldr(x6, kCpu, off(code_gen_), 4);
        e.str(x6, x9, 0, 4);
        e.str(x8, x9, 8);
        e.bind(no_link);
        e.bind(loop);
        e.mov(kPc, x2);
        e.br(x8);
        for (const Site& site : no) e.bind(site);
    }
    const std::size_t out = e.size();
    e.str(x2, kCpu, off(pc_));
    e.add_imm(x0, x2, 4);
    e.str(x0, kCpu, off(npc_));
    e.ldr(x0, sp, kBudget);  // the instructions started
    e.ldr(x1, sp, kLeft);
    e.sub(x0, x0, x1);
    e.bl_to(ccr);  // the codes, for what follows in C++
    epilogue(e);
    e.finish_literals();
    const auto* code = static_cast<const u8*>(jit_buf_->add(e.code().data(), e.size()));
    if (!code) return false;
    jit_stub_dyn_ = code + dyn;
    jit_stub_seq_ = code + seq;
    jit_stub_ccr_ = code + ccr;
    jit_stub_link_ = code + link;
    jit_stub_out_ = code + out;
    return true;
}

int UltraSparc::jit_compile_pass(Block& b, const unsigned* label_dirty, unsigned* edge_dirty) {
    if (!jit_stub_dyn_ && !jit_make_stubs()) return false;
    const u8* const base = jit_buf_->next();  // where this code will be (calls to the stubs are relative)
    Emitter e;
    auto off = [this](const auto& member) {
        return static_cast<s64>(reinterpret_cast<const u8*>(&member) - reinterpret_cast<const u8*>(this));
    };
    const s64 o_pc = off(pc_), o_npc = off(npc_), o_ccr = off(ccr_);
    const bool am = pstate_ & pstate::AM;

    // cond_icc() as a table: bit n of icc_table[cond] says whether cond
    // holds when the condition-code nibble is n.
    u16 icc_table[16] = {};
    for (unsigned c = 0; c < 16; ++c)
        for (unsigned nib = 0; nib < 16; ++nib)
            if (cond_icc(c, nib)) icc_table[c] |= static_cast<u16>(1u << nib);

    struct RegMem {
        Reg base;
        s64 off;
    };
    auto reg_mem = [&](unsigned r) -> RegMem {
        if (r < 8) return {kG, 8 * static_cast<s64>(r)};
        if (r < 16) return {kO, 8 * static_cast<s64>(r - 8)};
        if (r < 24) return {kI, 64 + 8 * static_cast<s64>(r - 16)};
        return {kI, 8 * static_cast<s64>(r - 24)};
    };
    // The registers kept in kCache (see jit_cache_choice()), and what the
    // code being made has in it, as in jit_x64.cpp: the registers that hold
    // their guest register's value, and those changed since it was in
    // memory (as the instruction being made began, too, for its paths to
    // execute()). A block that goes back to its own start, or blocks
    // translated together, load all of them first and write all of them
    // out; each of the blocks starts with them all.
    //
    // The blocks translated together (see jit_region()): b, then any that
    // go round a loop with it, each at its offset from b's pc; the one
    // being made, its offset and its length.
    const Block* region[kRegionBlocks] = {&b};
    s64 region_off[kRegionBlocks] = {0};
    const unsigned regions = jit_check_ ? 1 : jit_region(b, region, region_off, kRegionBlocks);
    const std::size_t links_before = jit_links_used_;
    const Block* cur = &b;
    s64 cur_off = 0;
    unsigned n = b.length;
    int slot_of[32];
    u8 cached[kCacheRegs];
    bool cache_load[kCacheRegs];
    const unsigned cache_used = jit_cache_choice(region, regions, kCacheRegs, cached, cache_load);
    for (int& slot : slot_of) slot = -1;
    for (unsigned slot = 0; slot < cache_used; ++slot) slot_of[cached[slot]] = static_cast<int>(slot);
    auto cache_reg = [&](unsigned r) { return kCache[slot_of[r]]; };
    bool self_loop = regions > 1;
    for (unsigned i = 0; i < b.length && !self_loop; ++i) {
        const u32 insn = b.insns[i];
        const unsigned op = insn >> 30, op2 = (insn >> 22) & 7;
        const s64 here = 4 * static_cast<s64>(i);
        if (op == 1) self_loop = here + (sext(insn, 30) << 2) == 0;
        else if (op == 0 && (op2 == 1 || op2 == 5)) self_loop = here + (sext(insn, 19) << 2) == 0;
        else if (op == 0 && (op2 == 2 || op2 == 6)) self_loop = here + (sext(insn, 22) << 2) == 0;
        else if (op == 0 && op2 == 3) self_loop = here + (sext((((insn >> 20) & 3) << 14) | (insn & 0x3FFF), 16) << 2) == 0;
    }
    const unsigned cache_all = (1u << cache_used) - 1;
    unsigned cache_valid = 0, cache_dirty = 0, pre_dirty = 0;
    auto slot_bit = [&](unsigned r) { return 1u << slot_of[r]; };
    // The changed registers to memory (`which` of them: by default all that
    // changed), and registers from memory (all, the window's, or those a
    // call to C clobbers).
    enum class Reload { All, Window, Call };
    auto in_scope = [&](unsigned slot, Reload scope) {
        return scope == Reload::All || (scope == Reload::Window && cached[slot] >= 8) || (scope == Reload::Call && slot >= kCacheSaved);
    };
    auto flush_cache = [&](Reload scope = Reload::All, unsigned which = ~0u) {
        if (which == ~0u) which = cache_dirty;
        for (unsigned slot = 0; slot < cache_used; ++slot) {
            if (!(which >> slot & 1) || !in_scope(slot, scope)) continue;
            const RegMem m = reg_mem(cached[slot]);
            e.str(kCache[slot], m.base, m.off);
        }
    };
    auto reload_cache = [&](Reload scope = Reload::All) {
        for (unsigned slot = 0; slot < cache_used; ++slot) {
            if (!in_scope(slot, scope)) continue;
            const RegMem m = reg_mem(cached[slot]);
            e.ldr(kCache[slot], m.base, m.off);
            cache_valid |= 1u << slot;
            cache_dirty &= ~(1u << slot);
        }
    };
    // A cached register read before the code has it, which the choice of
    // registers rules out: the translation is not used.
    bool cache_miss = false;
    auto have = [&](unsigned r) {
        if (cache_valid & slot_bit(r)) return;
        cache_miss = true;
        const RegMem m = reg_mem(r);
        e.ldr(cache_reg(r), m.base, m.off);
        cache_valid |= slot_bit(r);
    };
    auto changed = [&](unsigned r) { cache_valid |= slot_bit(r), cache_dirty |= slot_bit(r); };
    auto load_reg = [&](Reg dst, unsigned r) {
        if (r == 0) {
            e.mov(dst, xzr);
        } else if (slot_of[r] >= 0) {
            have(r);
            if (dst != cache_reg(r)) e.mov(dst, cache_reg(r));
        } else {
            const RegMem m = reg_mem(r);
            e.ldr(dst, m.base, m.off);
        }
    };
    auto store_reg = [&](unsigned r, Reg src) {
        if (r == 0) return;
        if (slot_of[r] < 0) {
            const RegMem m = reg_mem(r);
            e.str(src, m.base, m.off);
            return;
        }
        if (src != cache_reg(r)) e.mov(cache_reg(r), src);
        changed(r);
    };
    // A guest register as an operand: its cache register (which must not be
    // changed), or loaded into `scratch`.
    auto use_reg = [&](unsigned r, Reg scratch) -> Reg {
        if (r != 0 && slot_of[r] >= 0) return have(r), cache_reg(r);
        load_reg(scratch, r);
        return scratch;
    };
    // Where to make a result for rd: its cache register, or `scratch`, which
    // put_reg() then stores.
    auto def_reg = [&](unsigned rd, Reg scratch) -> Reg { return rd != 0 && slot_of[rd] >= 0 ? cache_reg(rd) : scratch; };
    auto put_reg = [&](unsigned rd, Reg r) {
        if (rd == 0) return;
        if (slot_of[rd] >= 0) {
            changed(rd);
            return;
        }
        const RegMem m = reg_mem(rd);
        e.str(r, m.base, m.off);
    };
    // One more of the CPU's effects_ (stores and TICK reads), through `tmp`.
    auto count_effect = [&](Reg tmp) {
        e.ldr(tmp, kCpu, off(effects_));
        e.add_imm(tmp, tmp, 1);
        e.str(tmp, kCpu, off(effects_));
    };
    // Operand 2: simm13 or rs2.
    auto load_op2 = [&](Reg dst, u32 insn) {
        if ((insn >> 13) & 1) e.mov_imm(dst, static_cast<u64>(sext(insn, 13)));
        else load_reg(dst, insn & 31);
    };
    auto load_bases = [&] {
        e.ldr(kG, kCpu, off(base_[0]));
        e.ldr(kO, kCpu, off(base_[1]));
        e.ldr(kI, kCpu, off(base_[3]));
    };
    // r = the pc of the block's first instruction + o.
    auto pc_plus = [&](Reg r, s64 o) {
        if (o) e.add_imm(r, kPc, o);
        else if (r != kPc) e.mov(r, kPc);
    };
    auto mask = [&](Reg r) {
        if (am) e.mov(r, r, false);
    };

    // r = the instructions started in the blocks before this one.
    auto total_into = [&](Reg r) {
        e.ldr(r, sp, kBudget);
        e.sub(r, r, kLeftReg);
    };
    std::vector<Site> exits;
    auto leave = [&](u32 started) {  // with this block's first `started` instructions started
        flush_cache();
        total_into(x0);
        e.add_imm(x0, x0, started);
        exits.push_back(e.b());
    };
    auto store_pc_npc = [&](s64 o) {  // pc_ = block pc + o, npc_ = pc_ + 4
        pc_plus(x0, o);
        e.str(x0, kCpu, o_pc);
        e.add_imm(x0, x0, 4);
        e.str(x0, kCpu, o_npc);
    };
    auto store_pc_npc_from = [&](Reg r) {  // pc_ = r, npc_ = r + 4
        e.str(r, kCpu, o_pc);
        e.add_imm(x0, r, 4);
        e.str(x0, kCpu, o_npc);
    };

    // Whether npc is in kNpc (the previous instruction was a control
    // transfer) rather than pc + 4; if so, whether that transfer's target is
    // known here (as an offset from the block's pc), and whether it may
    // instead go on to the instruction after the delay slot.
    bool dynamic = false;
    bool known_target = false, may_fall = false;
    s64 target = 0, fall = 0;
    // A known npc (that offset from the block's pc) not yet put in kNpc:
    // only the ways to execute() and out of the block read it there.
    bool npc_lazy = false;
    s64 npc_off = 0;
    auto set_npc = [&](s64 o) { npc_lazy = true, npc_off = o; };
    auto npc_ready = [&] {
        if (!npc_lazy) return;
        pc_plus(kNpc, npc_off);
        mask(kNpc);
    };

    // Going on into the next block, as in jit_x64.cpp: chain() takes the
    // next pc in x2, chain_to() an offset from this block's pc.
    const u8 mode = static_cast<u8>(jit_mode());
    const u16 fmode_fetch = fetch_mode();
    const s64 first_page_off = static_cast<s64>(pc_ & 0x1FFF);  // (the same for every virtual alias)
    s64 page_off = first_page_off;  // (the block being made's)
    static_assert(sizeof(FetchEntry) == 32);
    static_assert(offsetof(FetchEntry, va_page) == 0 && offsetof(FetchEntry, gen) == 8 && offsetof(FetchEntry, mode) == 12 &&
                  offsetof(FetchEntry, host) == 16);
    // On through jit_stub_link_ to the block at x2 (its host address in
    // x3, its set at x5), noting it at `link` (in x9 already) if there is
    // one.
    auto look_up = [&](JitLink* link) {
        if (!link) e.mov(x9, xzr);
        e.mov_imm32(x4, mode);
        e.b_abs(static_cast<const u8*>(jit_stub_link_), base);
    };
    auto take = [&](unsigned count) {  // what is left of the budget, less count
        e.sub_imm(kLeftReg, kLeftReg, count);
        e.str(kLeftReg, sp, kLeft);
    };
    // (`count`: how many of this block's instructions were started.)
    auto chain = [&](unsigned count) {
        flush_cache();
        take(count);
        std::vector<Site> no;
        e.eor(x3, x2, kPc);
        e.lsr(x3, x3, 13);
        const Site other_page = e.cbnz(x3);
        e.sub(x3, x2, kPc);  // its host address
        e.load_constant(x4, reinterpret_cast<u64>(cur->host));
        e.add(x3, x3, x4);
        const Site found = e.b();
        e.bind(other_page);
        const s64 fetch = off(fetch_cache_[0]);
        e.lsr(x4, x2, 13);
        e.and_imm(x5, x4, kFetchEntries - 1);
        e.add(x5, kCpu, x5, true, Shift::Lsl, 5);
        e.add_imm(x5, x5, fetch);
        e.ldr(x6, x5, 0);
        e.cmp(x6, x4);
        no.push_back(e.b(Cond::Ne));
        e.ldr(x6, x5, 8, 4);
        e.ldr(x7, kCpu, off(soft_gen_), 4);
        e.cmp(x6, x7, false);
        no.push_back(e.b(Cond::Ne));
        e.ldr(x6, x5, 12, 2);
        e.cmp_imm(x6, fmode_fetch, false);
        no.push_back(e.b(Cond::Ne));
        e.ldr(x3, x5, 16);
        no.push_back(e.cbz(x3));  // not host memory
        e.and_imm(x6, x2, 0x1FFF);
        e.add(x3, x3, x6);
        e.bind(found);
        e.lsr(x5, x3, 2);  // its set (block_set())
        e.lsr(x6, x5, kBlockSetBits);
        e.eor(x5, x5, x6);
        e.and_imm(x5, x5, (1u << kBlockSetBits) - 1);
        e.mov_imm(x6, kBlockWays * sizeof(Block));
        e.load_constant(x7, reinterpret_cast<u64>(blocks_.data()));
        e.madd(x5, x5, x6, x7);
        look_up(nullptr);
        for (const Site& site : no) e.bind(site);
        e.b_abs(static_cast<const u8*>(jit_stub_out_), base);
    };
    // Where the code of each block's instructions starts (0 until made), and
    // the branches to one not made yet.
    std::size_t body[kRegionBlocks] = {};
    struct Ahead {
        Site site;
        unsigned to;
    };
    std::vector<Ahead> ahead;
    auto chain_to = [&](s64 delta, unsigned count) {
        if (page_off + delta < 0 || page_off + delta >= 0x2000) {
            pc_plus(x2, delta);
            mask(x2);  // (a target in the same page needs none)
            chain(count);
            return;
        }
        take(count);
        std::vector<Site> no;
        const unsigned valid = cache_valid, dirty_now = cache_dirty;  // (as the code after, on another path, has them)
        // The back edge of a delay loop (to this block's start, or to an
        // earlier block of a polling loop ending here): the iterations that
        // fit at once.
        const s64 loop_back = (cur->loop >> 30) == 3 ? -4 * static_cast<s64>((cur->loop >> kLoopBackShift) & kLoopBackMask) : 0;
        if (cur->loop && delta == loop_back && count == n) {
            e.bl_abs(static_cast<const u8*>(jit_stub_ccr_), base);  // (the loop runs in C++)
            flush_cache();
            total_into(x3);  // the next instruction's step
            e.ldr(x0, sp, kCycles0);
            e.add(x3, x3, x0);
            e.mov(x0, kCpu);
            e.mov_imm(x1, cur->loop);
            e.ldr(x2, sp, kLeft);
            e.load_constant(x16, reinterpret_cast<u64>(&jit_loop_at_once));
            e.blr(x16);
            reload_cache();
            e.ldr(kLeftReg, sp, kLeft);
            const Site none = e.cbz(x0);
            e.sub(kLeftReg, kLeftReg, x0);
            e.str(kLeftReg, sp, kLeft);
            e.ldr(x5, kCpu, off(delay_charged_), 1);  // charged other than the usual rate: out (so the check below leaves)
            const Site usual = e.cbz(x5);
            total_into(x0);
            e.str(x0, sp, kBudget);
            e.str(xzr, sp, kLeft);
            e.mov(kLeftReg, xzr);
            e.bind(usual);
            e.bind(none);
        }
        unsigned to = 0;  // (a block translated with this one, if it goes there)
        while (to < regions && region_off[to] != cur_off + delta) ++to;
        if (to < regions) {
            // Straight on to its code, the registers kept, while it fits
            // (past the cache's loads: it holds the registers); the pc is
            // its either way.
            if (delta) pc_plus(kPc, delta);
            e.cmp_imm(kLeftReg, region[to]->length);
            edge_dirty[to] |= cache_dirty;
            const Site on = e.b(Cond::Ge);
            if (body[to]) e.bind_to(on, body[to]);
            else ahead.push_back({on, to});
            flush_cache();
            store_pc_npc(0);
            total_into(x0);
            exits.push_back(e.b());
        } else {
            flush_cache();
            // Straight into the translation found the last time, while no
            // code has been written since; otherwise looked up, and noted.
            JitLink* const link = new_jit_link();
            if (link) {
                e.load_constant(x9, reinterpret_cast<u64>(link));
                e.ldr(x6, kCpu, off(code_gen_), 4);
                e.ldp(x7, x16, x9, 0);  // its generation (the low half of x7) and target
                e.cmp(x6, x7, false);
                const Site look = e.b(Cond::Ne);
                pc_plus(kPc, delta);
                e.br(x16);
                e.bind(look);
            }
            const u8* host = cur->host + delta;
            e.load_constant(x5, reinterpret_cast<u64>(&blocks_[block_set(host)]));
            e.load_constant(x3, reinterpret_cast<u64>(host));
            pc_plus(x2, delta);
            look_up(link);
        }
        cache_valid = valid, cache_dirty = dirty_now;
    };
    bool self = true;  // no instruction goes to execute()

    // Instruction i goes to execute() (through the stub, which leaves the
    // translation if run_blocks() would end the block there; otherwise the
    // window and global set are loaded again, and the cache).
    auto call_execute = [&](unsigned i, u32 insn) {
        self = false;
        if (dynamic) npc_ready();
        flush_cache(Reload::All, pre_dirty);
        e.mov_imm32(x9, i);
        e.mov_imm32(x10, insn);
        e.bl_abs(static_cast<const u8*>(dynamic ? jit_stub_dyn_ : jit_stub_seq_), base);
        const unsigned valid = cache_valid, dirty_now = cache_dirty;  // (this path's own; the code after goes on from the other's)
        reload_cache();
        cache_valid = valid, cache_dirty = dirty_now;
    };
    // The way to execute() of an instruction done here most of the time,
    // from the branches at `sites`, made after the end of the block, as in
    // jit_x64.cpp.
    enum class Then { Back, Npc, Leave };
    struct Cold {
        std::vector<Site> sites;
        unsigned i;
        u32 insn;
        std::size_t back;
        Then then;
        bool dynamic, npc_lazy;
        s64 npc_off;
        unsigned pre_dirty, valid, dirty;
    };
    std::vector<Cold> cold;
    auto out_of_line = [&](std::vector<Site> sites, unsigned i, u32 insn, Then then = Then::Back) {
        self = false;
        cold.push_back({std::move(sites), i, insn, e.size(), then, dynamic, npc_lazy, npc_off, pre_dirty, cache_valid, cache_dirty});
    };

    // The condition codes from the flags of a 32-bit operation (NZCV in
    // `f32`) and a 64-bit one (`f64`), into w(f64). Clobbers x16.
    auto ccr_into = [&](Reg f32, Reg f64, bool sub) {
        e.lsr(f32, f32, 28, false);
        e.lsr(f64, f64, 28, false);
        e.orr(f32, f32, f64, false, Shift::Lsl, 4);
        e.load_constant(x16, reinterpret_cast<u64>(sub ? kCcrSub.data() : kCcrAdd.data()));
        e.ldr_index(f64, x16, f32, 1);
    };
    // Lazy condition codes, as in jit_x64.cpp (see cc_op_). What the
    // translation knows of them at the code being made: nothing (where a
    // translation is entered), that ccr_ holds them, or the kind of
    // operation whose operands are in cc_a_ and cc_b_.
    enum class Cc : u8 { Sub = kCcSub, Add = kCcAdd, Logic = kCcLogic, Unknown = 0x10, Valid };  // (the kinds as cc_op_'s)
    Cc codes = Cc::Unknown;
    auto lazy = [&] { return codes == Cc::Sub || codes == Cc::Add || codes == Cc::Logic; };
    auto ccr_ready = [&] {  // ccr_ = the codes (cc_op_ kCcNone)
        if (codes == Cc::Valid) return;
        e.bl_abs(static_cast<const u8*>(jit_stub_ccr_), base);
        codes = Cc::Valid;
    };
    auto write_ccr = [&](Reg r) {  // all of CCR
        if (codes != Cc::Valid) e.str(xzr, kCpu, off(cc_op_), 1);
        e.str(r, kCpu, o_ccr, 1);
        codes = Cc::Valid;
    };
    // An operation whose codes are left lazy, its operands (or result)
    // stored (through `tmp`).
    auto lazy_codes = [&](Cc kind, Reg tmp) {
        e.mov_imm32(tmp, static_cast<u8>(kind));
        e.str(tmp, kCpu, off(cc_op_), 1);
        codes = kind;
    };
    // The instruction that reads the lazy codes straight from the host's
    // flags, as the operation before it left them (for icc, or xcc).
    int flags_at = -1;
    bool flags_xcc = false;
    // A conditional branch one way of which is made after the rest (see
    // cond_branch), as in jit_x64.cpp.
    struct Split {
        Site taken;
        s64 target;
        bool annulled;
        unsigned count;
        unsigned valid, dirty;
        Cc codes;
    };
    std::optional<Split> split;
    // Whether instruction i + 1 is a conditional branch or move on icc (1)
    // or xcc (2) that may take the codes from the host's flags.
    auto flags_user = [&](unsigned i) -> int {
        if (dynamic || i + 1 >= n) return 0;
        const u32 next = cur->insns[i + 1];
        const unsigned op = next >> 30, cond = (next >> 25) & 15;
        if (op == 0) {
            const unsigned op2 = (next >> 22) & 7;
            if (op2 == 2 && (cond & 7)) return 1;                          // Bicc
            if (op2 == 1 && (cond & 7) && !((next >> 20) & 1)) return (next >> 21) & 1 ? 2 : 1;  // BPcc
            return 0;
        }
        if (op == 2 && ((next >> 19) & 0x3F) == 0x2C && ((next >> 18) & 1)) {  // MOVcc
            const unsigned cc = (next >> 11) & 3;
            if (!(cc & 1) && (((next >> 14) & 15) & 7)) return cc == 2 ? 2 : 1;
        }
        return 0;
    };
    auto store_ccr = [&](Reg f32, Reg f64, bool sub) {
        ccr_into(f32, f64, sub);
        write_ccr(f64);
    };
    // The condition codes of a logical result in x0 (V and C clear), as
    // ccr_into leaves them.
    auto logic_ccr_into = [&](Reg f32, Reg f64) {
        e.tst(x0, x0, false);
        e.mrs_nzcv(f32);
        e.tst(x0, x0);
        e.mrs_nzcv(f64);
        ccr_into(f32, f64, false);
    };
    // The host condition that holds when icc/xcc condition `cond` (not
    // always or never) does, for instruction i: the host's flags as the
    // operation before left them, or from the operands again while the codes
    // are lazy, or from ccr_. Clobbers x0 and x1.
    auto icc_cond = [&](unsigned cond, bool xcc, int i) -> Cond {
        if (lazy()) {
            Cond c = codes == Cc::Sub ? kIccSub[cond] : kIccAdd[cond];
            if (codes == Cc::Logic && (cond == 4 || cond == 12)) c = cond == 4 ? Cond::Eq : Cond::Ne;  // (C clear)
            if (c != Cond::Al) {
                if (i >= 0 && flags_at == i && flags_xcc == xcc) return c;
                e.ldr(x0, kCpu, off(cc_a_));
                if (codes != Cc::Logic) e.ldr(x1, kCpu, off(cc_b_));
                if (codes == Cc::Sub) e.cmp(x0, x1, xcc);
                else if (codes == Cc::Add) e.adds(xzr, x0, x1, xcc);
                else e.tst(x0, x0, xcc);
                return c;
            }
        }
        ccr_ready();
        e.ldr(x0, kCpu, o_ccr, 1);
        if (xcc) e.lsr(x0, x0, 4, false);
        e.and_imm(x0, x0, 15, false);
        e.mov_imm32(x1, icc_table[cond]);
        e.lsrv(x0, x1, x0, false);
        e.tst_imm(x0, 1, false);
        return Cond::Ne;
    };
    // w0 = 1 if icc/xcc condition `cond` holds, else 0. Clobbers x1.
    auto eval_icc = [&](unsigned cond, bool xcc) {
        if (cond == 0 || cond == 8) e.mov_imm32(x0, cond == 8);
        else e.cset(x0, icc_cond(cond, xcc, -1));
    };
    // w0 = 1 if register rcond `rcond` holds for r, else 0.
    auto eval_rcond = [&](unsigned rcond, unsigned r) {
        load_reg(x0, r);
        e.cmp_imm(x0, 0);
        e.cset(x0, kRcond[rcond]);
    };

    // Floating point: f_ holds the registers as 32-bit halves, a double's
    // upper half first. A double in a host register is its two halves read
    // as one 64-bit word, rotated by 32 bits.
    const s64 f_off = off(f_[0]);
    auto freg = [&](unsigned half) { return f_off + 4 * static_cast<s64>(half); };
    auto dnum = [](unsigned r) { return (r & 0x1E) | ((r & 1) << 5); };  // a double's 5-bit encoding
    auto load_double = [&](Reg r, unsigned enc) {
        e.ldr(r, kCpu, freg(dnum(enc)));
        e.ror(r, r, 32);
    };
    auto store_double = [&](unsigned enc, Reg r) {  // (rotates r)
        e.ror(r, r, 32);
        e.str(r, kCpu, freg(dnum(enc)));
    };
    // FPRS.DL or DU for a write to f0-f31 or f32-f62. Clobbers x16.
    auto dirty = [&](bool upper) {
        e.ldr(x16, kCpu, off(fprs_), 1);
        e.orr_imm(x16, x16, upper ? 2 : 1, false);
        e.str(x16, kCpu, off(fprs_), 1);
    };
    auto clear_cexc = [&] {  // clobbers x16
        e.ldr(x16, kCpu, off(fsr_));
        e.and_imm(x16, x16, ~u64{0x1F});
        e.str(x16, kCpu, off(fsr_));
    };
    // To `off` unless PSTATE.PEF and FPRS.FEF are both set. Clobbers x16.
    auto fp_check = [&](std::vector<Site>& to) {
        e.ldr(x16, kCpu, off(pstate_), 2);
        to.push_back(e.cbz_bit(x16, 4));  // PEF
        e.ldr(x16, kCpu, off(fprs_), 1);
        to.push_back(e.cbz_bit(x16, 2));  // FEF
    };
    // w0 = 1 if fcc condition `cond` holds for fcc_n, else 0. Clobbers x1.
    u16 fcc_table[16] = {};
    for (unsigned c = 0; c < 16; ++c)
        for (unsigned f = 0; f < 4; ++f)
            if (cond_fcc(c, f)) fcc_table[c] |= static_cast<u16>(1u << f);
    auto eval_fcc = [&](unsigned cond, unsigned fcc_n) {
        e.ldr(x0, kCpu, off(fsr_));
        e.ubfx(x0, x0, fcc_n == 0 ? 10 : 30 + 2 * fcc_n, 2);
        e.mov_imm32(x1, fcc_table[cond]);
        e.lsrv(x0, x1, x0, false);
        e.and_imm(x0, x0, 1, false);
    };

    // Register operations (format 3, op 2). False if not done here.
    auto alu = [&](unsigned i, u32 insn) -> bool {
        const unsigned op3 = (insn >> 19) & 0x3F, rd = (insn >> 25) & 31, rs1 = (insn >> 14) & 31;
        const bool imm = (insn >> 13) & 1;
        if (op3 <= 0x07 || (op3 >= 0x10 && op3 <= 0x17)) {
            const unsigned kind = op3 & 7;
            const bool cc = op3 & 0x10;
            if (!cc && rd == 0) return true;
            // a = rs1, and operand 2 in bv or as v; the result made in d. The
            // codes go in the host's flags first when the next instruction
            // reads them (nothing after sets the flags).
            const bool arith = kind == 0 || kind == 4;
            const int user = cc ? flags_user(i) : 0;
            const Reg d = def_reg(rd, x2);
            const Reg a = use_reg(rs1, x0);
            const s64 v = imm ? sext(insn, 13) : 0;
            Reg bv = x1;
            if (!imm) bv = use_reg(insn & 31, x1);
            if (cc && arith) {  // (the operands kept for the codes)
                e.str(a, kCpu, off(cc_a_));
                if (imm) e.mov_imm(x1, static_cast<u64>(v));
                e.str(imm ? x1 : bv, kCpu, off(cc_b_));
                lazy_codes(kind == 0 ? Cc::Add : Cc::Sub, x3);
                if (user && kind == 4) {
                    if (imm) e.cmp_imm(a, v, user == 2);
                    else e.cmp(a, bv, user == 2);
                } else if (user) {
                    if (imm) e.adds_imm(xzr, a, v, user == 2);
                    else e.adds(xzr, a, bv, user == 2);
                }
                if (rd != 0) {
                    if (kind == 0) imm ? e.add_imm(d, a, v) : e.add(d, a, bv);
                    else imm ? e.sub_imm(d, a, v) : e.sub(d, a, bv);
                    put_reg(rd, d);
                }
            } else if (!cc && rs1 == 0 && (kind == 0 || kind == 2 || kind == 3)) {  // a move
                if (imm) e.mov_imm(d, static_cast<u64>(v));
                else if (d != bv) e.mov(d, bv);
                put_reg(rd, d);
            } else {
                if (kind == 0) imm ? e.add_imm(d, a, v) : e.add(d, a, bv);
                else if (kind == 4) imm ? e.sub_imm(d, a, v) : e.sub(d, a, bv);
                else if (imm) {
                    const u64 w = static_cast<u64>(kind == 5 || kind == 6 ? ~v : v);
                    if (kind == 1 || kind == 5) e.and_imm(d, a, w);
                    else if (kind == 2 || kind == 6) e.orr_imm(d, a, w);
                    else e.eor_imm(d, a, w);
                } else {
                    if (kind == 5 || kind == 6) e.mvn(x1, bv), bv = x1;
                    if (kind == 1 || kind == 5) e.and_(d, a, bv);
                    else if (kind == 2 || kind == 6) e.orr(d, a, bv);
                    else e.eor(d, a, bv);
                }
                if (kind == 7) e.mvn(d, d);
                put_reg(rd, d);
                if (cc) {
                    e.str(d, kCpu, off(cc_a_));
                    lazy_codes(Cc::Logic, x3);
                    if (user) e.tst(d, d, user == 2);
                }
            }
            if (user) flags_at = static_cast<int>(i + 1), flags_xcc = user == 2;
            return true;
        }
        switch (op3) {
            case 0x08:    // ADDC
            case 0x0C:    // SUBC
            case 0x18:    // ADDCcc
            case 0x1C: {  // SUBCcc
                const bool cc = op3 & 0x10, sub = op3 & 4;
                if (!cc && rd == 0) return true;
                auto carry_in = [&] {  // C = icc.C (for SBC, C = no borrow)
                    e.ldr(x5, kCpu, o_ccr, 1);
                    e.and_imm(x5, x5, 1, false);
                    if (sub) e.eor_imm(x5, x5, 1, false);
                    e.lsl(x5, x5, 29, false);
                    e.msr_nzcv(x5);
                };
                ccr_ready();
                load_reg(x0, rs1);
                load_op2(x1, insn);
                carry_in();
                if (cc) {
                    if (sub) e.sbcs(x2, x0, x1, false);
                    else e.adcs(x2, x0, x1, false);
                    e.mrs_nzcv(x3);
                    e.msr_nzcv(x5);
                    if (sub) e.sbcs(x0, x0, x1);
                    else e.adcs(x0, x0, x1);
                    e.mrs_nzcv(x4);
                    store_ccr(x3, x4, sub);
                } else if (sub) {
                    e.sbc(x0, x0, x1);
                } else {
                    e.adc(x0, x0, x1);
                }
                store_reg(rd, x0);
                return true;
            }
            case 0x0A:    // UMUL
            case 0x0B:    // SMUL
            case 0x1A:    // UMULcc
            case 0x1B: {  // SMULcc: the 64-bit product; Y = its upper half
                load_reg(x0, rs1);
                load_op2(x1, insn);
                if (op3 & 1) e.smull(x0, x0, x1);
                else e.umull(x0, x0, x1);
                e.lsr(x2, x0, 32);
                e.str(x2, kCpu, off(y_));
                if (op3 & 0x10) {  // (the codes of the product as a logical result's)
                    e.str(x0, kCpu, off(cc_a_));
                    lazy_codes(Cc::Logic, x3);
                }
                store_reg(rd, x0);
                return true;
            }
            case 0x0D:    // UDIVX
            case 0x2D:    // SDIVX
            case 0x0E:    // UDIV
            case 0x0F:    // SDIV
            case 0x1E:    // UDIVcc
            case 0x1F: {  // SDIVcc
                const bool wide = op3 == 0x0D || op3 == 0x2D;
                const bool sign = op3 == 0x2D || (op3 & 0x0F) == 0x0F;
                const bool cc = op3 & 0x10;
                load_reg(x0, rs1);
                if (!wide) {  // Y:rs1<31:0> by the low 32 bits of operand 2
                    e.mov(x0, x0, false);
                    e.ldr(x2, kCpu, off(y_));
                    e.orr(x0, x0, x2, true, Shift::Lsl, 32);
                }
                load_op2(x1, insn);
                if (!wide) {
                    if (sign) e.sxtw(x1, x1);
                    else e.mov(x1, x1, false);
                }
                const Site by_zero = e.cbz(x1);  // division_by_zero, from execute()
                if (sign && !wide) {
                    // -(-2^63) is taken as 2^63 - 1 (then clamped).
                    e.cmp_imm(x1, -1);
                    const Site general = e.b(Cond::Ne);
                    e.subs(x0, xzr, x0);
                    e.mov_imm(x2, 0x7FFFFFFFFFFFFFFFull);
                    e.csel(x0, x2, x0, Cond::Vs);
                    const Site quotient = e.b();
                    e.bind(general);
                    e.sdiv(x0, x0, x1);
                    e.bind(quotient);
                } else if (sign) {
                    e.sdiv(x0, x0, x1);  // (-2^63 / -1 gives -2^63, as SPARC)
                } else {
                    e.udiv(x0, x0, x1);
                }
                if (!wide) {
                    // The quotient clamped to 32 bits; w3 = 1 if it overflowed.
                    if (sign) {
                        e.mov_imm(x2, 0x7FFFFFFF);
                        e.cmp(x0, x2);
                        e.cset(x3, Cond::Gt);
                        e.csel(x0, x2, x0, Cond::Gt);
                        e.mov_imm(x2, static_cast<u64>(s64{INT32_MIN}));
                        e.cmp(x0, x2);
                        e.cset(x4, Cond::Lt);
                        e.csel(x0, x2, x0, Cond::Lt);
                        e.orr(x3, x3, x4);
                    } else {
                        e.mov_imm(x2, 0xFFFFFFFF);
                        e.cmp(x0, x2);
                        e.cset(x3, Cond::Hi);
                        e.csel(x0, x2, x0, Cond::Hi);
                    }
                    if (cc) {
                        e.mov(x7, x3);
                        logic_ccr_into(x5, x6);
                        e.orr(x6, x6, x7, false, Shift::Lsl, 1);  // icc.V
                        write_ccr(x6);
                    }
                }
                store_reg(rd, x0);
                out_of_line({by_zero}, i, insn);
                return true;
            }
            case 0x20:    // TADDcc
            case 0x21:    // TSUBcc
            case 0x22:    // TADDccTV
            case 0x23: {  // TSUBccTV: icc.V also set by a tag (bits 1:0) in either operand
                const bool sub = op3 & 1;
                load_reg(x0, rs1);
                load_op2(x1, insn);
                e.orr(x8, x0, x1);
                if (sub) e.subs(x2, x0, x1, false);
                else e.adds(x2, x0, x1, false);
                e.mrs_nzcv(x3);
                if (sub) e.subs(x0, x0, x1);
                else e.adds(x0, x0, x1);
                e.mrs_nzcv(x4);
                ccr_into(x3, x4, sub);
                e.tst_imm(x8, 3);
                e.mov_imm32(x3, 2);
                e.csel(x3, x3, xzr, Cond::Ne);
                e.orr(x4, x4, x3, false);
                Site overflow{};
                const bool tv = op3 & 2;
                if (tv) overflow = e.cbnz_bit(x4, 1);  // tag_overflow, from execute()
                write_ccr(x4);
                store_reg(rd, x0);
                if (tv) {
                    out_of_line({overflow}, i, insn);
                }
                return true;
            }
            case 0x2E:  // POPC (rs1 must be 0)
                if (rs1 != 0) return false;
                if (rd == 0) return true;
                load_op2(x1, insn);
                e.fmov_from_gpr(0, x1, true);
                e.cnt8b(0, 0);
                e.addv8b(0, 0);
                e.fmov_to_gpr(x0, 0, false);
                store_reg(rd, x0);
                return true;
            case 0x24: {  // MULScc: one step of a 32-bit multiply, shifting through Y
                // op1 = (icc.N ^ icc.V):rs1<31:1>; op2 = Y<0> ? operand 2<31:0> : 0
                ccr_ready();
                load_reg(x0, rs1);
                load_op2(x1, insn);
                e.ldr(x10, kCpu, off(y_));
                e.mov(x5, x0);  // rs1, for Y
                e.lsr(x0, x0, 1, false);
                e.ldr(x2, kCpu, o_ccr, 1);
                e.lsr(x3, x2, 3, false);  // N
                e.lsr(x2, x2, 1, false);  // V
                e.eor(x2, x2, x3, false);
                e.and_imm(x2, x2, 1, false);
                e.orr(x0, x0, x2, false, Shift::Lsl, 31);  // op1
                e.mov(x1, x1, false);
                e.tst_imm(x10, 1);
                e.csel(x1, x1, xzr, Cond::Ne);  // op2
                e.adds(x2, x0, x1, false);
                e.mrs_nzcv(x3);
                e.adds(x0, x0, x1);
                e.mrs_nzcv(x4);
                store_ccr(x3, x4, false);
                e.mov(x0, x0, false);
                store_reg(rd, x0);
                // Y = Y >> 1 with rs1<0> into bit 31.
                e.lsr(x10, x10, 1, false);
                e.and_imm(x5, x5, 1);
                e.orr(x10, x10, x5, false, Shift::Lsl, 31);
                e.str(x10, kCpu, off(y_));
                return true;
            }
            case 0x09:  // MULX
                if (rd == 0) return true;
                load_reg(x0, rs1);
                load_op2(x1, insn);
                e.mul(x0, x0, x1);
                store_reg(rd, x0);
                return true;
            case 0x25:    // SLL, SLLX
            case 0x26:    // SRL, SRLX
            case 0x27: {  // SRA, SRAX
                if (rd == 0) return true;
                const bool x = (insn >> 12) & 1;
                const Reg d = def_reg(rd, x0);
                Reg a = use_reg(rs1, x0);
                if (imm && !x && op3 != 0x25) {  // SRL, SRA: the shifted field of the low word, in one
                    const unsigned count = insn & 31;
                    if (op3 == 0x26) count ? e.ubfx(d, a, count, 32 - count) : e.mov(d, a, false);
                    else count ? e.sbfx(d, a, count, 32 - count) : e.sxtw(d, a);
                    put_reg(rd, d);
                    return true;
                }
                if (!x && op3 == 0x26) e.mov(x0, a, false), a = x0;
                if (!x && op3 == 0x27) e.sxtw(x0, a), a = x0;
                if (imm) {
                    const unsigned count = insn & (x ? 63 : 31);
                    if (!count) {
                        if (d != a) e.mov(d, a);
                    } else if (op3 == 0x25) {
                        e.lsl(d, a, count);
                    } else if (op3 == 0x26) {
                        e.lsr(d, a, count);
                    } else {
                        e.asr(d, a, count);
                    }
                } else {
                    const Reg c = use_reg(insn & 31, x1);
                    e.and_imm(x1, c, x ? 63 : 31);
                    if (op3 == 0x25) e.lslv(d, a, x1);
                    else if (op3 == 0x26) e.lsrv(d, a, x1);
                    else e.asrv(d, a, x1);
                }
                put_reg(rd, d);
                return true;
            }
            case 0x2C: {  // MOVcc on icc/xcc (MOVFcc on fcc_n: fp_disabled from execute())
                const unsigned cc = (insn >> 11) & 3;
                if (!((insn >> 18) & 1)) {
                    if (rd == 0) return false;
                    std::vector<Site> to;
                    fp_check(to);
                    eval_fcc((insn >> 14) & 15, cc);
                    if (imm) e.mov_imm(x2, static_cast<u64>(sext(insn, 11)));
                    else load_reg(x2, insn & 31);
                    load_reg(x3, rd);
                    e.cmp_imm(x0, 0, false);
                    e.csel(x3, x2, x3, Cond::Ne);
                    store_reg(rd, x3);
                    out_of_line(std::move(to), i, insn);
                    return true;
                }
                if (cc & 1) return false;
                const unsigned cond = (insn >> 14) & 15;
                if (rd == 0 || cond == 0) return true;
                const Cond c = cond == 8 ? Cond::Al : icc_cond(cond, cc == 2, static_cast<int>(i));
                Reg src = x2;
                if (imm) e.mov_imm(x2, static_cast<u64>(sext(insn, 11)));
                else src = use_reg(insn & 31, x2);
                const Reg d = def_reg(rd, x3);
                if (cond == 8) {
                    if (d != src) e.mov(d, src);
                } else {
                    if (d == x3) load_reg(x3, rd);
                    e.csel(d, src, d, c);
                }
                put_reg(rd, d);
                return true;
            }
            case 0x2F: {  // MOVr
                const unsigned rcond = (insn >> 10) & 7;
                if (!rcond_valid(rcond)) return false;
                if (rd == 0) return true;
                Reg src = x2;
                if (imm) e.mov_imm(x2, static_cast<u64>(sext(insn, 10)));
                else src = use_reg(insn & 31, x2);
                const Reg d = def_reg(rd, x3);
                if (d == x3) load_reg(x3, rd);
                e.cmp_imm(use_reg(rs1, x0), 0);
                e.csel(d, src, d, kRcond[rcond]);
                put_reg(rd, d);
                return true;
            }
            default: return false;
        }
    };

    // The new current window is in w1: its bases, in base_ and kO/kI.
    // Clobbers x0 and x2.
    auto set_window = [&] {
        const s64 windows = off(windows_);
        e.add(x0, kCpu, x1, true, Shift::Lsl, 7);  // 16 registers
        e.add_imm(kI, x0, windows);
        e.str(kI, kCpu, off(base_[3]));
        e.add_imm(x0, kI, 64);
        e.str(x0, kCpu, off(base_[2]));
        e.add_imm(x2, x1, 1, false);  // the outs are the next window's ins
        e.and_imm(x2, x2, kWindows - 1, false);
        e.add(x0, kCpu, x2, true, Shift::Lsl, 7);
        e.add_imm(kO, x0, windows);
        e.str(kO, kCpu, off(base_[1]));
    };
    // SAVE (to the next window) or RESTORE and RETURN (to the previous one),
    // once the traps have been ruled out, the cache's registers of the
    // window to and from memory. Clobbers x0-x2.
    auto move_window = [&](bool save) {
        flush_cache(Reload::Window);
        auto step = [&](const auto& field, unsigned by) {
            e.ldr(x0, kCpu, off(field), 4);
            e.add_imm(x0, x0, by, false);
            e.and_imm(x0, x0, kWindows - 1, false);
            e.str(x0, kCpu, off(field), 4);
        };
        step(cansave_, save ? kWindows - 1 : 1);
        step(canrestore_, save ? 1 : kWindows - 1);
        e.ldr(x1, kCpu, off(cwp_), 4);
        e.add_imm(x1, x1, save ? 1 : kWindows - 1, false);
        e.and_imm(x1, x1, kWindows - 1, false);
        e.str(x1, kCpu, off(cwp_), 4);
        set_window();
        reload_cache(Reload::Window);
    };
    // A conditional branch at block offset `here`, taken when host
    // condition c holds (on the flags as the caller left them). Returns
    // with npc in kNpc, or leaves the block (annulled and not taken).
    auto cond_branch = [&](unsigned i, s64 here, s64 disp, bool annul, Cond c) {
        known_target = true, target = here + disp;
        may_fall = !annul, fall = here + 8;
        // A branch back (a loop's, most often taken) goes on to its target
        // here, the other way being made at the end of the block; one
        // forward, the other way round.
        const bool back = disp <= 0;
        auto other_way = [&](bool taken, s64 to, bool annulled) {
            split = Split{e.b(taken ? c : invert(c)), to, annulled, i + 1, cache_valid, cache_dirty, codes};
        };
        if (!annul && i + 2 == n) {
            // The delay slot ends the block: it is made twice, for each way.
            if (back) other_way(false, here + 8, false);
            else other_way(true, here + disp, false), target = here + 8;
            may_fall = false;
            set_npc(target);
        } else if (annul && back) {
            other_way(false, here + 8, true);  // (the delay slot annulled)
            may_fall = false;
            set_npc(target);
        } else if (!annul) {
            pc_plus(kNpc, here + 8);
            pc_plus(x2, here + disp);
            mask(x2);
            e.csel(kNpc, x2, kNpc, c);
        } else {
            const Site taken = e.b(c);
            chain_to(here + 8, i + 1);  // the delay slot is annulled
            e.bind(taken);
            set_npc(here + disp);
        }
        dynamic = true;
    };
    // Bicc and BPcc on icc/xcc, and FBfcc and FBPfcc (with the fcc for
    // `xcc`, as fcc_n + 2).
    auto icc_branch = [&](unsigned i, s64 here, unsigned cond, unsigned xcc, s64 disp, bool annul) {
        if (cond == 8) {  // always
            if (annul) {
                chain_to(here + disp, i + 1);
                return;
            }
            set_npc(here + disp);
            dynamic = true;
            known_target = true, target = here + disp, may_fall = false;
        } else if (cond == 0) {  // never
            if (annul) chain_to(here + 8, i + 1);
        } else {
            Cond c = Cond::Ne;
            if (xcc > 1) eval_fcc(cond, xcc - 2), e.cmp_imm(x0, 0, false);
            else c = icc_cond(cond, xcc, static_cast<int>(i));
            cond_branch(i, here, disp, annul, c);
        }
    };
    // Control transfers. False if not done here.
    auto cti = [&](unsigned i, u32 insn) -> bool {
        if (dynamic) return false;  // a control transfer in a delay slot
        const s64 here = 4 * static_cast<s64>(i);
        const unsigned op = insn >> 30;
        if (op == 1) {  // CALL
            pc_plus(x0, here);
            mask(x0);
            store_reg(15, x0);
            set_npc(here + (sext(insn, 30) << 2));
            dynamic = true;
            known_target = true, target = here + (sext(insn, 30) << 2), may_fall = false;
            return true;
        }
        if (op == 0) {
            const unsigned op2 = (insn >> 22) & 7, cond = (insn >> 25) & 15;
            const bool annul = (insn >> 29) & 1;
            switch (op2) {
                case 1: {  // BPcc
                    const unsigned cc = (insn >> 20) & 3;
                    if (cc & 1) return false;
                    icc_branch(i, here, cond, cc == 2, sext(insn, 19) << 2, annul);
                    return true;
                }
                case 2:  // Bicc
                    icc_branch(i, here, cond, false, sext(insn, 22) << 2, annul);
                    return true;
                case 5:    // FBPfcc
                case 6: {  // FBfcc: fp_disabled from execute()
                    std::vector<Site> to;
                    fp_check(to);
                    out_of_line(std::move(to), i, insn);
                    const unsigned fcc_n = op2 == 5 ? (insn >> 20) & 3 : 0;
                    icc_branch(i, here, cond, 2 + fcc_n, (op2 == 5 ? sext(insn, 19) : sext(insn, 22)) << 2, annul);
                    return true;
                }
                case 3: {  // BPr
                    const unsigned rcond = (insn >> 25) & 7;
                    if (((insn >> 28) & 1) || !rcond_valid(rcond)) return false;
                    e.cmp_imm(use_reg((insn >> 14) & 31, x0), 0);
                    const u64 d16 = (((insn >> 20) & 3) << 14) | (insn & 0x3FFF);
                    cond_branch(i, here, sext(d16, 16) << 2, annul, kRcond[rcond]);
                    return true;
                }
                default: return false;
            }
        }
        if (op == 2 && ((insn >> 19) & 0x3F) == 0x39) {  // RETURN: the target from the old window, then RESTORE's move
            self = false;
            load_reg(x3, (insn >> 14) & 31);
            load_op2(x1, insn);
            e.add(x3, x3, x1);
            mask(x3);
            std::vector<Site> slow;
            e.tst_imm(x3, 3);
            slow.push_back(e.b(Cond::Ne));  // mem_address_not_aligned
            e.ldr(x0, kCpu, off(canrestore_), 4);
            slow.push_back(e.cbz(x0, false));  // fill
            e.mov(kNpc, x3);
            move_window(false);
            out_of_line(std::move(slow), i, insn, Then::Npc);
            dynamic = true;
            return true;
        }
        if (op == 2 && ((insn >> 19) & 0x3F) == 0x38) {  // JMPL
            load_reg(x3, (insn >> 14) & 31);
            load_op2(x1, insn);
            e.add(x3, x3, x1);
            mask(x3);
            out_of_line({e.cbnz_bit(x3, 0), e.cbnz_bit(x3, 1)}, i, insn, Then::Leave);  // it takes the alignment trap
            e.mov(kNpc, x3);
            pc_plus(x1, here);
            mask(x1);
            store_reg((insn >> 25) & 31, x1);
            dynamic = true;
            return true;
        }
        return false;
    };

    // Loads, stores and atomics through the fast data translations
    // (fast_); anything else (an ASI without a fast table, a miss,
    // misalignment, a write to a page with decoded code) goes to execute().
    // False if not done here.
    const unsigned fmode = fast_mode();
    static_assert(sizeof(u64) == 8 && sizeof(s64) == 8 && sizeof(const u8*) == 8);
    enum class Kind {
        Load, Store, LoadPair, StorePair, Ldstub, Swap, Cas, FLoad, FStore, BlockLoad, BlockStore,
        FsrLoad, FsrStore, Partial, ShortLoad, ShortStore, Quad
    };
    auto mem = [&](unsigned i, u32 insn) -> bool {
        const unsigned op3 = (insn >> 19) & 0x3F, rd = (insn >> 25) & 31, rs1 = (insn >> 14) & 31;
        const bool imm = (insn >> 13) & 1;
        if ((op3 & 0x2F) == 0x2D) {  // PREFETCH(A): nothing to do, unless the ASI is restricted
            return !(op3 & 0x10) || (pstate_ & pstate::PRIV) || (!imm && ((insn >> 5) & 0xFF) >= 0x80);
        }
        // The ASI: the implicit one (TL, PSTATE.CLE and privilege are part of
        // the mode), one given in the instruction, or %asi, taken as it is
        // now and checked when the code runs. VIS block loads and stores
        // (eight doubles), partial stores and short FP loads and stores go
        // by the memory ASI theirs translates as. Any memory ASI with a
        // fast table (fast_class()) is done here, in its byte order.
        const bool asi_reg = (op3 & 0x10) && imm;
        const u8 the_asi = !(op3 & 0x10) ? implicit_asi() : imm ? asi_ : static_cast<u8>(insn >> 5);
        const bool vis_fp = op3 == 0x33 || op3 == 0x37;
        const bool block = vis_fp && asi_is_block(the_asi) && !(rd & 0x0E) &&
                           !(op3 == 0x33 && (the_asi == asi::BLK_COMMIT_P || the_asi == asi::BLK_COMMIT_S));
        const bool partial = vis_fp && op3 == 0x37 && asi_is_partial(the_asi) && !imm;  // (rs2 is the mask)
        const bool short_fp = vis_fp && asi_is_short_fp(the_asi);
        const u8 mem_asi = block || partial || short_fp ? base_memory_asi(the_asi) : the_asi;
        const bool little = asi_is_little(mem_asi);
        const u8 asi_field = the_asi;
        if (op3 == 0x1B && (pstate_ & pstate::PRIV) && internal_read_is_plain(the_asi)) {  // LDXA of an internal register
            self = false;
            load_reg(x0, rs1);
            if (imm) {
                if (const s64 d = sext(insn, 13)) e.add_imm(x0, x0, d);
            } else if (insn & 31) {
                load_reg(x1, insn & 31);
                e.add(x0, x0, x1);
            }
            mask(x0);
            std::vector<Site> slow;
            if (asi_reg) {
                e.ldr(x1, kCpu, off(asi_), 1);
                e.cmp_imm(x1, the_asi, false);
                slow.push_back(e.b(Cond::Ne));
            }
            e.tst_imm(x0, 7);
            slow.push_back(e.b(Cond::Ne));
            flush_cache(Reload::Call);
            e.mov(x2, x0);
            e.mov(x0, kCpu);
            e.mov_imm32(x1, the_asi);
            e.load_constant(x16, reinterpret_cast<u64>(&UltraSparc::jit_internal_read));
            e.blr(x16);
            reload_cache(Reload::Call);
            e.ldr(kLeftReg, sp, kLeft);
            store_reg(rd, x0);
            out_of_line(std::move(slow), i, insn);
            return true;
        }
        unsigned size;
        bool sign = false;
        Kind kind = Kind::Load;
        if (block) {
            size = 64, kind = op3 == 0x33 ? Kind::BlockLoad : Kind::BlockStore;
        } else if (partial) {
            size = 8, kind = Kind::Partial;
        } else if (short_fp) {
            size = asi_field & 2 ? 2 : 1, kind = op3 == 0x33 ? Kind::ShortLoad : Kind::ShortStore;
        } else if (op3 == 0x13 && (the_asi == asi::NQUAD_LDD || the_asi == asi::NQUAD_LDD_L)) {  // the quad load
            if (rd & 1) return false;  // illegal_instruction
            size = 16, kind = Kind::Quad;
        } else if ((op3 == 0x21 || op3 == 0x25) && rd <= 1) {  // LDFSR, LDXFSR, STFSR, STXFSR
            size = rd ? 8 : 4, kind = op3 == 0x21 ? Kind::FsrLoad : Kind::FsrStore;
        } else switch (op3 & 0x2F) {
            case 0x00: size = 4; break;                         // LDUW
            case 0x01: size = 1; break;                         // LDUB
            case 0x02: size = 2; break;                         // LDUH
            case 0x03: size = 8, kind = Kind::LoadPair; break;  // LDD
            case 0x04: size = 4, kind = Kind::Store; break;     // STW
            case 0x05: size = 1, kind = Kind::Store; break;     // STB
            case 0x06: size = 2, kind = Kind::Store; break;     // STH
            case 0x07: size = 8, kind = Kind::StorePair; break; // STD
            case 0x08: size = 4, sign = true; break;            // LDSW
            case 0x09: size = 1, sign = true; break;            // LDSB
            case 0x0A: size = 2, sign = true; break;            // LDSH
            case 0x0B: size = 8; break;                         // LDX
            case 0x0D: size = 1, kind = Kind::Ldstub; break;    // LDSTUB
            case 0x0E: size = 8, kind = Kind::Store; break;     // STX
            case 0x0F: size = 4, kind = Kind::Swap; break;      // SWAP
            case 0x20: size = 4, kind = Kind::FLoad; break;     // LDF
            case 0x23: size = 8, kind = Kind::FLoad; break;     // LDDF
            case 0x24: size = 4, kind = Kind::FStore; break;    // STF
            case 0x27: size = 8, kind = Kind::FStore; break;    // STDF
            case 0x2C:                                          // CASA
            case 0x2E:                                          // CASXA
                if (!(op3 & 0x10)) return false;
                size = op3 == 0x3C ? 4 : 8, kind = Kind::Cas;
                break;
            default: return false;
        }
        if ((kind == Kind::LoadPair || kind == Kind::StorePair) && (rd & 1)) return false;  // illegal_instruction
        // The table: no-fault loads use P's or S's (and make no entries).
        const bool no_fault = mem_asi == asi::PNF || mem_asi == asi::SNF || mem_asi == asi::PNFL || mem_asi == asi::SNFL;
        const bool load_only = kind == Kind::Load || kind == Kind::FLoad || kind == Kind::LoadPair;
        const int cls = no_fault && load_only ? fast_class(static_cast<u8>(mem_asi & ~2)) : fast_class(mem_asi);
        if (cls < 0) return false;
        const bool writes = kind != Kind::Load && kind != Kind::LoadPair && kind != Kind::FLoad && kind != Kind::BlockLoad &&
                            kind != Kind::FsrLoad && kind != Kind::ShortLoad && kind != Kind::Quad;
        const bool plain_store = kind == Kind::Store || kind == Kind::StorePair || kind == Kind::FStore ||
                                 kind == Kind::BlockStore || kind == Kind::FsrStore || kind == Kind::Partial ||
                                 kind == Kind::ShortStore;
        const u8 perm = kind == Kind::Quad ? kFastAtomic : !writes ? kFastRead : plain_store ? kFastWrite : kFastAtomic;
        self = false;  // (the check mode must not do it twice)
        if (kind == Kind::Cas || kind == Kind::Partial) {
            load_reg(x0, rs1);  // [rs1] only (a partial store's rs2 is the mask)
        } else if (imm) {
            const Reg a = use_reg(rs1, x0);
            if (const s64 d = sext(insn, 13)) e.add_imm(x0, a, d);
            else if (a != x0) e.mov(x0, a);
        } else if (insn & 31) {
            const Reg a = use_reg(rs1, x0);
            e.add(x0, a, use_reg(insn & 31, x1));
        } else {
            load_reg(x0, rs1);
        }
        mask(x0);
        std::vector<Site> slow;
        const bool fpu = kind == Kind::FLoad || kind == Kind::FStore || kind == Kind::FsrLoad || kind == Kind::FsrStore;
        if (fpu || block || partial || short_fp) fp_check(slow);  // (the misaligned LDDF/STDF traps also go there)
        if (asi_reg) {
            e.ldr(x1, kCpu, off(asi_), 1);
            e.cmp_imm(x1, the_asi, false);
            slow.push_back(e.b(Cond::Ne));
        }
        if (size == 2) {
            slow.push_back(e.cbnz_bit(x0, 0));
        } else if (size > 1) {
            e.tst_imm(x0, size - 1);
            slow.push_back(e.b(Cond::Ne));
        }
        // x4 = the entry's index, x2 = its place in the table's arrays: the
        // index * 8 from the CPU, or (when a field this uses is out of a
        // load's reach from there) from x5, the CPU plus the table's offset
        // but for its low 12 bits, which go in each field's. (x5 is made
        // first, from nothing the address waits on.)
        const FastTable& table = fast_[cls];
        // (Loads and plain stores have keys of their own; the rest look at
        // the perms.)
        const bool own_key = perm == kFastRead || perm == kFastWrite;
        const auto& keys = perm == kFastRead ? table.read : perm == kFastWrite ? table.write : table.key;
        auto in_reach = [&](const auto& array) { return off(array[0]) / 8 < 4096; };
        const bool near = in_reach(keys) && in_reach(table.addend) && (!writes || in_reach(table.code));
        const s64 table_high = near ? 0 : off(table) & ~s64{0xFFF};
        auto field = [&](const auto& array) { return off(array[0]) - table_high; };
        if (!near) e.add_imm(x5, kCpu, table_high);
        static_assert(kFastEntries == 512);
        e.eor(x4, x0, x0, true, Shift::Lsr, 9);  // fast_index(): the page with page >> 9 folded in,
        e.ubfx(x4, x4, 13, 9);                   // in the address's bits 13-21
        e.add(x2, near ? kCpu : x5, x4, true, Shift::Lsl, 3);
        // The key with the page taken out leaves the table's generation.
        e.ldr(x3, x2, field(keys));
        e.eor(x1, x3, x0, true, Shift::Lsr, 13);
        e.ldr(x3, kCpu, off(fast_key_[cls]));
        e.cmp(x1, x3);
        slow.push_back(e.b(Cond::Ne));
        if (!own_key) {
            e.add(x3, kCpu, x4);  // (the perms are bytes)
            e.ldr(x3, x3, off(table.perms[0]), 1);
            e.tst_imm(x3, perm, false);
            slow.push_back(e.b(Cond::Eq));
        }
        if (writes) {
            count_effect(x3);  // (see idle_visit)
            e.ldr(x3, x2, field(table.code));
            e.ldr(x3, x3, 0, 2);  // both halves' code flags
            slow.push_back(e.cbnz(x3, false));
        }
        e.ldr(x3, x2, field(table.addend));  // host address - va
        // (Loads and stores go to [x0 + x3]; the rest have the host address
        // in x0.)
        const bool indexed = kind == Kind::Load || kind == Kind::Store;
        if (!indexed) e.add(x0, x0, x3);
        // Big-endian memory to and from host order (nothing to do for a
        // little-endian ASI).
        auto swap64 = [&](Reg r) { if (!little) e.rev64(r, r); };
        auto swap32 = [&](Reg r) { if (!little) e.rev32(r, r); };
        auto swap16 = [&](Reg r) { if (!little) e.rev16(r, r); };
        switch (kind) {
            case Kind::Load: {
                const Reg t = def_reg(rd, x1);
                const bool load_signed = sign && (size == 1 || little);  // (else extended once in host order)
                e.ldr_index(t, x0, x3, size, load_signed);
                switch (size) {
                    case 8: swap64(t); break;
                    case 4:
                        swap32(t);
                        if (sign && !load_signed) e.sxtw(t, t);
                        break;
                    case 2:
                        swap16(t);
                        if (sign && !load_signed) e.sxth(t, t);
                        break;
                    default: break;
                }
                put_reg(rd, t);
                break;
            }
            case Kind::Store: {
                Reg v = x1;
                if (size == 1 || little) v = use_reg(rd, x1);  // (no byte swap: as it is)
                else load_reg(x1, rd);
                if (size == 8) swap64(v);
                else if (size == 4) swap32(v);
                else if (size == 2) swap16(v);
                e.str_index(v, x0, x3, size);
                break;
            }
            case Kind::LoadPair:  // rd = the word at va, rd + 1 = the word at va + 4 (each in the ASI's byte order)
                e.ldr(x1, x0, 0);
                swap64(x1);
                if (little) e.ror(x1, x1, 32);  // (the word at va is the low half)
                e.mov(x2, x1, false);
                e.lsr(x1, x1, 32);
                store_reg(rd, x1);
                store_reg(rd + 1, x2);
                break;
            case Kind::StorePair:
                load_reg(x1, rd);
                load_reg(x2, rd + 1);
                e.mov(x2, x2, false);
                e.orr(x1, x2, x1, true, Shift::Lsl, 32);
                if (little) e.ror(x1, x1, 32);
                swap64(x1);
                e.str(x1, x0, 0);
                break;
            case Kind::Ldstub:
                e.ldr(x1, x0, 0, 1);
                e.mov_imm32(x2, 0xFF);
                e.str(x2, x0, 0, 1);
                store_reg(rd, x1);
                break;
            case Kind::Swap:
                e.ldr(x1, x0, 0, 4);
                swap32(x1);
                load_reg(x2, rd);
                swap32(x2);
                e.str(x2, x0, 0, 4);
                store_reg(rd, x1);
                break;
            case Kind::FLoad:
                if (size == 8) {
                    e.ldr(x1, x0, 0);
                    swap64(x1);
                    store_double(rd, x1);
                    dirty(rd & 1);
                } else {
                    e.ldr(x1, x0, 0, 4);
                    swap32(x1);
                    e.str(x1, kCpu, freg(rd), 4);
                    dirty(false);
                }
                break;
            case Kind::FStore:
                if (size == 8) {
                    load_double(x1, rd);
                    swap64(x1);
                    e.str(x1, x0, 0);
                } else {
                    e.ldr(x1, kCpu, freg(rd), 4);
                    swap32(x1);
                    e.str(x1, x0, 0, 4);
                }
                break;
            case Kind::BlockLoad:
                for (unsigned k = 0; k < 8; ++k) {
                    e.ldr(x1, x0, 8 * k);
                    swap64(x1);
                    store_double(rd + 2 * k, x1);
                }
                dirty(rd & 1);
                break;
            case Kind::BlockStore:
                for (unsigned k = 0; k < 8; ++k) {
                    load_double(x1, rd + 2 * k);
                    swap64(x1);
                    e.str(x1, x0, 8 * k);
                }
                break;
            case Kind::FsrLoad:  // RD, TEM, NS, fcc0, aexc, cexc (and LDXFSR fcc1-3); ver, ftt, qne kept
                e.ldr(x2, kCpu, off(fsr_));
                if (size == 8) {
                    e.ldr(x1, x0, 0);
                    swap64(x1);
                    e.and_imm(x1, x1, 0x3FCFC00FFFull);
                } else {
                    e.ldr(x1, x0, 0, 4);
                    swap32(x1);
                    e.and_imm(x1, x1, 0xCFC00FFF, false);
                    e.and_imm(x3, x2, 0xFFFFFFFF00000000ull);
                    e.orr(x1, x1, x3);
                }
                e.and_imm(x2, x2, 0x000FE000);
                e.orr(x1, x1, x2);
                e.str(x1, kCpu, off(fsr_));
                break;
            case Kind::FsrStore:  // then ftt cleared
                e.ldr(x2, kCpu, off(fsr_));
                e.mov(x1, x2);
                if (size == 8) swap64(x1);
                else swap32(x1);
                e.str(x1, x0, 0, size);
                e.and_imm(x2, x2, ~u64{0x1C000});
                e.str(x2, kCpu, off(fsr_));
                break;
            case Kind::Partial: {  // memory = memory with the selected bytes of rd
                const unsigned esize = 1u << ((asi_field >> 1) & 3);
                load_reg(x1, insn & 31);
                e.and_imm(x1, x1, (1u << (8 / esize)) - 1);
                e.lsl(x1, x1, 3);
                e.load_constant(x2, reinterpret_cast<u64>(vis::partial_masks(esize, little)));
                e.ldr_index(x1, x2, x1, 8);
                load_double(x3, rd);
                swap64(x3);
                e.ldr(x4, x0, 0);
                e.eor(x3, x3, x4);
                e.and_(x3, x3, x1);
                e.eor(x4, x4, x3);
                e.str(x4, x0, 0);
                break;
            }
            case Kind::ShortLoad:  // zero-extended into the double
                e.ldr(x1, x0, 0, size);
                if (size == 2) swap16(x1);
                store_double(rd, x1);
                dirty(rd & 1);
                break;
            case Kind::ShortStore:  // the double's low byte or halfword
                load_double(x1, rd);
                if (size == 2) swap16(x1);
                e.str(x1, x0, 0, size);
                break;
            case Kind::Quad:  // rd = the doubleword at va, rd + 1 = the one at va + 8
                e.ldr(x1, x0, 0);
                swap64(x1);
                e.ldr(x2, x0, 8);
                swap64(x2);
                store_reg(rd, x1);
                store_reg(rd + 1, x2);
                break;
            case Kind::Cas: {  // compare with rs2; store rd if equal; rd = the old value
                e.ldr(x1, x0, 0, size);
                if (size == 8) swap64(x1);
                else swap32(x1);
                load_reg(x2, insn & 31);
                if (size == 4) e.mov(x2, x2, false);
                e.cmp(x1, x2);
                const Site differ = e.b(Cond::Ne);
                load_reg(x3, rd);
                if (size == 8) swap64(x3);
                else swap32(x3);
                e.str(x3, x0, 0, size);
                e.bind(differ);
                store_reg(rd, x1);
                break;
            }
        }
        out_of_line(std::move(slow), i, insn);
        return true;
    };

    // FPops that raise no exceptions (moves, conditional moves, VIS logical
    // operations) and ordered compares; the rest, an unordered compare, and
    // anything with the FPU disabled go to execute(). False if not done here.
    auto fp = [&](unsigned i, u32 insn) -> bool {
        const unsigned op3 = (insn >> 19) & 0x3F, rd = (insn >> 25) & 31, rs1 = (insn >> 14) & 31, rs2 = insn & 31;
        const unsigned opf = (insn >> 5) & 0x1FF;
        enum class F { None, Move, MoveR, MoveCc, Compare, Logic, AlignAddr, AlignData } kind = F::None;
        if (op3 == 0x34 && (opf == 0x001 || opf == 0x002 || opf == 0x005 || opf == 0x006 || opf == 0x009 || opf == 0x00A)) {
            kind = F::Move;
        } else if (op3 == 0x35) {
            const unsigned occ = (opf >> 6) & 7;
            if ((opf & 0x1F8) == 0x050 && ((opf & 3) == 1 || (opf & 3) == 2)) kind = F::Compare;
            else if (!(opf & 0x100) && ((opf & 0x1F) == 0x05 || (opf & 0x1F) == 0x06) && rcond_valid((opf >> 5) & 7))
                kind = F::MoveR;
            else if (((opf & 0x3F) == 0x01 || (opf & 0x3F) == 0x02) && occ != 5 && occ != 7) kind = F::MoveCc;
        } else if (op3 == 0x36 && opf >= 0x060 && opf <= 0x07F) {
            kind = F::Logic;
        } else if (op3 == 0x36 && (opf == 0x018 || opf == 0x01A)) {
            kind = F::AlignAddr;
        } else if (op3 == 0x36 && opf == 0x048) {
            kind = F::AlignData;
        }
        if (kind == F::None) return false;
        std::vector<Site> slow;
        fp_check(slow);
        // rd = rs2, single (one half) or double (both).
        auto copy = [&](bool dbl, u32 sign_xor, u32 sign_and) {
            const unsigned s = dbl ? dnum(rs2) : rs2, d = dbl ? dnum(rd) : rd;
            e.ldr(x1, kCpu, freg(s), 4);
            if (sign_xor) e.eor_imm(x1, x1, sign_xor, false);
            if (sign_and != 0xFFFFFFFF) e.and_imm(x1, x1, sign_and, false);
            e.str(x1, kCpu, freg(d), 4);
            if (dbl) {
                e.ldr(x1, kCpu, freg(s + 1), 4);
                e.str(x1, kCpu, freg(d + 1), 4);
            }
            dirty(dbl && (rd & 1));
        };
        switch (kind) {
            case F::Move: {  // FMOV, FNEG, FABS: cexc cleared
                const bool dbl = !(opf & 1);
                clear_cexc();
                copy(dbl, (opf & 0xC) == 0x4 ? 0x80000000 : 0, (opf & 0xC) == 0x8 ? 0x7FFFFFFF : 0xFFFFFFFF);
                break;
            }
            case F::MoveR:
            case F::MoveCc: {  // cexc cleared whether or not the move is made
                if (kind == F::MoveR) {
                    eval_rcond((opf >> 5) & 7, rs1);
                } else {
                    const unsigned occ = (opf >> 6) & 7, cond = (insn >> 14) & 15;
                    if (occ < 4) eval_fcc(cond, occ);
                    else eval_icc(cond, occ == 6);
                }
                clear_cexc();
                const Site no = e.cbz(x0, false);
                copy((opf & 3) == 2, 0, 0xFFFFFFFF);
                e.bind(no);
                break;
            }
            case F::Compare: {  // FCMP{E}{s,d}: fcc_n; invalid on a signaling NaN (FCMPE: any NaN), as the host's compares
                const bool dbl = (opf & 3) == 2;
                const unsigned fcc_n = rd & 3;
                if (dbl) {
                    load_double(x0, rs1);
                    e.fmov_from_gpr(0, x0, true);
                    load_double(x0, rs2);
                    e.fmov_from_gpr(1, x0, true);
                } else {
                    e.fldr(0, kCpu, freg(rs1), 4);
                    e.fldr(1, kCpu, freg(rs2), 4);
                }
                e.msr_fpsr(xzr);
                if (opf & 4) e.fcmpe(dbl, 0, 1);
                else e.fcmp(dbl, 0, 1);
                e.cset(x1, Cond::Mi);  // 1: rs1 < rs2
                e.cset(x2, Cond::Gt);  // 2: rs1 > rs2
                e.cset(x3, Cond::Vs);  // 3: unordered
                e.orr(x2, x2, x3);
                e.orr(x1, x1, x3);
                e.orr(x1, x1, x2, true, Shift::Lsl, 1);
                // cexc = NV if invalid (unless NVM traps: execute()), else 0; aexc gathers it.
                e.mrs_fpsr(x4);
                e.and_imm(x4, x4, 1);
                e.lsl(x4, x4, 4);
                const unsigned shift = fcc_n == 0 ? 10 : 30 + 2 * fcc_n;
                e.ldr(x2, kCpu, off(fsr_));
                const Site valid = e.cbz(x4);
                slow.push_back(e.cbnz_bit(x2, 27));  // TEM.NVM
                e.bind(valid);
                e.and_imm(x2, x2, ~((u64{3} << shift) | 0x1F));
                e.orr(x2, x2, x1, true, Shift::Lsl, shift);
                e.orr(x2, x2, x4);
                e.orr(x2, x2, x4, true, Shift::Lsl, 5);
                e.str(x2, kCpu, off(fsr_));
                break;
            }
            case F::Logic: {  // even opf the double form, odd the single
                const bool single = opf & 1;
                const unsigned size = single ? 4 : 8;
                e.ldr(x0, kCpu, freg(single ? rs1 : dnum(rs1)), size);
                e.ldr(x1, kCpu, freg(single ? rs2 : dnum(rs2)), size);
                switch ((opf - 0x060) >> 1) {
                    case 0: e.mov(x0, xzr); break;                      // FZERO
                    case 1: e.orr(x0, x0, x1), e.mvn(x0, x0); break;     // FNOR
                    case 2: e.mvn(x1, x1), e.and_(x0, x0, x1); break;    // FANDNOT2
                    case 3: e.mvn(x0, x1); break;                       // FNOT2
                    case 4: e.mvn(x0, x0), e.and_(x0, x0, x1); break;    // FANDNOT1
                    case 5: e.mvn(x0, x0); break;                       // FNOT1
                    case 6: e.eor(x0, x0, x1); break;                   // FXOR
                    case 7: e.and_(x0, x0, x1), e.mvn(x0, x0); break;    // FNAND
                    case 8: e.and_(x0, x0, x1); break;                  // FAND
                    case 9: e.eor(x0, x0, x1), e.mvn(x0, x0); break;     // FXNOR
                    case 10: break;                                     // FSRC1
                    case 11: e.mvn(x1, x1), e.orr(x0, x0, x1); break;    // FORNOT2
                    case 12: e.mov(x0, x1); break;                      // FSRC2
                    case 13: e.mvn(x0, x0), e.orr(x0, x0, x1); break;    // FORNOT1
                    case 14: e.orr(x0, x0, x1); break;                  // FOR
                    default: e.mov_imm(x0, ~u64{0}); break;             // FONE
                }
                e.str(x0, kCpu, freg(single ? rd : dnum(rd)), size);
                dirty(!single && (rd & 1));
                break;
            }
            case F::AlignAddr: {  // ALIGNADDRESS(_LITTLE): rd = the sum aligned down; GSR.ALIGN = its low bits (negated)
                load_reg(x0, rs1);
                load_reg(x1, rs2);
                e.add(x0, x0, x1);
                if (opf == 0x01A) e.sub(x2, xzr, x0);
                else e.mov(x2, x0);
                e.and_imm(x2, x2, 7);
                e.ldr(x3, kCpu, off(gsr_));
                e.and_imm(x3, x3, ~u64{7});
                e.orr(x3, x3, x2);
                e.str(x3, kCpu, off(gsr_));
                e.and_imm(x0, x0, ~u64{7});
                store_reg(rd, x0);
                break;
            }
            case F::AlignData: {  // FALIGNDATA: rs1:rs2 from byte GSR.ALIGN on
                e.ldr(x2, kCpu, off(gsr_));
                e.and_imm(x2, x2, 7);
                e.lsl(x2, x2, 3);
                load_double(x0, rs1);
                load_double(x1, rs2);
                e.lslv(x0, x0, x2);
                e.lsr(x1, x1, 1);  // (rs2 >> (64 - shift), 0 when the shift is 0)
                e.mov_imm(x3, 63);
                e.sub(x3, x3, x2);
                e.lsrv(x1, x1, x3);
                e.orr(x0, x0, x1);
                store_double(rd, x0);
                dirty(rd & 1);
                break;
            }
            case F::None: break;
        }
        out_of_line(std::move(slow), i, insn);
        return true;
    };

    // The VIS operations in vis.h, by a call to the same function the
    // interpreter uses. False if not done here.
    auto vis_call = [&](unsigned i, u32 insn) -> bool {
        const unsigned op3 = (insn >> 19) & 0x3F, rd = (insn >> 25) & 31, rs1 = (insn >> 14) & 31, rs2 = insn & 31;
        if (op3 != 0x36) return false;
        const vis::Op* o = vis::op((insn >> 5) & 0x1FF);
        if (!o) return false;
        std::vector<Site> slow;
        fp_check(slow);
        if (o->edge) {  // the condition codes as by SUBcc rs1, rs2
            load_reg(x0, rs1);
            load_reg(x1, rs2);
            e.subs(x2, x0, x1, false);
            e.mrs_nzcv(x3);
            e.subs(x2, x0, x1);
            e.mrs_nzcv(x4);
            store_ccr(x3, x4, true);
        }
        auto operand = [&](vis::Src k, unsigned r, Reg to) {
            switch (k) {
                case vis::Src::R:
                    load_reg(to, r);
                    if (o->edge) mask(to);
                    break;
                case vis::Src::S: e.ldr(to, kCpu, freg(r), 4); break;
                case vis::Src::D: load_double(to, r); break;
                case vis::Src::Gsr: e.ldr(to, kCpu, off(gsr_)); break;
                case vis::Src::Rd: load_double(to, rd); break;
                default: break;
            }
        };
        operand(o->a, rs1, x0);
        operand(o->b, rs2, x1);
        operand(o->c, rd, x2);
        flush_cache(Reload::Call);
        e.load_constant(x16, reinterpret_cast<u64>(o->fn));
        e.blr(x16);
        reload_cache(Reload::Call);
        e.ldr(kLeftReg, sp, kLeft);
        switch (o->dst) {
            case vis::Src::R: store_reg(rd, x0); break;
            case vis::Src::S:
                e.str(x0, kCpu, freg(rd), 4);
                dirty(false);
                break;
            default:
                store_double(rd, x0);
                dirty(rd & 1);
                break;
        }
        out_of_line(std::move(slow), i, insn);
        return true;
    };

    // FPops that may raise exceptions: arithmetic, square roots and
    // conversions, under the guest rounding mode (FPCR), with the
    // exceptions they raise accrued in FSR (an invalid result is SPARC's
    // default NaN). An exception whose trap is enabled, an invalid
    // conversion to an integer, a NaN operand, and nonstandard mode (FSR.NS)
    // go to execute(); so do the cases the IIi leaves unfinished to the
    // operating system or may: a subnormal operand, a result whose
    // exponent field is 0 or 1 or that underflowed, and a divide or square
    // root while the inexact (or for a divide, overflow) trap is enabled.
    // False if not done here.
    auto fp_arith = [&](unsigned i, u32 insn) -> bool {
        const unsigned op3 = (insn >> 19) & 0x3F, rd = (insn >> 25) & 31, rs1 = (insn >> 14) & 31, rs2 = insn & 31;
        const unsigned opf = (insn >> 5) & 0x1FF;
        if (op3 != 0x34) return false;
        enum class T { F32, F64, I32, I64 };
        T src, dst;
        bool two = false;  // rs1 and rs2
        switch (opf) {
            case 0x041: case 0x045: case 0x049: case 0x04D: src = dst = T::F32, two = true; break;
            case 0x042: case 0x046: case 0x04A: case 0x04E: src = dst = T::F64, two = true; break;
            case 0x069: src = T::F32, dst = T::F64, two = true; break;  // FsMULd
            case 0x029: src = dst = T::F32; break;                       // FSQRTs
            case 0x02A: src = dst = T::F64; break;                       // FSQRTd
            case 0x0C9: src = T::F32, dst = T::F64; break;               // FsTOd
            case 0x0C6: src = T::F64, dst = T::F32; break;               // FdTOs
            case 0x0C4: src = T::I32, dst = T::F32; break;               // FiTOs
            case 0x0C8: src = T::I32, dst = T::F64; break;               // FiTOd
            case 0x084: src = T::I64, dst = T::F32; break;               // FxTOs
            case 0x088: src = T::I64, dst = T::F64; break;               // FxTOd
            case 0x0D1: src = T::F32, dst = T::I32; break;               // FsTOi
            case 0x0D2: src = T::F64, dst = T::I32; break;               // FdTOi
            case 0x081: src = T::F32, dst = T::I64; break;               // FsTOx
            case 0x082: src = T::F64, dst = T::I64; break;               // FdTOx
            default: return false;
        }
        std::vector<Site> slow;
        fp_check(slow);
        e.ldr(x16, kCpu, off(fsr_));
        slow.push_back(e.cbnz_bit(x16, 22));  // NS
        const bool divide = opf == 0x04D || opf == 0x04E, root = opf == 0x029 || opf == 0x02A;
        if (divide || root) slow.push_back(e.cbnz_bit(x16, 23));  // TEM.NX
        if (divide) slow.push_back(e.cbnz_bit(x16, 26));          // TEM.OF
        // A subnormal operand: the exponent field 0 and the fraction not.
        auto subnormal = [&](unsigned r) {
            const bool dbl = src == T::F64;
            if (dbl) load_double(x2, r);
            else e.ldr(x2, kCpu, freg(r), 4);
            e.lsl(x2, x2, 1, dbl);
            e.sub_imm(x2, x2, 1, dbl);
            e.mov_imm(x3, dbl ? 0x001FFFFFFFFFFFFFull : 0x00FFFFFF);
            e.cmp(x2, x3, dbl);
            slow.push_back(e.b(Cond::Lo));
        };
        if (src == T::F32 || src == T::F64) {
            subnormal(rs2);
            if (two) subnormal(rs1);
        }
        auto load = [&](unsigned v, unsigned r) {
            if (src == T::F32) {
                e.fldr(v, kCpu, freg(r), 4);
            } else {
                load_double(x0, r);
                e.fmov_from_gpr(v, x0, true);
            }
        };
        const bool fsrc = src == T::F32 || src == T::F64;
        if (fsrc) {
            load(1, rs2);
            if (two) load(0, rs1);
            e.fcmp(src == T::F64, two ? 0 : 1, 1);
            slow.push_back(e.b(Cond::Vs));  // a NaN
        } else if (src == T::I32) {
            e.ldr(x0, kCpu, freg(rs2), 4);
        } else {
            load_double(x0, rs2);
        }
        // Rounding to nearest (FSR.RD 0), the host's own mode: the operation
        // as it is, with no change to FPCR or FPSR (whose writes are slow),
        // and its exceptions found from the result. A NaN or infinite
        // result (invalid, division by zero, overflow), one that may be
        // tiny, and a zero product, quotient or FdTOs of nonzero operands
        // (an underflow to zero) go to execute(); what is left can only be
        // inexact, which the result's exact error says: TwoSum's for a sum
        // or difference, a fused multiply-add's for a product, quotient or
        // square root, and the result converted back for a conversion.
        // x8 = cexc (NX or nothing).
        using Fop = Emitter::Fop;
        const bool dsrc = src == T::F64;
        Site soft_done{};
        {
            e.ldr(x6, kCpu, off(fsr_), 4);
            e.tst_imm(x6, 0xC0000000u, false);  // RD (bits 31:30)
            const Site other_rd = e.b(Cond::Ne);
            const bool f64 = dst == T::F64;
            // The result in v0 (a float): to execute() if NaN, infinite or
            // perhaps tiny; returns where a zero result goes.
            auto result_check = [&] {
                e.fmov_to_gpr(x2, 0, f64);
                e.lsl(x2, x2, 1, f64);  // (the sign out)
                const Site zero = e.cbz(x2, f64);
                e.mov_imm(x3, f64 ? 0xFFE0000000000000ull : 0xFF000000u);
                e.cmp(x2, x3, f64);
                slow.push_back(e.b(Cond::Hs));
                e.mov_imm(x3, f64 ? 0x0040000000000000ull : 0x02000000u);
                e.cmp(x2, x3, f64);
                slow.push_back(e.b(Cond::Lo));
                return zero;
            };
            // To execute() unless source register r (in the source format) is zero.
            auto zero_or_slow = [&](unsigned r) {
                if (dsrc) load_double(x2, r);
                else e.ldr(x2, kCpu, freg(r), 4);
                e.lsl(x2, x2, 1, dsrc);
                slow.push_back(e.cbnz(x2, dsrc));
            };
            // x8 = 1 if the error in v3 is not zero.
            auto err_inexact = [&] {
                e.fmov_to_gpr(x2, 3, f64);
                e.lsl(x2, x2, 1, f64);
                e.cmp_imm(x2, 0, f64);
                e.cset(x8, Cond::Ne);
            };
            // A result whose zero needs `check` (exact then); `nonzero` otherwise.
            auto zero_or = [&](Site zero, auto check, auto nonzero) {
                nonzero();
                const Site done = e.b();
                e.bind(zero);
                check();
                e.mov_imm(x8, 0);
                e.bind(done);
            };
            switch (opf) {
                case 0x041: case 0x042: case 0x045: case 0x046: {  // FADD, FSUB
                    const bool add = opf <= 0x042;
                    e.fmov(dsrc, 2, 0);
                    e.fop(add ? Fop::Add : Fop::Sub, dsrc, 0, 0, 1);
                    e.bind(result_check());
                    e.fop(Fop::Sub, dsrc, 3, 0, 2);  // bv = s - a
                    e.fop(Fop::Sub, dsrc, 4, 0, 3);  // av = s - bv
                    e.fop(Fop::Sub, dsrc, 2, 2, 4);  // ar = a - av
                    e.fop(add ? Fop::Sub : Fop::Add, dsrc, 3, 3, 1);  // the other part, negated: bv - b (bv + b for a difference)
                    e.fcmp(dsrc, 2, 3);              // exact if they are equal
                    e.cset(x8, Cond::Ne);
                    break;
                }
                case 0x049: case 0x04A: {  // FMUL
                    e.fmov(dsrc, 2, 0);
                    e.fop(Fop::Mul, dsrc, 0, 0, 1);
                    const Site zero = result_check();
                    zero_or(zero, [&] { zero_or_slow(rs1); zero_or_slow(rs2); }, [&] {
                        e.fnmsub(dsrc, 3, 2, 1, 0);  // a * b - p
                        err_inexact();
                    });
                    break;
                }
                case 0x04D: case 0x04E: {  // FDIV
                    e.fmov(dsrc, 2, 0);
                    e.fop(Fop::Div, dsrc, 0, 0, 1);
                    const Site zero = result_check();
                    zero_or(zero, [&] { zero_or_slow(rs1); }, [&] {
                        e.fmsub(dsrc, 3, 0, 1, 2);  // a - q * b
                        err_inexact();
                    });
                    break;
                }
                case 0x029: case 0x02A: {  // FSQRT
                    e.fsqrt(dsrc, 0, 1);
                    const Site zero = result_check();
                    zero_or(zero, [] {}, [&] {
                        e.fmsub(dsrc, 3, 0, 0, 1);  // b - r * r
                        err_inexact();
                    });
                    break;
                }
                case 0x069:  // FsMULd: exact
                    e.fcvt_to_double(0, 0);
                    e.fcvt_to_double(1, 1);
                    e.fop(Fop::Mul, true, 0, 0, 1);
                    e.bind(result_check());
                    e.mov_imm(x8, 0);
                    break;
                case 0x0C9:  // FsTOd: exact
                    e.fcvt_to_double(0, 1);
                    e.bind(result_check());
                    e.mov_imm(x8, 0);
                    break;
                case 0x0C6: {  // FdTOs
                    e.fcvt_to_single(0, 1);
                    const Site zero = result_check();
                    zero_or(zero, [&] { zero_or_slow(rs2); }, [&] {
                        e.fcvt_to_double(3, 0);  // back to double
                        e.fcmp(true, 3, 1);
                        e.cset(x8, Cond::Ne);
                    });
                    break;
                }
                case 0x0C4: case 0x0C8: case 0x084: case 0x088: {  // FiTOs, FiTOd, FxTOs, FxTOd
                    const bool w = src == T::I64;
                    e.scvtf(f64, 0, x0, w);
                    e.fcvt_to_int(f64, 1, x2, 0, w);  // back (exact for a whole number)
                    e.mov_imm(x3, w ? 0x7FFFFFFFFFFFFFFFull : 0x7FFFFFFFu);
                    e.cmp(x2, x3, w);
                    slow.push_back(e.b(Cond::Eq));  // (perhaps saturated: rounded up past the largest)
                    e.cmp(x2, x0, w);
                    e.cset(x8, Cond::Ne);
                    break;
                }
                default: {  // FsTOi, FdTOi, FsTOx, FdTOx (toward zero)
                    const bool w = dst == T::I64;
                    e.fcvt_to_int(dsrc, 1, x0, 1, w);
                    // The host saturates: its largest and least values may be out of range (invalid).
                    e.mov_imm(x3, w ? 0x7FFFFFFFFFFFFFFFull : 0x7FFFFFFFu);
                    e.cmp(x0, x3, w);
                    slow.push_back(e.b(Cond::Eq));
                    e.mov_imm(x3, w ? 0x8000000000000000ull : 0x80000000u);
                    e.cmp(x0, x3, w);
                    slow.push_back(e.b(Cond::Eq));
                    e.scvtf(dsrc, 3, x0, w);
                    e.fcmp(dsrc, 3, 1);
                    e.cset(x8, Cond::Ne);
                    break;
                }
            }
            soft_done = e.b();
            e.bind(other_rd);
        }
        // The guest's FPCR: the host's with RMode (bits 23:22) from FSR.RD
        // (nearest, zero, +inf, -inf as AArch64 0, 3, 1, 2), the flags clear.
        e.mrs_fpcr(x5);
        e.str(x5, sp, kFpcr);
        e.ldr(x6, kCpu, off(fsr_), 4);
        e.ubfx(x6, x6, 30, 2, false);
        e.lsl(x6, x6, 1, false);
        e.mov_imm32(x7, 0x9C);
        e.lsrv(x7, x7, x6, false);
        e.and_imm(x7, x7, 3, false);
        e.and_imm(x5, x5, ~(u64{3} << 22));
        e.orr(x5, x5, x7, true, Shift::Lsl, 22);
        e.msr_fpcr(x5);
        e.msr_fpsr(xzr);
        switch (opf) {
            case 0x041: case 0x042: e.fop(Fop::Add, dsrc, 0, 0, 1); break;
            case 0x045: case 0x046: e.fop(Fop::Sub, dsrc, 0, 0, 1); break;
            case 0x049: case 0x04A: e.fop(Fop::Mul, dsrc, 0, 0, 1); break;
            case 0x04D: case 0x04E: e.fop(Fop::Div, dsrc, 0, 0, 1); break;
            case 0x069:  // the exact product
                e.fcvt_to_double(0, 0);
                e.fcvt_to_double(1, 1);
                e.fop(Fop::Mul, true, 0, 0, 1);
                break;
            case 0x029: case 0x02A: e.fsqrt(dsrc, 0, 1); break;
            case 0x0C9: e.fcvt_to_double(0, 1); break;
            case 0x0C6: e.fcvt_to_single(0, 1); break;
            case 0x0C4: case 0x0C8: case 0x084: case 0x088: e.scvtf(dst == T::F64, 0, x0, src == T::I64); break;
            default: e.fcvt_to_int(dsrc, 1, x0, 1, dst == T::I64); break;  // toward zero
        }
        e.mrs_fpsr(x6);
        e.ldr(x5, sp, kFpcr);
        e.msr_fpcr(x5);
        const bool to_int = dst == T::I32 || dst == T::I64;
        if (to_int) slow.push_back(e.cbnz_bit(x6, 0));  // (an invalid conversion's result is not the host's)
        // A result that may be in the subnormal window: underflowed, or an
        // exponent field of 0 or 1 but not zero.
        if ((opf >= 0x041 && opf <= 0x04E) || opf == 0x0C6) {
            const bool dbl = dst == T::F64;
            slow.push_back(e.cbnz_bit(x6, 3));  // UFC
            e.fmov_to_gpr(x2, 0, dbl);
            e.lsl(x2, x2, 1, dbl);
            e.sub_imm(x2, x2, 1, dbl);
            e.mov_imm(x3, dbl ? 0x003FFFFFFFFFFFFFull : 0x01FFFFFF);
            e.cmp(x2, x3, dbl);
            slow.push_back(e.b(Cond::Lo));
        }
        // x8 = the exceptions as cexc bits.
        e.and_imm(x6, x6, 0x1F);
        e.load_constant(x8, reinterpret_cast<u64>(kFpsrExc.v));
        e.ldr_index(x8, x8, x6, 1);
        if (!to_int) {  // invalid: the default NaN
            const Site valid = e.cbz_bit(x6, 0);
            e.mov_imm(x5, dst == T::F64 ? 0x7FFFFFFFFFFFFFFFull : 0x7FFFFFFF);
            e.fmov_from_gpr(0, x5, dst == T::F64);
            e.bind(valid);
        }
        e.bind(soft_done);
        // To execute() if any traps.
        e.ldr(x7, kCpu, off(fsr_));
        e.lsr(x5, x7, 23);  // TEM
        e.and_(x5, x5, x8);
        slow.push_back(e.cbnz(x5));
        // cexc = the exceptions; aexc gathers them.
        e.and_imm(x7, x7, ~u64{0x1F});
        e.orr(x7, x7, x8);
        e.orr(x7, x7, x8, true, Shift::Lsl, 5);
        e.str(x7, kCpu, off(fsr_));
        switch (dst) {
            case T::F32: e.fstr(0, kCpu, freg(rd), 4); break;
            case T::F64: e.fmov_to_gpr(x0, 0, true), store_double(rd, x0); break;
            case T::I32: e.str(x0, kCpu, freg(rd), 4); break;
            case T::I64: store_double(rd, x0); break;
        }
        dirty((dst == T::F64 || dst == T::I64) && (rd & 1));
        out_of_line(std::move(slow), i, insn);
        return true;
    };

    // TICK as read at instruction i, into x0 (bits 62:0): the step count
    // then, through the CPU's time base and charge per step. Clobbers x1 and x16.
    auto tick = [&](unsigned i) {
        e.ldr(x0, sp, kCycles0);
        total_into(x1);
        e.add(x0, x0, x1);
        if (i) e.add_imm(x0, x0, i);
        e.ldr(x1, kCpu, off(step_base_));
        e.sub(x0, x0, x1);
        e.ldr(x1, kCpu, off(cpi_fx_));
        e.mul(x0, x0, x1);
        e.lsr(x0, x0, 16);
        e.ldr(x1, kCpu, off(time_base_));
        e.add(x0, x0, x1);
        e.ldr(x1, kCpu, off(tick_offset_));
        e.add(x0, x0, x1);
        e.and_imm(x0, x0, 0x7FFFFFFFFFFFFFFFull);
    };
    // Instructions that need their place in the block: SAVE and RESTORE
    // (spill, fill and clean-window traps go to execute()), reading TICK,
    // and the ancillary registers. False if not done here.
    auto special = [&](unsigned i, u32 insn) -> bool {
        const unsigned op3 = (insn >> 19) & 0x3F, rd = (insn >> 25) & 31, rs1 = (insn >> 14) & 31;
        if (op3 == 0x3C || op3 == 0x3D) {  // SAVE, RESTORE
            const bool save = op3 == 0x3C;
            self = false;  // (the check mode compares registers only)
            std::vector<Site> slow;
            if (save) {
                e.ldr(x0, kCpu, off(cansave_), 4);
                slow.push_back(e.cbz(x0, false));  // spill
                e.ldr(x0, kCpu, off(cleanwin_), 4);
                e.ldr(x1, kCpu, off(canrestore_), 4);
                e.cmp(x0, x1, false);
                slow.push_back(e.b(Cond::Eq));  // clean_window
            } else {
                e.ldr(x0, kCpu, off(canrestore_), 4);
                slow.push_back(e.cbz(x0, false));  // fill
            }
            load_reg(x3, rs1);  // the sum, in the old window
            load_op2(x1, insn);
            e.add(x3, x3, x1);
            move_window(save);
            store_reg(rd, x3);
            out_of_line(std::move(slow), i, insn);
            return true;
        }
        const bool priv = pstate_ & pstate::PRIV;  // (part of the mode)
        if ((op3 == 0x2A || op3 == 0x28) && rs1 == 4 && (fmode == 1 || fmode == 2)) {  // RDPR, RD %tick
            self = false;  // (TICK follows cycles_, which the check mode does not)
            if (rd == 0) return true;
            count_effect(x0);
            tick(i);
            e.ldr(x1, kCpu, off(tick_npt_), 1);
            e.orr(x0, x0, x1, true, Shift::Lsl, 63);
            store_reg(rd, x0);
            return true;
        }
        if (op3 == 0x32 && rd == 8 && priv && !dynamic) {  // WRPR %pil
            // A lower PIL may let a level interrupt in (one SOFTINT
            // asks for): then out, for run_blocks() to take it before the
            // next instruction. (Interrupt vectors do not depend on PIL.)
            self = false;
            load_reg(x0, rs1);
            load_op2(x1, insn);
            e.eor(x0, x0, x1);
            e.and_imm(x0, x0, 15);
            e.str(x0, kCpu, off(pil_), 4);
            e.ldr(x1, kCpu, off(softint_));
            const Site none = e.cbz(x1);
            store_pc_npc(4 * static_cast<s64>(i) + 4);
            leave(i + 1);
            e.bind(none);
            return true;
        }
        if (op3 == 0x2A && priv && rs1 != 4) {  // RDPR (the trap registers need TL > 0)
            static_assert(sizeof(TrapLevel) == 32);
            const bool trap_level = rs1 <= 3;
            if (trap_level && tl_ == 0) return false;  // (TL > 0 is part of the mode)
            if (rs1 > 14 && rs1 != 31) return false;
            if (rd == 0) return true;
            if (trap_level) {
                e.ldr(x0, kCpu, off(tl_), 4);
                e.add(x0, kCpu, x0, true, Shift::Lsl, 5);
                const s64 field = rs1 == 0 ? offsetof(TrapLevel, tpc) : rs1 == 1 ? offsetof(TrapLevel, tnpc)
                                  : rs1 == 2 ? offsetof(TrapLevel, tstate) : offsetof(TrapLevel, tt);
                e.add_imm(x0, x0, off(trap_[0]) + field);
                e.ldr(x0, x0, 0, rs1 == 3 ? 4 : 8);
            } else {
                switch (rs1) {
                    case 5: e.ldr(x0, kCpu, off(tba_)); break;
                    case 6: e.ldr(x0, kCpu, off(pstate_), 2); break;
                    case 7: e.ldr(x0, kCpu, off(tl_), 4); break;
                    case 8: e.ldr(x0, kCpu, off(pil_), 4); break;
                    case 9: e.ldr(x0, kCpu, off(cwp_), 4); break;
                    case 10: e.ldr(x0, kCpu, off(cansave_), 4); break;
                    case 11: e.ldr(x0, kCpu, off(canrestore_), 4); break;
                    case 12: e.ldr(x0, kCpu, off(cleanwin_), 4); break;
                    case 13: e.ldr(x0, kCpu, off(otherwin_), 4); break;
                    case 14: e.ldr(x0, kCpu, off(wstate_), 4); break;
                    default: e.ldr(x0, kCpu, off(opt_.version)); break;
                }
            }
            store_reg(rd, x0);
            return true;
        }
        if (op3 == 0x28 && rs1 == 4 && !priv) {  // RD %tick: privileged_action if TICK.NPT
            self = false;
            e.ldr(x0, kCpu, off(tick_npt_), 1);
            const Site npt = e.cbnz(x0, false);
            if (rd) {
                count_effect(x0);
                tick(i);
                store_reg(rd, x0);
            }
            out_of_line({npt}, i, insn);
            return true;
        }
        if ((op3 == 0x28 && rs1 == 19) || (op3 == 0x30 && rd == 19)) {  // RD, WR %gsr: fp_disabled from execute()
            std::vector<Site> to;
            fp_check(to);
            if (op3 == 0x28) {
                if (rd) {
                    e.ldr(x0, kCpu, off(gsr_));
                    store_reg(rd, x0);
                }
            } else {
                load_reg(x0, rs1);
                load_op2(x1, insn);
                e.eor(x0, x0, x1);
                e.and_imm(x0, x0, 0x7F);
                e.str(x0, kCpu, off(gsr_));
            }
            out_of_line(std::move(to), i, insn);
            return true;
        }
        if (op3 == 0x28) {  // RDASR: Y, CCR, ASI, PC, FPRS; STBAR and MEMBAR
            if (rs1 == 15 && rd == 0) return true;  // memory is always ordered here
            switch (rs1) {
                case 0: e.ldr(x0, kCpu, off(y_)); break;
                case 2:
                    ccr_ready();
                    e.ldr(x0, kCpu, o_ccr, 1);
                    break;
                case 3: e.ldr(x0, kCpu, off(asi_), 1); break;
                case 5:
                    pc_plus(x0, 4 * static_cast<s64>(i));
                    mask(x0);
                    break;
                case 6: e.ldr(x0, kCpu, off(fprs_), 1); break;
                case 16:  // PCR, DCR, SOFTINT, TICK_CMPR: privileged here (PIC, which counts, through execute())
                case 18:
                case 22:
                case 23: {
                    if (!priv) return false;
                    const u64& r = rs1 == 16 ? pcr_ : rs1 == 18 ? dcr_ : rs1 == 22 ? softint_ : tick_cmpr_;
                    e.ldr(x0, kCpu, off(r));
                    break;
                }
                default: return false;
            }
            store_reg(rd, x0);
            return true;
        }
        // WR %y, %ccr, %asi, %fprs: rs1 xor operand 2 (%pcr, %pic and %dcr,
        // which count or keep only their fields, through execute())
        if (op3 == 0x30 && (rd == 0 || rd == 2 || rd == 3 || rd == 6)) {
            load_reg(x0, rs1);
            load_op2(x1, insn);
            e.eor(x0, x0, x1);
            if (rd == 0) {
                e.mov(x0, x0, false);
                e.str(x0, kCpu, off(y_));
            } else if (rd == 6) {
                e.and_imm(x0, x0, 7);
                e.str(x0, kCpu, off(fprs_), 1);
            } else if (rd == 2) {
                write_ccr(x0);
            } else {
                e.str(x0, kCpu, off(asi_), 1);
            }
            return true;
        }
        if (op3 == 0x3B) return true;  // FLUSH: stores to code are seen at once here
        if (op3 == 0x36 && ((insn >> 5) & 0x1FF) == 0x080 && priv) {  // SHUTDOWN: nothing to do (fp_disabled from execute())
            std::vector<Site> to;
            fp_check(to);
            out_of_line(std::move(to), i, insn);
            return true;
        }
        if (op3 == 0x2B) {  // FLUSHW: a spill trap (from execute()) unless only the current window is in use
            e.ldr(x0, kCpu, off(cansave_), 4);
            e.cmp_imm(x0, kWindows - 2, false);
            out_of_line({e.b(Cond::Ne)}, i, insn);
            return true;
        }
        if (op3 == 0x3A) {  // Tcc: the trap from execute()
            const unsigned cc = (insn >> 11) & 3, cond = (insn >> 25) & 15;
            if ((cc & 1) || cond == 8) return false;
            if (cond == 0) return true;
            self = false;
            out_of_line({e.b(icc_cond(cond, cc == 2, static_cast<int>(i)))}, i, insn);
            return true;
        }
        if (op3 == 0x31 && priv && rd <= 1) {  // SAVED, RESTORED
            auto step = [&](const auto& field, unsigned by) {
                e.ldr(x0, kCpu, off(field), 4);
                e.add_imm(x0, x0, by, false);
                e.and_imm(x0, x0, kWindows - 1, false);
                e.str(x0, kCpu, off(field), 4);
            };
            if (rd == 0) {
                step(cansave_, 1);
            } else {
                step(canrestore_, 1);
                e.ldr(x0, kCpu, off(cleanwin_), 4);
                e.cmp_imm(x0, kWindows - 1, false);
                e.csinc(x0, x0, x0, Cond::Hs, false);  // up by one unless already at the limit
                e.str(x0, kCpu, off(cleanwin_), 4);
            }
            e.ldr(x0, kCpu, off(otherwin_), 4);
            const Site other = e.cbnz(x0, false);
            step(rd == 0 ? canrestore_ : cansave_, kWindows - 1);
            const Site done = e.b();
            e.bind(other);
            step(otherwin_, kWindows - 1);
            e.bind(done);
            return true;
        }
        return false;
    };

    // Prologue: the callee-saved registers, then the entry counts.
    prologue(e);
    e.mov(kCpu, x0);
    e.mov(kPc, x1);
    e.str(x2, sp, kBudget);
    e.str(x2, sp, kLeft);
    e.ldr(x0, kCpu, off(cycles_));
    e.str(x0, sp, kCycles0);
    e.ldr(x0, kCpu, off(instret_));
    e.str(x0, sp, kInstret0);
    load_bases();
    // Blocks going on from another enter here, so everything before must be
    // the same in every translation.
    if (jit_link_offset_ == 0) jit_link_offset_ = e.size();
    if (e.size() != jit_link_offset_) return false;
    // The block must fit in the budget (blocks going on from another are
    // not looked at before).
    e.ldr(kLeftReg, sp, kLeft);
    e.cmp_imm(kLeftReg, n);
    const Site no_room = e.b(Cond::Lt);
    for (unsigned slot = 0; slot < cache_used; ++slot) {  // (those read before they are written)
        if (!self_loop && !cache_load[slot]) continue;
        const RegMem m = reg_mem(cached[slot]);
        e.ldr(kCache[slot], m.base, m.off);
        cache_valid |= 1u << slot;
    }

    bool last_executed = false;  // the last instruction went to execute()
    bool ended = false;          // the code for the end of the block is out
    auto translate = [&](unsigned i) {
        const u32 insn = cur->insns[i];
        const s64 here = 4 * static_cast<s64>(i);
        const unsigned op = insn >> 30;
        const bool was_dynamic = dynamic;
        if (!was_dynamic) known_target = false;
        pre_dirty = cache_dirty;
        bool done = false;
        if (delayed_cti(insn)) {
            done = cti(i, insn);
        } else if (op == 0 && ((insn >> 22) & 7) == 4) {  // SETHI
            const unsigned rd = (insn >> 25) & 31;
            if (rd) {
                const Reg d = def_reg(rd, x0);
                e.mov_imm32(d, (insn & 0x3FFFFF) << 10);
                put_reg(rd, d);
            }
            done = true;
        } else if (op == 2) {
            done = fp(i, insn) || vis_call(i, insn) || fp_arith(i, insn) || special(i, insn) || alu(i, insn);
        } else if (op == 3) {
            done = mem(i, insn);
        }
        if (done) {
            if (was_dynamic) {
                // A delay slot: pc goes to kNpc.
                dynamic = false;
                if (i + 1 < n) {
                    npc_ready();
                    pc_plus(x0, here + 4);
                    e.cmp(x0, kNpc);
                    const Site on = e.b(Cond::Eq);
                    store_pc_npc_from(kNpc);
                    leave(i + 1);
                    e.bind(on);
                } else if (known_target) {
                    if (may_fall) {
                        pc_plus(x0, target);
                        mask(x0);
                        e.cmp(x0, kNpc);
                        const Site fell = e.b(Cond::Ne);
                        chain_to(target, n);
                        e.bind(fell);
                        chain_to(fall, n);
                    } else {
                        chain_to(target, n);
                    }
                    ended = true;
                } else {
                    e.mov(x2, kNpc);
                    chain(n);
                    ended = true;
                }
                npc_lazy = false;
            }
            return;
        }
        // execute() runs it.
        last_executed = i + 1 == n;
        call_execute(i, insn);
        cache_valid = cache_all, cache_dirty = 0;  // (all loaded again)
        codes = Cc::Valid;  // (the stub worked the codes out, and the instruction may change them)
        dynamic = delayed_cti(insn);
        known_target = false, npc_lazy = false;
        if (dynamic) e.ldr(kNpc, kCpu, o_npc);
    };
    // The end of the block.
    auto finish = [&] {
        if (ended) {
        } else if (last_executed) {
            // execute() left pc_ and npc_ in place.
            e.ldr(x2, kCpu, o_pc);
            e.ldr(x0, kCpu, o_npc);
            e.sub(x0, x0, x2);
            e.cmp_imm(x0, 4);
            const Site sequential = e.b(Cond::Eq);
            leave(n);
            e.bind(sequential);
            chain(n);
        } else if (dynamic) {  // the last instruction was a control transfer: its delay slot is next
            npc_ready();
            pc_plus(x0, 4 * static_cast<s64>(n));
            e.str(x0, kCpu, o_pc);
            e.str(kNpc, kCpu, o_npc);
            leave(n);
        } else {
            chain_to(4 * static_cast<s64>(n), n);
        }
    };
    for (unsigned k = 0; k < regions; ++k) {
        if (k) {  // the next block, entered with every register in the cache and nothing known of the codes
            cur = region[k], cur_off = region_off[k], n = cur->length, page_off = first_page_off + cur_off;
            cache_valid = cache_all, cache_dirty = label_dirty[k], codes = Cc::Unknown, flags_at = -1;
            dynamic = known_target = npc_lazy = last_executed = ended = false;
        }
        if (!k) cache_dirty = label_dirty[0];  // (none when entered from outside, more coming round)
        body[k] = e.size();
        for (unsigned i = 0; i < n; ++i) translate(i);
        finish();
        if (split) {  // the branch's other way
            e.bind(split->taken);
            cache_valid = split->valid, cache_dirty = split->dirty, codes = split->codes;
            if (split->annulled) {
                chain_to(split->target, split->count);
            } else {
                dynamic = known_target = true, may_fall = false, target = split->target;
                set_npc(target);
                last_executed = ended = false;
                translate(n - 1);
                finish();
            }
            split.reset();
        }
    }
    for (const Ahead& a : ahead) e.bind_to(a.site, body[a.to]);
    for (Cold& c : cold) {  // (as the code had things where each was asked for)
        for (const Site& site : c.sites) e.bind(site);
        dynamic = c.dynamic, npc_lazy = c.npc_lazy, npc_off = c.npc_off;
        pre_dirty = c.pre_dirty, cache_valid = c.valid, cache_dirty = c.dirty;
        call_execute(c.i, c.insn);
        if (c.then == Then::Npc) e.ldr(kNpc, kCpu, o_npc);
        if (c.then == Then::Leave) leave(c.i + 1);
        else e.b_to(c.back);
    }
    e.bind(no_room);
    store_pc_npc(0);
    total_into(x0);
    for (const Site& site : exits) e.bind(site);
    e.bl_abs(static_cast<const u8*>(jit_stub_ccr_), base);  // the codes, for what follows in C++
    epilogue(e);
    e.finish_literals();
    if (cache_miss || !e.in_reach()) return 0;  // (the block runs in run_blocks() instead)
    for (unsigned k = 0; k < regions; ++k)
        if (edge_dirty[k] & ~label_dirty[k]) {  // (to be made again: none of this pass's links are used)
            jit_links_used_ = links_before;
            return 2;
        }

    const void* code = jit_buf_->add(e.code().data(), e.size());
    if (!code) {
        // The buffer is full: start again with an empty one, the stubs
        // first, and translate the block again for its new place.
        for (Block& other : blocks_) other.code = nullptr, other.runs = 0;
        jit_buf_->clear();
        jit_links_used_ = 0;
        ++jit_flushes_;
        jit_stub_dyn_ = jit_stub_seq_ = jit_stub_ccr_ = nullptr;
        if (!jit_make_stubs() || jit_buf_->next() == base) return false;  // (nothing more can be done)
        return jit_compile(b) ? 1 : 0;
    }
    b.code = reinterpret_cast<JitFn>(const_cast<void*>(code));
    b.jit_mode = mode;
    b.jit_self = self;
    ++jit_compiles_;
    return true;
}

}  // namespace uv::usparc

#endif
