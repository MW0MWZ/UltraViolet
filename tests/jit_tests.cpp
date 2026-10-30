// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
// The decoded-block cache and the recompiler against the interpreter: loops
// run long enough to be translated must end exactly as step() leaves them.
#include <array>
#include <functional>
#include <vector>

#include "cpu/ultrasparc/ultrasparc.h"
#include "cpu_rig.h"
#include "sparc_asm.h"
#include "test.h"

using namespace uv;
using namespace uv::usparc;
using namespace uv::test::sparc;
using uv::test::Rig;

namespace {

constexpr u64 kData = 0x10000, kDataSize = 0x1000;

struct State {
    u64 pc, npc, y, fsr;
    unsigned ccr, cwp, tl, tt, fprs;
    std::array<u64, 32> r;
    std::array<u32, 64> f;
    std::vector<u8> data;
};

State state_of(const Rig& rig) {
    State s{rig.cpu.pc(), rig.cpu.npc(), rig.cpu.y(), rig.cpu.fsr(), rig.cpu.ccr(), rig.cpu.cwp(), rig.cpu.tl(), 0,
            rig.cpu.fprs(), {}, {}, {}};
    s.tt = s.tl ? rig.cpu.trap_type(s.tl) : 0;
    for (unsigned r = 0; r < 32; ++r) s.r[r] = rig.cpu.reg(r);
    for (unsigned r = 0; r < 64; ++r) s.f[r] = rig.cpu.freg32(r);
    s.data.assign(rig.ram.begin() + kData, rig.ram.begin() + kData + kDataSize);
    return s;
}

void check_same(const State& a, const State& b) {
    CHECK_EQ(a.pc, b.pc);
    CHECK_EQ(a.npc, b.npc);
    CHECK_EQ(a.y, b.y);
    CHECK_EQ(a.fsr, b.fsr);
    CHECK_EQ(a.fprs, b.fprs);
    for (unsigned r = 0; r < 64; ++r) CHECK_EQ(a.f[r], b.f[r]);
    CHECK_EQ(a.ccr, b.ccr);
    CHECK_EQ(a.cwp, b.cwp);
    CHECK_EQ(a.tl, b.tl);
    CHECK_EQ(a.tt, b.tt);
    for (unsigned r = 0; r < 32; ++r) CHECK_EQ(a.r[r], b.r[r]);
    CHECK(a.data == b.data);
}

// Runs `body` `passes` times, %g1 counting down from `passes` to 1, then
// spins; once on a rig with the recompiler (or, on hosts without one, the
// block cache) and once with every instruction through step(). `setup`
// prepares both the same way; `extra` is how long the spin runs (if less
// than the loop needs, it stops part way).
void check_against_interpreter(const std::vector<u32>& body, unsigned passes, const std::function<void(Rig&)>& setup = {},
                               u16 pstate = pstate::PRIV | pstate::PEF, unsigned extra = 500, std::size_t buffer = 0) {
    const auto n = static_cast<int>(body.size());
    std::vector<u32> code;
    code.push_back(MOVi(g1, static_cast<int>(passes)));
    code.insert(code.end(), body.begin(), body.end());
    code.push_back(SUBcci(g1, g1, 1));
    code.push_back(BPcc(cNE, kXcc, -(n + 1)));
    code.push_back(NOP);
    code.push_back(Bicc(cA, 0));
    code.push_back(NOP);

    Rig fast, ref;
    ref.cpu.set_block_cache(false);
    if (buffer) fast.cpu.set_jit(true, buffer);
    for (Rig* r : {&fast, &ref}) {
        r->cpu.set_pstate(pstate);
        for (std::size_t i = 0; i < code.size(); ++i) r->bus.write(Rig::kCode + 4 * i, code[i]);
        r->cpu.set_reg(o5, kData);
        r->cpu.set_reg(o4, kData + kDataSize / 2);
        if (setup) setup(*r);
        r->run_at(Rig::kCode, passes * (code.size() + 2) + extra);
    }
    if (UltraSparc::jit_available()) CHECK(fast.cpu.jit_compiles() > 0);
    CHECK(!ref.cpu.halted());  // (no trap went unhandled)
    if (extra >= 500) CHECK_EQ(ref.cpu.reg(g1), 0u);  // (the loop ran to the end)
    check_same(state_of(fast), state_of(ref));
}

}  // namespace

TEST(jit_alu_operations_and_condition_codes_match_the_interpreter) {
    std::vector<u32> body = {SLLXi(g2, g1, 58), SLLi(g3, g1, 26), XORi(g5, g1, -1), SRAXi(g6, g2, 3)};
    // Each condition-code operation, then the codes it left, stored.
    const u32 cc_ops[] = {ADDcc(g4, g2, g3),  ADDcc(g4, g3, g3),   ADDcc(g4, g6, g2), SUBcc(g4, g2, g3),
                          SUBcc(g4, g3, g2),  SUBcci(g4, g1, 32),  ADDcci(g4, g3, -1), ADDcci(g4, g5, 1),
                          ANDcc(g4, g2, g3),  ORcc(g4, g2, g0),    XORcc(g4, g3, g3), ANDNcc(g4, g2, g3),
                          ORNcc(g4, g0, g3),  XNORcc(g4, g2, g3),  SUBcc(g0, g6, g5), ADDcc(g0, g5, g5)};
    int k = 0;
    for (u32 op : cc_ops) {
        body.push_back(op);
        body.push_back(RDASR(o0, 2));
        body.push_back(STB(o0, o5, k++));
        body.push_back(ADD(l0, l0, g4));
    }
    const u32 more[] = {SUB(l1, l1, g2),  AND(l2, g5, g3),  OR(l3, g2, g6),   XOR(l4, l4, g5),  ANDN(l5, g6, g3),
                        ORN(l6, g3, g2),  XNOR(l7, l7, g1), MULX(i0, g2, g5), MULXi(i1, g5, -77),
                        SLL(i2, g5, g1),  SRL(i3, g5, g1),  SRA(i4, g6, g1),  SLLX(i5, g5, g1),  SRLX(o1, g6, g1),
                        SRAX(o2, g6, g1), SRAi(o3, g5, 5),  SRLi(g7, g6, 31), SRLXi(l0, l0, 1),
                        ADDi(g0, g1, 1),  ORi(g0, g0, 5)};
    body.insert(body.end(), std::begin(more), std::end(more));
    body.push_back(ADDi(o5, o5, 16));
    check_against_interpreter(body, 64);
}

TEST(jit_branches_annulment_and_conditional_moves_match_the_interpreter) {
    const std::vector<u32> body = {
        ANDcci(g0, g1, 1),
        Bicc(cE, 3, true), ADDi(o1, o1, 1), ADDi(o2, o2, 1),        // be,a
        ANDcci(g0, g1, 2),
        BPcc(cNE, kXcc, 3, true), ADDi(o1, o1, 2), ADDi(o2, o2, 2),  // bne,a %xcc
        ANDcci(g0, g1, 4),
        Bicc(cE, 3), ADDi(o1, o1, 4), ADDi(o2, o2, 4),               // be
        Bicc(cA, 2, true), ADDi(o3, o3, 1),                          // ba,a: the delay slot is annulled
        Bicc(cN, 2, true), ADDi(o3, o3, 2),                          // bn,a
        Bicc(cN, 2), ADDi(o3, o3, 4),                                // bn: the delay slot runs
        ANDi(l0, g1, 3), SUBi(l0, l0, 1),                            // -1 to 2
        BPr(1, l0, 3), ADDi(o4, o4, 1), ADDi(o4, o4, 2),             // brz
        BPr(3, l0, 3, true), ADDi(o4, o4, 4), ADDi(o4, o4, 8),       // brlz,a
        BPr(6, l0, 3), ADDi(o4, o4, 16), ADDi(o4, o4, 32),           // brgz
        SUBcci(g0, l0, 1),
        MOVcc(cG, kIcc, l1, 5), MOVcc(cLE, kXcc, l2, -3), MOVr(7, l3, l0, 9), MOVr(2, l4, l0, -7),
        ADD(l5, l5, l1), ADD(l5, l5, l2), ADD(l5, l5, l3), ADD(l5, l5, l4),
        CALL(2), ADDi(o3, o3, 64),                                   // to the jmpl, o7 = here
        JMPLi(g0, o7, 16), ADDi(o3, o3, 128),                        // to just after its delay slot
        ADD(l6, l6, o7)};
    check_against_interpreter(body, 64);
    check_against_interpreter(body, 64, {}, pstate::PRIV | pstate::PEF | pstate::AM);
}

TEST(jit_loads_and_stores_of_every_width_match_the_interpreter) {
    const std::vector<u32> body = {
        SLLXi(g2, g1, 57), ORi(g2, g2, 0x5A5), XOR(g2, g2, g1), SUBi(g3, g0, 0x123),
        STX(g2, o5, 0), STW(g3, o5, 8), STH(g2, o5, 12), STB(g3, o5, 14), STB(g1, o5, 15),
        LDX(l0, o5, 0), LDUW(l1, o5, 4), LDSW(l2, o5, 8), LDUH(l3, o5, 12), LDSH(l4, o5, 8), LDUB(l5, o5, 14),
        LDSB(l6, o5, 14), f3r(3, 0x00, l7, o5, g0) /* lduw [%o5 + %g0] */,
        ADD(i0, i0, l0), ADD(i0, i0, l1), ADD(i0, i0, l2), ADD(i0, i0, l3), ADD(i0, i0, l4), ADD(i0, i0, l5),
        ADD(i0, i0, l6), ADD(i0, i0, l7), STX(i0, o4, 0), LDUB(g0, o5, 1),
        ADDi(o5, o5, 16)};
    check_against_interpreter(body, 64);
    check_against_interpreter(body, 64, {}, pstate::PRIV | pstate::PEF | pstate::AM);
}

TEST(jit_sees_a_store_into_code_it_has_translated) {
    // At %g1 == 24 the loop overwrites one of its own instructions.
    const std::vector<u32> body = {
        CMPi(g1, 24),
        BPcc(cNE, kXcc, 3), NOP,
        STW(o3, o4, 0),
        ADDi(o1, o1, 1),  // replaced by o3
        ADDi(o2, o2, 1)};
    check_against_interpreter(body, 64, [](Rig& r) {
        r.cpu.set_reg(o3, ADDi(o1, o1, 100));
        r.cpu.set_reg(o4, Rig::kCode + 4 * 5);
    });
}

TEST(jit_calls_and_register_windows_match_the_interpreter) {
    // The return lands on code that uses the caller's window at once (the
    // routine's last instruction, the RESTORE, changes it).
    const std::vector<u32> body = {
        ADDi(o0, g1, 3),
        CALL(6), ADDi(o1, g1, 5),      // to the SAVE
        ADD(l6, l6, o2), ADD(l5, l5, o0),
        Bicc(cA, 6, true), NOP,        // over the routine
        SAVEi(sp, sp, -192), ADD(l0, i0, i1), JMPLi(g0, i7, 8), RESTORE(o2, l0, i0),
        ADD(l4, l4, o1)};
    check_against_interpreter(body, 64, [](Rig& r) { r.cpu.set_reg(sp, 0x20000); });
}

TEST(jit_window_spills_and_fills_match_the_interpreter) {
    // Eight SAVEs deep and back: the spill and fill handlers run part way
    // (they only move the window counters, then retry).
    std::vector<u32> body;
    for (int k = 0; k < 8; ++k) body.push_back(SAVEi(o0, o0, 1));
    body.push_back(ADD(l1, l1, i0));
    for (int k = 0; k < 8; ++k) body.push_back(RESTOREi(o0, o0, 1));
    body.push_back(ADD(l2, l2, o0));
    check_against_interpreter(body, 40, [](Rig& r) {
        r.put(Rig::kTba + (tt::kSpillNormal << 5), {SAVED, RETRY});
        r.put(Rig::kTba + (tt::kFillNormal << 5), {RESTORED, RETRY});
    });
    // Each pass starting with two windows of another address space
    // (OTHERWIN), which the first spills and fills use up, and CLEANWIN
    // below its limit (the clean_window handler raises it).
    std::vector<u32> other = {WRPRi(prOTHERWIN, g0, 2), WRPRi(prCANSAVE, g0, 4), WRPRi(prCANRESTORE, g0, 0),
                              WRPRi(prCLEANWIN, g0, 3)};
    other.insert(other.end(), body.begin(), body.end());
    for (unsigned pr : {prCLEANWIN, prOTHERWIN, prCANSAVE, prCANRESTORE}) {
        other.push_back(RDPR(o1, pr));
        other.push_back(SLLXi(l3, l3, 3));
        other.push_back(ADD(l3, l3, o1));
    }
    check_against_interpreter(other, 40, [](Rig& r) {
        for (unsigned t : {tt::kSpillNormal, tt::kSpillOther}) r.put(Rig::kTba + (t << 5), {SAVED, RETRY});
        for (unsigned t : {tt::kFillNormal, tt::kFillOther}) r.put(Rig::kTba + (t << 5), {RESTORED, RETRY});
        r.put(Rig::kTba + (tt::kCleanWindow << 5), {WRPRi(prCLEANWIN, g0, 7), RETRY});
    }, pstate::PRIV | pstate::PEF, 4000);
}

TEST(jit_tick_reads_match_the_interpreter) {
    const std::vector<u32> body = {RDPR(l0, prTICK), ADD(l1, l1, l0), RDASR(l2, 4), SUB(l3, l2, l0), ADD(l4, l4, l3)};
    check_against_interpreter(body, 64);
    // At other charges per step (a faster and a slower processor).
    for (double cpi : {0.25, 0.7, 3.0})
        check_against_interpreter(body, 64, [cpi](Rig& r) { r.cpu.set_cycles_per_step(cpi); });
    const std::vector<u32> user = {RDASR(l2, 4), SUB(l3, l2, l0), ADD(l4, l4, l3), ORi(l0, l2, 0)};  // RD %tick in user code
    check_against_interpreter(user, 64, [](Rig& r) {
        r.cpu.set_cycles_per_step(0.3);
        r.cpu.set_pstate(pstate::PRIV | pstate::PEF);
        r.put(0x400, {WRPR(prTICK, g0, g0)});  // TICK.NPT clear
        r.run_at(0x400, 1);
        r.cpu.set_pstate(pstate::PEF);
    }, pstate::PEF);
}

TEST(jit_pairs_atomics_and_alternate_space_match_the_interpreter) {
    const std::vector<u32> body = {
        SLLXi(g2, g1, 40), XOR(g2, g2, g1), ADDi(g3, g1, -9),
        STD(g2, o5, 0), LDD(l0, o5, 0), ADD(i0, i0, l0), ADD(i0, i0, l1),
        LDSTUB(l2, o5, 3), LDSTUB(l3, o5, 3), ADD(i1, i1, l2), ADD(i1, i1, l3),
        SWAP(g3, o5, 4), ADD(i2, i2, g3),
        ADDi(o3, o5, 8), LDXA(l4, o3, g0, asi::P), CASXA(g2, o3, l4, asi::P), ADD(i3, i3, g2),
        ORi(g4, g1, 0), CASA(g4, o3, g0, asi::P) /* only if the word is zero */, ADD(i3, i3, g4),
        STXA(g3, o3, g0, asi::PL) /* little-endian: through execute() */, LDXA(l5, o3, g0, asi::P), ADD(i4, i4, l5),
        // Through %asi: the implicit ASI on even passes, little-endian on odd.
        ANDi(o1, g1, 1), SLLi(o1, o1, 3), ORi(o1, o1, asi::P), WRASR(3, o1, g0), SLLXi(o2, g1, 44), XORi(o2, o2, 0x5A5),
        f3i(3, 0x1E, o2, o3, 0) /* stxa %o2, [%o3]%asi */, f3i(3, 0x11, l6, o3, 1) /* lduba [%o3 + 1]%asi */,
        f3i(3, 0x1A, l7, o3, 2) /* ldsha */, f3i(3, 0x18, o2, o3, 4) /* ldswa */, f3i(3, 0x1B, o1, o3, 0) /* ldxa */,
        ADD(i5, i5, l6), ADD(i5, i5, l7), ADD(i5, i5, o2), ADD(i5, i5, o1),
        f3i(3, 0x2D, 0, o3, 0) /* prefetch */, f3r(3, 0x3D, 0, o3, g0) | (asi::P << 5) /* prefetcha */,
        ADDi(o5, o5, 16)};
    check_against_interpreter(body, 64);
}

TEST(jit_calls_into_another_page_match_the_interpreter) {
    // The routine is two 8 KiB pages away, so going there and back crosses
    // pages both ways.
    constexpr u64 kRoutine = 0x5000;
    const std::vector<u32> body = {ADDi(o0, g1, 1), CALL(static_cast<int>((kRoutine - (Rig::kCode + 8)) / 4)), NOP,
                                   ADD(l0, l0, o0), XOR(l1, l1, o0)};
    check_against_interpreter(body, 64, [](Rig& r) {
        r.cpu.set_reg(sp, 0x20000);
        r.put(kRoutine, {SAVEi(sp, sp, -192), SLLXi(l0, i0, 3), ADD(i0, i0, l0), JMPLi(g0, i7, 8), RESTORE(o0, i0, g0)});
    });
}

TEST(jit_sees_a_store_into_a_block_it_goes_on_into) {
    // At %g1 == 24 the loop overwrites the first instruction of a block that
    // the block before it goes on into directly.
    const std::vector<u32> body = {
        CMPi(g1, 24),
        BPcc(cNE, kXcc, 3), NOP,
        STW(o3, o4, 0),
        Bicc(cA, 2), NOP,
        ADDi(o1, o1, 1),  // replaced by o3
        ADDi(o2, o2, 1)};
    check_against_interpreter(body, 64, [](Rig& r) {
        r.cpu.set_reg(o3, ADDi(o1, o1, 100));
        r.cpu.set_reg(o4, Rig::kCode + 4 * 7);
    });
}

TEST(jit_tight_loops_stop_where_the_interpreter_does) {
    // A countdown in a branch's delay slot and a %tick wait, each a block
    // that goes back to its own start, stopped at every point in them.
    const std::vector<u32> body = {
        MOVi(l0, 40),
        BPcc(cG, kXcc, 0), SUBcci(l0, l0, 1),
        RDPR(l1, prTICK), ADDi(l1, l1, 30),
        SUBcc(g0, l1, l2), RDPR(l2, prTICK), BPcc(cCC, kXcc, -2), NOP,
        ADD(l3, l3, l2)};
    for (unsigned extra = 0; extra < 8; ++extra) check_against_interpreter(body, 8, {}, pstate::PRIV | pstate::PEF, extra);
}

TEST(jit_carries_multiplies_divides_and_tagged_arithmetic_match_the_interpreter) {
    // Divisors run through zero and -1 (the zero ones trap, as do some
    // tagged adds and the conditional trap; the handlers count them in %i5
    // and skip the instruction), and Y is set so that some 32-bit quotients
    // overflow, one being -2^63 / -1.
    const std::vector<u32> body = {
        SLLXi(g2, g1, 59), XOR(g2, g2, g1), SUBi(g3, g0, g1), ADDi(g4, g1, -32),
        ADDcc(g0, g2, g2),
        ADDC(l0, l0, g3), SUBC(l1, l1, g2),
        ADDCcc(l2, g2, g3), RDASR(o0, 2), STB(o0, o5, 0),
        SUBCcc(l3, g3, g2), RDASR(o0, 2), STB(o0, o5, 1),
        ADDCcci(l3, l3, -1), RDASR(o0, 2), STB(o0, o5, 2),
        UMUL(l4, g2, g3), RDASR(o1, 0), ADD(i0, i0, o1),
        SMUL(l5, g3, g2), RDASR(o1, 0), ADD(i0, i0, o1),
        UMULcc(o2, g3, g3), RDASR(o0, 2), STB(o0, o5, 3), ADD(i0, i0, o2),
        SMULcci(o2, g3, -3), RDASR(o0, 2), STB(o0, o5, 4), ADD(i0, i0, o2),
        WRASR(0, g3, g2), RDASR(o1, 0), ADD(i0, i0, o1),
        UDIV(l6, g2, g4), ADD(i1, i1, l6),
        SDIV(l6, g3, g4), ADD(i1, i1, l6),
        UDIVcc(o3, g2, g1), RDASR(o0, 2), STB(o0, o5, 5), ADD(i1, i1, o3),
        SDIVcc(o3, g3, g4), RDASR(o0, 2), STB(o0, o5, 6), ADD(i1, i1, o3),
        WRASRi(0, g0, 0),
        SDIVcc(o3, g3, g4), RDASR(o0, 2), STB(o0, o5, 7), ADD(i1, i1, o3),
        UDIVcci(o3, g2, 7), RDASR(o0, 2), STB(o0, o5, 8), ADD(i1, i1, o3),
        SRLXi(o1, l7, 32), WRASR(0, o1, g0), SDIVcc(l6, g0, g4), RDASR(o0, 2), STB(o0, o5, 12), ADD(i2, i2, l6),
        SDIVX(l6, l7, g4), ADD(i2, i2, l6),
        UDIVX(l6, g2, g4), ADD(i2, i2, l6),
        SDIVX(l6, g2, g4), ADD(i2, i2, l6),
        TADDcc(o3, g1, g4), RDASR(o0, 2), STB(o0, o5, 9),
        TSUBcc(o3, g2, g1), RDASR(o0, 2), STB(o0, o5, 10),
        TADDccTV(o4, g4, g4), ADD(i3, i3, o4),
        TSUBccTVi(o4, g1, 8), ADD(i3, i3, o4),
        ANDcci(g0, g1, 3), Tcc(cE, kIcc, 0x10), ADDi(i3, i3, 1),
        RDASR(o0, 5), ADD(i4, i4, o0),
        WRASRi(3, g1, 0x80), RDASR(o1, 3), ADD(i4, i4, o1),
        WRASRi(2, g1, 0), RDASR(o0, 2), STB(o0, o5, 11),
        RDASR(o0, 6), ADD(i4, i4, o0),
        RDASR(g0, 15) /* STBAR */, f3i(2, 0x28, 0, 15, 0x0F) /* MEMBAR */, FLUSHi(g0, o5, 0),
        ADDi(o5, o5, 13)};
    check_against_interpreter(body, 64, [](Rig& r) {
        r.cpu.set_reg(l7, u64{1} << 63);
        r.put(Rig::kTba + (tt::kDivisionByZero << 5), {ADDi(i5, i5, 1), DONE});
        r.put(Rig::kTba + (tt::kTagOverflow << 5), {ADDi(i5, i5, 0x20), DONE});
        r.put(Rig::kTba + ((tt::kTrapInstruction + 0x10) << 5), {ADDi(i5, i5, 0x400), DONE});
    });
    check_against_interpreter(body, 64, [](Rig& r) {
        r.cpu.set_reg(l7, u64{1} << 63);
        r.put(Rig::kTba + (tt::kDivisionByZero << 5), {ADDi(i5, i5, 1), DONE});
        r.put(Rig::kTba + (tt::kTagOverflow << 5), {ADDi(i5, i5, 0x20), DONE});
        r.put(Rig::kTba + ((tt::kTrapInstruction + 0x10) << 5), {ADDi(i5, i5, 0x400), DONE});
    }, pstate::PRIV | pstate::PEF | pstate::AM);
    // With room for only a few translations at a time, so that the buffer
    // is emptied and filled again many times.
    check_against_interpreter(body, 64, [](Rig& r) {
        r.cpu.set_reg(l7, u64{1} << 63);
        r.put(Rig::kTba + (tt::kDivisionByZero << 5), {ADDi(i5, i5, 1), DONE});
        r.put(Rig::kTba + (tt::kTagOverflow << 5), {ADDi(i5, i5, 0x20), DONE});
        r.put(Rig::kTba + ((tt::kTrapInstruction + 0x10) << 5), {ADDi(i5, i5, 0x400), DONE});
    }, pstate::PRIV | pstate::PEF, 500, 4096);
}

TEST(jit_return_matches_the_interpreter) {
    // Eight SAVEs deep, then back up by RETURN to just past each delay slot
    // (the fills run the handler, which retries the RETURN); then a RETURN
    // to a misaligned address, which traps.
    std::vector<u32> body;
    for (int k = 0; k < 8; ++k) body.push_back(SAVEi(o0, o0, 1));
    body.push_back(ADD(l1, l1, i0));
    for (int k = 0; k < 8; ++k) {
        body.push_back(CALL(2));
        body.push_back(NOP);
        body.push_back(RETURNi(g0, o7, 16));
        body.push_back(ADDi(o1, o1, 1));  // in the caller's window
    }
    body.push_back(ADD(l2, l2, o0));
    body.push_back(SAVEi(sp, sp, -192));
    body.push_back(CALL(2));
    body.push_back(NOP);
    body.push_back(RETURNi(g0, o7, 18));
    body.push_back(NOP);
    body.push_back(ADD(l3, l3, o0));
    check_against_interpreter(body, 40, [](Rig& r) {
        r.cpu.set_reg(sp, 0x20000);
        r.put(Rig::kTba + (tt::kSpillNormal << 5), {SAVED, RETRY});
        r.put(Rig::kTba + (tt::kFillNormal << 5), {RESTORED, RETRY});
        r.put(Rig::kTba + (tt::kMemAddressNotAligned << 5), {RESTORE(g0, g0, g0), DONE});  // on past the RETURN
    });
}

TEST(jit_floating_point_moves_compares_and_logic_match_the_interpreter) {
    // Doubles from integer patterns (a NaN every eighth pass) and singles
    // whose order changes half way (the singles in f0-f31 kept apart from
    // the doubles), through moves, compares into each fcc,
    // branches and conditional moves on them, and the VIS logical
    // operations, with FSR stored after each group; on four passes the FPU
    // is disabled before a different kind of instruction (the handler counts
    // it in %i5 and enables it again).
    auto disable_on = [](int pass) {
        return std::vector<u32>{SUBcci(g0, g1, pass), MOVi(o1, 4), MOVcc(cE, kIcc, o1, 0), WRASR(6, o1, g0)};
    };
    auto stxfsr = [](int off) { return f3i(3, 0x25, 1, o5, off); };
    std::vector<u32> body = {
        WRASRi(6, g0, 4),  // FPRS: FEF only, the dirty bits clear
        ADDi(g2, g1, 1000), SLLXi(g2, g2, 52), SLLXi(g3, g1, 63), XOR(g2, g2, g3),
        ANDi(g4, g1, 7), SETHI(g5, 0x7FF80000), SLLXi(g5, g5, 32), MOVr(5, g5, g4, 0), OR(g2, g2, g5) /* NaN if g1 % 8 == 0 */,
        SUBi(g3, g0, g1), SLLXi(g3, g3, 41), STX(g2, o5, 0), STX(g3, o5, 8),
        ADDi(g4, g1, 100), SLLi(g4, g4, 23), STW(g4, o5, 16), SETHI(g4, 112u << 23), STW(g4, o5, 20)};
    for (int pass : {10}) for (u32 w : disable_on(pass)) body.push_back(w);
    const std::vector<u32> moves = {
        LDDF(dreg(2), o5, 0), LDDF(dreg(36), o5, 8), LDF(5, o5, 16), LDF(7, o5, 20),
        FPOP2(3, dreg(2), dreg(2), 0x056) /* cexc NV on the NaN */, RDASR(o0, 6), ADD(i3, i3, o0), WRASRi(6, g0, 4),
        FPOP1(dreg(32), 0, dreg(2), 0x002), FPOP1(dreg(40), 0, dreg(36), 0x006), FPOP1(dreg(34), 0, dreg(36), 0x00A),
        FPOP1(9, 0, 5, 0x001), FPOP1(10, 0, 5, 0x005), FPOP1(11, 0, 10, 0x009), stxfsr(24),
        RDASR(o0, 6), ADD(i3, i3, o0), SLLXi(i3, i3, 1)};
    body.insert(body.end(), moves.begin(), moves.end());
    for (u32 w : disable_on(11)) body.push_back(w);
    const std::vector<u32> compares = {
        FPOP2(3, dreg(2), dreg(2), 0x056) /* FCMPEd: invalid on the NaN */, FPOP2(2, 5, 7, 0x051), stxfsr(32),
        FPOP2(0, dreg(2), dreg(36), 0x052), FPOP2(1, dreg(36), dreg(2), 0x052)};
    body.insert(body.end(), compares.begin(), compares.end());
    for (u32 w : disable_on(12)) body.push_back(w);
    const std::vector<u32> branches = {
        FBfcc(4, 3), ADDi(l0, l0, 1), ADDi(l1, l1, 1),          // fbl
        FBPfcc(4, 1, 3, true), ADDi(l0, l0, 2), ADDi(l1, l1, 2), // fbl,a %fcc1
        FBPfcc(6, 2, 3), ADDi(l0, l0, 4), ADDi(l1, l1, 4),       // fbg %fcc2
        FBfcc(8, 2, true), ADDi(l0, l0, 8),                      // fba,a
        FPOP2(12, 0, 5, (2u << 6) | 0x01) | (6u << 14) /* fmovsg %fcc2 */,
        FPOP2(dreg(38), 0, dreg(2), (0u << 6) | 0x02) | (13u << 14) /* fmovdle %fcc0 */,
        FPOP2(dreg(46), g3, dreg(36), (3u << 5) | 0x06) /* fmovrdlz */,
        ANDcc(g0, g2, g2), FPOP2(13, 0, 7, (4u << 6) | 0x01) | (cNE << 14) /* fmovsne %icc */,
        FPOP2(15, 0, 7, (6u << 6) | 0x01) | (cE << 14) /* fmovse %xcc */};
    body.insert(body.end(), branches.begin(), branches.end());
    for (u32 w : disable_on(13)) body.push_back(w);
    const std::vector<u32> logic = {
        VIS(dreg(16), dreg(2), dreg(36), 0x06C) /* fxor */, VIS(dreg(18), dreg(2), dreg(36), 0x070) /* fand */,
        VIS(dreg(20), dreg(2), dreg(36), 0x064) /* fandnot2 */, VIS(dreg(52), dreg(2), dreg(36), 0x068) /* fandnot1 */,
        VIS(dreg(48), dreg(36), dreg(2), 0x07A) /* fornot1 */, VIS(dreg(50), dreg(36), dreg(2), 0x076) /* fornot2 */,
        VIS(dreg(22), 0, 0, 0x060) /* fzero */, VIS(dreg(42), 0, 0, 0x07E) /* fone */, VIS(25, 5, 7, 0x06F) /* fnands */,
        VIS(26, 5, 7, 0x07D) /* fors */, VIS(27, 5, 0, 0x075) /* fsrc1s */, VIS(dreg(44), 0, dreg(2), 0x06A) /* fnot1 */,
        VIS(dreg(54), dreg(2), dreg(36), 0x062) /* fnor */, VIS(dreg(56), dreg(2), dreg(36), 0x072) /* fxnor */,
        VIS(dreg(58), dreg(2), dreg(36), 0x06E) /* fnand */, VIS(dreg(60), 0, dreg(36), 0x078) /* fsrc2 */,
        VIS(dreg(62), 0, dreg(36), 0x066) /* fnot2 */, VIS(29, 5, 7, 0x063) /* fnors */,
        STDF(dreg(16), o5, 40), STDF(dreg(48), o5, 48), STF(25, o5, 56), STF(26, o5, 60), STDF(dreg(46), o5, 64),
        STDF(dreg(52), o5, 72), STDF(dreg(54), o5, 80), STDF(dreg(56), o5, 88),
        RDASR(o0, 6), ADD(i4, i4, o0), SLLXi(i4, i4, 1), ADDi(o5, o5, 96)};
    body.insert(body.end(), logic.begin(), logic.end());
    check_against_interpreter(body, 40, [](Rig& r) {
        r.put(Rig::kTba + (tt::kFpDisabled << 5), {ADDi(i5, i5, 1), WRASRi(6, g0, 4), RETRY});
    }, pstate::PRIV | pstate::PEF, 4000);
}

TEST(jit_floating_point_arithmetic_and_conversions_match_the_interpreter) {
    // Each pass loads FSR with the next rounding mode (with %g1 at 7, with
    // the inexact trap enabled, whose handler counts it in %i5 and skips the
    // instruction; at 2, in nonstandard mode, with a subnormal operand),
    // then runs arithmetic and conversions on values from integers; operands
    // chosen by pass make the same instructions overflow (6), divide by zero
    // (5), add two NaNs (4), underflow (3) or convert a value too big for an
    // integer (1), all once the code has been translated. FSR is stored
    // after each group.
    auto ldxfsr = [](int off) { return f3i(3, 0x21, 1, o5, off); };
    auto stxfsr = [](int off) { return f3i(3, 0x25, 1, o5, off); };
    auto choose = [](int pass, unsigned rd, unsigned rs) {  // fmovde %icc after g1 == pass
        return std::vector<u32>{SUBcci(g0, g1, pass), FPOP2(rd, 0, rs, (4u << 6) | 0x02) | (cE << 14)};
    };
    std::vector<u32> body = {
        ANDi(g2, g1, 3), SLLXi(g2, g2, 30),
        SUBcci(g0, g1, 7), SETHI(g3, 1u << 23), MOVcc(cNE, kIcc, g3, 0), OR(g2, g2, g3),
        SUBcci(g0, g1, 2), SETHI(g3, 1u << 22), MOVcc(cNE, kIcc, g3, 0), OR(g2, g2, g3),
        SUBcci(g0, g1, 6), SETHI(g3, 1u << 26), MOVcc(cNE, kIcc, g3, 0), OR(g2, g2, g3),  // OFM with the overflow
        SUBcci(g0, g1, 5), SETHI(g3, 1u << 27), MOVcc(cNE, kIcc, g3, 0), OR(g2, g2, g3),  // NVM
        STX(g2, o5, 0), ldxfsr(0),
        SLLi(g3, g1, 2), ADD(g3, g3, g1), SUBi(g3, g3, 37), STW(g3, o5, 8), MOVi(g3, 7), STW(g3, o5, 12),
        SETHI(g4, 0x7FE00000), SLLXi(g4, g4, 32), STX(g4, o5, 16),   // 2^1023
        SETHI(g4, 0x00100000), SLLXi(g4, g4, 32), STX(g4, o5, 24),   // 2^-1022
        MOVi(g4, 7), SLLXi(g4, g4, 60), SUBi(g4, g4, 1), STX(g4, o5, 32),  // inexact as a double
        LDF(1, o5, 8), LDF(3, o5, 12), FPOP1(dreg(4), 0, 1, 0x0C8), FPOP1(dreg(6), 0, 3, 0x0C8),  // fitod
        FPOP1(dreg(8), dreg(4), dreg(6), 0x04E),   // fdivd: x / 7
        FPOP1(dreg(10), dreg(8), dreg(8), 0x04A),  // fmuld
        FPOP1(dreg(12), dreg(10), dreg(4), 0x042), // faddd
        FPOP1(dreg(14), dreg(12), dreg(6), 0x046), // fsubd
        FPOP1(dreg(16), 0, dreg(10), 0x02A),       // fsqrtd
        FPOP1(17, 0, dreg(8), 0x0C6),              // fdtos
        FPOP1(18, 17, 17, 0x049), FPOP1(19, 18, 17, 0x04D), FPOP1(20, 19, 18, 0x041), FPOP1(21, 20, 19, 0x045),
        FPOP1(22, 0, 18, 0x029),                   // fsqrts
        FPOP1(dreg(24), 17, 19, 0x069),            // fsmuld
        FPOP1(dreg(26), 0, 21, 0x0C9),             // fstod
        FPOP1(28, 0, 1, 0x0C4),                    // fitos
        FPOP1(29, 0, dreg(12), 0x0D2), FPOP1(30, 0, 20, 0x0D1),                // fdtoi, fstoi
        FPOP1(dreg(32), 0, dreg(12), 0x082), FPOP1(dreg(34), 0, 20, 0x081),   // fdtox, fstox
        LDDF(dreg(36), o5, 32), FPOP1(dreg(38), 0, dreg(36), 0x088), FPOP1(23, 0, dreg(36), 0x084),  // fxtod, fxtos
        stxfsr(40),
        LDDF(dreg(40), o5, 16), LDDF(dreg(42), o5, 24), VIS(dreg(46), 0, 0, 0x07E) /* a NaN */,
        FPOP1(dreg(30), 0, dreg(46), 0x00A) /* another */, MOVi(g4, 1), STX(g4, o5, 160), LDDF(dreg(28), o5, 160) /* 2^-1074 */,
        FPOP1(dreg(44), 0, dreg(8), 0x002), FPOP1(dreg(52), 0, dreg(6), 0x002), FPOP1(dreg(56), 0, dreg(4), 0x002),
        FPOP1(dreg(62), 0, dreg(8), 0x002), FPOP1(dreg(0), 0, dreg(12), 0x002), FPOP1(dreg(20), 0, dreg(8), 0x002)};
    for (auto c : {choose(6, dreg(44), dreg(40)), choose(5, dreg(52), dreg(60)), choose(4, dreg(56), dreg(46)),
                   choose(3, dreg(62), dreg(42)), choose(1, dreg(0), dreg(40)), choose(4, dreg(4), dreg(30)),
                   choose(2, dreg(20), dreg(28))})
        body.insert(body.end(), c.begin(), c.end());
    const std::vector<u32> more = {
        FPOP1(dreg(50), dreg(44), dreg(44), 0x04A),  // overflow on pass 6
        FPOP1(dreg(54), dreg(4), dreg(52), 0x04E),   // x / 0 on pass 5
        FPOP1(dreg(58), dreg(56), dreg(4), 0x042),   // two NaNs on pass 4
        FPOP1(dreg(48), dreg(62), dreg(62), 0x04A),  // underflow on pass 3
        FPOP1(dreg(18), dreg(20), dreg(40), 0x04A),  // a subnormal operand on pass 2
        FPOP1(dreg(2), 0, dreg(0), 0x082), FPOP1(27, 0, dreg(0), 0x0D2),  // too big on pass 1
        // Invalid with no NaN operand (the default NaN), every pass; NVM traps on pass 5.
        MOVi(i0, -1), STW(i0, o5, 160), LDF(1, o5, 160), FPOP1(dreg(22), 0, 1, 0x0C8), FPOP1(dreg(22), 0, dreg(22), 0x02A),
        STDF(dreg(22), o5, 160), stxfsr(160), LDX(i1, o5, 160), ADD(i4, i4, i1),
        // Compares on NaNs: quiet and signaling (FCMP raises invalid only for the signaling).
        SETHI(i0, 0x7FF00000), SLLXi(i0, i0, 32), ORi(i0, i0, 1), STX(i0, o5, 160), LDDF(dreg(24), o5, 160),
        FPOP2(1, dreg(24), dreg(4), 0x052), stxfsr(160), LDX(i1, o5, 160), ADD(i4, i4, i1),
        FPOP2(2, dreg(46), dreg(4), 0x052), stxfsr(160), LDX(i1, o5, 160), ADD(i4, i4, i1),
        FPOP2(3, dreg(46), dreg(4), 0x056), stxfsr(160), LDX(i1, o5, 160), ADD(i4, i4, i1),
        stxfsr(48),
        STDF(dreg(50), o5, 56), STDF(dreg(54), o5, 64), STDF(dreg(58), o5, 72), STDF(dreg(48), o5, 80),
        STDF(dreg(2), o5, 88), STF(27, o5, 96), STDF(dreg(18), o5, 160),
        STDF(dreg(8), o5, 104), STDF(dreg(14), o5, 112), STDF(dreg(16), o5, 120), STF(22, o5, 128), STF(21, o5, 132),
        STDF(dreg(24), o5, 136), STDF(dreg(32), o5, 144), STDF(dreg(38), o5, 152), STF(23, o5, 100),
        ADDi(o5, o5, 168)};
    body.insert(body.end(), more.begin(), more.end());
    check_against_interpreter(body, 24, [](Rig& r) {
        r.put(Rig::kTba + (tt::kFpExceptionIeee754 << 5), {ADDi(i5, i5, 1), DONE});
    }, pstate::PRIV | pstate::PEF, 6000);
}

TEST(jit_unfinished_fp_cases_match_the_interpreter) {
    // The cases the IIi leaves to the operating system (fp_exception_other,
    // unfinished_FPop; the handler counts them in %i5 and skips them): a
    // subnormal operand, a result in the subnormal window, a divide or
    // square root with the inexact trap enabled. Passes run with FSR clear,
    // NS set, NXM set and UFM set in turn, so the same instructions finish,
    // flush or trap once translated.
    auto ldxfsr = [](int off) { return f3i(3, 0x21, 1, o5, off); };
    auto stxfsr = [](int off) { return f3i(3, 0x25, 1, o5, off); };
    auto single = [](unsigned f, u32 bits) {
        return std::vector<u32>{SETHI(g4, bits & ~0x3FFu), ORi(g4, g4, static_cast<int>(bits & 0x3FF)), STW(g4, o5, 8),
                                LDF(f, o5, 8)};
    };
    std::vector<u32> body = {
        ANDi(g2, g1, 3), MOVi(g3, 0),
        SUBcci(g0, g2, 1), SETHI(g4, 1u << 22), MOVcc(cE, kIcc, g4, g3),  // NS
        SUBcci(g0, g2, 2), SETHI(g4, 1u << 23), MOVcc(cE, kIcc, g4, g3),  // NXM
        SUBcci(g0, g2, 3), SETHI(g4, 1u << 25), MOVcc(cE, kIcc, g4, g3),  // UFM
        STX(g3, o5, 0), ldxfsr(0)};
    for (auto v : {single(1, 0x00000003), single(2, 0x3F800000), single(3, 0x00C00000), single(4, 0x00800000),
                   single(5, 0x1F800000), single(6, 0x40800000), single(7, 0x3F000000)})
        body.insert(body.end(), v.begin(), v.end());
    const std::vector<u32> ops = {
        FPOP1(10, 1, 2, 0x041),   // subnormal + 1
        FPOP1(11, 3, 4, 0x045),   // 1.5 * 2^-126 - 2^-126: a subnormal result
        FPOP1(12, 5, 5, 0x049),   // 2^-64 * 2^-64
        FPOP1(13, 4, 6, 0x04D),   // 2^-126 / 4
        FPOP1(14, 2, 7, 0x04D),   // 1 / 0.5 (inexact predicted with NXM)
        FPOP1(15, 2, 7, 0x049),   // 1 * 0.5
        FPOP1(dreg(16), 0, 1, 0x0C9),  // fstod of a subnormal
        FPOP1(18, 0, 1, 0x0D1),        // fstoi of a subnormal
        FPOP1(19, 0, 6, 0x029),        // fsqrts 4
        SETHI(g4, 0x37D00000), SLLXi(g4, g4, 32), STX(g4, o5, 16), LDDF(dreg(20), o5, 16),  // 2^-130
        FPOP1(22, 0, dreg(20), 0x0C6),  // fdtos into the subnormals
        MOVi(g4, 1), STX(g4, o5, 16), LDDF(dreg(24), o5, 16),  // the least double subnormal
        FPOP1(dreg(26), dreg(24), dreg(24), 0x042),
        stxfsr(24), STF(10, o5, 32), STF(11, o5, 36), STF(12, o5, 40), STF(13, o5, 44), STF(14, o5, 48),
        STF(15, o5, 52), STDF(dreg(16), o5, 56), STF(18, o5, 64), STF(19, o5, 68), STF(22, o5, 72),
        STDF(dreg(26), o5, 80), ADDi(o5, o5, 88)};
    body.insert(body.end(), ops.begin(), ops.end());
    check_against_interpreter(body, 64, [](Rig& r) {
        r.put(Rig::kTba + (tt::kFpExceptionOther << 5), {ADDi(i5, i5, 1), DONE});
        r.put(Rig::kTba + (tt::kFpExceptionIeee754 << 5), {ADDi(i4, i4, 1), DONE});
    }, pstate::PRIV | pstate::PEF, 10000);
}

TEST(jit_vis_block_transfers_and_alignment_match_the_interpreter) {
    // Each pass fills a 64-byte block, block-loads it big- and little-
    // endian, block-stores it back both ways (one through the commit ASI),
    // then aligns an address and data with ALIGNADDRESS(_LITTLE) and
    // FALIGNDATA and reads GSR. With %g1 at 2 the block address is off by 8,
    // which traps (the handler counts it in %i5 and skips the instruction).
    std::vector<u32> body;
    for (int k = 0; k < 8; ++k) {
        body.push_back(SLLXi(g2, g1, 8 * k));
        body.push_back(XORi(g2, g2, 0x135 * (k + 1)));
        body.push_back(STX(g2, o5, 8 * k));
    }
    const std::vector<u32> more = {
        SUBcci(g0, g1, 2), MOVi(o1, 0), MOVcc(cE, kIcc, o1, 8), ADD(o2, o5, o1),  // the block address
        WRASRi(6, g0, 4), LDDFA(0, o2, g0, asi::BLK_P), RDASR(l4, 6), LDDFA(1, o5, g0, asi::BLK_PL), RDASR(l5, 6),
        SLLXi(l6, l6, 2), ADD(l6, l6, l4), SLLXi(l6, l6, 2), ADD(l6, l6, l5),
        ADDi(o3, o5, 64), STDFA(0, o3, g0, asi::BLK_COMMIT_P),
        ADDi(o3, o5, 128), STDFA(1, o3, g0, asi::BLK_PL),
        ANDi(o4, g1, 7), VIS(l0, o5, o4, 0x018) /* alignaddr */, VIS(dreg(16), dreg(2), dreg(4), 0x048) /* faligndata */,
        RDASR(l1, 19), ADD(l2, l2, l1), ADD(l3, l3, l0),
        VIS(l0, o5, o4, 0x01A) /* alignaddrl */, VIS(dreg(18), dreg(6), dreg(32), 0x048),
        RDASR(l1, 19), ADD(l2, l2, l1), ADD(l3, l3, l0),
        STDF(dreg(16), o5, 192 - 16), STDF(dreg(18), o5, 192 - 8),
        ADDi(o5, o5, 192)};
    body.insert(body.end(), more.begin(), more.end());
    check_against_interpreter(body, 20, [](Rig& r) {
        r.put(Rig::kTba + (tt::kMemAddressNotAligned << 5), {ADDi(i5, i5, 1), DONE});
    }, pstate::PRIV | pstate::PEF, 3000);
}

TEST(jit_privileged_register_reads_match_the_interpreter) {
    // Every privileged register at TL 0 (with PIL changing each pass), and
    // the trap registers at TL 1, from a handler for a Tcc that is taken on
    // every pass; each value folded into a register.
    std::vector<u32> body = {ANDi(o1, g1, 15), WRPR(prPIL, o1, g0)};
    for (unsigned pr : {prTBA, prPSTATE, prTL, prPIL, prCWP, prCANSAVE, prCANRESTORE, prCLEANWIN, prOTHERWIN, prWSTATE, prVER}) {
        body.push_back(RDPR(o2, pr));
        body.push_back(SLLXi(l0, l0, 3));
        body.push_back(XOR(l0, l0, o2));
    }
    // The other privileged ancillary registers, written each pass (through
    // the interpreter) and read back; FPRS written; SHUTDOWN (nothing).
    const u32 asrs[] = {MULXi(o3, g1, 0x1F3), WRASR(16, o3, g0), XORi(o4, o3, 0x2A), WRASR(17, o4, g0), ANDi(o4, o3, 0xF),
                        WRASR(18, o4, g0), SLLXi(o4, o3, 3), WRASR(22, o4, g0), SETHI(o4, 0x80000000), SLLXi(o4, o4, 32),
                        OR(o4, o4, o3), WRASR(23, o4, g0)};
    body.insert(body.end(), std::begin(asrs), std::end(asrs));
    for (unsigned asr : {16u, 17u, 18u, 22u, 23u}) {
        body.push_back(RDASR(o2, asr));
        body.push_back(SLLXi(l4, l4, 3));
        body.push_back(XOR(l4, l4, o2));  // (not %l1/%l2: the handler uses them)
    }
    body.push_back(ANDi(o2, g1, 3));
    body.push_back(ORi(o2, o2, 4));
    body.push_back(WRASR(6, o2, g1));
    body.push_back(RDASR(o2, 6));
    body.push_back(ADD(l3, l3, o2));
    body.push_back(WRASRi(6, g0, 4));
    body.push_back(VIS(0, 0, 0, 0x080));  // SHUTDOWN
    body.push_back(CMP(g0, g0));
    body.push_back(Tcc(cE, kIcc, 0x11));
    check_against_interpreter(body, 64, [](Rig& r) {
        std::vector<u32> handler;
        for (unsigned pr : {prTPC, prTNPC, prTSTATE, prTT, prTL, prPSTATE}) {
            handler.push_back(RDPR(l2, pr));
            handler.push_back(SLLXi(l1, l1, 5));
            handler.push_back(XOR(l1, l1, l2));
        }
        handler.push_back(DONE);
        for (std::size_t k = 0; k < handler.size(); ++k)
            r.bus.write(Rig::kTba + ((tt::kTrapInstruction + 0x11) << 5) + 4 * k, handler[k]);
    }, pstate::PRIV | pstate::PEF, 3000);
}

TEST(jit_popc_and_multiply_steps_match_the_interpreter) {
    // POPC of changing values, and a 32-bit multiply done the SPARC V7 way:
    // Y = multiplier, 32 MULScc steps and a final one with %g0.
    std::vector<u32> body = {SLLXi(g2, g1, 37), XOR(g2, g2, g1), SUBi(g3, g0, g1), POPC(l0, g2), POPC(l1, g3),
                             f3i(2, 0x2E, l2, 0, -77) /* popc of an immediate */, ADD(l3, l3, l0), ADD(l3, l3, l1), ADD(l3, l3, l2),
                             SLLi(o1, g1, 21), XORi(o1, o1, 0x5A5), WRASR(0, o1, g0), ANDcc(o2, g0, g0)};
    for (int k = 0; k < 32; ++k) body.push_back(MULScc(o2, o2, g3));
    body.push_back(MULScc(o2, o2, g0));
    body.push_back(RDASR(o3, 0));
    body.push_back(ADD(l4, l4, o2));
    body.push_back(XOR(l5, l5, o3));
    body.push_back(RDASR(o4, 2));
    body.push_back(ADD(l6, l6, o4));
    check_against_interpreter(body, 40, {}, pstate::PRIV | pstate::PEF, 2000);
}

TEST(jit_vis_partitioned_and_pixel_operations_match_the_interpreter) {
    // Every VIS operation in vis.h on values that change each pass, with
    // GSR.SCALE changing too; integer results folded into registers, FP
    // results stored.
    std::vector<u32> body = {
        WRASRi(6, g0, 4),  // FPRS: the dirty bits clear
        SLLXi(g2, g1, 41), XOR(g2, g2, g1), MULXi(g3, g1, 0x3D5), SLLXi(g4, g3, 23), XOR(g3, g3, g4),
        STX(g2, o5, 0), STX(g3, o5, 8), LDDF(dreg(2), o5, 0), LDDF(dreg(4), o5, 8), LDF(6, o5, 4), LDF(7, o5, 12),
        ANDi(o1, g1, 15), SLLi(o1, o1, 3), ORi(o1, o1, 5), WRASR(19, o1, g0),  // GSR: scale and align
        VIS(dreg(8), dreg(2), dreg(4), 0x050), VIS(9, 6, 7, 0x051), VIS(dreg(10), dreg(2), dreg(4), 0x052),
        VIS(11, 6, 7, 0x053), VIS(dreg(12), dreg(2), dreg(4), 0x054), VIS(13, 6, 7, 0x055),
        VIS(dreg(14), dreg(2), dreg(4), 0x056), VIS(15, 6, 7, 0x057),
        VIS(dreg(16), 6, dreg(4), 0x031), VIS(dreg(18), 6, 7, 0x033), VIS(dreg(20), 6, 7, 0x035),
        VIS(dreg(22), dreg(2), dreg(4), 0x036), VIS(dreg(24), dreg(2), dreg(4), 0x037),
        VIS(dreg(26), 6, 7, 0x038), VIS(dreg(28), 6, 7, 0x039),
        VIS(dreg(30), dreg(2), dreg(4), 0x03A), VIS(1, 0, dreg(4), 0x03B), VIS(0, 0, dreg(2), 0x03D),
        VIS(dreg(32), dreg(2), dreg(4), 0x03E), VIS(dreg(34), 6, 7, 0x04B), VIS(dreg(36), 0, 7, 0x04D)};
    for (unsigned opf = 0x020; opf <= 0x02E; opf += 2) {
        body.push_back(VIS(l0, dreg(2), dreg(4), opf));
        body.push_back(SLLXi(l1, l1, 4));
        body.push_back(XOR(l1, l1, l0));
    }
    // EDGE (the codes too), on addresses in different 8-byte blocks, and
    // in the same block of the low 32 bits but not of all 64.
    const u32 edges[] = {SLLXi(o3, g1, 40), ANDi(o4, g1, 7), OR(o3, o3, o4), ORi(o3, o3, 0x100),
                         SLLXi(o4, g1, 36), SRLi(o2, g1, 1), ANDi(o2, o2, 7), OR(o4, o4, o2), ORi(o4, o4, 0x100)};
    body.insert(body.end(), std::begin(edges), std::end(edges));
    for (unsigned opf = 0x000; opf <= 0x00A; opf += 2) {
        for (auto [x, y] : {std::pair{g2, g3}, std::pair{o3, o4}}) {
            body.push_back(VIS(l2, x, y, opf));
            body.push_back(RDASR(l3, 2));
            body.push_back(SLLXi(l4, l4, 5));
            body.push_back(XOR(l4, l4, l2));
            body.push_back(XOR(l4, l4, l3));
        }
    }
    for (unsigned opf = 0x010; opf <= 0x014; opf += 2) {  // ARRAY, sizes from %g1
        body.push_back(ANDi(o2, g1, 7));
        body.push_back(VIS(l5, g3, o2, opf));
        body.push_back(ADD(l6, l6, l5));
    }
    for (unsigned k = 0; k < 15; ++k) body.push_back(STDF(dreg(8 + 2 * k), o5, 16 + 8 * static_cast<int>(k)));
    body.push_back(STDF(dreg(32), o5, 136));
    body.push_back(STDF(dreg(34), o5, 144));
    body.push_back(STDF(dreg(36), o5, 152));
    body.push_back(STDF(dreg(0), o5, 160));
    body.push_back(RDASR(o2, 6));
    body.push_back(SLLXi(l7, l7, 2));
    body.push_back(XOR(l7, l7, o2));
    body.push_back(ADDi(o5, o5, 168));
    check_against_interpreter(body, 24, {}, pstate::PRIV | pstate::PEF, 2000);
    check_against_interpreter(body, 24, {}, pstate::PRIV | pstate::PEF | pstate::AM, 2000);
}

TEST(jit_fsr_partial_and_short_fp_transfers_gsr_and_movfcc_match_the_interpreter) {
    // An unimplemented FPop (a quad) to set FSR.ftt (the handler skips it);
    // LDXFSR/STXFSR and LDFSR/STFSR of changing FSR images; partial stores
    // of a double under masks from %g1 through each partial ASI; 8- and
    // 16-bit FP loads and stores both ways round; GSR written and read; and
    // MOVcc on each fcc after compares that change with the pass.
    auto ldfsr = [](unsigned x, int off) { return f3i(3, 0x21, x, o5, off); };
    auto stfsr = [](unsigned x, int off) { return f3i(3, 0x25, x, o5, off); };
    auto movfcc = [](unsigned cond, unsigned fcc, unsigned rd, int simm11) {
        return (2u << 30) | (rd << 25) | (0x2Cu << 19) | (cond << 14) | (1u << 13) | (fcc << 11) | (static_cast<u32>(simm11) & 0x7FF);
    };
    std::vector<u32> body = {
        MULXi(g2, g1, 0x6F5), SLLXi(o1, g2, 21), XOR(g2, g2, o1), SLLXi(o1, g2, 13), XOR(g2, g2, o1),
        SETHI(o1, 0x0F800000), SLLXi(o1, o1, 1), ANDN(g2, g2, o1),  // TEM clear
        FPOP1(0, 0, 4, 0x043) /* ftt = unimplemented */, STX(g2, o5, 0), ldfsr(1, 0), stfsr(1, 8), stfsr(1, 112),
        XORi(g3, g2, 0x3C0), STW(g3, o5, 20), ldfsr(0, 20), stfsr(0, 16),
        MULXi(g4, g1, 0x2B3), SLLXi(o1, g4, 29), XOR(g4, g4, o1), SLLXi(o1, g1, 56), XOR(g4, g4, o1), SLLXi(o1, g1, 49),
        XOR(g4, g4, o1), STX(g4, o5, 24), LDDF(dreg(2), o5, 24)};
    const u8 partial_asis[] = {0xC0, 0xC2, 0xC4, 0xC8, 0xCA, 0xCC};
    int off = 32;
    for (u8 a : partial_asis) {
        body.push_back(STX(g2, o5, off));
        body.push_back(ADDi(o2, o5, off));
        body.push_back(XORi(o3, g1, a));
        body.push_back(STDFA(dreg(2), o2, o3, a));
        off += 8;
    }
    const u8 short_asis[] = {0xD0, 0xD2, 0xD8, 0xDA};
    for (u8 a : short_asis) {
        body.push_back(ADDi(o2, o5, off));
        body.push_back(STDFA(dreg(2), o2, g0, a));
        body.push_back(ADDi(o2, o5, 24));
        body.push_back(LDDFA(dreg(4 + 2 * (a & 0xA)), o2, g0, a));
        off += 8;
    }
    for (unsigned k = 0; k < 4; ++k) {
        body.push_back(STDF(dreg(4 + 2 * k * 2), o5, off));
        off += 8;
    }
    const std::vector<u32> more = {
        SLLXi(o1, g1, 5), XOR(o1, o1, g1), WRASR(19, o1, g0), RDASR(l0, 19), ADD(l1, l1, l0),
        FPOP2(1, dreg(2), dreg(4), 0x052), FPOP2(2, dreg(4), dreg(2), 0x052), FPOP2(3, dreg(2), dreg(2), 0x052),
        movfcc(4, 1, l2, 7), movfcc(6, 2, l3, -9), movfcc(9, 3, l4, 11), movfcc(13, 0, l5, 3),
        ADD(l6, l6, l2), ADD(l6, l6, l3), ADD(l6, l6, l4), ADD(l6, l6, l5),
        ADDi(o5, o5, 128)};
    body.insert(body.end(), more.begin(), more.end());
    check_against_interpreter(body, 28, [](Rig& r) {
        r.put(Rig::kTba + (tt::kFpExceptionOther << 5), {DONE});
    }, pstate::PRIV | pstate::PEF, 2000);
}

TEST(jit_flushw_matches_the_interpreter) {
    // FLUSHW after four SAVEs spills them (the handler retries it until
    // only the current window is in use), then the RESTOREs fill.
    std::vector<u32> body;
    for (int k = 0; k < 4; ++k) body.push_back(SAVEi(o0, o0, 1));
    body.push_back(FLUSHW);
    body.push_back(RDPR(l1, prCANSAVE));
    body.push_back(SLLXi(g4, g4, 3));
    body.push_back(ADD(g4, g4, l1));  // (a global: the window's locals are not kept)
    for (int k = 0; k < 4; ++k) body.push_back(RESTOREi(o0, o0, 1));
    body.push_back(FLUSHW);
    body.push_back(ADD(l3, l3, o0));
    check_against_interpreter(body, 40, [](Rig& r) {
        r.put(Rig::kTba + (tt::kSpillNormal << 5), {SAVED, RETRY});
        r.put(Rig::kTba + (tt::kFillNormal << 5), {RESTORED, RETRY});
    }, pstate::PRIV | pstate::PEF, 3000);
}

TEST(jit_accesses_through_each_memory_asi_match_the_interpreter) {
    // With the D-MMU on: virtual 0x40000000 is privileged physical 0x40000
    // in context 0 (primary and nucleus) and user physical 0x50000 in
    // context 5 (secondary); 0x42004000 is user physical 0x60000 in
    // context 0; 0x44002000 is physical 0x70000 with TTE.IE (little-endian,
    // which the fast path leaves to the interpreter); the data area is
    // mapped as itself. (The pages fall in different fast-table slots.) Each pass stores and loads through P, PL, N, NL, S,
    // SL, AIUP, AIUPL, AIUS and AIUSL, with block, partial, short, atomic
    // and pair accesses through some of them; then again with PSTATE.CLE,
    // where the implicit ASI is little-endian.
    auto setup = [](Rig& r) {
        auto stxa = [&](u64 value, u64 va, u8 a) {
            r.cpu.set_reg(g1, value);
            r.cpu.set_reg(g2, va);
            r.put(0x400, {STXA(g1, g2, g0, a)});
            r.run_at(0x400, 1);
        };
        auto map = [&](u64 tag, u64 pa, u64 bits) {
            stxa(tag, 0x30, asi::DMMU);  // tag access: VA | context
            stxa(tte::V | tte::CP | pa | bits, 0, asi::DTLB_DATA_IN);
        };
        map(0x40000000, 0x40000, tte::P | tte::W);
        map(0x40000000 | 5, 0x50000, tte::W);
        map(0x42004000, 0x60000, tte::W);
        map(0x44002000, 0x70000, tte::P | tte::W | tte::IE);
        map(kData, kData, tte::P | tte::W);
        map(0x46006000 | 5, 0x58000, tte::P | tte::W);  // privileged, in the secondary context
        map(0x48008000, 0x5A000, tte::P | tte::W | tte::NFO);  // no-fault loads only
        stxa(5, 0x10, asi::DMMU);  // secondary context
        stxa(8, 0, asi::LSU_CONTROL);  // D-MMU on
        for (unsigned t : {tt::kDataAccessException, tt::kFastDataAccessProtection, tt::kPrivilegedAction,
                           tt::kFastDataAccessMmuMiss})
            r.put(Rig::kTba + (t << 5), {ADDi(i5, i5, 1), DONE});
    };
    const std::vector<u32> body = {
        SETHI(o1, 0x40000000), SETHI(o2, 0x42004000), SETHI(o3, 0x44002000),
        MOVi(l0, 8), MOVi(l1, 16), MOVi(l2, 24), MOVi(l3, 64),
        SLLXi(g2, g1, 40), MULXi(g3, g1, 0x2F1), XOR(g2, g2, g3), SLLXi(g3, g2, 9), XOR(g3, g3, g1),
        STXA(g2, o1, g0, asi::S), STXA(g3, o1, g0, asi::P), STXA(g3, o1, l0, asi::AIUS), STXA(g2, o2, g0, asi::AIUP),
        STXA(g3, o1, l1, asi::PL), STXA(g2, o1, l2, asi::SL), STXA(g2, o2, l0, asi::AIUPL), STXA(g3, o1, l1, asi::AIUSL),
        STXA(g2, o3, g0, asi::P), STWA(g3, o1, l3, asi::NL),
        LDXA(i0, o1, g0, asi::S), LDXA(i1, o1, g0, asi::N), LDXA(i2, o1, l0, asi::AIUSL), LDXA(i3, o2, g0, asi::AIUPL),
        LDUWA(i4, o1, l1, asi::P), LDUHA(i5, o1, l2, asi::S), LDSWA(l4, o1, l3, asi::N), LDXA(l5, o3, g0, asi::P),
        LDXA(l6, o2, l0, asi::AIUP), LDXA(l7, o1, l1, asi::SL),
        ADD(g4, i0, i1), XOR(g4, g4, i2), ADD(g4, g4, i3), XOR(g4, g4, i4), ADD(g4, g4, i5), XOR(g4, g4, l4),
        ADD(g4, g4, l5), XOR(g4, g4, l6), ADD(g4, g4, l7), STX(g4, o5, 0),
        CASXA(g3, o1, i0, asi::S) /* swaps (i0 is what is there) */, LDXA(i0, o1, g0, asi::S), ADD(g5, g5, i0), ADD(g5, g5, g3),
        LDDFA(0, o1, g0, 0xF1) /* block, secondary */, STDFA(0, o1, l3, 0xE0) /* commit, primary */,
        LDDFA(dreg(16), o1, l3, 0xF0), STDF(dreg(16), o5, 8), STDF(dreg(30), o5, 16),
        XORi(o4, g1, 0x55), STDFA(dreg(2), o1, o4, 0xC9) /* partial 8-bit secondary little */,
        STDFA(dreg(4), o1, g0, 0xD1) /* short 8-bit secondary */, LDDFA(dreg(18), o1, l0, 0xD3) /* short 16-bit secondary */,
        LDXA(i0, o1, g0, asi::S), STX(i0, o5, 24), STDF(dreg(18), o5, 32),
        LDX(i1, o5, 0), ADD(g5, g5, i1),  // the implicit ASI (PL with PSTATE.CLE)
        SETHI(i2, 0x70000), LDXA(i3, i2, g0, asi::PHYS_USE_EC), ADD(g5, g5, i3),  // the IE page as stored
        LDXA(i3, o1, g0, asi::AIUP), ADD(g5, g5, i3),  // a privileged page with user permissions: faults
        SETHI(i2, 0x46006000), STXA(g2, i2, g0, asi::S), LDXA(i3, i2, g0, asi::S), ADD(g5, g5, i3),
        LDXA(i3, i2, g0, asi::AIUS), ADD(g5, g5, i3),  // likewise in the secondary context
        // Through %asi (AIUS, as copyin and copyout use it; then P).
        WRASRi(3, g0, asi::AIUS), f3i(3, 0x1B, i3, o1, 8) /* ldxa [%o1 + 8] %asi */, ADD(g5, g5, i3),
        f3i(3, 0x1E, g3, o1, 32) /* stxa %g3, [%o1 + 32] %asi */, LDXA(i3, o1, l3, asi::S), XOR(g5, g5, i3),
        WRASRi(3, g0, asi::P), f3i(3, 0x1B, i3, o1, 8), ADD(g5, g5, i3),
        // Physical: the data area (virtual = physical here) and the secondary page.
        SETHI(i2, 0x50000), LDXA(i3, i2, g0, asi::PHYS_USE_EC), ADD(g5, g5, i3),
        LDXA(i3, i2, g0, asi::P), ADD(g5, g5, i3),  // (virtual 0x50000 is unmapped: a miss)
        STXA(g2, o5, g0, asi::PHYS_USE_EC_L), LDX(i3, o5, 0), XOR(g5, g5, i3),
        // No-fault: succeeds on a no-fault-only page where P faults (and
        // must not open the way for P there).
        SETHI(i2, 0x48008000), LDXA(i3, i2, g0, asi::PNF), ADD(g5, g5, i3), LDXA(i3, i2, g0, asi::P), ADD(g5, g5, i3),
        LDXA(i3, o1, g0, asi::SNF), XOR(g5, g5, i3),
        // Doubleword pairs through little-endian ASIs.
        f3r(3, 0x17, l4, o1, l3) | (u32{asi::PL} << 5) /* stda %l4, %l5 */, f3r(3, 0x13, i2, o1, l3) | (u32{asi::P} << 5) /* ldda */,
        ADD(g5, g5, i2), XOR(g5, g5, i3), f3r(3, 0x13, i2, o1, g0) | (u32{asi::SL} << 5), ADD(g5, g5, i2), XOR(g5, g5, i3),
        ADDi(o5, o5, 40)};
    check_against_interpreter(body, 40, setup, pstate::PRIV | pstate::PEF, 2000);
    check_against_interpreter(body, 40, setup, pstate::PRIV | pstate::PEF | pstate::CLE, 2000);
}

TEST(jit_tlb_misses_through_a_tsb_match_the_interpreter) {
    // Seventy data pages, more than the 64-entry D-TLB holds, are touched
    // on every pass, so most accesses miss to a handler written as
    // Solaris's are: the 8 KiB TSB pointer and the tag target from the
    // D-MMU, a quad load of the TSB entry, and DTLB data-in if the tag
    // matches, then RETRY. The data area and the TSB are locked in the TLB.
    // Each pass also moves the first page to another physical page (in its
    // TSB entry) and demaps it, so a translation kept from before is wrong.
    constexpr u64 kTsb = 0x12000, kPages = 0x60000000;
    constexpr unsigned kCount = 70;
    auto setup = [](Rig& r) {
        auto stxa = [&](u64 value, u64 va, u8 a) {
            r.cpu.set_reg(g1, value);
            r.cpu.set_reg(g2, va);
            r.put(0x400, {STXA(g1, g2, g0, a)});
            r.run_at(0x400, 1);
        };
        auto map = [&](u64 tag, u64 pa) {
            stxa(tag, 0x30, asi::DMMU);
            stxa(tte::V | tte::CP | tte::P | tte::W | tte::L | pa, 0, asi::DTLB_DATA_IN);
        };
        map(kData, kData);
        map(kTsb, kTsb);
        stxa(kTsb, 0x28, asi::DMMU);  // TSB base, 512 entries, not split
        for (unsigned k = 0; k < kCount; ++k) {
            const u64 va = kPages + 0x2000 * k, entry = kTsb + 16 * ((va >> 13) & 511);
            const u64 tag = va >> 22, data = tte::V | tte::CP | tte::P | tte::W | (0x80000 + 0x2000 * (k % 8));
            r.bus.write(entry, static_cast<u32>(tag >> 32)), r.bus.write(entry + 4, static_cast<u32>(tag));
            r.bus.write(entry + 8, static_cast<u32>(data >> 32)), r.bus.write(entry + 12, static_cast<u32>(data));
        }
        r.put(Rig::kTba + (tt::kFastDataAccessMmuMiss << 5),
              {LDXA(g1, g0, g0, asi::DMMU_TSB_8K_PTR), LDXA(g2, g0, g0, asi::DMMU) /* tag target */,
               f3r(3, 0x13, g4, g1, g0) | (u32{asi::NQUAD_LDD} << 5) /* ldda [%g1] quad, %g4 */, CMP(g4, g2),
               BPcc(cE, kXcc, 2, true), STXA(g5, g0, g0, asi::DTLB_DATA_IN), RETRY});
        stxa(8, 0, asi::LSU_CONTROL);  // D-MMU on
    };
    std::vector<u32> body = {
        SETHI(o3, 0x80000000), SLLXi(o3, o3, 32), ORi(o3, o3, static_cast<int>(tte::CP | tte::P | tte::W)),
        ANDi(o4, g1, 1), SLLXi(o4, o4, 17), SETHI(i1, 0x80000), OR(i1, i1, o4), OR(o3, o3, i1),
        SETHI(i2, static_cast<u32>(kTsb)), STX(o3, i2, 8), SETHI(i3, static_cast<u32>(kPages)), STXA(g0, i3, g0, asi::DMMU_DEMAP)};
    for (unsigned k = 0; k < kCount; ++k) {
        body.push_back(SETHI(o1, static_cast<u32>(kPages + 0x2000 * k)));
        body.push_back(LDX(o2, o1, static_cast<int>(8 * (k % 4))));
        body.push_back(ADD(l1, l1, o2));
        body.push_back(STX(l1, o1, static_cast<int>(8 * ((k + 1) % 4))));
    }
    body.push_back(STX(l1, o5, 0));
    body.push_back(ADDi(o5, o5, 8));
    check_against_interpreter(body, 30, setup, pstate::PRIV | pstate::PEF, 30000);
}

TEST(jit_fp_compare_traps_match_the_interpreter) {
    // FCMPE on a quiet NaN raises invalid: on odd passes FSR.TEM.NVM is set
    // and it traps (the handler counts it in %i5 and skips it); on even
    // ones it accrues. FCMP on a signaling NaN likewise.
    auto ldxfsr = [](int off) { return f3i(3, 0x21, 1, o5, off); };
    auto stxfsr = [](int off) { return f3i(3, 0x25, 1, o5, off); };
    const std::vector<u32> body = {
        ANDi(o1, g1, 1), SLLXi(o1, o1, 27), STX(o1, o5, 0), ldxfsr(0),
        SETHI(o2, 0x7FF80000), SLLXi(o2, o2, 32), STX(o2, o5, 8), LDDF(dreg(2), o5, 8),  // quiet NaN
        ORi(o2, g0, 1), SETHI(o3, 0x7FF00000), SLLXi(o3, o3, 32), OR(o2, o2, o3), STX(o2, o5, 16), LDDF(dreg(4), o5, 16),  // signaling
        MOVi(o3, 3), STW(o3, o5, 24), LDF(6, o5, 24), FPOP1(dreg(6), 0, 6, 0x0C8),  // 3.0
        FPOP2(1, dreg(2), dreg(6), 0x056), stxfsr(32), FPOP2(2, dreg(4), dreg(6), 0x052), stxfsr(40),
        FPOP2(3, dreg(2), dreg(6), 0x052), stxfsr(48), ADDi(o5, o5, 56)};
    check_against_interpreter(body, 40, [](Rig& r) {
        r.put(Rig::kTba + (tt::kFpExceptionIeee754 << 5), {ADDi(i5, i5, 1), DONE});
    }, pstate::PRIV | pstate::PEF, 2000);
}

namespace {

// Runs `code` from Rig::kCode for `n` instructions with the recompiler (or
// with `jit` false, the block cache alone) and with step() alone, and
// compares the two.
void check_run_against_interpreter(const std::vector<u32>& code, u64 n, bool jit = true, double cpi = 1.0) {
    Rig fast, ref;
    ref.cpu.set_block_cache(false);
    if (!jit) fast.cpu.set_jit(false);
    for (Rig* r : {&fast, &ref}) {
        r->cpu.set_cycles_per_step(cpi);
        r->cpu.set_pstate(pstate::PRIV | pstate::PEF);
        for (std::size_t i = 0; i < code.size(); ++i) r->bus.write(Rig::kCode + 4 * i, code[i]);
        r->cpu.set_reg(o5, kData);
        r->run_at(Rig::kCode, n);
    }
    if (jit && UltraSparc::jit_available()) CHECK(fast.cpu.jit_compiles() > 0);
    CHECK(!ref.cpu.halted());
    CHECK_EQ(fast.cpu.instructions(), ref.cpu.instructions());
    CHECK_EQ(fast.cpu.cycles(), ref.cpu.cycles());
    check_same(state_of(fast), state_of(ref));
}

}  // namespace

// Delay loops (a branch to itself counting a register down in its delay
// slot) end, or stop part way, exactly where stepping them does: every
// form, counts in the low 32 bits under a non-zero upper half, and a count
// that overflows on the way.
TEST(jit_counted_delay_loops_match_the_interpreter) {
    enum Start { kCount, kCountUpper, kMostNegative, kZeroUpper };  // the last two with the codes saying "greater"
    struct Loop {
        u32 branch, step;
        Start start;
    };
    const Loop loops[] = {
        {BPcc(cG, kXcc, 0), SUBcci(l1, l1, 1), kCount},
        {BPcc(cNE, kXcc, 0, true), ADDcci(l1, l1, -1), kCount},
        {Bicc(cNE, 0), SUBcci(l1, l1, 1), kCountUpper},
        {BPcc(cG, kIcc, 0), ADDcci(l1, l1, -1), kCountUpper},
        {BPr(5, l1, 0), SUBi(l1, l1, 1), kCount},         // brnz
        {BPr(6, l1, 0, true), ADDi(l1, l1, -1), kCount},  // brgz,a
        {BPcc(cG, kXcc, 0), SUBcci(l1, l1, 1), kMostNegative},
        {BPcc(cG, kIcc, 0), SUBcci(l1, l1, 1), kMostNegative},
        {BPcc(cG, kXcc, 0), SUBcci(l1, l1, 1), kZeroUpper},
        {BPcc(cG, kIcc, 0), SUBcci(l1, l1, 1), kZeroUpper},
        // Not counts: run as they are.
        {BPcc(cNE, kXcc, 0), SUBcc(l1, l1, g0), kCount},
        {BPcc(cG, kXcc, 0), SUBcci(l1, l1, 2), kCount},
        {BPcc(cG, kXcc, 0), SUBcci(l1, o5, 1), kCount},
        {BPcc(cG, kXcc, 0), SUBcc(l1, l1, g1), kCount},  // (rs2 = 1 where an immediate would be)
    };
    for (const Loop& l : loops) {
        // (Starts that run few iterations go round more, for the loop to be translated.)
        std::vector<u32> code = {MOVi(g1, l.start == kMostNegative || l.start == kZeroUpper ? 200 : 16)};
        const auto top = static_cast<int>(code.size());
        const unsigned op2 = (l.branch >> 22) & 7;
        const bool icc = op2 == 2 || (op2 == 1 && ((l.branch >> 20) & 3) == kIcc);
        switch (l.start) {
            case kCount:
            case kCountUpper:
                code.push_back(MULXi(l1, g1, 97));
                code.push_back(ADDi(l1, l1, 3));
                if (l.start == kCountUpper) {
                    code.push_back(SLLXi(l2, g1, 33));
                    code.push_back(OR(l1, l1, l2));
                }
                code.push_back(ORcc(g0, l1, g0));
                break;
            case kMostNegative:
                code.push_back(MOVi(l1, 1));
                code.push_back(SLLXi(l1, l1, icc ? 31 : 63));
                code.push_back(CMPi(g1, 0));
                break;
            case kZeroUpper:
                code.push_back(SLLXi(l1, g1, 33));
                code.push_back(CMPi(g1, 0));
                break;
        }
        code.push_back(Bicc(cA, 1, true));  // (the loop is entered by a branch, as well as from itself)
        code.push_back(l.branch);
        code.push_back(l.step);
        code.push_back(RDASR(o0, 2));
        code.push_back(STX(l1, o5, 0));
        code.push_back(STB(o0, o5, 8));
        code.push_back(ADDi(o5, o5, 16));
        code.push_back(SUBcci(g1, g1, 1));
        code.push_back(BPcc(cNE, kXcc, top - static_cast<int>(code.size())));
        code.push_back(NOP);
        code.push_back(Bicc(cA, 0));
        code.push_back(NOP);
        for (u64 n : {40000u, 7777u, 12346u, 25001u}) {
            check_run_against_interpreter(code, n);
            check_run_against_interpreter(code, n, false);
        }
        // With a delay charge other than the usual one, run() ends right
        // after a loop is run at once: the state there, condition codes
        // included, is the interpreter's after as many instructions.
        for (bool jit : {true, false}) {
            Rig fast, ref;
            ref.cpu.set_block_cache(false);
            if (!jit) fast.cpu.set_jit(false);
            fast.cpu.set_delay_cycles_per_step(4.0);
            for (Rig* x : {&fast, &ref}) {
                x->cpu.set_pstate(pstate::PRIV | pstate::PEF);
                for (std::size_t i = 0; i < code.size(); ++i) x->bus.write(Rig::kCode + 4 * i, code[i]);
                x->cpu.set_reg(o5, kData);
            }
            const u64 s0 = fast.cpu.steps();  // (after the rig's own setup)
            fast.cpu.set_pc(Rig::kCode);
            for (int k = 0; k < 40 && fast.cpu.delay_steps() == 0; ++k) fast.cpu.run(40000);
            ref.run_at(Rig::kCode, fast.cpu.steps() - s0);
            CHECK_EQ(fast.cpu.instructions(), ref.cpu.instructions());
            check_same(state_of(fast), state_of(ref));
        }
    }
}

// A delay loop run at once costs the real CPU's time, half the delay
// charge a step (an iteration a cycle), and every other step the usual
// charge, whether that is less (a fast host) or more (a slow one); run()
// ends after it; and a TICK_CMPR that comes due during it interrupts on
// time, neither early nor skipped over. The recompiler and the block cache
// alone alike.
TEST(jit_delay_loops_take_the_real_cpu_time_and_keep_tick_compare) {
    for (double cpi : {0.25, 2.0})
    for (bool jit : {true, false}) {
        // A countdown from 40960, then a spin that counts (so is no polling loop).
        const std::vector<u32> code = {SETHI(l0, 40960), ORcc(g0, l0, g0), BPcc(cG, kXcc, 0), SUBcci(l0, l0, 1),
                                       Bicc(cA, 0), ADDi(l7, l7, 1)};
        constexpr u64 kRun = 100000;
        Rig r, ref;
        ref.cpu.set_block_cache(false);
        if (!jit) r.cpu.set_jit(false);
        for (Rig* x : {&r, &ref}) {
            x->cpu.set_pstate(pstate::PRIV);
            x->cpu.set_cycles_per_step(cpi);
            for (std::size_t i = 0; i < code.size(); ++i) x->bus.write(Rig::kCode + 4 * i, code[i]);
        }
        r.cpu.set_delay_cycles_per_step(1.0);
        const u64 t0 = r.cpu.cycles(), s0 = r.cpu.steps(), i0 = r.cpu.instructions();  // (after the rig's own setup)
        r.cpu.set_pc(Rig::kCode);
        unsigned runs = 0;
        while (r.cpu.instructions() - i0 < kRun && runs < 100000) r.cpu.run(kRun - (r.cpu.instructions() - i0)), ++runs;
        ref.run_at(Rig::kCode, kRun);
        check_same(state_of(r), state_of(ref));
        const u64 delay = r.cpu.delay_steps(), steps = r.cpu.steps() - s0;
        CHECK(delay > 70000);
        CHECK(runs > 1);
        CHECK_EQ(r.cpu.delay_cycles(), delay / 2);
        const u64 expect = static_cast<u64>(static_cast<double>(steps - delay) * cpi) + delay / 2;
        const u64 took = r.cpu.cycles() - t0;
        CHECK(took + 1 >= expect && took <= expect + 1);

        // TICK_CMPR at 20480, inside the loop: the level 14 handler reads
        // TICK as it is taken (into locals: the globals are the alternates),
        // clears TICK_INT and goes back.
        Rig t;
        if (!jit) t.cpu.set_jit(false);
        t.cpu.set_cycles_per_step(cpi);
        t.cpu.set_delay_cycles_per_step(1.0);
        t.cpu.set_pstate(pstate::PRIV);
        t.exec({WRPR(prTICK, g0, g0), SETHI(g1, 20480), WRASR(23, g1, g0)});
        t.put(Rig::kTba + (0x4E << 5), {RDPR(l5, prTICK), ADDi(l6, l6, 1), WRASRi(21, g0, 1), RETRY});
        for (std::size_t i = 0; i < code.size(); ++i) t.bus.write(Rig::kCode + 4 * i, code[i]);
        t.cpu.set_pstate(pstate::PRIV | pstate::IE);
        t.cpu.set_pc(Rig::kCode);
        runs = 0;
        const u64 t_i0 = t.cpu.instructions();
        while (t.cpu.instructions() - t_i0 < kRun && runs < 100000) t.cpu.run(kRun - (t.cpu.instructions() - t_i0)), ++runs;
        CHECK_EQ(t.cpu.reg(l6), 1u);
        const u64 at = t.cpu.reg(l5) & ~(u64{1} << 63);
        CHECK(at >= 20480 && at <= 20480 + 4);
        CHECK(t.cpu.delay_steps() > 0);
        CHECK_EQ(t.cpu.reg(l0), ~u64{0});  // (the loop went on to its end after the interrupt)
    }
}

// Wherever TICK_CMPR falls in a delay loop run at once on a fast host (a
// charge a step below the loop's), its interrupt comes: guest time that
// moves onto or past it in one go raises TICK_INT. The loop is OpenBoot's
// ms (a countdown in a branch's delay slot, inside a count of
// milliseconds), translated after its first runs, run as the machine runs
// it, 4096 steps at a go; OpenBoot's 100 Hz clock on TICK_CMPR stopped for
// good when a translated delay ended exactly on it (as it did at these
// charges and compares).
TEST(jit_delay_loops_never_skip_tick_compare) {
    for (bool jit : {true, false})
    for (double cpi : {0.2925, 0.4075})
    for (u64 cmpr = 30000; cmpr < 30040; ++cmpr) {
        const std::vector<u32> code = {
            MOVi(l0, 100), MOVi(g4, 2000),
            ORcc(l1, l0, g0),             // +8: a millisecond's count
            BPcc(cG, kXcc, 0), SUBcci(l1, l1, 1),
            SUBcci(g4, g4, 1),
            BPcc(cG, kXcc, -3), ORcc(l1, l0, g0),
            Bicc(cA, 0), ADDi(l7, l7, 1)};
        Rig t;
        if (!jit) t.cpu.set_jit(false);
        t.cpu.set_cycles_per_step(cpi);
        t.cpu.set_delay_cycles_per_step(1.0);
        t.cpu.set_pstate(pstate::PRIV);
        t.exec({WRPR(prTICK, g0, g0), SETHI(g1, cmpr), ORi(g1, g1, static_cast<int>(cmpr & 0x3FF)), WRASR(23, g1, g0)});
        t.put(Rig::kTba + (0x4E << 5), {RDPR(l5, prTICK), ADDi(l6, l6, 1), WRASRi(21, g0, 1), RETRY});
        for (std::size_t i = 0; i < code.size(); ++i) t.bus.write(Rig::kCode + 4 * i, code[i]);
        t.cpu.set_pstate(pstate::PRIV | pstate::IE);
        t.cpu.set_pc(Rig::kCode);
        const u64 i0 = t.cpu.instructions();
        while (t.cpu.instructions() - i0 < 200000 && t.cpu.reg(l6) == 0) t.cpu.run(4096);
        CHECK_EQ(t.cpu.reg(l6), 1u);
        const u64 at = t.cpu.reg(l5) & ~(u64{1} << 63);
        CHECK(at >= cmpr && at <= cmpr + 4);
    }
}

// A delay loop run at once leaves the condition codes as its last iteration
// would, including the overflow of a count crossing the most negative value
// (of 32 or 64 bits) and an add's carry: a run ending right after one (a
// delay charge below the usual one ends it there) is the interpreter's state
// after as many steps, wherever it lands.
TEST(jit_delay_loops_run_at_once_leave_the_last_iterations_condition_codes) {
    struct Loop {
        u32 branch, step;
        bool wide;  // count from 2^63 + 24 (else from 2^31 + 24, upper half 5)
    };
    const Loop loops[] = {
        {Bicc(cNE, 0), SUBcci(l1, l1, 1), false},
        {BPcc(cNE, kIcc, 0), ADDcci(l1, l1, -1), false},
        {BPcc(cNE, kXcc, 0), SUBcci(l1, l1, 1), true},
        {BPcc(cNE, kXcc, 0), ADDcci(l1, l1, -1), true},
    };
    for (const Loop& l : loops) {
        const std::vector<u32> code = {MOVi(l1, 1), SLLXi(l1, l1, l.wide ? 63 : 31), ORi(l1, l1, 24),
                                       MOVi(l2, l.wide ? 0 : 5), SLLXi(l2, l2, 32), OR(l1, l1, l2),
                                       ORcc(g0, l1, g0), l.branch, l.step, Bicc(cA, 0), NOP};
        for (bool jit : {true, false})
        for (u64 n = 30; n < 140; ++n) {
            Rig fast, ref;
            ref.cpu.set_block_cache(false);
            if (!jit) fast.cpu.set_jit(false);
            fast.cpu.set_delay_cycles_per_step(1.0);  // (half a cycle a step: below the usual one)
            for (Rig* x : {&fast, &ref})
                for (std::size_t i = 0; i < code.size(); ++i) x->bus.write(Rig::kCode + 4 * i, code[i]);
            const u64 s0 = fast.cpu.steps();
            fast.run_at(Rig::kCode, n);
            ref.run_at(Rig::kCode, fast.cpu.steps() - s0);
            check_same(state_of(fast), state_of(ref));
        }
    }
}

// Timed loops (waiting for TICK to pass a value) end, or stop part way,
// exactly where stepping them does, at any charge per step: each test of x
// against t that holds until TICK passes x, with x first or t first, RDPR
// or RD of TICK; and with run() ending right after a batch (a delay charge
// other than the usual one), the TICK read and the condition codes are the
// last batched iteration's.
TEST(jit_timed_loops_match_the_interpreter) {
    const u32 rdpr = RDPR(o2, prTICK), rd = RDASR(o2, 4);
    struct Loop {
        unsigned cond;
        bool t_first;
        int bias;  // added to the target (so a "less or equal" ends one later)
    };
    const Loop loops[] = {{cCC, false, 0}, {cGU, false, 0}, {cGE, false, 1}, {cG, false, -1},
                          {cCS, true, 0},  {cLEU, true, 1}, {cL, true, -1},  {cLE, true, 0},
                          {cNE, false, 0}};  // (not a timed loop: TICK may pass x without meeting it)
    for (const Loop& l : loops)
    for (u32 read : {rdpr, rd}) {
        std::vector<u32> code = {MOVi(g1, 12)};
        const auto top = static_cast<int>(code.size());
        code.push_back(MULXi(o3, g1, 37));
        code.push_back(ADDi(o3, o3, 200 + l.bias));
        code.push_back(read);
        code.push_back(ADD(o1, o2, o3));            // the target: 200-650 cycles on
        code.push_back(l.t_first ? CMP(o2, o1) : CMP(o1, o2));
        code.push_back(read);
        code.push_back(BPcc(l.cond, kXcc, -2));
        code.push_back(NOP);
        code.push_back(RDASR(o0, 2));
        code.push_back(STX(o2, o5, 0));
        code.push_back(STB(o0, o5, 8));
        code.push_back(ADDi(o5, o5, 16));
        code.push_back(SUBcci(g1, g1, 1));
        code.push_back(BPcc(cNE, kXcc, top - static_cast<int>(code.size())));
        code.push_back(NOP);
        code.push_back(Bicc(cA, 0));
        code.push_back(NOP);
        for (double cpi : {0.25, 1.0, 3.0})
        for (u64 n : {40000u, 1999u, 4321u, 7777u}) {
            check_run_against_interpreter(code, n, true, cpi);
            check_run_against_interpreter(code, n, false, cpi);
        }
        for (bool jit : {true, false}) {
        u64 batched = 0;
        for (u64 n = 20; n < 400; n += 7) {
            Rig fast, ref;
            ref.cpu.set_block_cache(false);
            if (!jit) fast.cpu.set_jit(false);
            fast.cpu.set_delay_cycles_per_step(1.0);
            for (Rig* x : {&fast, &ref}) {
                x->cpu.set_cycles_per_step(0.25);
                x->cpu.set_reg(o5, kData);
                for (std::size_t i = 0; i < code.size(); ++i) x->bus.write(Rig::kCode + 4 * i, code[i]);
            }
            const u64 s0 = fast.cpu.steps();
            fast.run_at(Rig::kCode, n);
            ref.run_at(Rig::kCode, fast.cpu.steps() - s0);
            check_same(state_of(fast), state_of(ref));
            batched += fast.cpu.delay_steps();
        }
        if (l.cond == cNE) CHECK_EQ(batched, 0u);
        else CHECK(batched > 0);
        }
    }
}

// A loop whose delay slot goes through execute() (an FP add with a NaN
// operand) goes on from the branch's target as stepping it does, taken or
// not, with the codes and FSR as the interpreter leaves them.
TEST(jit_delay_slots_run_through_execute_go_on_to_the_branch_target) {
    const std::vector<u32> body = {
        STW(g0, o5, 0), LDF(5, o5, 0),                             // f5 = +0
        SETHI(g2, 0x7FC00000), STW(g2, o5, 4), LDF(6, o5, 4),      // f6 = a quiet NaN
        ANDi(l0, g1, 3), ADDi(l0, l0, 2),
        SUBcci(l0, l0, 1), BPcc(cNE, kXcc, -1), FPOP1(7, 5, 6, 0x041),  // fadds with the NaN in the delay slot
        ADD(l1, l1, l0), STF(7, o5, 8), ADDi(o5, o5, 16)};
    check_against_interpreter(body, 64);
}

// Blocks that use the same registers again and again around calls (VIS
// operations on integer registers, each operand once the other's
// argument) and around changes of window (locals used on both sides of
// SAVE and RESTORE).
TEST(jit_registers_used_around_calls_and_window_changes_match_the_interpreter) {
    const std::vector<u32> vis_body = {
        ADD(g2, g2, g1), XOR(g2, g2, g1), ADD(g3, g3, g2), XOR(g3, g3, g1),
        VIS(l0, g1, g2, 0x000),  // EDGE8 g1, g2
        VIS(l1, g2, g1, 0x004),  // EDGE16 g2, g1
        ADD(l2, l2, l0), ADD(l2, l2, l1), ADD(l3, l3, g3)};
    check_against_interpreter(vis_body, 32);
    const std::vector<u32> window_body = {
        ADD(l0, l0, g1), ADD(l1, l1, l0), ADDi(l0, l0, 3),
        SAVEi(g0, g0, 0), ADD(l0, l0, g1), ADD(l1, l1, l0), ADDi(l0, l0, 5),  // the new window's locals
        RESTOREi(g0, g0, 0), ADD(l2, l2, l0), ADD(l2, l2, l1)};             // and the old ones again
    check_against_interpreter(window_body, 32);
}

namespace {

// A device whose register 0 reads 0 until guest time `at`, then 1, and
// says so (`promise` false: it promises nothing), and whose register 8 is a
// clock: guest time / 7 (register 16 too, little-endian). Reads are counted.
struct TimedFlag : Device {
    const UltraSparc* cpu = nullptr;
    u64 at = 0;
    bool promise = true;
    u64 reads = 0;
    const char* name() const override { return "timed-flag"; }
    bool read(u64 offset, unsigned, u64& value) override {
        ++reads;
        value = offset == 8 ? cpu->cycles() / 7 : offset == 16 ? __builtin_bswap64(cpu->cycles() / 7) : cpu->cycles() >= at ? 1 : 0;
        return true;
    }
    bool write(u64, unsigned, u64) override { return true; }
    u64 stable_until(u64 offset, unsigned) const override {
        if (!promise) return 0;
        if (offset == 8) return kClock;
        if (offset == 16) return kClockLittle;
        return cpu->cycles() < at ? at : kForever;
    }
};

constexpr u64 kFlagPa = 0x100000000;  // (above the rigs' RAM)

// Runs `code` for `n` steps with the recompiler (or the block cache alone)
// and with step() alone, each with a TimedFlag at kFlagPa (%o4 holds its
// address, %l4 that of one of its clocks); compares them, and returns the
// steps run at once.
u64 check_poll_against_interpreter(const std::vector<u32>& code, u64 n, bool jit, u64 at, bool promise,
                                   double cpi = 1.0, u64 clock_pa = kFlagPa + 8) {
    Rig fast, ref;
    TimedFlag ffast, fref;
    ref.cpu.set_block_cache(false);
    if (!jit) fast.cpu.set_jit(false);
    for (auto [r, f] : {std::pair{&fast, &ffast}, std::pair{&ref, &fref}}) {
        f->cpu = &r->cpu, f->at = at, f->promise = promise;
        r->bus.map_device(kFlagPa, Bus::kPageSize, f);
        r->cpu.set_cycles_per_step(cpi);
        r->cpu.set_pstate(pstate::PRIV | pstate::PEF);
        for (std::size_t i = 0; i < code.size(); ++i) r->bus.write(Rig::kCode + 4 * i, code[i]);
        r->cpu.set_reg(o5, kData);
        r->cpu.set_reg(o4, kFlagPa);
        r->cpu.set_reg(l4, clock_pa);
        r->run_at(Rig::kCode, n);
    }
    CHECK_EQ(fast.cpu.instructions(), ref.cpu.instructions());
    CHECK_EQ(fast.cpu.cycles(), ref.cpu.cycles());
    if (!promise) CHECK_EQ(ffast.reads, fref.reads);
    check_same(state_of(fast), state_of(ref));
    return fast.cpu.delay_steps();
}

}  // namespace

// Polling loops (a block going back to itself that loads, tests and
// perhaps reads TICK, its state the same each time round) run many
// iterations at once while what they read holds still: memory, and a
// device register until the time it says; they end, or stop part way,
// exactly where stepping them does. Loops whose state moves (a count, a
// TICK reading used) and devices that promise nothing run as they are.
TEST(jit_polling_loops_match_the_interpreter) {
    // Wait for the flag, reading TICK each time round (into %o3, unused).
    const std::vector<u32> wait_flag = {
        MOVi(o1, 0), RDPR(o3, prTICK), LDUBA(o2, o4, g0, asi::PHYS_BYPASS_EC_E), ANDcci(g0, o2, 1),
        BPcc(cE, kXcc, -3), NOP,                                   // (the loop: rdpr, lduba, andcc, be, nop)
        ADD(l0, l0, o3), ADDi(o1, o1, 1), Bicc(cA, 0), ADDi(l6, l6, 1)};  // (then a spin that counts: no polling loop)
    // The load in the delay slot, so each test is of the last iteration's
    // reading: the first is of %l1 as it was before the loop.
    const std::vector<u32> wait_flag_slot = {
        MOVi(l1, 0), Bicc(cA, 1, true),                                                        // (into the loop)
        ANDcci(g0, l1, 1), BPcc(cE, kXcc, -1), LDUBA(l1, o4, g0, asi::PHYS_BYPASS_EC_E),  // (the loop)
        ADD(l0, l0, l1), Bicc(cA, 0), ADDi(l6, l6, 1)};
    // Wait on memory that never changes (to the end of the run).
    const std::vector<u32> wait_mem = {LDUW(l1, o5, 0), ANDcci(g0, l1, 1), BPcc(cE, kXcc, -2), LDUW(l1, o5, 0)};
    // Not polling loops: a count in the loop; a TICK reading used.
    const std::vector<u32> counting = {LDUW(l1, o5, 0), ADDi(l2, l2, 1), ANDcci(g0, l1, 1), BPcc(cE, kXcc, -3), NOP};
    const std::vector<u32> tick_used = {RDPR(o3, prTICK), ADD(l3, o3, g0), LDUW(l1, o5, 0), ANDcci(g0, l1, 1),
                                        BPcc(cE, kXcc, -4), NOP};
    // A wait on TICK in another order than a timed loop's: TICK is a clock,
    // compared with a settled value, the loop going on while it is short.
    const std::vector<u32> tick_compared = {RDPR(o4, prTICK), ADDi(o4, o4, 2000),
                                            RDPR(o3, prTICK), CMP(o3, o4), BPcc(cL, kXcc, -2), NOP,
                                            ADD(l0, l0, o3), Bicc(cA, 0), ADDi(l6, l6, 1)};
    for (bool jit : {true, false}) {
        u64 batched = 0;
        for (double cpi : {0.25, 1.0, 3.0})
            for (u64 at : {0u, 777u, 20000u, 100000u})
                for (u64 n : {300u, 5003u, 30000u, 150001u}) batched += check_poll_against_interpreter(wait_flag, n, jit, at, true, cpi);
        CHECK(batched > 0);
        CHECK(check_poll_against_interpreter(wait_mem, 50000, jit, 0, true) > 0);
        CHECK_EQ(check_poll_against_interpreter(counting, 50000, jit, 0, true), 0u);
        CHECK_EQ(check_poll_against_interpreter(tick_used, 50000, jit, 0, true), 0u);
        for (u64 n : {300u, 30000u}) CHECK_EQ(check_poll_against_interpreter(wait_flag, n, jit, 20000, false), 0u);
        for (double cpi : {0.25, 1.0, 3.0})
            for (u64 n : {1000u, 1997u, 2003u, 5000u}) CHECK(check_poll_against_interpreter(tick_compared, n, jit, 0, true, cpi) > 0);
        for (u64 at : {0u, 30u, 5000u}) check_poll_against_interpreter(wait_flag_slot, 20000, jit, at, true);
        // Runs ending at every point, right after a batch among them (TICK's
        // reading as the last batched iteration's).
        for (u64 n = 60; n < 160; ++n) check_poll_against_interpreter(wait_flag, n, jit, 1000000, true, 0.25);
    }
}

// A polling loop over two blocks, as the PROM's input with a timeout: poll
// a flag; if clear, read a device clock and go round again unless it has
// passed a limit. It ends, on the flag or the timeout, or stops part way,
// exactly where stepping it does.
TEST(jit_polling_loops_over_blocks_with_a_device_clock_match_the_interpreter) {
    const std::vector<u32> code = {
        LDXA(g5, l4, g0, asi::PHYS_BYPASS_EC_E), ADDi(o3, g5, 1500),  // the limit: the clock now + 1500
        Bicc(cA, 1, true),                                              // (into the loop)
        LDUBA(o2, o4, g0, asi::PHYS_BYPASS_EC_E), ANDcci(g0, o2, 1), BPcc(cE, kXcc, 5), NOP,  // the flag?
        ADD(l0, l0, o3), Bicc(cA, 0), ADDi(l6, l6, 1),                                         // (the flag: a spin that counts)
        LDXA(l3, l4, g0, asi::PHYS_BYPASS_EC_E), CMP(l3, o3), BPcc(cL, kXcc, -9), NOP,          // the clock, short of the limit?
        ADD(l1, l1, l3), Bicc(cA, 0), ADDi(l6, l6, 1)};                                        // (the timeout)
    for (bool jit : {true, false}) {
        u64 batched = 0;
        for (double cpi : {0.25, 1.0, 3.0})
            for (u64 at : {u64{3000}, u64{1} << 40})
                for (u64 n : {200u, 2001u, 4999u, 12000u, 40000u}) batched += check_poll_against_interpreter(code, n, jit, at, true, cpi);
        CHECK(batched > 0);
        for (u64 n = 100; n < 220; ++n) check_poll_against_interpreter(code, n, jit, u64{1} << 40, true, 0.25);
        // The clock read little-endian (register 16, through the
        // little-endian ASI): it still counts up; register 8 read so does not.
        std::vector<u32> little = code;
        for (unsigned i : {0u, 10u}) little[i] = LDXA(i ? l3 : g5, l4, g0, asi::PHYS_BYPASS_EC_E_L);
        CHECK(check_poll_against_interpreter(little, 40000, jit, u64{1} << 40, true, 1.0, kFlagPa + 16) > 0);
        CHECK_EQ(check_poll_against_interpreter(little, 40000, jit, u64{1} << 40, true, 1.0, kFlagPa + 8), 0u);
        // The clock read in the delay slot: each compare is of the last
        // iteration's reading.
        const std::vector<u32> slot = {
            LDXA(g5, l4, g0, asi::PHYS_BYPASS_EC_E), ADDi(o3, g5, 900), MOVi(l3, 0), Bicc(cA, 1, true),
            CMP(l3, o3), BPcc(cL, kXcc, -1), LDXA(l3, l4, g0, asi::PHYS_BYPASS_EC_E),
            ADD(l1, l1, l3), Bicc(cA, 0), ADDi(l6, l6, 1)};
        u64 slot_batched = 0;
        for (double cpi : {0.25, 3.0})
            for (u64 n = 100; n < 400; n += 3) slot_batched += check_poll_against_interpreter(slot, n, jit, 0, true, cpi);
        CHECK(slot_batched > 0);
        // A wait for TICK to equal a value is no polling loop: TICK may pass it.
        const std::vector<u32> tick_equal = {RDPR(o4, prTICK), ADDi(o4, o4, 2000),
                                             RDPR(o3, prTICK), CMP(o3, o4), BPcc(cNE, kXcc, -2), NOP,
                                             ADD(l0, l0, o3), Bicc(cA, 0), ADDi(l6, l6, 1)};
        for (double cpi : {0.25, 3.0}) CHECK_EQ(check_poll_against_interpreter(tick_equal, 9000, jit, 0, true, cpi), 0u);
        // A compare of TICK's low half (icc) is no clock's: there TICK
        // wraps, and the test turns back. TICK from 0x7FFF0000 to a limit
        // 0x8000 on: past it, the low half turns negative and "less" again.
        const std::vector<u32> tick_icc = {SETHI(g2, 0x7FFF0000), WRPR(prTICK, g2, g0), RDPR(o4, prTICK),
                                           SETHI(g3, 0x8000), ADD(o4, o4, g3), Bicc(cA, 1, true),
                                           RDPR(o3, prTICK), CMP(o3, o4), Bicc(cL, -2), NOP,
                                           ADD(l0, l0, o3), Bicc(cA, 0), ADDi(l6, l6, 1)};
        CHECK_EQ(check_poll_against_interpreter(tick_icc, 40000, jit, 0, true, 16.0), 0u);
        // A clock read in the byte order in which it does not count up
        // (register 16, big-endian): its readings go up and down, and the
        // loop must stop where they first reach the limit.
        const std::vector<u32> wrong_order = {MOVi(o3, 1), SLLXi(o3, o3, 62), Bicc(cA, 1, true),
                                              LDXA(l3, l4, g0, asi::PHYS_BYPASS_EC_E), CMP(l3, o3), BPcc(cL, kXcc, -2), NOP,
                                              ADD(l1, l1, l3), Bicc(cA, 0), ADDi(l6, l6, 1)};
        CHECK_EQ(check_poll_against_interpreter(wrong_order, 40000, jit, 0, true, 1.0, kFlagPa + 16), 0u);
    }
}

namespace {

// An idle loop as Solaris's: a load and a test of what it loaded, and a
// call to a function on another page that only returns. `slot` is the
// delay slot of the test's branch and `call_slot` the call's (for loops
// that must not count as idle). The level 14 handler (TICK_CMPR) reads
// TICK, sets the word loaded and clears the interrupt; the loop then
// leaves for a spin that counts in %l7. TICK_CMPR is `compare`.
void put_idle_loop(Rig& r, u32 slot, u32 call_slot, u32 compare = 204800) {
    constexpr u64 kFar = 0x3000;
    r.exec({WRPR(prTICK, g0, g0), SETHI(g1, compare), WRASR(23, g1, g0), SETHI(o5, kData), MOVi(g1, 0)});
    r.cpu.set_idle_skip(true);  // (a place the setup came to is no place the loop comes to)
    r.put(Rig::kCode, {LDUW(g2, o5, 0), CMPi(g2, 0), Bicc(cNE, 6), slot,
                       CALL(static_cast<int>((kFar - (Rig::kCode + 16)) / 4)), call_slot, Bicc(cA, -6), NOP,
                       Bicc(cA, 0), ADDi(l7, l7, 1)});
    r.put(kFar, {JMPLi(g0, o7, 8), NOP});
    r.put(Rig::kTba + (0x4E << 5), {RDPR(l5, prTICK), MOVi(l4, 1), STW(l4, o5, 0), WRASRi(21, g0, 1), RETRY});
    r.cpu.set_pstate(pstate::PRIV | pstate::PEF | pstate::IE);
    r.cpu.set_pc(Rig::kCode);
}

// Runs as the machine does: slices of up to 4096, and an idle loop gone
// round at once (to guest time `until` steps on, ~0: no limit), within
// `n` instructions in all. Returns the instructions skipped.
u64 run_skipping(Rig& r, u64 n, u64 until) {
    const u64 end = r.cpu.instructions() + n;
    while (r.cpu.instructions() < end && !r.cpu.halted()) {
        r.cpu.run(std::min<u64>(end - r.cpu.instructions(), 4096));
        if (r.cpu.idle_period())
            r.cpu.skip_idle(until == ~u64{0} ? until : r.cpu.cycles() + until, end - std::min(end, r.cpu.instructions()));
    }
    return r.cpu.idle_instructions();
}

}  // namespace

// An idle loop gone round at once ends exactly where stepping it does: the
// interrupt that ends it taken at the same step (the handler reads TICK),
// at every charge per step, through translated code and decoded blocks.
TEST(idle_loops_run_at_once_match_the_interpreter) {
    for (double cpi : {1.0, 0.25, 3.0})
    for (bool jit : {true, false})
    for (u64 until : {~u64{0}, u64{5000}}) {
        constexpr u64 kRun = 600000, kIdle = 300000;  // (steps idle before the interrupt)
        Rig r, ref;
        ref.cpu.set_block_cache(false);
        if (!jit) r.cpu.set_jit(false);
        for (Rig* x : {&r, &ref}) {
            x->cpu.set_cycles_per_step(cpi);
            put_idle_loop(*x, NOP, NOP, static_cast<u32>(kIdle * cpi) & ~0x3FFu);
        }
        const u64 skipped = run_skipping(r, kRun, until);
        run_skipping(ref, kRun, until);
        if (until == ~u64{0}) CHECK(skipped > kIdle / 2);
        else CHECK(skipped > 0);
        CHECK(r.cpu.idle_cycles() > 0);
        CHECK_EQ(r.cpu.instructions(), ref.cpu.instructions());
        CHECK_EQ(r.cpu.steps(), ref.cpu.steps());
        CHECK_EQ(r.cpu.cycles(), ref.cpu.cycles());
        CHECK(r.cpu.reg(l5) == ref.cpu.reg(l5) && r.cpu.reg(l5) != 0);  // (TICK as the interrupt was taken)
        CHECK(r.cpu.reg(l7) > 0);
        check_same(state_of(r), state_of(ref));
    }
}

// Loops that store, or read TICK, are not idle however the registers come
// round: none of them is gone round at once. Nor is any loop with idle
// skipping off.
TEST(idle_loops_that_store_or_read_tick_are_not_skipped) {
    struct Loop {
        u32 slot, call_slot;
        bool skip_on;
    };
    const Loop loops[] = {{STW(g0, o5, 8), NOP, true},          // the same value each time round
                          {RDPR(g3, prTICK), MOVi(g3, 0), true},  // TICK read, then dropped
                          {RDASR(g3, 4), MOVi(g3, 0), true},
                          {NOP, NOP, false}};
    for (const Loop& l : loops)
    for (bool jit : {true, false}) {
        constexpr u64 kRun = 200000;
        Rig r, ref;
        ref.cpu.set_block_cache(false);
        if (!jit) r.cpu.set_jit(false);
        for (Rig* x : {&r, &ref}) put_idle_loop(*x, l.slot, l.call_slot);
        r.cpu.set_idle_skip(l.skip_on);
        CHECK_EQ(run_skipping(r, kRun, ~u64{0}), 0u);
        run_skipping(ref, kRun, ~u64{0});
        CHECK_EQ(r.cpu.cycles(), ref.cpu.cycles());
        check_same(state_of(r), state_of(ref));
    }
}
