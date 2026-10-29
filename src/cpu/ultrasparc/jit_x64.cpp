// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
// UltraSPARC IIi dynamic recompiler, x86-64 back end.
//
// A block run often is translated into one host function that runs it from
// its first instruction exactly as run_blocks() would; the caller makes sure
// the whole block fits before the next interrupt or TICK_COMPARE check is
// due. Register operations, condition codes, conditional moves and control
// transfers are emitted inline. Any other instruction goes to execute() with
// pc and npc in place, and the function leaves after it on the conditions
// that end a block in run_blocks(): a trap, attention_, halted_, or a pc
// other than the next instruction's. The function returns how many of the
// block's instructions it started, with pc_ and npc_ as they would be.
//
// Guest registers are reached through three host registers holding the
// current global set, the current window's outs, and its ins (its locals
// follow them); they are loaded again after every call to execute(), which
// may change the window or the global set.
#include "cpu/ultrasparc/ultrasparc.h"

#if defined(__x86_64__) || defined(_M_X64)

#include <cstddef>
#include <vector>

#include "cpu/ultrasparc/vis.h"
#include "jit/exec_buffer.h"
#include "jit/x64_emitter.h"

namespace uv::usparc {

namespace {

using namespace uv::jit::x64;

// Live across the whole function (callee-saved).
constexpr Reg kCpu = rbx;  // UltraSparc*
constexpr Reg kPc = r12;   // pc of the block's first instruction
constexpr Reg kNpc = r13;  // npc chosen by a control transfer
constexpr Reg kG = r14;    // the current global set: %g0-%g7
constexpr Reg kO = r15;    // the current window's outs: %o0-%o7
constexpr Reg kI = rbp;    // the current window's ins, then its locals

#if defined(_WIN32)
constexpr Reg kArg0 = rcx, kArg1 = rdx, kArg2 = r8, kArg3 = r9;
constexpr s32 kShadow = 32;
constexpr bool kSaveSiDi = true;
#else
constexpr Reg kArg0 = rdi, kArg1 = rsi, kArg2 = rdx, kArg3 = rcx;
constexpr s32 kShadow = 0;
constexpr bool kSaveSiDi = false;
#endif

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

// The frame of a translation: the callee-saved registers, then (from rsp)
// any shadow space for calls, cycles_ and instret_ as they were on entry,
// the instructions started in the blocks before the current one (when one
// went on into another), the budget given on entry, and room for MXCSR.
constexpr s32 kCycles0 = kShadow, kInstret0 = kShadow + 8, kTotal = kShadow + 16, kBudget = kShadow + 24;
constexpr s32 kMxcsr = kShadow + 32;  // the host's MXCSR, and the guest's at + 4, around an FPop
constexpr s32 kFrame = 40 + kShadow;  // (with the pushes, keeps calls 16-byte aligned)

void prologue(Emitter& e) {
    e.push(rbx), e.push(rbp), e.push(r12), e.push(r13), e.push(r14), e.push(r15);
    if (kSaveSiDi) e.push(rsi), e.push(rdi);
    e.sub_rsp(kFrame);
}
void epilogue(Emitter& e) {  // (rsp at the frame)
    e.add_rsp(kFrame);
    if (kSaveSiDi) e.pop(rdi), e.pop(rsi);
    e.pop(r15), e.pop(r14), e.pop(r13), e.pop(r12), e.pop(rbp), e.pop(rbx);
    e.ret();
}

// MXCSR's exception flags (IE, DE, ZE, OE, UE, PE: bits 0-5) as SPARC's
// cexc bits (NV 0x10, DZ 0x02, OF 0x08, UF 0x04, NX 0x01); DE is not one.
struct ExcTable {
    u8 v[64];
};
constexpr ExcTable make_exc_table() {
    ExcTable t{};
    for (unsigned f = 0; f < 64; ++f)
        t.v[f] = static_cast<u8>(((f & 1) ? 0x10 : 0) | ((f & 4) ? 0x02 : 0) | ((f & 8) ? 0x08 : 0) | ((f & 16) ? 0x04 : 0) |
                                 ((f & 32) ? 0x01 : 0));
    return t;
}
constexpr ExcTable kMxcsrExc = make_exc_table();

// x86 condition for BPr/MOVr's rcond after `test r, r` (0 and 4 are reserved).
constexpr Cond kRcond[8] = {Cond::O, Cond::E, Cond::Le, Cond::L, Cond::O, Cond::Ne, Cond::G, Cond::Ge};
bool rcond_valid(unsigned rcond) { return (rcond & 3) != 0; }

}  // namespace

bool UltraSparc::jit_compile_supported() { return true; }

// The code a translation calls (ecx = the instruction's index in the block,
// edx = the instruction) to run an instruction through execute() with pc
// and npc in place and the counts as run_blocks() would have them. It
// returns to the translation, with the window and global set loaded again,
// unless run_blocks() would end the block there (a trap, attention_,
// halted_, or, outside a delay slot, a pc other than the next
// instruction's; after a delay slot, the translation chains to the next pc
// itself); then it leaves the translation itself, returning the
// instructions started.
bool UltraSparc::jit_make_stubs() {
    Emitter e;
    auto at = [this](const auto& member) {
        return Mem{kCpu, static_cast<s32>(reinterpret_cast<const u8*>(&member) - reinterpret_cast<const u8*>(this))};
    };
    constexpr s32 kStubFrame = 8 + kShadow;     // the index, above any shadow space (keeps the call aligned)
    constexpr s32 kUp = kStubFrame + 8;         // to the translation's frame, past the return address
    const Mem index{rsp, kShadow};
    auto pc_of_index = [&](Reg r) {  // r = kPc + 4 * rcx
        e.mov(r, rcx);
        e.shift_imm(Shift::Shl, r, 2, true);
        e.alu(Alu::Add, r, kPc);
    };
    // After execute(): back into the translation, unless the block ends
    // there. After a delay slot (`sequential` false) the translation itself
    // goes on to the next pc (a delay slot ends its block); after any other
    // instruction it goes on to the next one, so the pc must be that.
    auto body = [&](bool sequential) {
        e.sub_rsp(kStubFrame);
        e.mov(index, rcx);
        e.alu(Alu::Add, rcx, Mem{rsp, kUp + kTotal});
        e.mov(rax, Mem{rsp, kUp + kCycles0});
        e.alu(Alu::Add, rax, rcx);
        e.mov(at(cycles_), rax);
        e.mov(rax, Mem{rsp, kUp + kInstret0});
        e.alu(Alu::Add, rax, rcx);
        e.mov(at(instret_), rax);
        e.mov(kArg0, kCpu);
        if (kArg1 != rdx) e.mov32(kArg1, rdx);
        e.mov_imm64(rax, reinterpret_cast<u64>(&UltraSparc::jit_execute));
        e.call(rax);
        e.movzx8(rax, rax);  // (a bool return value is only al)
        e.test32(rax, rax);
        std::vector<std::size_t> out;
        out.push_back(e.jcc(Cond::Ne));
        if (sequential) {
            e.mov(rcx, index);
            pc_of_index(rax);
            e.alu_imm(Alu::Add, rax, 4);
            e.alu(Alu::Cmp, rax, at(pc_));
            out.push_back(e.jcc(Cond::Ne));
        }
        e.mov(kG, at(base_[0]));
        e.mov(kO, at(base_[1]));
        e.mov(kI, at(base_[3]));
        e.add_rsp(kStubFrame);
        e.ret();
        for (std::size_t site : out) e.bind(site);
        e.mov(rax, index);
        e.alu_imm(Alu::Add, rax, 1);
        e.alu(Alu::Add, rax, Mem{rsp, kUp + kTotal});
        e.add_rsp(kUp);
        epilogue(e);
    };
    const std::size_t dyn = 0;  // npc in kNpc
    pc_of_index(rax);
    e.mov(at(pc_), rax);
    e.mov(at(npc_), kNpc);
    body(false);
    const std::size_t seq = e.size();  // npc = pc + 4
    pc_of_index(rax);
    e.mov(at(pc_), rax);
    e.alu_imm(Alu::Add, rax, 4);
    e.mov(at(npc_), rax);
    body(true);
    const auto* code = static_cast<const u8*>(jit_buf_->add(e.code(), e.size()));
    if (!code) return false;
    jit_stub_dyn_ = code + dyn;
    jit_stub_seq_ = code + seq;
    return true;
}

bool UltraSparc::jit_compile(Block& b) {
    if (!jit_stub_dyn_ && !jit_make_stubs()) return false;
    const u8* const base = jit_buf_->next();  // where this code will be (calls to the stubs are relative)
    Emitter e;
    auto at = [this](const auto& member) {
        return Mem{kCpu, static_cast<s32>(reinterpret_cast<const u8*>(&member) - reinterpret_cast<const u8*>(this))};
    };
    const Mem m_pc = at(pc_), m_npc = at(npc_), m_ccr = at(ccr_);
    const bool am = pstate_ & pstate::AM;

    // cond_icc() as a table: bit n of kIcc[cond] says whether cond holds
    // when the condition-code nibble is n.
    u16 icc_table[16] = {};
    for (unsigned c = 0; c < 16; ++c)
        for (unsigned n = 0; n < 16; ++n)
            if (cond_icc(c, n)) icc_table[c] |= static_cast<u16>(1u << n);

    auto reg_mem = [&](unsigned r) -> Mem {
        if (r < 8) return {kG, static_cast<s32>(8 * r)};
        if (r < 16) return {kO, static_cast<s32>(8 * (r - 8))};
        if (r < 24) return {kI, static_cast<s32>(64 + 8 * (r - 16))};
        return {kI, static_cast<s32>(8 * (r - 24))};
    };
    // (Loads leave the host flags alone.)
    auto load_reg = [&](Reg dst, unsigned r) {
        if (r == 0) e.mov_imm32(dst, 0);
        else e.mov(dst, reg_mem(r));
    };
    auto store_reg = [&](unsigned r, Reg src) {
        if (r != 0) e.mov(reg_mem(r), src);
    };
    auto load_imm = [&](Reg dst, s64 v) {
        if (v >= 0 && v <= 0xFFFFFFFF) e.mov_imm32(dst, static_cast<u32>(v));
        else if (v >= INT32_MIN && v < 0) e.mov_imm_sx(dst, static_cast<s32>(v));
        else e.mov_imm64(dst, static_cast<u64>(v));
    };
    // Operand 2: simm13 or rs2.
    auto load_op2 = [&](Reg dst, u32 insn) {
        if ((insn >> 13) & 1) load_imm(dst, sext(insn, 13));
        else load_reg(dst, insn & 31);
    };
    auto load_bases = [&] {
        e.mov(kG, at(base_[0]));
        e.mov(kO, at(base_[1]));
        e.mov(kI, at(base_[3]));
    };
    // r = the pc of the block's first instruction + off (r11 is scratch).
    auto pc_plus = [&](Reg r, s64 off) {
        e.mov(r, kPc);
        if (off >= INT32_MIN && off <= INT32_MAX) {
            if (off) e.alu_imm(Alu::Add, r, static_cast<s32>(off));
        } else {
            e.mov_imm64(r11, static_cast<u64>(off));
            e.alu(Alu::Add, r, r11);
        }
    };
    auto mask = [&](Reg r) {
        if (am) e.mov32(r, r);
    };

    // On the stack: cycles_ and instret_ as they were on entry; the
    // instructions started in the blocks before this one (when one went on
    // into another); and the budget given on entry.
    const Mem m_cycles0{rsp, kCycles0}, m_instret0{rsp, kInstret0}, m_total{rsp, kTotal}, m_budget{rsp, kBudget};

    std::vector<std::size_t> exits;
    auto leave = [&](u32 started) {  // with this block's first `started` instructions started
        e.mov_imm32(rax, started);
        e.alu(Alu::Add, rax, m_total);
        exits.push_back(e.jmp());
    };
    auto store_pc_npc = [&](s64 off) {  // pc_ = block pc + off, npc_ = pc_ + 4
        pc_plus(rax, off);
        e.mov(m_pc, rax);
        e.alu_imm(Alu::Add, rax, 4);
        e.mov(m_npc, rax);
    };
    auto store_pc_npc_from = [&](Reg r) {  // pc_ = r, npc_ = r + 4
        e.mov(m_pc, r);
        e.mov(rax, r);
        e.alu_imm(Alu::Add, rax, 4);
        e.mov(m_npc, rax);
    };

    // Whether npc is in kNpc (the previous instruction was a control
    // transfer) rather than pc + 4; if so, whether that transfer's target is
    // known here (as an offset from the block's pc), and whether it may
    // instead go on to the instruction after the delay slot.
    bool dynamic = false;
    bool known_target = false, may_fall = false;
    s64 target = 0, fall = 0;
    const unsigned n = b.length;

    // The block is done: go on into the translation of the block run_blocks()
    // would run next, if it has one for the same mode that fits in the
    // budget; else return, with pc_ = the next pc and npc_ = pc_ + 4.
    // chain() takes the next pc in rdx. In this 8 KiB page the next block's
    // host address follows from this block's; in another, it comes from the
    // page's entry in fetch_cache_, if there is one that run_blocks() would
    // use. chain_to() takes it as an offset from this block's pc, so that a
    // target in the same page has a slot known here, and a block going back
    // to its own start needs only the budget checked.
    const u8 mode = static_cast<u8>(jit_mode());
    const u16 fmode_fetch = fetch_mode();
    const s64 page_off = static_cast<s64>(pc_ & 0x1FFF);  // (the same for every virtual alias)
    static_assert(sizeof(FetchEntry) == 32);
    static_assert(offsetof(FetchEntry, va_page) == 0 && offsetof(FetchEntry, gen) == 8 && offsetof(FetchEntry, mode) == 12 &&
                  offsetof(FetchEntry, host) == 16);
    // Jumps to the translation in r8 for the block at rdx if its Block
    // (rcx) is for the same mode and fits in the budget; else to `no`.
    auto enter = [&](std::vector<std::size_t>& no) {
        e.cmp8_imm(Mem{rcx, static_cast<s32>(offsetof(Block, jit_mode))}, mode);
        no.push_back(e.jcc(Cond::Ne));
        e.mov(r9, Mem{rcx, static_cast<s32>(offsetof(Block, length))}, false);
        e.alu(Alu::Add, r9, m_total);
        e.alu(Alu::Cmp, r9, m_budget);
        no.push_back(e.jcc(Cond::A));
        e.mov(kPc, rdx);
        e.alu_imm(Alu::Add, r8, static_cast<s32>(jit_link_offset_));
        e.jmp(r8);
    };
    // (`count`: how many of this block's instructions were started.)
    auto chain = [&](unsigned count) {
        e.alu_imm(Alu::Add, m_total, static_cast<s32>(count));
        std::vector<std::size_t> no;
        e.mov(rax, rdx);
        e.alu(Alu::Xor, rax, kPc);
        e.shift_imm(Shift::Shr, rax, 13, true);
        const std::size_t other_page = e.jcc(Cond::Ne);
        e.mov(rax, rdx);  // its host address
        e.alu(Alu::Sub, rax, kPc);
        e.mov_imm64(rcx, reinterpret_cast<u64>(b.host));
        e.alu(Alu::Add, rax, rcx);
        const std::size_t found = e.jmp();
        e.bind(other_page);
        const s32 fetch = at(fetch_cache_[0]).disp;
        e.mov(rcx, rdx);
        e.shift_imm(Shift::Shr, rcx, 13, true);
        e.mov32(r8, rcx);
        e.alu_imm(Alu::And, r8, kFetchEntries - 1, false);
        e.shift_imm(Shift::Shl, r8, 5, false);
        e.alu(Alu::Add, r8, kCpu);
        e.alu(Alu::Cmp, rcx, Mem{r8, fetch});
        no.push_back(e.jcc(Cond::Ne));
        e.mov(r9, Mem{r8, fetch + 8}, false);
        e.alu(Alu::Cmp, r9, at(soft_gen_), false);
        no.push_back(e.jcc(Cond::Ne));
        e.movzx16(r9, Mem{r8, fetch + 12});
        e.alu_imm(Alu::Cmp, r9, fmode_fetch, false);
        no.push_back(e.jcc(Cond::Ne));
        e.mov(rax, Mem{r8, fetch + 16});
        e.test(rax, rax);
        no.push_back(e.jcc(Cond::E));  // not host memory
        e.mov32(rcx, rdx);
        e.alu_imm(Alu::And, rcx, 0x1FFF, false);
        e.alu(Alu::Add, rax, rcx);
        e.bind(found);
        e.mov(rcx, rax);  // its slot
        e.shift_imm(Shift::Shr, rcx, 2, true);
        e.alu_imm(Alu::And, rcx, (1 << kBlockBits) - 1, false);
        e.imul_imm(rcx, rcx, static_cast<s32>(sizeof(Block)));
        e.mov_imm64(r8, reinterpret_cast<u64>(blocks_.data()));
        e.alu(Alu::Add, rcx, r8);
        e.alu(Alu::Cmp, rax, Mem{rcx, static_cast<s32>(offsetof(Block, host))});
        no.push_back(e.jcc(Cond::Ne));
        e.mov(r8, Mem{rcx, static_cast<s32>(offsetof(Block, code))});
        e.test(r8, r8);
        no.push_back(e.jcc(Cond::E));
        enter(no);
        for (std::size_t site : no) e.bind(site);
        store_pc_npc_from(rdx);
        e.mov(rax, m_total);
        exits.push_back(e.jmp());
    };
    std::size_t body = 0;  // where the translation of this block's instructions starts
    auto chain_to = [&](s64 delta, unsigned count) {
        if (page_off + delta < 0 || page_off + delta >= 0x2000) {
            pc_plus(rdx, delta);
            mask(rdx);  // (a target in the same page needs none)
            chain(count);
            return;
        }
        e.alu_imm(Alu::Add, m_total, static_cast<s32>(count));
        std::vector<std::size_t> no;
        // The back edge of a delay loop (to this block's start, or to an
        // earlier block of a polling loop ending here): the iterations that
        // fit at once.
        const s64 loop_back = (b.loop >> 30) == 3 ? -4 * static_cast<s64>((b.loop >> 16) & 0x3FFF) : 0;
        if (b.loop && delta == loop_back && count == n) {
            e.mov(kArg0, kCpu);
            e.mov_imm32(kArg1, b.loop);
            e.mov(kArg2, m_budget);
            e.alu(Alu::Sub, kArg2, m_total);
            e.mov(kArg3, m_cycles0);  // the next instruction's step
            e.alu(Alu::Add, kArg3, m_total);
            e.mov_imm64(rax, reinterpret_cast<u64>(&jit_loop_at_once));
            e.call(rax);
            e.test(rax, rax);
            const std::size_t none = e.jcc(Cond::E);
            e.alu(Alu::Add, rax, m_total);
            e.mov(m_total, rax);
            e.cmp8_imm(at(delay_charged_), 0);  // charged other than the usual rate: out (so the check below leaves)
            const std::size_t usual = e.jcc(Cond::E);
            e.mov(m_budget, rax);
            e.bind(usual);
            e.bind(none);
        }
        if (delta == 0 && count == n) {
            e.mov(r9, m_total);
            e.alu_imm(Alu::Add, r9, static_cast<s32>(n));
            e.alu(Alu::Cmp, r9, m_budget);
            no.push_back(e.jcc(Cond::A));
            e.jmp_to(body);
        } else {
            const u8* host = b.host + delta;
            e.mov_imm64(rcx, reinterpret_cast<u64>(&blocks_[block_index(host)]));
            e.mov_imm64(rax, reinterpret_cast<u64>(host));
            e.alu(Alu::Cmp, rax, Mem{rcx, static_cast<s32>(offsetof(Block, host))});
            no.push_back(e.jcc(Cond::Ne));
            e.mov(r8, Mem{rcx, static_cast<s32>(offsetof(Block, code))});
            e.test(r8, r8);
            no.push_back(e.jcc(Cond::E));
            pc_plus(rdx, delta);
            enter(no);
        }
        for (std::size_t site : no) e.bind(site);
        store_pc_npc(delta);
        e.mov(rax, m_total);
        exits.push_back(e.jmp());
    };
    bool self = true;  // no instruction goes to execute()

    // Instruction i goes to execute() (through the stub, which leaves the
    // translation if run_blocks() would end the block there; otherwise the
    // window and global set are loaded again, for the rest of the block or
    // a block it goes on into).
    auto call_execute = [&](unsigned i, u32 insn) {
        self = false;
        e.mov_imm32(rcx, i);
        e.mov_imm32(rdx, insn);
        e.call_abs(static_cast<const u8*>(dynamic ? jit_stub_dyn_ : jit_stub_seq_), base);
    };

    // The condition codes from the host flags of a 32-bit operation (in
    // `f32`) and a 64-bit one (`f64`). The flags SF ZF OF CF map one for one
    // onto N Z V C; multiplying CF (bit 0), ZF (6), SF (7) and OF (11) by
    // 0x441 puts them in N Z V C order at bits 13:10 (and again, for the
    // other half, at bits 9:6), with no carries into those bits.
    // (ccr_into leaves the value in f64.)
    auto ccr_into = [&](Reg f32, Reg f64) {
        e.alu_imm(Alu::And, f32, 0x8C1, false);
        e.imul_imm(f32, f32, 0x441);
        e.shift_imm(Shift::Shr, f32, 10, false);
        e.alu_imm(Alu::And, f32, 0xF, false);
        e.alu_imm(Alu::And, f64, 0x8C1, false);
        e.imul_imm(f64, f64, 0x441);
        e.shift_imm(Shift::Shr, f64, 6, false);
        e.alu_imm(Alu::And, f64, 0xF0, false);
        e.alu(Alu::Or, f64, f32, false);
    };
    auto store_ccr = [&](Reg f32, Reg f64) {
        ccr_into(f32, f64);
        e.mov8(m_ccr, f64);
    };
    // The condition codes of a logical result in rax (V and C clear), as
    // ccr_into leaves them.
    auto logic_ccr_into = [&](Reg f32, Reg f64) {
        e.test32(rax, rax);
        e.pushfq();
        e.pop(f32);
        e.test(rax, rax);
        e.pushfq();
        e.pop(f64);
        ccr_into(f32, f64);
    };
    // eax = 1 if icc/xcc condition `cond` holds, else 0 (ZF set if not).
    // Clobbers rcx.
    auto eval_icc = [&](unsigned cond, bool xcc) {
        e.movzx8(rax, m_ccr);
        if (xcc) e.shift_imm(Shift::Shr, rax, 4, false);
        e.alu_imm(Alu::And, rax, 15, false);
        e.mov32(rcx, rax);
        e.mov_imm32(rax, icc_table[cond]);
        e.shift_cl(Shift::Shr, rax, false);
        e.alu_imm(Alu::And, rax, 1, false);
    };
    // eax = 1 if register rcond `rcond` holds for r, else 0.
    auto eval_rcond = [&](unsigned rcond, unsigned r) {
        load_reg(rax, r);
        e.test(rax, rax);
        e.setcc(kRcond[rcond], rax);
        e.movzx8(rax, rax);
    };

    // Floating point: f_ holds the registers as 32-bit halves, a double's
    // upper half first. A double in a host register is its two halves read
    // as one 64-bit word, rotated by 32 bits.
    const s32 f_disp = at(f_[0]).disp;
    auto freg = [&](unsigned half) { return Mem{kCpu, f_disp + static_cast<s32>(4 * half)}; };
    auto dnum = [](unsigned r) { return (r & 0x1E) | ((r & 1) << 5); };  // a double's 5-bit encoding
    auto load_double = [&](Reg r, unsigned enc) {
        e.mov(r, freg(dnum(enc)));
        e.shift_imm(Shift::Ror, r, 32, true);
    };
    auto store_double = [&](unsigned enc, Reg r) {  // (rotates r)
        e.shift_imm(Shift::Ror, r, 32, true);
        e.mov(freg(dnum(enc)), r);
    };
    // FPRS.DL or DU for a write to f0-f31 or f32-f62.
    auto dirty = [&](bool upper) { e.alu8_imm(Alu::Or, at(fprs_), upper ? 2 : 1); };
    auto clear_cexc = [&] { e.alu_imm(Alu::And, at(fsr_), ~0x1F); };
    // To `off` unless PSTATE.PEF and FPRS.FEF are both set.
    auto fp_check = [&](std::vector<std::size_t>& off) {
        e.test8_imm(at(pstate_), pstate::PEF);
        off.push_back(e.jcc(Cond::E));
        e.test8_imm(at(fprs_), 4);
        off.push_back(e.jcc(Cond::E));
    };
    // eax = 1 if fcc condition `cond` holds for fcc_n, else 0 (ZF set if
    // not). Clobbers rcx.
    u16 fcc_table[16] = {};
    for (unsigned c = 0; c < 16; ++c)
        for (unsigned f = 0; f < 4; ++f)
            if (cond_fcc(c, f)) fcc_table[c] |= static_cast<u16>(1u << f);
    auto eval_fcc = [&](unsigned cond, unsigned fcc_n) {
        e.mov(rax, at(fsr_));
        e.shift_imm(Shift::Shr, rax, static_cast<u8>(fcc_n == 0 ? 10 : 30 + 2 * fcc_n), true);
        e.alu_imm(Alu::And, rax, 3, false);
        e.mov32(rcx, rax);
        e.mov_imm32(rax, fcc_table[cond]);
        e.shift_cl(Shift::Shr, rax, false);
        e.alu_imm(Alu::And, rax, 1, false);
    };

#if defined(__GNUC__) || defined(__clang__)
    static const bool host_popcnt = __builtin_cpu_supports("popcnt");
#else
    constexpr bool host_popcnt = false;
#endif
    // Register operations (format 3, op 2). False if not done here.
    auto alu = [&](unsigned i, u32 insn) -> bool {
        const unsigned op3 = (insn >> 19) & 0x3F, rd = (insn >> 25) & 31, rs1 = (insn >> 14) & 31;
        const bool imm = (insn >> 13) & 1;
        if (op3 <= 0x07 || (op3 >= 0x10 && op3 <= 0x17)) {
            const unsigned kind = op3 & 7;
            const bool cc = op3 & 0x10;
            if (!cc && rd == 0) return true;
            load_reg(rax, rs1);
            load_op2(rcx, insn);
            if (kind == 0 || kind == 4) {
                const Alu op = kind == 0 ? Alu::Add : Alu::Sub;
                if (cc) {
                    e.mov(rdx, rax);
                    e.alu(op, rdx, rcx, false);
                    e.pushfq();
                    e.pop(r8);
                    e.alu(op, rax, rcx);
                    e.pushfq();
                    e.pop(r9);
                    store_ccr(r8, r9);
                } else {
                    e.alu(op, rax, rcx);
                }
            } else {
                if (kind == 5 || kind == 6) e.not_(rcx);
                e.alu(kind == 1 || kind == 5 ? Alu::And : kind == 2 || kind == 6 ? Alu::Or : Alu::Xor, rax, rcx);
                if (kind == 7) e.not_(rax);
                if (cc) {
                    logic_ccr_into(r8, r9);
                    e.mov8(m_ccr, r9);
                }
            }
            store_reg(rd, rax);
            return true;
        }
        switch (op3) {
            case 0x08:    // ADDC
            case 0x0C:    // SUBC
            case 0x18:    // ADDCcc
            case 0x1C: {  // SUBCcc
                const bool cc = op3 & 0x10;
                const Alu op = op3 & 4 ? Alu::Sbb : Alu::Adc;
                if (!cc && rd == 0) return true;
                auto carry_in = [&] {  // CF = icc.C
                    e.movzx8(r10, m_ccr);
                    e.shift_imm(Shift::Shr, r10, 1, false);
                };
                load_reg(rax, rs1);
                load_op2(rcx, insn);
                if (cc) {
                    e.mov(rdx, rax);
                    carry_in();
                    e.alu(op, rdx, rcx, false);
                    e.pushfq();
                    e.pop(r8);
                    carry_in();
                    e.alu(op, rax, rcx);
                    e.pushfq();
                    e.pop(r9);
                    store_ccr(r8, r9);
                } else {
                    carry_in();
                    e.alu(op, rax, rcx);
                }
                store_reg(rd, rax);
                return true;
            }
            case 0x0A:    // UMUL
            case 0x0B:    // SMUL
            case 0x1A:    // UMULcc
            case 0x1B: {  // SMULcc: the 64-bit product; Y = its upper half
                load_reg(rax, rs1);
                load_op2(rcx, insn);
                if (op3 & 1) {
                    e.movsxd(rax, rax);
                    e.movsxd(rcx, rcx);
                } else {
                    e.mov32(rax, rax);
                    e.mov32(rcx, rcx);
                }
                e.imul(rax, rcx);
                e.mov(rdx, rax);
                e.shift_imm(Shift::Shr, rdx, 32, true);
                e.mov(at(y_), rdx);
                if (op3 & 0x10) {
                    logic_ccr_into(r8, r9);
                    e.mov8(m_ccr, r9);
                }
                store_reg(rd, rax);
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
                if (wide) {
                    load_reg(rax, rs1);
                    load_op2(rcx, insn);
                } else {  // Y:rs1<31:0> by the low 32 bits of operand 2
                    load_reg(rax, rs1);
                    e.mov32(rax, rax);
                    e.mov(rdx, at(y_));
                    e.shift_imm(Shift::Shl, rdx, 32, true);
                    e.alu(Alu::Or, rax, rdx);
                    load_op2(rcx, insn);
                    if (sign) e.movsxd(rcx, rcx);
                    else e.mov32(rcx, rcx);
                }
                e.test(rcx, rcx);
                const std::size_t by_zero = e.jcc(Cond::E);  // division_by_zero, from execute()
                std::size_t quotient = 0;
                if (sign) {
                    // x86 faults on the one quotient too big for 64 bits.
                    e.alu_imm(Alu::Cmp, rcx, -1);
                    const std::size_t general = e.jcc(Cond::Ne);
                    e.neg(rax);
                    if (!wide) {  // -(-2^63) is taken as 2^63 - 1, then clamped
                        const std::size_t fits = e.jcc(Cond::No);
                        e.mov_imm64(rax, 0x7FFFFFFFFFFFFFFFull);
                        e.bind(fits);
                    }
                    quotient = e.jmp();
                    e.bind(general);
                    e.cdq(true);
                    e.idiv(rcx, true);
                } else {
                    e.alu(Alu::Xor, rdx, rdx, false);
                    e.div(rcx, true);
                }
                if (quotient) e.bind(quotient);
                if (!wide) {
                    // The quotient clamped to 32 bits; edx = 1 if it overflowed.
                    if (sign) {
                        e.mov_imm32(r8, 0x7FFFFFFF);
                        e.alu(Alu::Cmp, rax, r8);
                        e.setcc(Cond::G, rdx);
                        e.cmov(Cond::G, rax, r8);
                        e.mov_imm_sx(r8, INT32_MIN);
                        e.alu(Alu::Cmp, rax, r8);
                        e.setcc(Cond::L, rcx);
                        e.cmov(Cond::L, rax, r8);
                        e.alu(Alu::Or, rdx, rcx, false);
                    } else {
                        e.mov_imm32(r8, 0xFFFFFFFF);
                        e.alu(Alu::Cmp, rax, r8);
                        e.setcc(Cond::A, rdx);
                        e.cmov(Cond::A, rax, r8);
                    }
                    if (cc) {
                        e.movzx8(rdx, rdx);
                        e.shift_imm(Shift::Shl, rdx, 1, false);  // icc.V
                        logic_ccr_into(r8, r9);
                        e.alu(Alu::Or, r9, rdx, false);
                        e.mov8(m_ccr, r9);
                    }
                }
                store_reg(rd, rax);
                const std::size_t done = e.jmp();
                e.bind(by_zero);
                call_execute(i, insn);
                e.bind(done);
                return true;
            }
            case 0x20:    // TADDcc
            case 0x21:    // TSUBcc
            case 0x22:    // TADDccTV
            case 0x23: {  // TSUBccTV: icc.V also set by a tag (bits 1:0) in either operand
                const Alu op = op3 & 1 ? Alu::Sub : Alu::Add;
                load_reg(rax, rs1);
                load_op2(rcx, insn);
                e.mov(r10, rax);
                e.alu(Alu::Or, r10, rcx);
                e.mov(rdx, rax);
                e.alu(op, rdx, rcx, false);
                e.pushfq();
                e.pop(r8);
                e.alu(op, rax, rcx);
                e.pushfq();
                e.pop(r9);
                ccr_into(r8, r9);
                e.test_imm(r10, 3);
                const std::size_t untagged = e.jcc(Cond::E);
                e.alu_imm(Alu::Or, r9, 2, false);
                e.bind(untagged);
                std::size_t overflow = 0;
                if (op3 & 2) {  // tag_overflow, from execute()
                    e.test_imm(r9, 2);
                    overflow = e.jcc(Cond::Ne);
                }
                e.mov8(m_ccr, r9);
                store_reg(rd, rax);
                if (overflow) {
                    const std::size_t done = e.jmp();
                    e.bind(overflow);
                    call_execute(i, insn);
                    e.bind(done);
                }
                return true;
            }
            case 0x2E:  // POPC (rs1 must be 0)
                if (rs1 != 0) return false;
                if (rd == 0) return true;
                load_op2(rcx, insn);
                if (host_popcnt) {
                    e.popcnt(rax, rcx);
                } else {  // counted in parallel: pairs, nibbles, bytes, then summed by a multiply
                    e.mov(rax, rcx);
                    e.shift_imm(Shift::Shr, rax, 1, true);
                    e.mov_imm64(rdx, 0x5555555555555555ull);
                    e.alu(Alu::And, rax, rdx);
                    e.alu(Alu::Sub, rcx, rax);
                    e.mov_imm64(rdx, 0x3333333333333333ull);
                    e.mov(rax, rcx);
                    e.alu(Alu::And, rax, rdx);
                    e.shift_imm(Shift::Shr, rcx, 2, true);
                    e.alu(Alu::And, rcx, rdx);
                    e.alu(Alu::Add, rax, rcx);
                    e.mov(rcx, rax);
                    e.shift_imm(Shift::Shr, rcx, 4, true);
                    e.alu(Alu::Add, rax, rcx);
                    e.mov_imm64(rdx, 0x0F0F0F0F0F0F0F0Full);
                    e.alu(Alu::And, rax, rdx);
                    e.mov_imm64(rdx, 0x0101010101010101ull);
                    e.imul(rax, rdx);
                    e.shift_imm(Shift::Shr, rax, 56, true);
                }
                store_reg(rd, rax);
                return true;
            case 0x24: {  // MULScc: one step of a 32-bit multiply, shifting through Y
                // op1 = (icc.N ^ icc.V):rs1<31:1>; op2 = Y<0> ? operand 2<31:0> : 0
                load_reg(rax, rs1);
                load_op2(rcx, insn);
                e.mov(r10, at(y_));
                e.mov(r11, rax);  // rs1, for Y
                e.mov32(rax, rax);
                e.shift_imm(Shift::Shr, rax, 1, false);
                e.movzx8(rdx, m_ccr);
                e.mov32(r8, rdx);
                e.shift_imm(Shift::Shr, r8, 3, false);   // N
                e.shift_imm(Shift::Shr, rdx, 1, false);  // V
                e.alu(Alu::Xor, rdx, r8, false);
                e.alu_imm(Alu::And, rdx, 1, false);
                e.shift_imm(Shift::Shl, rdx, 31, false);
                e.alu(Alu::Or, rax, rdx, false);        // op1
                e.mov32(rcx, rcx);
                e.test_imm(r10, 1);
                e.mov_imm32(rdx, 0);
                e.cmov(Cond::E, rcx, rdx);              // op2
                e.mov(rdx, rax);
                e.alu(Alu::Add, rdx, rcx, false);
                e.pushfq();
                e.pop(r8);
                e.alu(Alu::Add, rax, rcx);
                e.pushfq();
                e.pop(r9);
                store_ccr(r8, r9);
                e.mov32(rax, rax);
                store_reg(rd, rax);
                // Y = Y >> 1 with rs1<0> into bit 31.
                e.mov32(r10, r10);
                e.shift_imm(Shift::Shr, r10, 1, false);
                e.alu_imm(Alu::And, r11, 1, false);
                e.shift_imm(Shift::Shl, r11, 31, false);
                e.alu(Alu::Or, r10, r11, false);
                e.mov(at(y_), r10);
                return true;
            }
            case 0x09:  // MULX
                if (rd == 0) return true;
                load_reg(rax, rs1);
                load_op2(rcx, insn);
                e.imul(rax, rcx);
                store_reg(rd, rax);
                return true;
            case 0x25:    // SLL, SLLX
            case 0x26:    // SRL, SRLX
            case 0x27: {  // SRA, SRAX
                if (rd == 0) return true;
                const bool x = (insn >> 12) & 1;
                const Shift sh = op3 == 0x25 ? Shift::Shl : op3 == 0x26 ? Shift::Shr : Shift::Sar;
                load_reg(rax, rs1);
                if (!x && op3 == 0x26) e.mov32(rax, rax);
                if (!x && op3 == 0x27) e.movsxd(rax, rax);
                if (imm) {
                    const unsigned count = insn & (x ? 63 : 31);
                    if (count) e.shift_imm(sh, rax, static_cast<u8>(count), true);
                } else {
                    load_reg(rcx, insn & 31);
                    e.alu_imm(Alu::And, rcx, x ? 63 : 31, false);
                    e.shift_cl(sh, rax, true);
                }
                store_reg(rd, rax);
                return true;
            }
            case 0x2C: {  // MOVcc on icc/xcc (MOVFcc on fcc_n: fp_disabled from execute())
                const unsigned cc = (insn >> 11) & 3;
                if (!((insn >> 18) & 1)) {
                    if (rd == 0) return false;
                    std::vector<std::size_t> off;
                    fp_check(off);
                    eval_fcc((insn >> 14) & 15, cc);
                    if (imm) load_imm(rdx, sext(insn, 11));
                    else load_reg(rdx, insn & 31);
                    load_reg(r8, rd);
                    e.test32(rax, rax);
                    e.cmov(Cond::Ne, r8, rdx);
                    store_reg(rd, r8);
                    const std::size_t done = e.jmp();
                    for (std::size_t site : off) e.bind(site);
                    call_execute(i, insn);
                    e.bind(done);
                    return true;
                }
                if (cc & 1) return false;
                if (rd == 0) return true;
                eval_icc((insn >> 14) & 15, cc == 2);
                if (imm) load_imm(rdx, sext(insn, 11));
                else load_reg(rdx, insn & 31);
                load_reg(r8, rd);
                e.test32(rax, rax);
                e.cmov(Cond::Ne, r8, rdx);
                store_reg(rd, r8);
                return true;
            }
            case 0x2F: {  // MOVr
                const unsigned rcond = (insn >> 10) & 7;
                if (!rcond_valid(rcond)) return false;
                if (rd == 0) return true;
                if (imm) load_imm(rdx, sext(insn, 10));
                else load_reg(rdx, insn & 31);
                load_reg(r8, rd);
                load_reg(rax, rs1);
                e.test(rax, rax);
                e.cmov(kRcond[rcond], r8, rdx);
                store_reg(rd, r8);
                return true;
            }
            default: return false;
        }
    };

    // The new current window is in ecx: its bases, in base_ and kO/kI.
    // Clobbers rax.
    auto set_window = [&] {
        const s32 windows = at(windows_).disp;
        e.mov32(rax, rcx);
        e.shift_imm(Shift::Shl, rax, 7, false);  // 16 registers
        e.alu(Alu::Add, rax, kCpu);
        e.alu_imm(Alu::Add, rax, windows);
        e.mov(kI, rax);
        e.mov(at(base_[3]), kI);
        e.alu_imm(Alu::Add, rax, 64);
        e.mov(at(base_[2]), rax);
        e.mov32(rax, rcx);  // the outs are the next window's ins
        e.alu_imm(Alu::Add, rax, 1, false);
        e.alu_imm(Alu::And, rax, kWindows - 1, false);
        e.shift_imm(Shift::Shl, rax, 7, false);
        e.alu(Alu::Add, rax, kCpu);
        e.alu_imm(Alu::Add, rax, windows);
        e.mov(kO, rax);
        e.mov(at(base_[1]), kO);
    };
    // SAVE (to the next window) or RESTORE and RETURN (to the previous one),
    // once the traps have been ruled out. Clobbers rax and rcx.
    auto move_window = [&](bool save) {
        e.alu_imm(save ? Alu::Sub : Alu::Add, at(cansave_), 1, false);
        e.alu_imm(Alu::And, at(cansave_), kWindows - 1, false);
        e.alu_imm(save ? Alu::Add : Alu::Sub, at(canrestore_), 1, false);
        e.alu_imm(Alu::And, at(canrestore_), kWindows - 1, false);
        e.mov(rcx, at(cwp_), false);
        e.alu_imm(Alu::Add, rcx, save ? 1 : kWindows - 1, false);
        e.alu_imm(Alu::And, rcx, kWindows - 1, false);
        e.mov(at(cwp_), rcx, false);
        set_window();
    };
    // A conditional branch at block offset `here` whose condition the
    // caller has put in eax. Returns with npc in kNpc, or leaves the block
    // (annulled and not taken).
    auto cond_branch = [&](unsigned i, s64 here, s64 disp, bool annul) {
        known_target = true, target = here + disp;
        may_fall = !annul, fall = here + 8;
        if (!annul) {
            pc_plus(kNpc, here + 8);
            pc_plus(rdx, here + disp);
            mask(rdx);
            e.test32(rax, rax);
            e.cmov(Cond::Ne, kNpc, rdx);
        } else {
            e.test32(rax, rax);
            const std::size_t taken = e.jcc(Cond::Ne);
            chain_to(here + 8, i + 1);  // the delay slot is annulled
            e.bind(taken);
            pc_plus(kNpc, here + disp);
            mask(kNpc);
        }
        dynamic = true;
    };
    // Bicc and BPcc on icc/xcc, and FBfcc and FBPfcc (with the fcc for
    // `xcc`, as fcc_n + 1).
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
            pc_plus(rax, here);
            mask(rax);
            store_reg(15, rax);
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
                    std::vector<std::size_t> off;
                    fp_check(off);
                    const std::size_t on = e.jmp();
                    for (std::size_t site : off) e.bind(site);
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
            load_reg(rax, (insn >> 14) & 31);
            load_op2(rcx, insn);
            e.alu(Alu::Add, rax, rcx);
            mask(rax);
            std::vector<std::size_t> slow;
            e.test_imm(rax, 3);
            slow.push_back(e.jcc(Cond::Ne));  // mem_address_not_aligned
            e.alu_imm(Alu::Cmp, at(canrestore_), 0, false);
            slow.push_back(e.jcc(Cond::E));  // fill
            e.mov(kNpc, rax);
            move_window(false);
            const std::size_t done = e.jmp();
            for (std::size_t site : slow) e.bind(site);
            call_execute(i, insn);
            e.mov(kNpc, m_npc);
            e.bind(done);
            dynamic = true;
            return true;
        }
        if (op == 2 && ((insn >> 19) & 0x3F) == 0x38) {  // JMPL
            load_reg(rax, (insn >> 14) & 31);
            load_op2(rcx, insn);
            e.alu(Alu::Add, rax, rcx);
            mask(rax);
            e.test_imm(rax, 3);
            const std::size_t aligned = e.jcc(Cond::E);
            call_execute(i, insn);  // it takes the alignment trap
            leave(i + 1);
            e.bind(aligned);
            e.mov(kNpc, rax);
            pc_plus(rcx, here);
            mask(rcx);
            store_reg((insn >> 25) & 31, rcx);
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
            load_reg(rax, rs1);
            if (imm) {
                if (const s64 d = sext(insn, 13)) e.alu_imm(Alu::Add, rax, static_cast<s32>(d));
            } else if (insn & 31) {
                e.alu(Alu::Add, rax, reg_mem(insn & 31));
            }
            mask(rax);
            std::vector<std::size_t> slow;
            if (asi_reg) {
                e.cmp8_imm(at(asi_), the_asi);
                slow.push_back(e.jcc(Cond::Ne));
            }
            e.test_imm(rax, 7);
            slow.push_back(e.jcc(Cond::Ne));
            e.mov(kArg2, rax);
            e.mov(kArg0, kCpu);
            e.mov_imm32(kArg1, the_asi);
            e.mov_imm64(rax, reinterpret_cast<u64>(&UltraSparc::jit_internal_read));
            e.call(rax);
            store_reg(rd, rax);
            const std::size_t done = e.jmp();
            for (std::size_t site : slow) e.bind(site);
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
        load_reg(rax, rs1);
        if (kind == Kind::Cas || kind == Kind::Partial) {
            // [rs1] only (a partial store's rs2 is the mask)
        } else if (imm) {
            if (const s64 d = sext(insn, 13)) e.alu_imm(Alu::Add, rax, static_cast<s32>(d));
        } else if (insn & 31) {
            e.alu(Alu::Add, rax, reg_mem(insn & 31));
        }
        mask(rax);
        std::vector<std::size_t> slow;
        const bool fpu = kind == Kind::FLoad || kind == Kind::FStore || kind == Kind::FsrLoad || kind == Kind::FsrStore;
        if (fpu || block || partial || short_fp) fp_check(slow);  // (the misaligned LDDF/STDF traps also go there)
        if (asi_reg) {
            e.cmp8_imm(at(asi_), the_asi);
            slow.push_back(e.jcc(Cond::Ne));
        }
        if (size > 1) {
            e.test_imm(rax, size - 1);
            slow.push_back(e.jcc(Cond::Ne));
        }
        const s32 table = at(fast_[cls][0]).disp;
        e.mov(rcx, rax);
        e.shift_imm(Shift::Shr, rcx, 13, true);
        e.mov32(rdx, rcx);
        e.alu_imm(Alu::And, rdx, kFastEntries - 1, false);
        e.shift_imm(Shift::Shl, rdx, 5, false);
        e.alu(Alu::Add, rdx, kCpu);
        e.alu(Alu::Or, rcx, at(fast_key_));
        e.alu(Alu::Cmp, rcx, Mem{rdx, table});
        slow.push_back(e.jcc(Cond::Ne));
        e.test8_imm(Mem{rdx, table + 24}, perm);
        slow.push_back(e.jcc(Cond::E));
        if (writes) {
            e.mov(rcx, Mem{rdx, table + 16});
            e.movzx16(r8, Mem{rcx, 0});  // both halves' code flags
            e.test32(r8, r8);
            slow.push_back(e.jcc(Cond::Ne));
        }
        e.alu(Alu::Add, rax, Mem{rdx, table + 8});
        const Mem host{rax, 0};
        // Big-endian memory to and from host order (nothing to do for a
        // little-endian ASI).
        auto swap64 = [&](Reg r) { if (!little) e.bswap64(r); };
        auto swap32 = [&](Reg r) { if (!little) e.bswap32(r); };
        auto swap16 = [&](Reg r) { if (!little) e.rol16_8(r); };
        switch (kind) {
            case Kind::Load:
                switch (size) {
                    case 8: e.mov(rcx, host), swap64(rcx); break;
                    case 4:
                        e.mov(rcx, host, false), swap32(rcx);
                        if (sign) e.movsxd(rcx, rcx);
                        break;
                    case 2:
                        e.movzx16(rcx, host), swap16(rcx);
                        if (sign) e.movsx16(rcx, rcx);
                        break;
                    default:
                        if (sign) e.movsx8(rcx, host);
                        else e.movzx8(rcx, host);
                        break;
                }
                store_reg(rd, rcx);
                break;
            case Kind::Store:
                load_reg(rcx, rd);
                switch (size) {
                    case 8: swap64(rcx), e.mov(host, rcx); break;
                    case 4: swap32(rcx), e.mov(host, rcx, false); break;
                    case 2: swap16(rcx), e.mov16(host, rcx); break;
                    default: e.mov8(host, rcx); break;
                }
                break;
            case Kind::LoadPair:  // rd = the word at va, rd + 1 = the word at va + 4 (each in the ASI's byte order)
                e.mov(rcx, host), swap64(rcx);
                if (little) e.shift_imm(Shift::Ror, rcx, 32, true);  // (the word at va is the low half)
                e.mov32(r8, rcx);
                e.shift_imm(Shift::Shr, rcx, 32, true);
                store_reg(rd, rcx);
                store_reg(rd + 1, r8);
                break;
            case Kind::StorePair:
                load_reg(rcx, rd);
                e.shift_imm(Shift::Shl, rcx, 32, true);
                load_reg(r8, rd + 1);
                e.mov32(r8, r8);
                e.alu(Alu::Or, rcx, r8);
                if (little) e.shift_imm(Shift::Ror, rcx, 32, true);
                swap64(rcx), e.mov(host, rcx);
                break;
            case Kind::Ldstub:
                e.movzx8(rcx, host);
                e.mov_store_imm8(host, 0xFF);
                store_reg(rd, rcx);
                break;
            case Kind::Swap:
                e.mov(rcx, host, false), swap32(rcx);
                load_reg(r8, rd);
                swap32(r8), e.mov(host, r8, false);
                store_reg(rd, rcx);
                break;
            case Kind::FLoad:
                if (size == 8) {
                    e.mov(rcx, host), swap64(rcx);
                    store_double(rd, rcx);
                    dirty(rd & 1);
                } else {
                    e.mov(rcx, host, false), swap32(rcx);
                    e.mov(freg(rd), rcx, false);
                    dirty(false);
                }
                break;
            case Kind::FStore:
                if (size == 8) {
                    load_double(rcx, rd);
                    swap64(rcx), e.mov(host, rcx);
                } else {
                    e.mov(rcx, freg(rd), false);
                    swap32(rcx), e.mov(host, rcx, false);
                }
                break;
            case Kind::BlockLoad:
                for (unsigned k = 0; k < 8; ++k) {
                    e.mov(rcx, Mem{rax, static_cast<s32>(8 * k)});
                    swap64(rcx);
                    store_double(rd + 2 * k, rcx);
                }
                dirty(rd & 1);
                break;
            case Kind::BlockStore:
                for (unsigned k = 0; k < 8; ++k) {
                    load_double(rcx, rd + 2 * k);
                    swap64(rcx);
                    e.mov(Mem{rax, static_cast<s32>(8 * k)}, rcx);
                }
                break;
            case Kind::FsrLoad:  // RD, TEM, NS, fcc0, aexc, cexc (and LDXFSR fcc1-3); ver, ftt, qne kept
                e.mov(rdx, at(fsr_));
                if (size == 8) {
                    e.mov(rcx, host), swap64(rcx);
                    e.mov_imm64(r8, 0x3FCFC00FFFull);
                    e.alu(Alu::And, rcx, r8);
                } else {
                    e.mov(rcx, host, false), swap32(rcx);
                    e.alu_imm(Alu::And, rcx, static_cast<s32>(0xCFC00FFF), false);
                    e.mov_imm64(r8, 0xFFFFFFFF00000000ull);
                    e.mov(r9, rdx);
                    e.alu(Alu::And, r9, r8);
                    e.alu(Alu::Or, rcx, r9);
                }
                e.alu_imm(Alu::And, rdx, 0x000FE000);
                e.alu(Alu::Or, rcx, rdx);
                e.mov(at(fsr_), rcx);
                break;
            case Kind::FsrStore:  // then ftt cleared
                e.mov(rcx, at(fsr_));
                if (size == 8) swap64(rcx), e.mov(host, rcx);
                else swap32(rcx), e.mov(host, rcx, false);
                e.alu_imm(Alu::And, at(fsr_), ~0x1C000);
                break;
            case Kind::Partial: {  // memory = memory with the selected bytes of rd
                const unsigned esize = 1u << ((asi_field >> 1) & 3);
                load_reg(rcx, insn & 31);
                e.alu_imm(Alu::And, rcx, (1 << (8 / esize)) - 1, false);
                e.shift_imm(Shift::Shl, rcx, 3, false);
                e.mov_imm64(rdx, reinterpret_cast<u64>(vis::partial_masks(esize, little)));
                e.alu(Alu::Add, rcx, rdx);
                e.mov(rcx, Mem{rcx, 0});
                load_double(r8, rd);
                swap64(r8);
                e.mov(r9, host);
                e.alu(Alu::Xor, r8, r9);
                e.alu(Alu::And, r8, rcx);
                e.alu(Alu::Xor, r9, r8);
                e.mov(host, r9);
                break;
            }
            case Kind::ShortLoad:  // zero-extended into the double
                if (size == 2) {
                    e.movzx16(rcx, host);
                    swap16(rcx);
                } else {
                    e.movzx8(rcx, host);
                }
                store_double(rd, rcx);
                dirty(rd & 1);
                break;
            case Kind::ShortStore:  // the double's low byte or halfword
                load_double(rcx, rd);
                if (size == 2) {
                    swap16(rcx);
                    e.mov16(host, rcx);
                } else {
                    e.mov8(host, rcx);
                }
                break;
            case Kind::Quad:  // rd = the doubleword at va, rd + 1 = the one at va + 8
                e.mov(rcx, host), swap64(rcx);
                e.mov(r8, Mem{rax, 8}), swap64(r8);
                store_reg(rd, rcx);
                store_reg(rd + 1, r8);
                break;
            case Kind::Cas: {  // compare with rs2; store rd if equal; rd = the old value
                if (size == 8) e.mov(rcx, host), swap64(rcx);
                else e.mov(rcx, host, false), swap32(rcx);
                load_reg(r8, insn & 31);
                if (size == 4) e.mov32(r8, r8);
                e.alu(Alu::Cmp, rcx, r8);
                const std::size_t differ = e.jcc(Cond::Ne);
                load_reg(r9, rd);
                if (size == 8) swap64(r9), e.mov(host, r9);
                else swap32(r9), e.mov(host, r9, false);
                e.bind(differ);
                store_reg(rd, rcx);
                break;
            }
        }
        const std::size_t done = e.jmp();
        for (std::size_t site : slow) e.bind(site);
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
        std::vector<std::size_t> slow;
        fp_check(slow);
        // rd = rs2, single (one half) or double (both).
        auto copy = [&](bool dbl, u32 sign_xor, u32 sign_and) {
            const unsigned s = dbl ? dnum(rs2) : rs2, d = dbl ? dnum(rd) : rd;
            e.mov(rax, freg(s), false);
            if (sign_xor) e.alu_imm(Alu::Xor, rax, static_cast<s32>(sign_xor), false);
            if (sign_and != 0xFFFFFFFF) e.alu_imm(Alu::And, rax, static_cast<s32>(sign_and), false);
            e.mov(freg(d), rax, false);
            if (dbl) {
                e.mov(rax, freg(s + 1), false);
                e.mov(freg(d + 1), rax, false);
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
                e.test32(rax, rax);
                const std::size_t no = e.jcc(Cond::E);
                copy((opf & 3) == 2, 0, 0xFFFFFFFF);
                e.bind(no);
                break;
            }
            case F::Compare: {  // FCMP{E}{s,d}: fcc_n; invalid on a signaling NaN (FCMPE: any NaN), as the host's compares
                const bool dbl = (opf & 3) == 2;
                const unsigned fcc_n = rd & 3;
                const Mem host_mxcsr{rsp, kMxcsr}, guest_mxcsr{rsp, kMxcsr + 4};
                if (dbl) {
                    load_double(rax, rs1);
                    e.movq_to_xmm(0, rax);
                    load_double(rax, rs2);
                    e.movq_to_xmm(1, rax);
                } else {
                    e.movd_load(0, freg(rs1));
                    e.movd_load(1, freg(rs2));
                }
                e.stmxcsr(host_mxcsr);
                e.mov_imm32(rax, 0x1F80);  // flags clear
                e.mov(guest_mxcsr, rax, false);
                e.ldmxcsr(guest_mxcsr);
                if (opf & 4) e.comis(dbl, 0, 1);
                else e.ucomis(dbl, 0, 1);
                e.setcc(Cond::B, rcx);  // 1: rs1 < rs2 (or unordered)
                e.setcc(Cond::A, rdx);  // 2: rs1 > rs2
                e.setcc(Cond::P, rax);  // 3: unordered
                e.stmxcsr(guest_mxcsr);
                e.ldmxcsr(host_mxcsr);
                e.movzx8(rcx, rcx);
                e.movzx8(rdx, rdx);
                e.movzx8(rax, rax);
                e.alu(Alu::Or, rdx, rax, false);
                e.alu(Alu::Add, rdx, rdx, false);
                e.alu(Alu::Or, rcx, rdx, false);
                // cexc = NV if invalid (unless NVM traps: execute()), else 0; aexc gathers it.
                e.mov(r8, guest_mxcsr, false);
                e.alu_imm(Alu::And, r8, 1, false);
                e.shift_imm(Shift::Shl, r8, 4, false);
                const std::size_t valid = e.jcc(Cond::E);
                e.test8_imm(Mem{kCpu, at(fsr_).disp + 3}, 0x08);  // TEM.NVM (bit 27)
                slow.push_back(e.jcc(Cond::Ne));
                e.bind(valid);
                const unsigned shift = fcc_n == 0 ? 10 : 30 + 2 * fcc_n;
                e.mov(rdx, at(fsr_));
                e.mov_imm64(rax, ~((u64{3} << shift) | 0x1F));
                e.alu(Alu::And, rdx, rax);
                e.shift_imm(Shift::Shl, rcx, static_cast<u8>(shift), true);
                e.alu(Alu::Or, rdx, rcx);
                e.alu(Alu::Or, rdx, r8);
                e.shift_imm(Shift::Shl, r8, 5, false);
                e.alu(Alu::Or, rdx, r8);
                e.mov(at(fsr_), rdx);
                break;
            }
            case F::Logic: {  // even opf the double form, odd the single
                const bool single = opf & 1;
                if (single) {
                    e.mov(rax, freg(rs1), false);
                    e.mov(rcx, freg(rs2), false);
                } else {
                    e.mov(rax, freg(dnum(rs1)));
                    e.mov(rcx, freg(dnum(rs2)));
                }
                switch ((opf - 0x060) >> 1) {
                    case 0: e.alu(Alu::Xor, rax, rax, false); break;                    // FZERO
                    case 1: e.alu(Alu::Or, rax, rcx), e.not_(rax); break;               // FNOR
                    case 2: e.not_(rcx), e.alu(Alu::And, rax, rcx); break;              // FANDNOT2
                    case 3: e.mov(rax, rcx), e.not_(rax); break;                        // FNOT2
                    case 4: e.not_(rax), e.alu(Alu::And, rax, rcx); break;              // FANDNOT1
                    case 5: e.not_(rax); break;                                         // FNOT1
                    case 6: e.alu(Alu::Xor, rax, rcx); break;                           // FXOR
                    case 7: e.alu(Alu::And, rax, rcx), e.not_(rax); break;              // FNAND
                    case 8: e.alu(Alu::And, rax, rcx); break;                           // FAND
                    case 9: e.alu(Alu::Xor, rax, rcx), e.not_(rax); break;              // FXNOR
                    case 10: break;                                                     // FSRC1
                    case 11: e.not_(rcx), e.alu(Alu::Or, rax, rcx); break;              // FORNOT2
                    case 12: e.mov(rax, rcx); break;                                    // FSRC2
                    case 13: e.not_(rax), e.alu(Alu::Or, rax, rcx); break;              // FORNOT1
                    case 14: e.alu(Alu::Or, rax, rcx); break;                           // FOR
                    default: e.mov_imm_sx(rax, -1); break;                              // FONE
                }
                if (single) {
                    e.mov(freg(rd), rax, false);
                    dirty(false);
                } else {
                    e.mov(freg(dnum(rd)), rax);
                    dirty(rd & 1);
                }
                break;
            }
            case F::AlignAddr: {  // ALIGNADDRESS(_LITTLE): rd = the sum aligned down; GSR.ALIGN = its low bits (negated)
                load_reg(rax, rs1);
                load_reg(rcx, rs2);
                e.alu(Alu::Add, rax, rcx);
                e.mov(rdx, rax);
                if (opf == 0x01A) e.neg(rdx);
                e.alu_imm(Alu::And, rdx, 7, false);
                e.mov(rcx, at(gsr_));
                e.alu_imm(Alu::And, rcx, -8);
                e.alu(Alu::Or, rcx, rdx);
                e.mov(at(gsr_), rcx);
                e.alu_imm(Alu::And, rax, -8);
                store_reg(rd, rax);
                break;
            }
            case F::AlignData: {  // FALIGNDATA: rs1:rs2 from byte GSR.ALIGN on
                e.mov(rcx, at(gsr_), false);
                e.alu_imm(Alu::And, rcx, 7, false);
                e.shift_imm(Shift::Shl, rcx, 3, false);
                load_double(rax, rs1);
                load_double(rdx, rs2);
                e.shld_cl(rax, rdx);
                store_double(rd, rax);
                dirty(rd & 1);
                break;
            }
            case F::None: break;
        }
        const std::size_t done = e.jmp();
        for (std::size_t site : slow) e.bind(site);
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
        std::vector<std::size_t> slow;
        fp_check(slow);
        if (o->edge) {  // the condition codes as by SUBcc rs1, rs2
            load_reg(rax, rs1);
            load_reg(rcx, rs2);
            e.mov(rdx, rax);
            e.alu(Alu::Sub, rdx, rcx, false);
            e.pushfq();
            e.pop(r8);
            e.alu(Alu::Sub, rax, rcx);
            e.pushfq();
            e.pop(r9);
            store_ccr(r8, r9);
        }
        auto operand = [&](vis::Src k, unsigned r, Reg to) {
            switch (k) {
                case vis::Src::R:
                    load_reg(to, r);
                    if (o->edge) mask(to);
                    break;
                case vis::Src::S: e.mov(to, freg(r), false); break;
                case vis::Src::D: load_double(to, r); break;
                case vis::Src::Gsr: e.mov(to, at(gsr_)); break;
                case vis::Src::Rd: load_double(to, rd); break;
                default: break;
            }
        };
        operand(o->a, rs1, kArg0);
        operand(o->b, rs2, kArg1);
        operand(o->c, rd, kArg2);
        e.mov_imm64(rax, reinterpret_cast<u64>(o->fn));
        e.call(rax);
        switch (o->dst) {
            case vis::Src::R: store_reg(rd, rax); break;
            case vis::Src::S:
                e.mov(freg(rd), rax, false);
                dirty(false);
                break;
            default:
                store_double(rd, rax);
                dirty(rd & 1);
                break;
        }
        const std::size_t done = e.jmp();
        for (std::size_t site : slow) e.bind(site);
        call_execute(i, insn);
        e.bind(done);
        return true;
    };

    // FPops that may raise exceptions: arithmetic, square roots and
    // conversions, on SSE under the guest rounding mode, with the
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
        const Mem host_mxcsr{rsp, kMxcsr}, guest_mxcsr{rsp, kMxcsr + 4};
        const Mem fsr_byte2{kCpu, at(fsr_).disp + 2};  // NS (bit 22)
        std::vector<std::size_t> slow;
        fp_check(slow);
        e.test8_imm(fsr_byte2, 0x40);
        slow.push_back(e.jcc(Cond::Ne));
        auto load = [&](unsigned x, unsigned r) {
            if (src == T::F32) e.movd_load(x, freg(r));
            else load_double(rax, r), e.movq_to_xmm(x, rax);
        };
        const bool fsrc = src == T::F32 || src == T::F64;
        if (fsrc) {
            load(1, rs2);
            if (two) load(0, rs1);
            e.ucomis(src == T::F64, two ? 0 : 1, 1);
            slow.push_back(e.jcc(Cond::P));  // a NaN
        } else if (src == T::I32) {
            e.mov(rax, freg(rs2), false);
        } else {
            load_double(rax, rs2);
        }
        // The guest's MXCSR: all exceptions masked, flags clear, RC from
        // FSR.RD (nearest, zero, +inf, -inf as x86 0, 3, 2, 1).
        e.stmxcsr(host_mxcsr);
        e.mov(rcx, at(fsr_), false);
        e.shift_imm(Shift::Shr, rcx, 29, false);
        e.alu_imm(Alu::And, rcx, 6, false);  // RD * 2
        e.mov_imm32(rdx, 0x6C);
        e.shift_cl(Shift::Shr, rdx, false);
        e.alu_imm(Alu::And, rdx, 3, false);
        e.shift_imm(Shift::Shl, rdx, 13, false);
        e.alu_imm(Alu::Or, rdx, 0x1F80, false);
        e.mov(guest_mxcsr, rdx, false);
        e.ldmxcsr(guest_mxcsr);
        using Sse = Emitter::Sse;
        switch (opf) {
            case 0x041: case 0x042: e.sse(Sse::Add, src == T::F64, 0, 1); break;
            case 0x045: case 0x046: e.sse(Sse::Sub, src == T::F64, 0, 1); break;
            case 0x049: case 0x04A: e.sse(Sse::Mul, src == T::F64, 0, 1); break;
            case 0x04D: case 0x04E: e.sse(Sse::Div, src == T::F64, 0, 1); break;
            case 0x069:  // the exact product
                e.sse(Sse::Cvt, false, 0, 0);
                e.sse(Sse::Cvt, false, 1, 1);
                e.sse(Sse::Mul, true, 0, 1);
                break;
            case 0x029: case 0x02A: e.sse(Sse::Sqrt, src == T::F64, 0, 1); break;
            case 0x0C9: case 0x0C6: e.sse(Sse::Cvt, src == T::F64, 0, 1); break;
            case 0x0C4: case 0x0C8: case 0x084: case 0x088: e.cvt_from_int(dst == T::F64, 0, rax, src == T::I64); break;
            default: e.cvt_to_int(src == T::F64, true, rax, 1, dst == T::I64); break;
        }
        e.stmxcsr(guest_mxcsr);
        e.ldmxcsr(host_mxcsr);
        e.mov(rcx, guest_mxcsr, false);
        const bool to_int = dst == T::I32 || dst == T::I64;
        if (to_int) {  // (an invalid conversion's result is not the host's)
            e.test_imm(rcx, 1);
            slow.push_back(e.jcc(Cond::Ne));
        }
        // r9 = the exceptions as cexc bits; to execute() if any traps.
        e.alu_imm(Alu::And, rcx, 0x3F, false);
        e.mov_imm64(r8, reinterpret_cast<u64>(kMxcsrExc.v));
        e.alu(Alu::Add, r8, rcx);
        e.movzx8(r9, Mem{r8, 0});
        e.mov(rdx, at(fsr_));
        e.mov(r10, rdx);
        e.shift_imm(Shift::Shr, r10, 23, true);  // TEM
        e.alu(Alu::And, r10, r9, false);
        e.test_imm(r10, 0x1F);
        slow.push_back(e.jcc(Cond::Ne));
        if (!to_int) {  // invalid: the default NaN
            e.test_imm(rcx, 1);
            const std::size_t valid = e.jcc(Cond::E);
            if (dst == T::F64) {
                e.mov_imm64(rax, 0x7FFFFFFFFFFFFFFFull);
                e.movq_to_xmm(0, rax);
            } else {
                e.mov_imm32(rax, 0x7FFFFFFF);
                e.movq_to_xmm(0, rax, false);
            }
            e.bind(valid);
        }
        // cexc = the exceptions; aexc gathers them.
        e.alu_imm(Alu::And, rdx, ~0x1F);
        e.alu(Alu::Or, rdx, r9);
        e.shift_imm(Shift::Shl, r9, 5, false);
        e.alu(Alu::Or, rdx, r9);
        e.mov(at(fsr_), rdx);
        switch (dst) {
            case T::F32: e.movd_store(freg(rd), 0); break;
            case T::F64: e.movq_from_xmm(rax, 0), store_double(rd, rax); break;
            case T::I32: e.mov(freg(rd), rax, false); break;
            case T::I64: store_double(rd, rax); break;
        }
        dirty((dst == T::F64 || dst == T::I64) && (rd & 1));
        const std::size_t done = e.jmp();
        for (std::size_t site : slow) e.bind(site);
        call_execute(i, insn);
        e.bind(done);
        return true;
    };

    // rax = guest time (cycles()) as instruction i starts: the step count
    // then, through the CPU's time base and charge per step. Clobbers rcx.
    auto tick_time = [&](unsigned i) {
        e.mov(rax, m_cycles0);
        e.alu(Alu::Add, rax, m_total);
        if (i) e.alu_imm(Alu::Add, rax, static_cast<s32>(i));
        e.alu(Alu::Sub, rax, at(step_base_));
        e.imul(rax, at(cpi_fx_));
        e.shift_imm(Shift::Shr, rax, 16, true);
        e.alu(Alu::Add, rax, at(time_base_));
    };
    // Instructions that need their place in the block: SAVE and RESTORE
    // (spill, fill and clean-window traps go to execute()), and reading
    // TICK in privileged code. False if not done here.
    auto special = [&](unsigned i, u32 insn) -> bool {
        const unsigned op3 = (insn >> 19) & 0x3F, rd = (insn >> 25) & 31, rs1 = (insn >> 14) & 31;
        if (op3 == 0x3C || op3 == 0x3D) {  // SAVE, RESTORE
            const bool save = op3 == 0x3C;
            self = false;  // (the check mode compares registers only)
            std::vector<std::size_t> slow;
            if (save) {
                e.mov(rax, at(cansave_), false);
                e.test32(rax, rax);
                slow.push_back(e.jcc(Cond::E));  // spill
                e.mov(rax, at(cleanwin_), false);
                e.alu(Alu::Cmp, rax, at(canrestore_), false);
                slow.push_back(e.jcc(Cond::E));  // clean_window
            } else {
                e.mov(rax, at(canrestore_), false);
                e.test32(rax, rax);
                slow.push_back(e.jcc(Cond::E));  // fill
            }
            load_reg(rdx, rs1);  // the sum, in the old window
            load_op2(rcx, insn);
            e.alu(Alu::Add, rdx, rcx);
            move_window(save);
            store_reg(rd, rdx);
            const std::size_t done = e.jmp();
            for (std::size_t site : slow) e.bind(site);
            call_execute(i, insn);
            e.bind(done);
            return true;
        }
        const bool priv = pstate_ & pstate::PRIV;  // (part of the mode)
        if ((op3 == 0x2A || op3 == 0x28) && rs1 == 4 && (fmode == 1 || fmode == 2)) {  // RDPR, RD %tick
            self = false;  // (TICK follows cycles_, which the check mode does not)
            if (rd == 0) return true;
            tick_time(i);
            e.alu(Alu::Add, rax, at(tick_offset_));
            e.shift_imm(Shift::Shl, rax, 1, true);  // bits 62:0
            e.shift_imm(Shift::Shr, rax, 1, true);
            e.movzx8(rcx, at(tick_npt_));
            e.shift_imm(Shift::Shl, rcx, 63, true);
            e.alu(Alu::Or, rax, rcx);
            store_reg(rd, rax);
            return true;
        }
        if (op3 == 0x2A && priv && rs1 != 4) {  // RDPR (the trap registers need TL > 0)
            static_assert(sizeof(TrapLevel) == 32);
            const bool trap_level = rs1 <= 3;
            if (trap_level && tl_ == 0) return false;  // (TL > 0 is part of the mode)
            if (rs1 > 14 && rs1 != 31) return false;
            if (rd == 0) return true;
            if (trap_level) {
                e.mov(rax, at(tl_), false);
                e.shift_imm(Shift::Shl, rax, 5, false);
                e.alu(Alu::Add, rax, kCpu);
                const s32 field = rs1 == 0 ? offsetof(TrapLevel, tpc) : rs1 == 1 ? offsetof(TrapLevel, tnpc)
                                  : rs1 == 2 ? offsetof(TrapLevel, tstate) : offsetof(TrapLevel, tt);
                e.mov(rax, Mem{rax, at(trap_[0]).disp + field}, rs1 != 3);
            } else {
                switch (rs1) {
                    case 5: e.mov(rax, at(tba_)); break;
                    case 6: e.movzx16(rax, at(pstate_)); break;
                    case 7: e.mov(rax, at(tl_), false); break;
                    case 8: e.mov(rax, at(pil_), false); break;
                    case 9: e.mov(rax, at(cwp_), false); break;
                    case 10: e.mov(rax, at(cansave_), false); break;
                    case 11: e.mov(rax, at(canrestore_), false); break;
                    case 12: e.mov(rax, at(cleanwin_), false); break;
                    case 13: e.mov(rax, at(otherwin_), false); break;
                    case 14: e.mov(rax, at(wstate_), false); break;
                    default: e.mov(rax, at(opt_.version)); break;
                }
            }
            store_reg(rd, rax);
            return true;
        }
        if (op3 == 0x28 && rs1 == 4 && !priv) {  // RD %tick: privileged_action if TICK.NPT
            self = false;
            e.cmp8_imm(at(tick_npt_), 0);
            const std::size_t npt = e.jcc(Cond::Ne);
            if (rd) {
                tick_time(i);
                e.alu(Alu::Add, rax, at(tick_offset_));
                e.shift_imm(Shift::Shl, rax, 1, true);  // bits 62:0 (NPT clear)
                e.shift_imm(Shift::Shr, rax, 1, true);
                store_reg(rd, rax);
            }
            const std::size_t done = e.jmp();
            e.bind(npt);
            call_execute(i, insn);
            e.bind(done);
            return true;
        }
        if ((op3 == 0x28 && rs1 == 19) || (op3 == 0x30 && rd == 19)) {  // RD, WR %gsr: fp_disabled from execute()
            std::vector<std::size_t> off;
            fp_check(off);
            if (op3 == 0x28) {
                if (rd) {
                    e.mov(rax, at(gsr_));
                    store_reg(rd, rax);
                }
            } else {
                load_reg(rax, rs1);
                load_op2(rcx, insn);
                e.alu(Alu::Xor, rax, rcx);
                e.alu_imm(Alu::And, rax, 0x7F);
                e.mov(at(gsr_), rax);
            }
            const std::size_t done = e.jmp();
            for (std::size_t site : off) e.bind(site);
            call_execute(i, insn);
            e.bind(done);
            return true;
        }
        if (op3 == 0x28) {  // RDASR: Y, CCR, ASI, PC, FPRS; STBAR and MEMBAR
            if (rs1 == 15 && rd == 0) return true;  // memory is always ordered here
            switch (rs1) {
                case 0: e.mov(rax, at(y_)); break;
                case 2: e.movzx8(rax, m_ccr); break;
                case 3: e.movzx8(rax, at(asi_)); break;
                case 5:
                    pc_plus(rax, 4 * static_cast<s64>(i));
                    mask(rax);
                    break;
                case 6: e.movzx8(rax, at(fprs_)); break;
                case 16:  // PCR, PIC, DCR, SOFTINT, TICK_CMPR: privileged here
                case 17:
                case 18:
                case 22:
                case 23: {
                    if (!priv) return false;
                    const u64& r = rs1 == 16 ? pcr_ : rs1 == 17 ? pic_ : rs1 == 18 ? dcr_ : rs1 == 22 ? softint_ : tick_cmpr_;
                    e.mov(rax, at(r));
                    break;
                }
                default: return false;
            }
            store_reg(rd, rax);
            return true;
        }
        // WR %y, %ccr, %asi, %fprs, and in privileged code %pcr, %pic, %dcr: rs1 xor operand 2
        if (op3 == 0x30 && (rd == 0 || rd == 2 || rd == 3 || rd == 6 || (priv && rd >= 16 && rd <= 18))) {
            load_reg(rax, rs1);
            load_op2(rcx, insn);
            e.alu(Alu::Xor, rax, rcx);
            if (rd == 0) {
                e.mov32(rax, rax);
                e.mov(at(y_), rax);
            } else if (rd == 6) {
                e.alu_imm(Alu::And, rax, 7, false);
                e.mov8(at(fprs_), rax);
            } else if (rd >= 16) {
                e.mov(at(rd == 16 ? pcr_ : rd == 17 ? pic_ : dcr_), rax);
            } else {
                e.mov8(rd == 2 ? m_ccr : at(asi_), rax);
            }
            return true;
        }
        if (op3 == 0x3B) return true;  // FLUSH: stores to code are seen at once here
        if (op3 == 0x36 && ((insn >> 5) & 0x1FF) == 0x080 && priv) {  // SHUTDOWN: nothing to do (fp_disabled from execute())
            std::vector<std::size_t> off;
            fp_check(off);
            const std::size_t done = e.jmp();
            for (std::size_t site : off) e.bind(site);
            call_execute(i, insn);
            e.bind(done);
            return true;
        }
        if (op3 == 0x2B) {  // FLUSHW: a spill trap (from execute()) unless only the current window is in use
            e.alu_imm(Alu::Cmp, at(cansave_), kWindows - 2, false);
            const std::size_t flushed = e.jcc(Cond::E);
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
            const std::size_t not_taken = e.jcc(Cond::E);
            call_execute(i, insn);
            e.bind(not_taken);
            return true;
        }
        if (op3 == 0x31 && priv && rd <= 1) {  // SAVED, RESTORED
            auto dec = [&](const auto& field) {
                e.alu_imm(Alu::Add, at(field), kWindows - 1, false);
                e.alu_imm(Alu::And, at(field), kWindows - 1, false);
            };
            auto inc = [&](const auto& field) {
                e.alu_imm(Alu::Add, at(field), 1, false);
                e.alu_imm(Alu::And, at(field), kWindows - 1, false);
            };
            if (rd == 0) {
                inc(cansave_);
            } else {
                inc(canrestore_);
                e.alu_imm(Alu::Cmp, at(cleanwin_), kWindows - 1, false);
                const std::size_t full = e.jcc(Cond::Ae);
                e.alu_imm(Alu::Add, at(cleanwin_), 1, false);
                e.bind(full);
            }
            e.alu_imm(Alu::Cmp, at(otherwin_), 0, false);
            const std::size_t other = e.jcc(Cond::Ne);
            if (rd == 0) dec(canrestore_);
            else dec(cansave_);
            const std::size_t done = e.jmp();
            e.bind(other);
            dec(otherwin_);
            e.bind(done);
            return true;
        }
        return false;
    };

    // Prologue: the callee-saved registers, then the entry counts and any
    // shadow space, keeping calls 16-byte aligned.
    prologue(e);
    e.mov(kCpu, kArg0);
    e.mov(kPc, kArg1);
    e.mov(m_budget, kArg2);
    e.mov_store_imm(m_total, 0);
    e.mov(rax, at(cycles_));
    e.mov(m_cycles0, rax);
    e.mov(rax, at(instret_));
    e.mov(m_instret0, rax);
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
                e.mov_imm32(rax, (insn & 0x3FFFFF) << 10);
                store_reg(rd, rax);
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
                    pc_plus(rax, here + 4);
                    e.alu(Alu::Cmp, rax, kNpc);
                    const std::size_t on = e.jcc(Cond::E);
                    store_pc_npc_from(kNpc);
                    leave(i + 1);
                    e.bind(on);
                } else if (known_target) {
                    if (may_fall) {
                        pc_plus(rax, target);
                        mask(rax);
                        e.alu(Alu::Cmp, rax, kNpc);
                        const std::size_t fell = e.jcc(Cond::Ne);
                        chain_to(target, n);
                        e.bind(fell);
                        chain_to(fall, n);
                    } else {
                        chain_to(target, n);
                    }
                    ended = true;
                } else {
                    e.mov(rdx, kNpc);
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
        if (dynamic) e.mov(kNpc, m_npc);
    }
    // The end of the block.
    if (ended) {
    } else if (last_executed) {
        // execute() left pc_ and npc_ in place.
        e.mov(rdx, m_pc);
        e.mov(rax, m_npc);
        e.alu(Alu::Sub, rax, rdx);
        e.alu_imm(Alu::Cmp, rax, 4);
        const std::size_t sequential = e.jcc(Cond::E);
        leave(n);
        e.bind(sequential);
        chain(n);
    } else if (dynamic) {  // the last instruction was a control transfer: its delay slot is next
        pc_plus(rax, 4 * static_cast<s64>(n));
        e.mov(m_pc, rax);
        e.mov(m_npc, kNpc);
        leave(n);
    } else {
        chain_to(4 * static_cast<s64>(n), n);
    }
    for (std::size_t site : exits) e.bind(site);
    epilogue(e);

    const void* code = jit_buf_->add(e.code(), e.size());
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

#elif !(defined(__aarch64__) || defined(_M_ARM64))  // (jit_a64.cpp)

namespace uv::usparc {
bool UltraSparc::jit_compile_supported() { return false; }
bool UltraSparc::jit_compile(Block&) { return false; }
}  // namespace uv::usparc

#endif
