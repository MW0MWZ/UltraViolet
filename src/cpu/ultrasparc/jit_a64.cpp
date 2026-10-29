// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
// UltraSPARC IIi dynamic recompiler, AArch64 back end: the same translation
// as the x86-64 one (jit_x64.cpp, which describes it), for AArch64 hosts.
//
// Guest registers are reached through three host registers holding the
// current global set, the current window's outs, and its ins (its locals
// follow them); they are loaded again after every call to execute(), which
// may change the window or the global set.
#include "cpu/ultrasparc/ultrasparc.h"

#if defined(__aarch64__) || defined(_M_ARM64)

#include <array>
#include <cstddef>
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
// and instret_ as they were on entry, the instructions started in the
// blocks before the current one (when one went on into another), the budget
// given on entry, and the host's FPCR around an FPop.
constexpr s32 kCycles0 = 64, kInstret0 = 72, kTotal = 80, kBudget = 88, kFpcr = 96;
constexpr s32 kFrame = 112;

void prologue(Emitter& e) {
    e.stp_pre(x29, x30, sp, -kFrame);
    e.stp(x19, x20, sp, 16);
    e.stp(x21, x22, sp, 32);
    e.stp(x23, x24, sp, 48);
}
void epilogue(Emitter& e) {  // (sp at the frame)
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
    // After execute(): back into the translation, unless the block ends
    // there. After a delay slot (`sequential` false) the translation itself
    // goes on to the next pc (a delay slot ends its block); after any other
    // instruction it goes on to the next one, so the pc must be that.
    auto body = [&](bool sequential) {
        e.stp_pre(x30, x9, sp, -16);
        e.ldr(x1, sp, kUp + kTotal);
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
        e.ldp_post(x30, x9, sp, 16);
        e.ret();
        for (const Site& site : out) e.bind(site);
        e.ldr(x9, sp, 8);
        e.ldr(x0, sp, kUp + kTotal);
        e.add(x0, x0, x9);
        e.add_imm(x0, x0, 1);
        e.add_imm(sp, sp, kUp);
        epilogue(e);
    };
    const std::size_t dyn = 0;  // npc in kNpc
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
    e.finish_literals();
    const auto* code = static_cast<const u8*>(jit_buf_->add(e.code().data(), e.size()));
    if (!code) return false;
    jit_stub_dyn_ = code + dyn;
    jit_stub_seq_ = code + seq;
    return true;
}

bool UltraSparc::jit_compile(Block& b) {
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
    auto load_reg = [&](Reg dst, unsigned r) {
        if (r == 0) {
            e.mov(dst, xzr);
        } else {
            const RegMem m = reg_mem(r);
            e.ldr(dst, m.base, m.off);
        }
    };
    auto store_reg = [&](unsigned r, Reg src) {
        if (r == 0) return;
        const RegMem m = reg_mem(r);
        e.str(src, m.base, m.off);
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
        else e.mov(r, kPc);
    };
    auto mask = [&](Reg r) {
        if (am) e.mov(r, r, false);
    };

    std::vector<Site> exits;
    auto leave = [&](u32 started) {  // with this block's first `started` instructions started
        e.ldr(x0, sp, kTotal);
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
    const unsigned n = b.length;

    // Going on into the next block, as in jit_x64.cpp: chain() takes the
    // next pc in x2, chain_to() an offset from this block's pc.
    const u8 mode = static_cast<u8>(jit_mode());
    const u16 fmode_fetch = fetch_mode();
    const s64 page_off = static_cast<s64>(pc_ & 0x1FFF);  // (the same for every virtual alias)
    static_assert(sizeof(FetchEntry) == 32);
    static_assert(offsetof(FetchEntry, va_page) == 0 && offsetof(FetchEntry, gen) == 8 && offsetof(FetchEntry, mode) == 12 &&
                  offsetof(FetchEntry, host) == 16);
    // Jumps to the translation in x8 for the block at x2 if its Block (x5)
    // is for the same mode and fits in the budget; else to `no`.
    auto enter = [&](std::vector<Site>& no) {
        e.ldr(x6, x5, static_cast<s64>(offsetof(Block, jit_mode)), 1);
        e.cmp_imm(x6, mode, false);
        no.push_back(e.b(Cond::Ne));
        e.ldr(x6, x5, static_cast<s64>(offsetof(Block, length)), 4);
        e.ldr(x7, sp, kTotal);
        e.add(x6, x6, x7);
        e.ldr(x7, sp, kBudget);
        e.cmp(x6, x7);
        no.push_back(e.b(Cond::Hi));
        e.mov(kPc, x2);
        e.add_imm(x8, x8, static_cast<s64>(jit_link_offset_));
        e.br(x8);
    };
    auto add_total = [&](unsigned count) {
        e.ldr(x1, sp, kTotal);
        e.add_imm(x1, x1, count);
        e.str(x1, sp, kTotal);
    };
    // (`count`: how many of this block's instructions were started.)
    auto chain = [&](unsigned count) {
        add_total(count);
        std::vector<Site> no;
        e.eor(x3, x2, kPc);
        e.lsr(x3, x3, 13);
        const Site other_page = e.cbnz(x3);
        e.sub(x3, x2, kPc);  // its host address
        e.load_constant(x4, reinterpret_cast<u64>(b.host));
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
        e.ubfx(x5, x3, 2, kBlockBits);  // its slot
        e.mov_imm(x6, sizeof(Block));
        e.load_constant(x7, reinterpret_cast<u64>(blocks_.data()));
        e.madd(x5, x5, x6, x7);
        e.ldr(x6, x5, static_cast<s64>(offsetof(Block, host)));
        e.cmp(x6, x3);
        no.push_back(e.b(Cond::Ne));
        e.ldr(x8, x5, static_cast<s64>(offsetof(Block, code)));
        no.push_back(e.cbz(x8));
        enter(no);
        for (const Site& site : no) e.bind(site);
        store_pc_npc_from(x2);
        e.ldr(x0, sp, kTotal);
        exits.push_back(e.b());
    };
    std::size_t body = 0;  // where the translation of this block's instructions starts
    auto chain_to = [&](s64 delta, unsigned count) {
        if (page_off + delta < 0 || page_off + delta >= 0x2000) {
            pc_plus(x2, delta);
            mask(x2);  // (a target in the same page needs none)
            chain(count);
            return;
        }
        add_total(count);
        std::vector<Site> no;
        // The back edge of a delay loop (to this block's start, or to an
        // earlier block of a polling loop ending here): the iterations that
        // fit at once.
        const s64 loop_back = (b.loop >> 30) == 3 ? -4 * static_cast<s64>((b.loop >> 16) & 0x3FFF) : 0;
        if (b.loop && delta == loop_back && count == n) {
            e.mov(x0, kCpu);
            e.mov_imm(x1, b.loop);
            e.ldr(x2, sp, kBudget);
            e.ldr(x4, sp, kTotal);
            e.sub(x2, x2, x4);
            e.ldr(x3, sp, kCycles0);  // the next instruction's step
            e.add(x3, x3, x4);
            e.load_constant(x16, reinterpret_cast<u64>(&jit_loop_at_once));
            e.blr(x16);
            e.ldr(x1, sp, kTotal);
            const Site none = e.cbz(x0);
            e.add(x1, x1, x0);
            e.str(x1, sp, kTotal);
            e.ldr(x5, kCpu, off(delay_charged_), 1);  // charged other than the usual rate: out (so the check below leaves)
            const Site usual = e.cbz(x5);
            e.str(x1, sp, kBudget);
            e.bind(usual);
            e.bind(none);
        }
        if (delta == 0 && count == n) {
            e.add_imm(x1, x1, n);
            e.ldr(x3, sp, kBudget);
            e.cmp(x1, x3);
            no.push_back(e.b(Cond::Hi));
            e.b_to(body);
        } else {
            const u8* host = b.host + delta;
            e.load_constant(x5, reinterpret_cast<u64>(&blocks_[block_index(host)]));
            e.load_constant(x3, reinterpret_cast<u64>(host));
            e.ldr(x6, x5, static_cast<s64>(offsetof(Block, host)));
            e.cmp(x6, x3);
            no.push_back(e.b(Cond::Ne));
            e.ldr(x8, x5, static_cast<s64>(offsetof(Block, code)));
            no.push_back(e.cbz(x8));
            pc_plus(x2, delta);
            enter(no);
        }
        for (const Site& site : no) e.bind(site);
        store_pc_npc(delta);
        e.ldr(x0, sp, kTotal);
        exits.push_back(e.b());
    };
    bool self = true;  // no instruction goes to execute()

    // Instruction i goes to execute() (through the stub, which leaves the
    // translation if run_blocks() would end the block there; otherwise the
    // window and global set are loaded again).
    auto call_execute = [&](unsigned i, u32 insn) {
        self = false;
        e.mov_imm32(x9, i);
        e.mov_imm32(x10, insn);
        e.bl_abs(static_cast<const u8*>(dynamic ? jit_stub_dyn_ : jit_stub_seq_), base);
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
    auto store_ccr = [&](Reg f32, Reg f64, bool sub) {
        ccr_into(f32, f64, sub);
        e.str(f64, kCpu, o_ccr, 1);
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
    // w0 = 1 if icc/xcc condition `cond` holds, else 0. Clobbers x1.
    auto eval_icc = [&](unsigned cond, bool xcc) {
        e.ldr(x0, kCpu, o_ccr, 1);
        if (xcc) e.lsr(x0, x0, 4, false);
        e.and_imm(x0, x0, 15, false);
        e.mov_imm32(x1, icc_table[cond]);
        e.lsrv(x0, x1, x0, false);
        e.and_imm(x0, x0, 1, false);
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
            load_reg(x0, rs1);
            load_op2(x1, insn);
            if (kind == 0 || kind == 4) {
                if (cc) {
                    if (kind == 0) e.adds(x2, x0, x1, false);
                    else e.subs(x2, x0, x1, false);
                    e.mrs_nzcv(x3);
                    if (kind == 0) e.adds(x0, x0, x1);
                    else e.subs(x0, x0, x1);
                    e.mrs_nzcv(x4);
                    store_ccr(x3, x4, kind == 4);
                } else if (kind == 0) {
                    e.add(x0, x0, x1);
                } else {
                    e.sub(x0, x0, x1);
                }
            } else {
                if (kind == 5 || kind == 6) e.mvn(x1, x1);
                if (kind == 1 || kind == 5) e.and_(x0, x0, x1);
                else if (kind == 2 || kind == 6) e.orr(x0, x0, x1);
                else e.eor(x0, x0, x1);
                if (kind == 7) e.mvn(x0, x0);
                if (cc) {
                    logic_ccr_into(x3, x4);
                    e.str(x4, kCpu, o_ccr, 1);
                }
            }
            store_reg(rd, x0);
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
                if (op3 & 0x10) {
                    logic_ccr_into(x3, x4);
                    e.str(x4, kCpu, o_ccr, 1);
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
                        e.str(x6, kCpu, o_ccr, 1);
                    }
                }
                store_reg(rd, x0);
                const Site done = e.b();
                e.bind(by_zero);
                call_execute(i, insn);
                e.bind(done);
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
                e.str(x4, kCpu, o_ccr, 1);
                store_reg(rd, x0);
                if (tv) {
                    const Site done = e.b();
                    e.bind(overflow);
                    call_execute(i, insn);
                    e.bind(done);
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
                load_reg(x0, rs1);
                load_op2(x1, insn);
                e.ldr(x10, kCpu, off(y_));
                e.mov(x11, x0);  // rs1, for Y
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
                e.and_imm(x11, x11, 1);
                e.orr(x10, x10, x11, false, Shift::Lsl, 31);
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
                load_reg(x0, rs1);
                if (!x && op3 == 0x26) e.mov(x0, x0, false);
                if (!x && op3 == 0x27) e.sxtw(x0, x0);
                if (imm) {
                    const unsigned count = insn & (x ? 63 : 31);
                    if (count) {
                        if (op3 == 0x25) e.lsl(x0, x0, count);
                        else if (op3 == 0x26) e.lsr(x0, x0, count);
                        else e.asr(x0, x0, count);
                    }
                } else {
                    load_reg(x1, insn & 31);
                    e.and_imm(x1, x1, x ? 63 : 31);
                    if (op3 == 0x25) e.lslv(x0, x0, x1);
                    else if (op3 == 0x26) e.lsrv(x0, x0, x1);
                    else e.asrv(x0, x0, x1);
                }
                store_reg(rd, x0);
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
                    const Site done = e.b();
                    for (const Site& site : to) e.bind(site);
                    call_execute(i, insn);
                    e.bind(done);
                    return true;
                }
                if (cc & 1) return false;
                if (rd == 0) return true;
                eval_icc((insn >> 14) & 15, cc == 2);
                if (imm) e.mov_imm(x2, static_cast<u64>(sext(insn, 11)));
                else load_reg(x2, insn & 31);
                load_reg(x3, rd);
                e.cmp_imm(x0, 0, false);
                e.csel(x3, x2, x3, Cond::Ne);
                store_reg(rd, x3);
                return true;
            }
            case 0x2F: {  // MOVr
                const unsigned rcond = (insn >> 10) & 7;
                if (!rcond_valid(rcond)) return false;
                if (rd == 0) return true;
                if (imm) e.mov_imm(x2, static_cast<u64>(sext(insn, 10)));
                else load_reg(x2, insn & 31);
                load_reg(x3, rd);
                load_reg(x0, rs1);
                e.cmp_imm(x0, 0);
                e.csel(x3, x2, x3, kRcond[rcond]);
                store_reg(rd, x3);
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
    // once the traps have been ruled out. Clobbers x0-x2.
    auto move_window = [&](bool save) {
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
    };
    // A conditional branch at block offset `here` whose condition the
    // caller has put in w0. Returns with npc in kNpc, or goes on (annulled
    // and not taken).
    auto cond_branch = [&](unsigned i, s64 here, s64 disp, bool annul) {
        known_target = true, target = here + disp;
        may_fall = !annul, fall = here + 8;
        if (!annul) {
            pc_plus(kNpc, here + 8);
            pc_plus(x2, here + disp);
            mask(x2);
            e.cmp_imm(x0, 0, false);
            e.csel(kNpc, x2, kNpc, Cond::Ne);
        } else {
            const Site taken = e.cbnz(x0, false);
            chain_to(here + 8, i + 1);  // the delay slot is annulled
            e.bind(taken);
            pc_plus(kNpc, here + disp);
            mask(kNpc);
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
            pc_plus(kNpc, here + disp);
            mask(kNpc);
            dynamic = true;
            known_target = true, target = here + disp, may_fall = false;
        } else if (cond == 0) {  // never
            if (annul) chain_to(here + 8, i + 1);
        } else {
            if (xcc > 1) eval_fcc(cond, xcc - 2);
            else eval_icc(cond, xcc);
            cond_branch(i, here, disp, annul);
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
            pc_plus(kNpc, here + (sext(insn, 30) << 2));
            mask(kNpc);
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
                    const Site on = e.b();
                    for (const Site& site : to) e.bind(site);
                    call_execute(i, insn);
                    e.bind(on);
                    const unsigned fcc_n = op2 == 5 ? (insn >> 20) & 3 : 0;
                    icc_branch(i, here, cond, 2 + fcc_n, (op2 == 5 ? sext(insn, 19) : sext(insn, 22)) << 2, annul);
                    return true;
                }
                case 3: {  // BPr
                    const unsigned rcond = (insn >> 25) & 7;
                    if (((insn >> 28) & 1) || !rcond_valid(rcond)) return false;
                    eval_rcond(rcond, (insn >> 14) & 31);
                    const u64 d16 = (((insn >> 20) & 3) << 14) | (insn & 0x3FFF);
                    cond_branch(i, here, sext(d16, 16) << 2, annul);
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
            const Site done = e.b();
            for (const Site& site : slow) e.bind(site);
            call_execute(i, insn);
            e.ldr(kNpc, kCpu, o_npc);
            e.bind(done);
            dynamic = true;
            return true;
        }
        if (op == 2 && ((insn >> 19) & 0x3F) == 0x38) {  // JMPL
            load_reg(x3, (insn >> 14) & 31);
            load_op2(x1, insn);
            e.add(x3, x3, x1);
            mask(x3);
            const Site misaligned = e.cbnz_bit(x3, 0);
            const Site misaligned2 = e.cbnz_bit(x3, 1);
            e.mov(kNpc, x3);
            pc_plus(x1, here);
            mask(x1);
            store_reg((insn >> 25) & 31, x1);
            const Site done = e.b();
            e.bind(misaligned);
            e.bind(misaligned2);
            call_execute(i, insn);  // it takes the alignment trap
            leave(i + 1);
            e.bind(done);
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
    static_assert(sizeof(FastEntry) == 32);
    static_assert(offsetof(FastEntry, key) == 0 && offsetof(FastEntry, addend) == 8 && offsetof(FastEntry, code) == 16 &&
                  offsetof(FastEntry, perms) == 24);
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
            e.mov(x2, x0);
            e.mov(x0, kCpu);
            e.mov_imm32(x1, the_asi);
            e.load_constant(x16, reinterpret_cast<u64>(&UltraSparc::jit_internal_read));
            e.blr(x16);
            store_reg(rd, x0);
            const Site done = e.b();
            for (const Site& site : slow) e.bind(site);
            call_execute(i, insn);
            e.bind(done);
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
        load_reg(x0, rs1);
        if (kind == Kind::Cas || kind == Kind::Partial) {
            // [rs1] only (a partial store's rs2 is the mask)
        } else if (imm) {
            if (const s64 d = sext(insn, 13)) e.add_imm(x0, x0, d);
        } else if (insn & 31) {
            load_reg(x1, insn & 31);
            e.add(x0, x0, x1);
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
        if (size > 1) {
            e.tst_imm(x0, size - 1);
            slow.push_back(e.b(Cond::Ne));
        }
        // x2 = the entry, x1 = the key looked for.
        e.lsr(x1, x0, 13);
        e.and_imm(x2, x1, kFastEntries - 1);
        e.add(x2, kCpu, x2, true, Shift::Lsl, 5);
        e.add_imm(x2, x2, off(fast_[cls][0]));
        e.ldr(x3, kCpu, off(fast_key_));
        e.orr(x1, x1, x3);
        e.ldr(x3, x2, 0);
        e.cmp(x1, x3);
        slow.push_back(e.b(Cond::Ne));
        e.ldr(x3, x2, 24, 1);
        e.tst_imm(x3, perm, false);
        slow.push_back(e.b(Cond::Eq));
        if (writes) {
            e.ldr(x3, x2, 16);
            e.ldr(x3, x3, 0, 2);  // both halves' code flags
            slow.push_back(e.cbnz(x3, false));
        }
        e.ldr(x3, x2, 8);
        e.add(x0, x0, x3);  // the host address
        // Big-endian memory to and from host order (nothing to do for a
        // little-endian ASI).
        auto swap64 = [&](Reg r) { if (!little) e.rev64(r, r); };
        auto swap32 = [&](Reg r) { if (!little) e.rev32(r, r); };
        auto swap16 = [&](Reg r) { if (!little) e.rev16(r, r); };
        switch (kind) {
            case Kind::Load:
                e.ldr(x1, x0, 0, size);
                switch (size) {
                    case 8: swap64(x1); break;
                    case 4:
                        swap32(x1);
                        if (sign) e.sxtw(x1, x1);
                        break;
                    case 2:
                        swap16(x1);
                        if (sign) e.sxth(x1, x1);
                        break;
                    default:
                        if (sign) e.sxtb(x1, x1);
                        break;
                }
                store_reg(rd, x1);
                break;
            case Kind::Store:
                load_reg(x1, rd);
                if (size == 8) swap64(x1);
                else if (size == 4) swap32(x1);
                else if (size == 2) swap16(x1);
                e.str(x1, x0, 0, size);
                break;
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
        const Site done = e.b();
        for (const Site& site : slow) e.bind(site);
        call_execute(i, insn);
        e.bind(done);
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
        const Site done = e.b();
        for (const Site& site : slow) e.bind(site);
        call_execute(i, insn);
        e.bind(done);
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
        e.load_constant(x16, reinterpret_cast<u64>(o->fn));
        e.blr(x16);
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
        const Site done = e.b();
        for (const Site& site : slow) e.bind(site);
        call_execute(i, insn);
        e.bind(done);
        return true;
    };

    // FPops that may raise exceptions: arithmetic, square roots and
    // conversions, under the guest rounding mode (FPCR), with the
    // exceptions they raise accrued in FSR (an invalid result is SPARC's
    // default NaN). An exception whose trap is enabled, an invalid
    // conversion to an integer, a NaN operand, and nonstandard mode (FSR.NS)
    // go to execute(). False if not done here.
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
        using Fop = Emitter::Fop;
        const bool dsrc = src == T::F64;
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
        // x8 = the exceptions as cexc bits; to execute() if any traps.
        e.and_imm(x6, x6, 0x1F);
        e.load_constant(x8, reinterpret_cast<u64>(kFpsrExc.v));
        e.ldr_index(x8, x8, x6, 1);
        e.ldr(x7, kCpu, off(fsr_));
        e.lsr(x5, x7, 23);  // TEM
        e.and_(x5, x5, x8);
        slow.push_back(e.cbnz(x5));
        if (!to_int) {  // invalid: the default NaN
            const Site valid = e.cbz_bit(x6, 0);
            e.mov_imm(x5, dst == T::F64 ? 0x7FFFFFFFFFFFFFFFull : 0x7FFFFFFF);
            e.fmov_from_gpr(0, x5, dst == T::F64);
            e.bind(valid);
        }
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
        const Site done = e.b();
        for (const Site& site : slow) e.bind(site);
        call_execute(i, insn);
        e.bind(done);
        return true;
    };

    // TICK as read at instruction i, into x0 (bits 62:0): the step count
    // then, through the CPU's time base and charge per step. Clobbers x1.
    auto tick = [&](unsigned i) {
        e.ldr(x0, sp, kCycles0);
        e.ldr(x1, sp, kTotal);
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
            const Site done = e.b();
            for (const Site& site : slow) e.bind(site);
            call_execute(i, insn);
            e.bind(done);
            return true;
        }
        const bool priv = pstate_ & pstate::PRIV;  // (part of the mode)
        if ((op3 == 0x2A || op3 == 0x28) && rs1 == 4 && (fmode == 1 || fmode == 2)) {  // RDPR, RD %tick
            self = false;  // (TICK follows cycles_, which the check mode does not)
            if (rd == 0) return true;
            tick(i);
            e.ldr(x1, kCpu, off(tick_npt_), 1);
            e.orr(x0, x0, x1, true, Shift::Lsl, 63);
            store_reg(rd, x0);
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
                tick(i);
                store_reg(rd, x0);
            }
            const Site done = e.b();
            e.bind(npt);
            call_execute(i, insn);
            e.bind(done);
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
            const Site done = e.b();
            for (const Site& site : to) e.bind(site);
            call_execute(i, insn);
            e.bind(done);
            return true;
        }
        if (op3 == 0x28) {  // RDASR: Y, CCR, ASI, PC, FPRS; STBAR and MEMBAR
            if (rs1 == 15 && rd == 0) return true;  // memory is always ordered here
            switch (rs1) {
                case 0: e.ldr(x0, kCpu, off(y_)); break;
                case 2: e.ldr(x0, kCpu, o_ccr, 1); break;
                case 3: e.ldr(x0, kCpu, off(asi_), 1); break;
                case 5:
                    pc_plus(x0, 4 * static_cast<s64>(i));
                    mask(x0);
                    break;
                case 6: e.ldr(x0, kCpu, off(fprs_), 1); break;
                case 16:  // PCR, PIC, DCR, SOFTINT, TICK_CMPR: privileged here
                case 17:
                case 18:
                case 22:
                case 23: {
                    if (!priv) return false;
                    const u64& r = rs1 == 16 ? pcr_ : rs1 == 17 ? pic_ : rs1 == 18 ? dcr_ : rs1 == 22 ? softint_ : tick_cmpr_;
                    e.ldr(x0, kCpu, off(r));
                    break;
                }
                default: return false;
            }
            store_reg(rd, x0);
            return true;
        }
        // WR %y, %ccr, %asi, %fprs, and in privileged code %pcr, %pic, %dcr: rs1 xor operand 2
        if (op3 == 0x30 && (rd == 0 || rd == 2 || rd == 3 || rd == 6 || (priv && rd >= 16 && rd <= 18))) {
            load_reg(x0, rs1);
            load_op2(x1, insn);
            e.eor(x0, x0, x1);
            if (rd == 0) {
                e.mov(x0, x0, false);
                e.str(x0, kCpu, off(y_));
            } else if (rd == 6) {
                e.and_imm(x0, x0, 7);
                e.str(x0, kCpu, off(fprs_), 1);
            } else if (rd >= 16) {
                e.str(x0, kCpu, off(rd == 16 ? pcr_ : rd == 17 ? pic_ : dcr_));
            } else {
                e.str(x0, kCpu, rd == 2 ? o_ccr : off(asi_), 1);
            }
            return true;
        }
        if (op3 == 0x3B) return true;  // FLUSH: stores to code are seen at once here
        if (op3 == 0x36 && ((insn >> 5) & 0x1FF) == 0x080 && priv) {  // SHUTDOWN: nothing to do (fp_disabled from execute())
            std::vector<Site> to;
            fp_check(to);
            const Site done = e.b();
            for (const Site& site : to) e.bind(site);
            call_execute(i, insn);
            e.bind(done);
            return true;
        }
        if (op3 == 0x2B) {  // FLUSHW: a spill trap (from execute()) unless only the current window is in use
            e.ldr(x0, kCpu, off(cansave_), 4);
            e.cmp_imm(x0, kWindows - 2, false);
            const Site flushed = e.b(Cond::Eq);
            call_execute(i, insn);
            e.bind(flushed);
            return true;
        }
        if (op3 == 0x3A) {  // Tcc: the trap from execute()
            const unsigned cc = (insn >> 11) & 3, cond = (insn >> 25) & 15;
            if ((cc & 1) || cond == 8) return false;
            if (cond == 0) return true;
            self = false;
            eval_icc(cond, cc == 2);
            const Site not_taken = e.cbz(x0, false);
            call_execute(i, insn);
            e.bind(not_taken);
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
    e.str(xzr, sp, kTotal);
    e.ldr(x0, kCpu, off(cycles_));
    e.str(x0, sp, kCycles0);
    e.ldr(x0, kCpu, off(instret_));
    e.str(x0, sp, kInstret0);
    load_bases();
    // Blocks going on from another enter here, so everything before must be
    // the same in every translation.
    if (jit_link_offset_ == 0) jit_link_offset_ = e.size();
    if (e.size() != jit_link_offset_) return false;
    body = e.size();

    bool last_executed = false;  // the last instruction went to execute()
    bool ended = false;          // the code for the end of the block is out
    for (unsigned i = 0; i < n; ++i) {
        const u32 insn = b.insns[i];
        const s64 here = 4 * static_cast<s64>(i);
        const unsigned op = insn >> 30;
        const bool was_dynamic = dynamic;
        if (!was_dynamic) known_target = false;
        bool done = false;
        if (delayed_cti(insn)) {
            done = cti(i, insn);
        } else if (op == 0 && ((insn >> 22) & 7) == 4) {  // SETHI
            const unsigned rd = (insn >> 25) & 31;
            if (rd) {
                e.mov_imm32(x0, (insn & 0x3FFFFF) << 10);
                store_reg(rd, x0);
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
            }
            continue;
        }
        // execute() runs it.
        last_executed = i + 1 == n;
        call_execute(i, insn);
        dynamic = delayed_cti(insn);
        known_target = false;
        if (dynamic) e.ldr(kNpc, kCpu, o_npc);
    }
    // The end of the block.
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
        pc_plus(x0, 4 * static_cast<s64>(n));
        e.str(x0, kCpu, o_pc);
        e.str(kNpc, kCpu, o_npc);
        leave(n);
    } else {
        chain_to(4 * static_cast<s64>(n), n);
    }
    for (const Site& site : exits) e.bind(site);
    epilogue(e);
    e.finish_literals();

    const void* code = jit_buf_->add(e.code().data(), e.size());
    if (!code) {
        // The buffer is full: start again with an empty one, the stubs
        // first, and translate the block again for its new place.
        for (Block& other : blocks_) other.code = nullptr;
        jit_buf_->clear();
        jit_stub_dyn_ = jit_stub_seq_ = nullptr;
        if (!jit_make_stubs() || jit_buf_->next() == base) return false;  // (nothing more can be done)
        return jit_compile(b);
    }
    b.code = reinterpret_cast<JitFn>(const_cast<void*>(code));
    b.jit_mode = mode;
    b.jit_self = self;
    ++jit_compiles_;
    return true;
}

}  // namespace uv::usparc

#endif
