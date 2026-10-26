// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
#include <initializer_list>
#include <vector>

#include "cpu/ultrasparc/disasm.h"
#include "cpu/ultrasparc/ultrasparc.h"
#include "mem/bus.h"
#include "cpu_rig.h"
#include "sparc_asm.h"
#include "test.h"

using namespace uv;
using namespace uv::usparc;
using namespace uv::test::sparc;
using uv::test::Rig;


TEST(cpu_por_starts_in_red_state_at_the_reset_vector) {
    std::vector<u8> rom(Bus::kPageSize);
    Bus bus;
    bus.map_ram(0x1FFF0000000ull, rom.size(), rom.data(), false);
    store_be<u32>(rom.data() + 0x20, MOVi(g1, 42));
    UltraSparc cpu(bus);
    CHECK_EQ(cpu.pc(), 0xFFFFFFFFF0000020ull);
    CHECK_EQ(cpu.tl(), kMaxTl);
    CHECK(cpu.pstate_reg() & pstate::RED);
    CHECK(cpu.pstate_reg() & pstate::PRIV);
    CHECK_EQ(cpu.trap_type(kMaxTl), tt::kPowerOnReset);
    cpu.step();
    CHECK_EQ(cpu.reg(g1), 42u);  // fetched from the physical reset vector, in the alternate globals
    CHECK_EQ(cpu.pc(), 0xFFFFFFFFF0000024ull);
}

TEST(cpu_add_and_sub_set_both_condition_code_sets) {
    Rig r;
    r.cpu.set_reg(g1, 0x7FFFFFFF);
    r.exec({ADDcci(g2, g1, 1)});
    CHECK_EQ(r.cpu.reg(g2), 0x80000000u);
    CHECK_EQ(r.cpu.ccr() & 15, 0xAu);  // icc: N and V
    CHECK_EQ(r.cpu.ccr() >> 4, 0x0u);  // xcc: positive, no overflow
    r.exec({SUBcci(g2, g0, 1)});
    CHECK_EQ(r.cpu.reg(g2), ~u64{0});
    CHECK_EQ(r.cpu.ccr(), 0x99u);      // N and C in both
    r.cpu.set_reg(g1, ~u64{0});
    r.exec({ADDcci(g2, g1, 1)});
    CHECK_EQ(r.cpu.reg(g2), 0u);
    CHECK_EQ(r.cpu.ccr(), 0x55u);  // Z and C in both
    r.cpu.set_reg(g1, ~u64{0});
    r.cpu.set_ccr(0x11);
    r.exec({ADDCcc(g2, g1, g0)});  // ~0 + 0 + carry
    CHECK_EQ(r.cpu.reg(g2), 0u);
    CHECK_EQ(r.cpu.ccr(), 0x55u);
}

TEST(cpu_sethi_clears_the_upper_word_and_shifts_work_at_both_widths) {
    Rig r;
    r.cpu.set_reg(g1, ~u64{0});
    r.exec({SETHI(g1, 0xFFFFFC00), SRLi(g2, g1, 4), SRAi(g3, g1, 4), SLLXi(g4, g1, 32), SRAXi(g5, g4, 40)});
    CHECK_EQ(r.cpu.reg(g1), 0xFFFFFC00u);
    CHECK_EQ(r.cpu.reg(g2), 0x0FFFFFC0u);
    CHECK_EQ(r.cpu.reg(g3), 0xFFFFFFFFFFFFFFC0ull);
    CHECK_EQ(r.cpu.reg(g4), 0xFFFFFC0000000000ull);
    CHECK_EQ(r.cpu.reg(g5), 0xFFFFFFFFFFFFFFFCull);
}

TEST(cpu_branches_run_the_delay_slot_and_honour_annul) {
    Rig r;
    // ba,a skips its delay slot; a not-taken bne,a skips it too; a taken
    // be (no annul) runs it.
    r.exec({Bicc(cA, 2, true), MOVi(g1, 1), MOVi(g2, 2), CMP(g0, g0), Bicc(cNE, 4, true), MOVi(g3, 3),
            Bicc(cE, 3), MOVi(g4, 4), MOVi(g5, 5), MOVi(g6, 6)},
           7);
    CHECK_EQ(r.cpu.reg(g1), 0u);
    CHECK_EQ(r.cpu.reg(g2), 2u);
    CHECK_EQ(r.cpu.reg(g3), 0u);
    CHECK_EQ(r.cpu.reg(g4), 4u);
    CHECK_EQ(r.cpu.reg(g5), 0u);
    CHECK_EQ(r.cpu.reg(g6), 6u);
}

TEST(cpu_bpcc_on_xcc_and_bpr_and_movcc) {
    Rig r;
    r.cpu.set_reg(g1, 0x100000000ull);
    // cmp of 0x1_0000_0000 with 0: icc says equal, xcc does not.
    r.exec({CMP(g1, g0), BPcc(cE, kXcc, 3), NOP, MOVi(g2, 1), MOVcc(cE, kIcc, g3, 7), BPr(5, g1, 3), NOP, MOVi(g4, 9),
            MOVr(1, g5, g0, -3)},
           8);
    CHECK_EQ(r.cpu.reg(g2), 1u);           // not taken on xcc
    CHECK_EQ(r.cpu.reg(g3), 7u);           // equal on icc
    CHECK_EQ(r.cpu.reg(g4), 0u);           // brnz taken over it
    CHECK_EQ(r.cpu.reg(g5), ~u64{2});      // movrz %g0
}

TEST(cpu_call_and_jmpl_link_and_return) {
    Rig r;
    r.exec({CALL(3), NOP, MOVi(g1, 1), JMPLi(g0, o7, 8), MOVi(g2, 2)}, 5);
    CHECK_EQ(r.cpu.reg(o7), Rig::kCode);
    CHECK_EQ(r.cpu.reg(g2), 2u);
    CHECK_EQ(r.cpu.reg(g1), 1u);
    CHECK_EQ(r.cpu.pc(), Rig::kCode + 12);
}

TEST(cpu_save_and_restore_move_the_window) {
    Rig r;
    r.cpu.set_reg(o0, 11);
    r.cpu.set_reg(l0, 22);
    r.exec({SAVEi(sp, sp, -192), ADDi(l0, i0, 1), RESTOREi(o1, l0, 0)});
    CHECK_EQ(r.cpu.reg(o1), 12u);   // %l0 of the inner window, through restore
    CHECK_EQ(r.cpu.reg(l0), 22u);   // the outer window's local is untouched
    CHECK_EQ(r.cpu.cwp(), 0u);
    CHECK_EQ(r.cpu.cansave(), 6u);
    CHECK_EQ(r.cpu.canrestore(), 0u);
}

TEST(cpu_save_with_no_window_to_spare_takes_a_spill_trap) {
    Rig r;
    r.cpu.set_window_state(0, 6, 7, 0, 2);  // WSTATE.NORMAL = 2
    r.cpu.set_cwp(3);
    r.exec({SAVEi(sp, sp, -192)}, 1);
    CHECK_EQ(r.cpu.tl(), 1u);
    CHECK_EQ(r.cpu.trap_type(1), tt::kSpillNormal + 8u);
    CHECK_EQ(r.cpu.pc(), Rig::kTba + ((tt::kSpillNormal + 8u) << 5));
    CHECK_EQ(r.cpu.tpc(1), Rig::kCode);
    CHECK_EQ(r.cpu.cwp(), 5u);                          // the window to spill: CWP + 2
    CHECK_EQ(r.cpu.tstate(1) & 0x1F, 3u);               // the original CWP
    CHECK(r.cpu.pstate_reg() & pstate::AG);
}

TEST(cpu_restore_with_nothing_to_restore_takes_a_fill_trap) {
    Rig r;
    r.cpu.set_window_state(6, 0, 7, 0, 0);
    r.cpu.set_cwp(0);
    r.exec({RESTORE(g0, g0, g0)}, 1);
    CHECK_EQ(r.cpu.trap_type(1), tt::kFillNormal);
    CHECK_EQ(r.cpu.cwp(), 7u);  // CWP - 1
}

TEST(cpu_clean_window_trap_runs_in_the_new_window) {
    Rig r;
    r.cpu.set_window_state(6, 0, 0, 0, 0);
    r.cpu.set_cwp(2);
    r.exec({SAVE(g0, g0, g0)}, 1);
    CHECK_EQ(r.cpu.trap_type(1), tt::kCleanWindow);
    CHECK_EQ(r.cpu.cwp(), 3u);
}

TEST(cpu_trap_instruction_and_done_retry) {
    Rig r;
    // Handler for trap 0x105 at TBA + 0x105 * 32: set %g1 then DONE.
    r.put(Rig::kTba + (0x105 << 5), {MOVi(g1, 0x55), DONE});
    r.cpu.set_ccr(0);
    r.exec({Tcc(cA, kIcc, 5), MOVi(g2, 1)}, 4);
    CHECK_EQ(r.cpu.reg(g2), 1u);
    CHECK_EQ(r.cpu.tl(), 0u);
    CHECK(!(r.cpu.pstate_reg() & pstate::AG));
    CHECK_EQ(r.cpu.pc(), Rig::kCode + 8);
    // The handler ran in the alternate globals; %g1 of the normal set is untouched.
    CHECK_EQ(r.cpu.reg(g1), 0u);
    // RETRY re-executes the trapping instruction: a not-taken trap on the second try.
    r.put(Rig::kTba + (0x106 << 5), {WRPRi(prTSTATE, g0, 0x4 << 8 /* PSTATE.PRIV, CCR 0 */), RETRY});
    r.cpu.set_ccr(0x04);  // icc.Z
    r.exec({Tcc(cE, kIcc, 6), MOVi(g3, 3)}, 5);
    CHECK_EQ(r.cpu.reg(g3), 3u);
    CHECK_EQ(r.cpu.trap_type(1), 0x106u);
    CHECK_EQ(r.cpu.tl(), 0u);
}

TEST(cpu_traps_at_tl_above_zero_use_the_upper_half_of_the_table) {
    Rig r;
    r.cpu.set_tl(1);
    r.exec({Tcc(cA, kIcc, 1)}, 1);
    CHECK_EQ(r.cpu.tl(), 2u);
    CHECK_EQ(r.cpu.pc(), Rig::kTba + 0x4000 + (0x101u << 5));
}

TEST(cpu_divide_uses_y_and_saturates) {
    Rig r;
    r.cpu.set_reg(g1, 0);
    r.exec({WRASRi(0, g0, 1), MOVi(g1, 0), MOVi(g2, 2), UDIV(g3, g1, g2), UDIVcci(g4, g1, 1)});
    CHECK_EQ(r.cpu.reg(g3), 0x80000000u);  // 0x1_0000_0000 / 2
    CHECK_EQ(r.cpu.reg(g4), 0xFFFFFFFFu);  // overflow saturates
    CHECK(r.cpu.ccr() & 2);
    r.exec({WRASRi(0, g0, -1), MOVi(g1, -8), MOVi(g2, 2), SDIV(g3, g1, g2)});
    CHECK_EQ(r.cpu.reg(g3), ~u64{3});      // -8 / 2, sign-extended
    r.exec({UDIVX(g3, g1, g0)}, 1);
    CHECK_EQ(r.cpu.trap_type(1), tt::kDivisionByZero);
}

TEST(cpu_multiply_writes_y) {
    Rig r;
    r.cpu.set_reg(g1, 0xFFFFFFFF);
    r.exec({UMUL(g2, g1, g1), RDASR(g3, 0), SMULi(g4, g1, 2)});
    CHECK_EQ(r.cpu.reg(g2), 0xFFFFFFFE00000001ull);
    CHECK_EQ(r.cpu.reg(g3), 0xFFFFFFFEu);
    CHECK_EQ(r.cpu.reg(g4), ~u64{1});  // -1 * 2
}

TEST(cpu_loads_and_stores_of_every_width) {
    Rig r;
    r.cpu.set_reg(g1, 0x2000);
    r.cpu.set_reg(g2, 0x8899AABBCCDDEEFFull);
    r.exec({STX(g2, g1, 0), LDUB(g3, g1, 0), LDSB(g4, g1, 0), LDUH(g5, g1, 2), LDSW(g6, g1, 4), LDD(o0, g1, 0),
            STD(o0, g1, 8), LDX(o2, g1, 8)});
    CHECK_EQ(r.cpu.reg(g3), 0x88u);
    CHECK_EQ(r.cpu.reg(g4), 0xFFFFFFFFFFFFFF88ull);
    CHECK_EQ(r.cpu.reg(g5), 0xAABBu);
    CHECK_EQ(r.cpu.reg(g6), 0xFFFFFFFFCCDDEEFFull);
    CHECK_EQ(r.cpu.reg(o0), 0x8899AABBu);
    CHECK_EQ(r.cpu.reg(o1), 0xCCDDEEFFu);
    CHECK_EQ(r.cpu.reg(o2), 0x8899AABBCCDDEEFFull);
    r.exec({LDUW(g3, g1, 2)}, 1);
    CHECK_EQ(r.cpu.trap_type(1), tt::kMemAddressNotAligned);
}

TEST(cpu_little_endian_asi_and_atomics) {
    Rig r;
    r.cpu.set_reg(g1, 0x2000);
    r.cpu.set_reg(g2, 0x11223344);
    r.exec({STWA(g2, g1, g0, asi::PL), LDUB(g3, g1, 0), LDUWA(g4, g1, g0, asi::PL)});
    CHECK_EQ(r.cpu.reg(g3), 0x44u);
    CHECK_EQ(r.cpu.reg(g4), 0x11223344u);
    r.cpu.set_reg(g5, 0x11223344);  // matches: swapped
    r.cpu.set_reg(g6, 0xCAFE);
    r.exec({STW(g2, g1, 8), ADDi(g7, g1, 8), CASA(g6, g7, g5, asi::P), LDUW(o0, g1, 8)});
    CHECK_EQ(r.cpu.reg(g6), 0x11223344u);
    CHECK_EQ(r.cpu.reg(o0), 0xCAFEu);
    r.exec({LDSTUB(o1, g1, 8), LDUB(o2, g1, 8)});
    CHECK_EQ(r.cpu.reg(o1), 0x00u);
    CHECK_EQ(r.cpu.reg(o2), 0xFFu);
}

TEST(cpu_restricted_asi_needs_privilege) {
    Rig r;
    r.cpu.set_pstate(pstate::PEF);
    r.cpu.set_reg(g1, 0x2000);
    r.exec({LDXA(g2, g1, g0, asi::N)}, 1);
    CHECK_EQ(r.cpu.trap_type(1), tt::kPrivilegedAction);
}

TEST(cpu_dmmu_translates_through_the_tlb_and_misses_to_the_mmu_globals) {
    Rig r;
    // Map virtual 0x40000000 (context 0, 8 KiB) to physical 0x4000, writable.
    const u64 tte_data = tte::V | 0x4000 | tte::CP | tte::P | tte::W;
    r.cpu.set_reg(g1, 0x40000000);  // tag access: VA | context 0
    r.cpu.set_reg(g2, tte_data);
    r.cpu.set_reg(g3, 0x30);
    r.cpu.set_reg(g4, 8);  // LSU control: DM
    r.cpu.set_reg(g5, 0xABCD);
    r.exec({STXA(g1, g3, g0, asi::DMMU), STXA(g2, g0, g0, asi::DTLB_DATA_IN), STXA(g4, g0, g0, asi::LSU_CONTROL),
            STX(g5, g1, 0x10)});
    CHECK_EQ(r.load64(0x4010), 0xABCDu);
    // A miss: virtual 0x50000000 is unmapped.
    r.cpu.set_reg(g6, 0x50000000);
    r.exec({LDX(g7, g6, 0x18)}, 1);
    CHECK_EQ(r.cpu.trap_type(1), tt::kFastDataAccessMmuMiss);
    CHECK(r.cpu.pstate_reg() & pstate::MG);
    CHECK_EQ(r.cpu.dmmu().tag_access, 0x50000000u);
}

TEST(cpu_dmmu_write_to_a_read_only_page_is_a_protection_trap) {
    Rig r;
    r.cpu.set_reg(g1, 0x40000000);
    r.cpu.set_reg(g2, tte::V | 0x4000 | tte::CP);
    r.cpu.set_reg(g3, 0x30);
    r.cpu.set_reg(g4, 8);
    r.exec({STXA(g1, g3, g0, asi::DMMU), STXA(g2, g0, g0, asi::DTLB_DATA_IN), STXA(g4, g0, g0, asi::LSU_CONTROL),
            STX(g0, g1, 0)});
    CHECK_EQ(r.cpu.trap_type(1), tt::kFastDataAccessProtection);
}

TEST(cpu_tsb_pointers_follow_the_tag_access_register) {
    Rig r;
    // TSB at 0x10_0000, size 0 (512 entries), not split; tag access VA 0x12346000.
    r.cpu.set_reg(g1, 0x100000);
    r.cpu.set_reg(g2, 0x28);
    r.cpu.set_reg(g3, 0x12346000);
    r.cpu.set_reg(g4, 0x30);
    r.exec({STXA(g1, g2, g0, asi::DMMU), STXA(g3, g4, g0, asi::DMMU), LDXA(g5, g0, g0, asi::DMMU_TSB_8K_PTR),
            LDXA(g6, g0, g0, asi::DMMU_TSB_64K_PTR)});
    CHECK_EQ(r.cpu.reg(g5), 0x100000u | (((0x12346000u >> 13) & 511) << 4));
    CHECK_EQ(r.cpu.reg(g6), 0x100000u | (((0x12346000u >> 16) & 511) << 4));
}

TEST(cpu_softint_raises_an_interrupt_above_pil) {
    Rig r;
    r.cpu.set_reg(g1, 1u << 5);
    r.exec({WRASR(20, g1, g0)});  // SET_SOFTINT level 5
    CHECK_EQ(r.cpu.softint(), 1u << 5);
    r.cpu.set_pstate(pstate::PRIV | pstate::PEF | pstate::IE);
    r.put(Rig::kCode, {NOP});
    r.run_at(Rig::kCode, 1);
    CHECK_EQ(r.cpu.trap_type(1), tt::kInterruptLevel1 + 4u);
    CHECK_EQ(r.cpu.tpc(1), Rig::kCode);
    CHECK(!(r.cpu.pstate_reg() & pstate::IE));
}

TEST(cpu_interrupt_vector_uses_the_interrupt_globals) {
    Rig r;
    r.cpu.set_pstate(pstate::PRIV | pstate::PEF | pstate::IE);
    CHECK(r.cpu.deliver_interrupt_vector(0x7E1, 0, 0));
    CHECK(!r.cpu.deliver_interrupt_vector(1, 0, 0));
    r.put(Rig::kCode, {NOP});
    r.run_at(Rig::kCode, 1);
    CHECK_EQ(r.cpu.trap_type(1), tt::kInterruptVector);
    CHECK(r.cpu.pstate_reg() & pstate::IG);
    r.put(Rig::kTba + (tt::kInterruptVector << 5), {MOVi(g2, 0x40), LDXA(g1, g2, g0, asi::UDB_INTR_R)});
    r.cpu.run(2);
    CHECK_EQ(r.cpu.reg(g1), 0x7E1u);
}

TEST(cpu_tick_compare_sets_softint_tick_int) {
    Rig r;
    r.exec({WRPR(prTICK, g0, g0), WRASRi(23, g0, 10)});  // TICK = 0, TICK_CMPR = 10
    for (int i = 0; i < 12; ++i) {
        r.put(Rig::kCode, {NOP});
        r.run_at(Rig::kCode, 1);
    }
    CHECK(r.cpu.softint() & 1);
}

TEST(cpu_popc_and_mulx) {
    Rig r;
    r.cpu.set_reg(g1, 0xF0F0);
    r.exec({POPC(g2, g1), MULXi(g3, g1, -1)});
    CHECK_EQ(r.cpu.reg(g2), 8u);
    CHECK_EQ(r.cpu.reg(g3), static_cast<u64>(-0xF0F0ll));
}

TEST(bus_maps_high_physical_addresses) {
    Bus bus;
    std::vector<u8> rom(Bus::kPageSize * 2);
    bus.map_ram(0x1FFF0000000ull, rom.size(), rom.data(), false);
    store_be<u32>(rom.data() + 0x10004, 0xDEADBEEF);
    u32 v = 0;
    CHECK(bus.read(0x1FFF0010004ull, v));
    CHECK_EQ(v, 0xDEADBEEFu);
    CHECK(!bus.read(0x1FFE0000000ull, v));
    CHECK(!bus.read(0x20000000000ull, v));  // beyond the 41-bit space
    CHECK(bus.write(0x1FFF0010004ull, u32{1}));  // ROM writes are ignored
    CHECK(bus.read(0x1FFF0010004ull, v));
    CHECK_EQ(v, 0xDEADBEEFu);
}

TEST(disasm_common_forms) {
    CHECK(disassemble(NOP, 0) == "nop");
    CHECK(disassemble(ADDi(o0, o1, -4), 0) == "add %o1, -0x4, %o0");
    CHECK(disassemble(Bicc(cNE, 4, true), 0x1000) == "bne,a 0x1010");
    CHECK(disassemble(CALL(-1), 0x1000) == "call 0xffc");
    CHECK(disassemble(LDX(g1, sp, 0x7ff), 0) == "ldx [%sp + 0x7ff], %g1");
    CHECK(disassemble(STXA(g0, g1, g0, 0x45), 0) == "stxa %g0, [%g1] 0x45");
    CHECK(disassemble(WRPR(prPSTATE, g1, g0), 0) == "wrpr %g1, %g0, %pstate");
    CHECK(disassemble(RETRY, 0) == "retry");
    CHECK(disassemble(JMPLi(g0, i7, 8), 0) == "ret");
}
