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

TEST(cpu_trap_at_maxtl_takes_a_watchdog_reset_not_a_halt) {
    Rig r;
    r.cpu.set_tl(kMaxTl);
    r.cpu.set_pstate(pstate::PRIV | pstate::PEF | pstate::IE | pstate::MM);
    r.exec({Tcc(cA, kIcc, 1)}, 1);  // error_state, left at once by a watchdog reset
    CHECK(!r.cpu.halted());
    CHECK_EQ(r.cpu.tl(), kMaxTl);
    CHECK_EQ(r.cpu.trap_type(kMaxTl), 0x101u);  // the trap that caused it
    CHECK_EQ(r.cpu.tpc(kMaxTl), Rig::kCode);
    CHECK_EQ(r.cpu.pc(), kRstvAddr + 0x40);
    CHECK_EQ(r.cpu.pstate_reg(), pstate::RED | pstate::PEF | pstate::PRIV | pstate::AG);  // IE and MM clear
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

namespace {
// D-MMU helpers for the tests below: map an 8 KiB page in context 0 and
// turn the D-MMU on.
void dmmu_map(Rig& r, u64 va, u64 pa) {
    r.cpu.set_reg(g1, va);
    r.cpu.set_reg(g2, tte::V | tte::CP | tte::P | tte::W | pa);
    r.cpu.set_reg(g3, 0x30);
    r.exec({STXA(g1, g3, g0, asi::DMMU), STXA(g2, g0, g0, asi::DTLB_DATA_IN)});
}
void dmmu_on(Rig& r) {
    r.cpu.set_reg(g4, 8);
    r.exec({STXA(g4, g0, g0, asi::LSU_CONTROL)});
}
}  // namespace

TEST(cpu_dmmu_demap_drops_translations_already_used) {
    Rig r;
    dmmu_map(r, 0x40000000, 0x4000);
    dmmu_on(r);
    r.cpu.set_reg(g5, 0x40000000);
    r.exec({LDX(g6, g5, 0), LDX(g6, g5, 8)});  // (remembered now)
    r.exec({STXA(g0, g5, g0, asi::DMMU_DEMAP)});
    r.exec({LDX(g6, g5, 0)}, 1);
    CHECK_EQ(r.cpu.trap_type(1), tt::kFastDataAccessMmuMiss);
}

TEST(cpu_dtlb_entry_replaced_stops_translating_even_after_use) {
    // Page A fills entry 0; 63 more fill the rest; all are used; the next
    // insert replaces entry 0 (no unused ones left), so A must then miss.
    Rig r;
    dmmu_map(r, 0x40000000, 0x4000);
    for (u64 k = 1; k < 64; ++k) dmmu_map(r, 0x50000000 + 0x2000 * k, 0x6000);
    dmmu_on(r);
    r.cpu.set_reg(g5, 0x40000000);
    r.exec({LDX(g6, g5, 0)});
    for (u64 k = 1; k < 64; ++k) {
        r.cpu.set_reg(g5, 0x50000000 + 0x2000 * k);
        r.exec({LDX(g6, g5, 0)});
    }
    r.cpu.set_reg(g5, 0x40000000);
    r.exec({LDX(g6, g5, 8)});  // A again (remembered)
    dmmu_map(r, 0x70000000, 0x8000);
    CHECK_EQ(r.cpu.dmmu().tlb[0].tag, 0x70000000u);  // (entry 0 replaced)
    r.cpu.set_reg(g5, 0x40000000);
    r.exec({LDX(g6, g5, 0)}, 1);
    CHECK_EQ(r.cpu.trap_type(1), tt::kFastDataAccessMmuMiss);
}

TEST(cpu_dtlb_translations_already_used_hold_only_in_their_context) {
    // A page of context 5, used: it translates after the primary context
    // goes to 6 and back to 5, and misses with 6.
    Rig r;
    r.cpu.set_reg(g1, 0x40000000 | 5);
    r.cpu.set_reg(g2, tte::V | tte::CP | tte::P | tte::W | 0x4000);
    r.cpu.set_reg(g3, 0x30);
    r.cpu.set_reg(o0, 0x08);
    r.cpu.set_reg(o1, 5);
    r.cpu.set_reg(o2, 6);
    r.exec({STXA(g1, g3, g0, asi::DMMU), STXA(g2, g0, g0, asi::DTLB_DATA_IN), STXA(o1, o0, g0, asi::DMMU)});
    dmmu_on(r);
    r.cpu.set_reg(g5, 0x40000000);
    r.exec({LDX(g6, g5, 0), LDX(g6, g5, 8)});  // (remembered now)
    r.exec({STXA(o2, o0, g0, asi::DMMU), STXA(o1, o0, g0, asi::DMMU), LDX(g6, g5, 0)});
    CHECK_EQ(r.cpu.tl(), 0u);  // (6 and back to 5: translates)
    r.exec({STXA(o2, o0, g0, asi::DMMU)});
    r.exec({LDX(g6, g5, 0)}, 1);
    CHECK_EQ(r.cpu.trap_type(1), tt::kFastDataAccessMmuMiss);
}

TEST(cpu_dtlb_contexts_switched_back_and_forth_keep_their_own_translations) {
    // One virtual page, mapped to 0x4000 in context 5 and to 0x6000 in
    // context 6, read in each in turn by a loop run often enough to be
    // translated: each read gets its own context's page.
    Rig r;
    r.bus.write(0x4000, u64{0x55});
    r.bus.write(0x6000, u64{0x66});
    for (const auto& [ctx, pa] : {std::pair<u64, u64>{5, 0x4000}, {6, 0x6000}}) {
        r.cpu.set_reg(g1, 0x40000000 | ctx);
        r.cpu.set_reg(g2, tte::V | tte::CP | tte::P | tte::W | pa);
        r.cpu.set_reg(g3, 0x30);
        r.exec({STXA(g1, g3, g0, asi::DMMU), STXA(g2, g0, g0, asi::DTLB_DATA_IN)});
    }
    dmmu_on(r);
    r.cpu.set_reg(o0, 0x08);  // the primary context register
    r.cpu.set_reg(o1, 5);
    r.cpu.set_reg(o2, 6);
    r.cpu.set_reg(g5, 0x40000000);
    r.put(Rig::kCode, {MOVi(g1, 200), STXA(o1, o0, g0, asi::DMMU), LDX(g3, g5, 0), ADD(g6, g6, g3), STXA(o2, o0, g0, asi::DMMU),
                       LDX(g3, g5, 0), ADD(g7, g7, g3), SUBcci(g1, g1, 1), BPcc(cNE, kXcc, -7), NOP, Bicc(cA, 0), NOP});
    r.run_at(Rig::kCode, 200 * 9 + 20);
    CHECK_EQ(r.cpu.reg(g1), 0u);
    CHECK_EQ(r.cpu.reg(g6), 200u * 0x55);
    CHECK_EQ(r.cpu.reg(g7), 200u * 0x66);
}

TEST(cpu_dtlb_entry_written_by_data_access_replaces_the_one_there) {
    // Page A in entry 0, used; a TLB data access write puts page B in entry
    // 0: A misses and B translates.
    Rig r;
    dmmu_map(r, 0x40000000, 0x4000);
    dmmu_on(r);
    r.cpu.set_reg(g5, 0x40000000);
    r.exec({LDX(g6, g5, 0), LDX(g6, g5, 8)});  // (remembered now)
    r.cpu.set_reg(g1, 0x60000000);
    r.cpu.set_reg(g2, tte::V | tte::CP | tte::P | tte::W | 0x6000);
    r.cpu.set_reg(g3, 0x30);
    r.cpu.set_reg(g7, 0xBEEF);
    r.exec({STXA(g1, g3, g0, asi::DMMU), STXA(g2, g0, g0, asi::DTLB_DATA_ACCESS), STX(g7, g1, 0)});
    CHECK_EQ(r.load64(0x6000), 0xBEEFu);
    r.exec({LDX(g6, g5, 0)}, 1);
    CHECK_EQ(r.cpu.trap_type(1), tt::kFastDataAccessMmuMiss);
}

TEST(cpu_dtlb_used_bits_count_accesses_after_they_are_cleared) {
    // All 64 entries used; the next insert clears the used bits and takes
    // entry 0. Entries 0 and 1 are then used (1 a page used before the
    // bits were cleared): the insert after must take entry 2, not 1.
    Rig r;
    for (u64 k = 0; k < 64; ++k) dmmu_map(r, 0x50000000 + 0x2000 * k, 0x6000);
    dmmu_on(r);
    for (u64 k = 0; k < 64; ++k) {
        r.cpu.set_reg(g5, 0x50000000 + 0x2000 * k);
        r.exec({LDX(g6, g5, 0)});
    }
    dmmu_map(r, 0x70000000, 0x8000);
    CHECK_EQ(r.cpu.dmmu().tlb[0].tag, 0x70000000u);
    r.cpu.set_reg(g5, 0x70000000);
    r.exec({LDX(g6, g5, 0)});
    r.cpu.set_reg(g5, 0x50002000);
    r.exec({LDX(g6, g5, 0)});
    dmmu_map(r, 0x72000000, 0x8000);
    CHECK_EQ(r.cpu.dmmu().tlb[2].tag, 0x72000000u);
    CHECK_EQ(r.cpu.dmmu().tlb[1].tag, 0x50002000u);
}

TEST(cpu_dtlb_used_bits_count_accesses_by_translated_code) {
    // As above, with page 1 read after the bits are cleared only by a loop
    // translated before (reads made through its fast translations count):
    // the insert after must still take entry 2, not 1.
    constexpr u64 kLoop = 0x3000;  // (a code page the tests' exec() leaves alone)
    Rig r;
    for (u64 k = 0; k < 64; ++k) dmmu_map(r, 0x50000000 + 0x2000 * k, 0x6000);
    dmmu_on(r);
    // (Entered at the loop itself, whose block is translated: the reads are
    // all its own. The sum stored makes it no polling loop, which would be
    // run at once outside the translation.)
    r.put(kLoop, {LDX(g6, g5, 0), ADD(g7, g7, g6), STX(g7, g5, 8), SUBcci(g1, g1, 1), BPcc(cNE, kXcc, -4), NOP, Bicc(cA, 0), NOP});
    r.cpu.set_reg(g5, 0x50002000);
    r.cpu.set_reg(g1, 200);
    r.run_at(kLoop, 200 * 6 + 20);  // (translated now)
    for (u64 k = 0; k < 64; ++k) {
        r.cpu.set_reg(g5, 0x50000000 + 0x2000 * k);
        r.exec({LDX(g6, g5, 0)});
    }
    dmmu_map(r, 0x70000000, 0x8000);
    CHECK_EQ(r.cpu.dmmu().tlb[0].tag, 0x70000000u);
    r.cpu.set_reg(g5, 0x70000000);
    r.exec({LDX(g6, g5, 0)});  // (entry 0 used)
    r.cpu.set_reg(g5, 0x50002000);
    r.cpu.set_reg(g1, 200);
    r.run_at(kLoop, 200 * 6 + 20);
    dmmu_map(r, 0x72000000, 0x8000);
    CHECK_EQ(r.cpu.dmmu().tlb[2].tag, 0x72000000u);
    CHECK_EQ(r.cpu.dmmu().tlb[1].tag, 0x50002000u);
}

TEST(cpu_dtlb_entry_replaced_stops_translating_in_translated_code) {
    // As above, with A read by a loop run often enough to be translated
    // (through the fast translations), then by the same loop once A's
    // entry has been replaced: it must miss.
    Rig r;
    dmmu_map(r, 0x40000000, 0x4000);
    for (u64 k = 1; k < 64; ++k) dmmu_map(r, 0x50000000 + 0x2000 * k, 0x6000);
    dmmu_on(r);
    for (u64 k = 1; k < 64; ++k) {
        r.cpu.set_reg(g5, 0x50000000 + 0x2000 * k);
        r.exec({LDX(g6, g5, 0)});
    }
    r.cpu.set_reg(g5, 0x40000000);
    r.cpu.set_reg(g1, 40);
    const std::initializer_list<u32> loop = {LDX(g6, g5, 0), SUBcci(g1, g1, 1), BPcc(cNE, kXcc, -2), NOP};
    r.exec(loop, 4 * 40);
    if (UltraSparc::jit_available()) CHECK(r.cpu.jit_compiles() > 0);
    r.cpu.set_reg(g1, 0x70000000);
    r.cpu.set_reg(g2, tte::V | tte::CP | tte::P | tte::W | 0x8000);
    r.cpu.set_reg(g3, 0x30);
    r.put(0x600, {STXA(g1, g3, g0, asi::DMMU), STXA(g2, g0, g0, asi::DTLB_DATA_IN)});  // (away from the loop)
    r.run_at(0x600, 2);
    CHECK_EQ(r.cpu.dmmu().tlb[0].tag, 0x70000000u);
    r.cpu.set_reg(g1, 40);
    r.run_at(Rig::kCode, 4);  // one pass of the loop as translated
    CHECK(r.cpu.tl() >= 1);  // (and on into the empty trap table)
    CHECK_EQ(r.cpu.trap_type(1), tt::kFastDataAccessMmuMiss);
    CHECK_EQ(r.cpu.tpc(1), Rig::kCode);
}

TEST(cpu_dmmu_demap_drops_translations_in_translated_code) {
    Rig r;
    dmmu_map(r, 0x40000000, 0x4000);
    dmmu_map(r, 0x50002000, 0x6000);  // (another fast-table slot)
    dmmu_on(r);
    r.cpu.set_reg(g5, 0x40000000);
    r.cpu.set_reg(g4, 0x50002000);
    r.cpu.set_reg(g1, 40);
    const std::initializer_list<u32> loop = {LDX(g6, g5, 0), LDX(g7, g4, 0), SUBcci(g1, g1, 1), BPcc(cNE, kXcc, -3), NOP};
    r.exec(loop, 5 * 40);
    if (UltraSparc::jit_available()) CHECK(r.cpu.jit_compiles() > 0);
    r.put(0x600, {STXA(g0, g5, g0, asi::DMMU_DEMAP)});  // A only: B stays (code away from the loop's)
    r.run_at(0x600, 1);
    r.cpu.set_reg(g1, 40);
    r.run_at(Rig::kCode, 5);  // one pass of the loop as translated
    CHECK(r.cpu.tl() >= 1);  // (and on into the empty trap table)
    CHECK_EQ(r.cpu.trap_type(1), tt::kFastDataAccessMmuMiss);
    CHECK_EQ(r.cpu.tpc(1), Rig::kCode);
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

TEST(cpu_watchpoints_trap_before_a_watched_byte_is_touched) {
    // PA watchpoint on byte 4 of the doubleword at 0x20000, for reads (MMU off).
    {
        Rig r;
        r.ram[0x20004] = 0x5A;
        r.cpu.set_reg(g1, 0x20000);
        r.cpu.set_reg(g2, 0x40);
        r.cpu.set_reg(g3, (u64{0x10} << 33) | (1u << 24));  // PM byte 4, PR
        r.exec({STXA(g1, g2, g0, asi::DMMU), STXA(g3, g0, g0, asi::LSU_CONTROL), LDUW(g4, g1, 0), LDUB(g5, g1, 5),
                STB(g0, g1, 4)});
        CHECK_EQ(r.cpu.reg(g4), 0u);  // bytes 0-3: not watched
        CHECK_EQ(r.cpu.reg(g5), 0u);
        CHECK_EQ(r.cpu.tl(), 0u);     // (a store, with only reads watched)
        r.exec({LDUH(g6, g1, 4)}, 1);
        CHECK_EQ(r.cpu.tl(), 1u);
        CHECK_EQ(r.cpu.trap_type(1), tt::kPaWatchpoint);
        CHECK_EQ(r.cpu.reg(g6), 0u);  // before the access
    }
    // VA watchpoint (D-MMU on) on writes to byte 0 at VA 0x40000008: it
    // comes before the MMU's own traps (the page is read-only).
    {
        Rig r;
        r.cpu.set_reg(g1, 0x40000000);
        r.cpu.set_reg(g2, tte::V | 0x4000 | tte::CP);
        r.cpu.set_reg(g3, 0x30);
        r.cpu.set_reg(g4, 0x40000008);
        r.cpu.set_reg(g5, 0x38);
        r.cpu.set_reg(g6, 8 | (u64{0x01} << 25) | (1u << 21));  // DM, VM byte 0, VW
        r.exec({STXA(g1, g3, g0, asi::DMMU), STXA(g2, g0, g0, asi::DTLB_DATA_IN), STXA(g4, g5, g0, asi::DMMU),
                STXA(g6, g0, g0, asi::LSU_CONTROL), LDX(g7, g1, 8)});
        CHECK_EQ(r.cpu.tl(), 0u);  // reads are not watched
        r.exec({STB(g0, g1, 8)}, 1);
        CHECK_EQ(r.cpu.trap_type(1), tt::kVaWatchpoint);
        // A trap that enters RED_state clears the LSU control register.
        CHECK(r.cpu.lsu_control() != 0);
    }
}

TEST(cpu_performance_counters_count_instructions_and_cycles_in_the_traced_modes) {
    Rig r;
    // PIC.D0 instructions, D1 cycles, privileged mode traced (ST).
    r.cpu.set_reg(g1, (1u << 4) | 2);
    r.exec({WRASR(16, g1, g0), WRASR(17, g0, g0), NOP, NOP, NOP, NOP, RDASR(g2, 17)});
    CHECK_EQ(r.cpu.reg(g2) & 0xFFFFFFFF, 5u);  // the write to PIC (which completes after it) and the four NOPs
    CHECK((r.cpu.reg(g2) >> 32) >= 5u);
    // Only user mode traced (UT): nothing counts here.
    r.cpu.set_reg(g1, (1u << 4) | 4);
    r.exec({WRASR(16, g1, g0), WRASR(17, g0, g0), NOP, NOP, RDASR(g2, 17)});
    CHECK_EQ(r.cpu.reg(g2), 0u);
    // PCR is privileged: privileged_opcode from user mode.
    r.cpu.set_pstate(pstate::PEF);
    r.exec({RDASR(g3, 16)}, 1);
    CHECK_EQ(r.cpu.trap_type(1), tt::kPrivilegedOpcode);
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

TEST(cpu_tick_compare_survives_a_change_of_charge_per_step) {
    // The charge changes between every two instructions (as the pacer may
    // between slices), including just as TICK reaches TICK_CMPR: the
    // interrupt still comes.
    Rig r;
    r.exec({WRPR(prTICK, g0, g0), WRASRi(23, g0, 10)});  // TICK = 0, TICK_CMPR = 10
    for (int i = 0; i < 12; ++i) {
        r.cpu.set_cycles_per_step(i % 2 ? 1.0 : 1.5);
        r.put(Rig::kCode, {NOP});
        r.run_at(Rig::kCode, 1);
    }
    CHECK(r.cpu.softint() & 1);
}

TEST(cpu_guest_time_follows_the_charge_per_step) {
    // At a quarter of a cycle a step, TICK advances a quarter as fast as
    // the steps; at three cycles, three times as fast; the change keeps
    // the time already counted.
    Rig r;
    r.cpu.set_cycles_per_step(0.25);
    const u64 t0 = r.cpu.cycles();
    std::vector<u32> nops(400, NOP);
    for (std::size_t k = 0; k < nops.size(); ++k) r.bus.write(Rig::kCode + 4 * k, nops[k]);
    r.run_at(Rig::kCode, 400);
    CHECK_EQ(r.cpu.cycles() - t0, 100u);
    r.cpu.set_cycles_per_step(3.0);
    r.run_at(Rig::kCode, 100);
    CHECK_EQ(r.cpu.cycles() - t0, 400u);
    r.exec({RDPR(g1, prTICK)});  // (TICK as the RDPR's step charged it)
    const u64 now = r.cpu.tick() & ~(u64{1} << 63), read = r.cpu.reg(g1) & ~(u64{1} << 63);
    CHECK(read <= now && now - read <= 3);
}

TEST(cpu_tick_compare_matches_guest_time_not_steps) {
    // TICK_CMPR 100 ahead at a quarter of a cycle a step: the interrupt
    // comes after 400 steps, not 100.
    Rig r;
    r.cpu.set_cycles_per_step(0.25);
    r.exec({WRPR(prTICK, g0, g0), WRASRi(23, g0, 100)});  // TICK = 0, TICK_CMPR = 100
    std::vector<u32> nops(500, NOP);
    for (std::size_t k = 0; k < nops.size(); ++k) r.bus.write(Rig::kCode + 4 * k, nops[k]);
    r.run_at(Rig::kCode, 380);
    CHECK(!(r.cpu.softint() & 1));
    r.run_at(Rig::kCode, 40);
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

namespace {

// A device that answers each read with its offset + 0x1100 and counts the
// reads and writes it sees.
struct CountingDevice : Device {
    const char* name() const override { return "counting"; }
    bool read(u64 offset, unsigned, u64& value) override {
        ++reads;
        value = offset + 0x1100;
        return true;
    }
    bool write(u64 offset, unsigned, u64 value) override {
        ++writes;
        last = offset << 32 | (value & 0xFFFFFFFF);
        return true;
    }
    unsigned reads = 0, writes = 0;
    u64 last = 0;
};

constexpr u64 kDevPa = 0x100000000;  // (above the rig's RAM)

void dmmu_map_tte(Rig& r, u64 va, u64 data) {
    r.cpu.set_reg(g1, va);
    r.cpu.set_reg(g2, data);
    r.cpu.set_reg(g3, 0x30);
    r.exec({STXA(g1, g3, g0, asi::DMMU), STXA(g2, g0, g0, asi::DTLB_DATA_IN)});
}

}  // namespace

// Loads and stores that reach a device keep its translation, and use it
// only where translating again would give the same: the values and the
// device's accesses as before, a demap, a read-only page, a no-fault load
// of a page with side effects and user code on a privileged page all
// still fault.
TEST(cpu_device_accesses_reuse_their_translation_only_where_it_holds) {
    Rig r;
    CountingDevice dev;
    r.bus.map_device(kDevPa, Bus::kPageSize, &dev);
    dmmu_map_tte(r, 0x40000000, tte::V | tte::E | tte::P | tte::W | kDevPa);
    dmmu_map_tte(r, 0x40002000, tte::V | tte::E | tte::P | (kDevPa + 0x2000));  // read-only
    dmmu_on(r);
    r.cpu.set_reg(g5, 0x40000000);
    r.cpu.set_reg(g6, 0x40002000);
    r.cpu.set_reg(g7, 0x55);
    for (int k = 0; k < 3; ++k) r.exec({LDUW(o1, g5, 8), STW(g7, g5, 12), LDUW(o2, g6, 4)});
    CHECK_EQ(r.cpu.reg(o1), 0x1108u);
    CHECK_EQ(r.cpu.reg(o2), 0x3104u);
    CHECK_EQ(dev.reads, 6u);
    CHECK_EQ(dev.writes, 3u);
    CHECK_EQ(dev.last, (u64{12} << 32) | 0x55);
    // An unmapped page that shares the entry's slot.
    r.cpu.set_reg(o4, 0x40400000);
    r.exec({LDUW(o1, o4, 8)}, 1);
    CHECK_EQ(r.cpu.trap_type(1), tt::kFastDataAccessMmuMiss);
    CHECK_EQ(dev.reads, 6u);
    r.cpu.set_tl(0);
    r.cpu.set_pstate(pstate::PRIV | pstate::PEF);
    // A store to the read-only page.
    r.exec({STW(g7, g6, 4)}, 1);
    CHECK_EQ(r.cpu.trap_type(1), tt::kFastDataAccessProtection);
    CHECK_EQ(dev.writes, 3u);
    // A no-fault load of a page with side effects. (After each trap, back
    // to TL 0 and the normal globals.)
    auto recover = [&](u16 pstate) {
        r.cpu.set_tl(0);
        r.cpu.set_pstate(pstate);
    };
    recover(pstate::PRIV | pstate::PEF);
    r.exec({LDUWA(o3, g5, g0, asi::PNF)}, 1);
    CHECK_EQ(r.cpu.trap_type(1), tt::kDataAccessException);
    CHECK_EQ(dev.reads, 6u);
    // User code on the privileged page.
    recover(pstate::PEF);
    r.exec({LDUW(o1, g5, 8)}, 1);
    CHECK_EQ(r.cpu.trap_type(1), tt::kDataAccessException);
    CHECK_EQ(dev.reads, 6u);
    // A demap.
    recover(pstate::PRIV | pstate::PEF);
    r.exec({STXA(g0, g5, g0, asi::DMMU_DEMAP)});
    r.exec({LDUW(o1, g5, 8)}, 1);
    CHECK_EQ(r.cpu.trap_type(1), tt::kFastDataAccessMmuMiss);
    CHECK_EQ(dev.reads, 6u);
    // Through the physical ASIs, big- and little-endian.
    recover(pstate::PRIV | pstate::PEF);
    r.cpu.set_reg(g5, kDevPa);
    for (int k = 0; k < 3; ++k) r.exec({LDUWA(o1, g5, g0, asi::PHYS_BYPASS_EC_E), LDUWA(o2, g5, g0, asi::PHYS_BYPASS_EC_E_L)});
    CHECK_EQ(r.cpu.reg(o1), 0x1100u);
    CHECK_EQ(r.cpu.reg(o2), 0x00110000u);
    CHECK_EQ(dev.reads, 12u);
}
