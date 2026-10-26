// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
#include <bit>

#include "cpu_rig.h"
#include "test.h"

using namespace uv;
using namespace uv::test;

namespace {
u32 fs(float v) { return std::bit_cast<u32>(v); }
u64 fd(double v) { return std::bit_cast<u64>(v); }
constexpr u64 kTemNv = u64{0x10} << 23, kTemDz = u64{0x02} << 23;
unsigned ftt(const UltraSparc& c) { return static_cast<unsigned>(c.fsr() >> 14) & 7; }
}  // namespace

TEST(fpu_single_and_double_arithmetic) {
    Rig r;
    r.cpu.set_freg32(1, fs(1.5f));
    r.cpu.set_freg32(2, fs(2.25f));
    r.cpu.set_freg64(dreg(4), fd(3.0));
    r.cpu.set_freg64(dreg(6), fd(0.5));
    r.exec({FPOP1(3, 1, 2, 0x041), FPOP1(dreg(8), dreg(4), dreg(6), 0x04A), FPOP1(dreg(10), dreg(4), dreg(6), 0x04E),
            FPOP1(dreg(12), 1, 2, 0x069)});
    CHECK_EQ(r.cpu.freg32(3), fs(3.75f));
    CHECK_EQ(r.cpu.freg64(dreg(8)), fd(1.5));
    CHECK_EQ(r.cpu.freg64(dreg(10)), fd(6.0));
    CHECK_EQ(r.cpu.freg64(dreg(12)), fd(3.375));  // FsMULd
    CHECK_EQ(r.cpu.fsr() & 0x1F, 0u);
}

TEST(fpu_rounding_follows_fsr_rd) {
    Rig r;
    r.cpu.set_freg32(1, fs(1.0f));
    r.cpu.set_freg32(2, fs(3.0f));
    r.exec({FPOP1(3, 1, 2, 0x04D)});
    CHECK_EQ(r.cpu.freg32(3), 0x3EAAAAABu);  // nearest
    CHECK_EQ(r.cpu.fsr() & 0x1F, 1u);         // inexact
    r.cpu.set_fsr(u64{1} << 30);              // toward zero
    r.exec({FPOP1(3, 1, 2, 0x04D)});
    CHECK_EQ(r.cpu.freg32(3), 0x3EAAAAAAu);
}

TEST(fpu_nans_follow_sparc_rules) {
    Rig r;
    r.cpu.set_freg32(1, 0x7FC00001);  // quiet NaN
    r.cpu.set_freg32(2, 0x7F800002);  // signaling NaN
    r.exec({FPOP1(3, 1, 2, 0x041)});
    CHECK_EQ(r.cpu.freg32(3), 0x7FC00002u);  // rs2's signaling NaN, quieted
    CHECK(r.cpu.fsr() & 0x10);
    r.cpu.set_fsr(0);
    r.cpu.set_freg32(1, fs(0.0f));
    r.cpu.set_freg32(2, 0x7F800000);  // infinity
    r.exec({FPOP1(3, 1, 2, 0x049)});
    CHECK_EQ(r.cpu.freg32(3), 0x7FFFFFFFu);  // the default NaN
    CHECK(r.cpu.fsr() & 0x10);
}

TEST(fpu_enabled_exception_traps_without_writing_the_result) {
    Rig r;
    r.cpu.set_fsr(kTemDz);
    r.cpu.set_freg64(dreg(2), fd(1.0));
    r.cpu.set_freg64(dreg(4), 0);
    r.cpu.set_freg64(dreg(6), fd(42.0));
    r.exec({FPOP1(dreg(6), dreg(2), dreg(4), 0x04E)}, 1);
    CHECK_EQ(r.cpu.trap_type(1), tt::kFpExceptionIeee754);
    CHECK_EQ(ftt(r.cpu), 1u);
    CHECK_EQ(r.cpu.fsr() & 0x1F, 0x02u);
    CHECK_EQ(r.cpu.freg64(dreg(6)), fd(42.0));
    // Disabled: the result is infinity and dz accrues.
    Rig q;
    q.cpu.set_freg64(dreg(2), fd(1.0));
    q.cpu.set_freg64(dreg(4), 0);
    q.exec({FPOP1(dreg(6), dreg(2), dreg(4), 0x04E)});
    CHECK_EQ(q.cpu.freg64(dreg(6)), 0x7FF0000000000000ull);
    CHECK_EQ((q.cpu.fsr() >> 5) & 0x1F, 0x02u);
}

TEST(fpu_conversions_to_integer_truncate_and_saturate) {
    Rig r;
    r.cpu.set_freg32(1, fs(-1.5f));
    r.cpu.set_freg32(2, fs(3e9f));
    r.cpu.set_freg64(dreg(4), fd(-1e30));
    r.exec({FPOP1(3, 0, 1, 0x0D1), FPOP1(5, 0, 2, 0x0D1), FPOP1(dreg(6), 0, dreg(4), 0x082)});
    CHECK_EQ(r.cpu.freg32(3), 0xFFFFFFFFu);  // -1
    CHECK_EQ(r.cpu.freg32(5), 0x7FFFFFFFu);
    CHECK_EQ(r.cpu.freg64(dreg(6)), 0x8000000000000000ull);
    CHECK((r.cpu.fsr() >> 5) & 0x10);  // invalid accrued
    r.cpu.set_freg64(dreg(8), static_cast<u64>(-7ll));
    r.cpu.set_freg32(25, static_cast<u32>(-3));
    r.exec({FPOP1(dreg(10), 0, dreg(8), 0x088), FPOP1(27, 0, 25, 0x0C4), FPOP1(dreg(12), 0, 27, 0x0C9),
            FPOP1(29, 0, dreg(12), 0x0C6)});
    CHECK_EQ(r.cpu.freg64(dreg(10)), fd(-7.0));
    CHECK_EQ(r.cpu.freg32(27), fs(-3.0f));
    CHECK_EQ(r.cpu.freg64(dreg(12)), fd(-3.0));
    CHECK_EQ(r.cpu.freg32(29), fs(-3.0f));
}

TEST(fpu_compare_sets_the_named_fcc_and_branches_follow_it) {
    Rig r;
    r.cpu.set_freg64(dreg(2), fd(1.0));
    r.cpu.set_freg64(dreg(4), fd(2.0));
    r.exec({FPOP2(1, dreg(2), dreg(4), 0x052), FPOP2(0, dreg(4), dreg(2), 0x052)});
    CHECK_EQ((r.cpu.fsr() >> 32) & 3, 1u);  // fcc1: less
    CHECK_EQ((r.cpu.fsr() >> 10) & 3, 2u);  // fcc0: greater
    r.cpu.set_freg32(1, 0x7FC00000);
    r.exec({FPOP2(0, 1, 1, 0x051)});
    CHECK_EQ((r.cpu.fsr() >> 10) & 3, 3u);  // unordered, no exception for FCMP on a quiet NaN
    CHECK_EQ(r.cpu.fsr() & 0x1F, 0u);
    r.cpu.set_fsr(kTemNv);
    r.exec({FPOP2(0, 1, 1, 0x055)}, 1);    // FCMPEs traps on any NaN
    CHECK_EQ(r.cpu.trap_type(1), tt::kFpExceptionIeee754);
}

TEST(fpu_quad_operations_are_left_to_software) {
    Rig r;
    r.exec({FPOP1(0, 0, 4, 0x043)}, 1);
    CHECK_EQ(r.cpu.trap_type(1), tt::kFpExceptionOther);
    CHECK_EQ(ftt(r.cpu), 3u);
}

TEST(fpu_disabled_without_fprs_fef) {
    Rig r;
    r.exec({WRASRi(6, g0, 0), FPOP1(3, 1, 2, 0x041)}, 2);
    CHECK_EQ(r.cpu.trap_type(1), tt::kFpDisabled);
}

TEST(fpu_fmovcc_and_fmovr) {
    Rig r;
    r.cpu.set_freg32(2, 0x12345678);
    r.cpu.set_reg(g1, 0);
    // fmovse %icc after cmp %g0, %g0; fmovrsnz %g1 (not taken).
    r.exec({CMP(g0, g0), FPOP2(3, 0, 2, (4u << 6) | 0x01) | (cE << 14), FPOP2(5, g1, 2, (5u << 5) | 0x05)});
    CHECK_EQ(r.cpu.freg32(3), 0x12345678u);
    CHECK_EQ(r.cpu.freg32(5), 0u);
}

TEST(fpu_fprs_dirty_bits) {
    Rig r;
    r.exec({FPOP1(dreg(40), 0, dreg(2), 0x002)});
    CHECK_EQ(r.cpu.fprs() & 3, 2u);  // DU: an upper register was written
    r.exec({FPOP1(1, 0, 2, 0x001)});
    CHECK_EQ(r.cpu.fprs() & 3, 3u);
}

TEST(vis_logical_partitioned_add_and_merge) {
    Rig r;
    r.cpu.set_freg64(dreg(2), 0x0001000200030004ull);
    r.cpu.set_freg64(dreg(4), 0x00100020FFFF0040ull);
    r.cpu.set_freg32(25, 0x11223344);
    r.cpu.set_freg32(27, 0xAABBCCDD);
    r.exec({VIS(dreg(6), dreg(2), dreg(4), 0x050), VIS(dreg(10), 0, 0, 0x07E), VIS(dreg(12), 0, 0, 0x060),
            VIS(dreg(14), 25, 27, 0x04B), VIS(dreg(16), dreg(2), dreg(4), 0x06C)});
    CHECK_EQ(r.cpu.freg64(dreg(6)), 0x0011002200020044ull);
    CHECK_EQ(r.cpu.freg64(dreg(10)), ~u64{0});
    CHECK_EQ(r.cpu.freg64(dreg(12)), 0u);
    CHECK_EQ(r.cpu.freg64(dreg(14)), 0x11AA22BB33CC44DDull);
    CHECK_EQ(r.cpu.freg64(dreg(16)), 0x0001000200030004ull ^ 0x00100020FFFF0040ull);  // FXOR
}

TEST(vis_alignaddress_and_faligndata) {
    Rig r;
    r.cpu.set_reg(g1, 0x2003);
    r.cpu.set_freg64(dreg(2), 0x0001020304050607ull);
    r.cpu.set_freg64(dreg(4), 0x08090A0B0C0D0E0Full);
    r.exec({VIS(g2, g1, g0, 0x018), VIS(dreg(6), dreg(2), dreg(4), 0x048)});
    CHECK_EQ(r.cpu.reg(g2), 0x2000u);
    CHECK_EQ(r.cpu.freg64(dreg(6)), 0x030405060708090Aull);
}

TEST(vis_edge8_masks_and_pdist) {
    Rig r;
    r.cpu.set_reg(g1, 0x1002);
    r.cpu.set_reg(g2, 0x1005);
    r.cpu.set_reg(g3, 0x1010);
    r.exec({VIS(g4, g1, g2, 0x000), VIS(g5, g1, g3, 0x000)});
    CHECK_EQ(r.cpu.reg(g4), 0x3Cu);  // bytes 2..5
    CHECK_EQ(r.cpu.reg(g5), 0x3Fu);  // from byte 2 to the end of the block
    r.cpu.set_freg64(dreg(2), 0x0A00000000000005ull);
    r.cpu.set_freg64(dreg(4), 0x0000000000000008ull);
    r.cpu.set_freg64(dreg(6), 100);
    r.exec({VIS(dreg(6), dreg(2), dreg(4), 0x03E)});
    CHECK_EQ(r.cpu.freg64(dreg(6)), 113u);  // 100 + 10 + 3
}

TEST(fpu_block_store_and_load) {
    Rig r;
    for (unsigned i = 0; i < 8; ++i) r.cpu.set_freg64(dreg(16 + 2 * i), 0x1111111111111111ull * (i + 1));
    r.cpu.set_reg(g1, 0x3000);
    r.exec({STDFA(dreg(16), g1, g0, asi::BLK_P), LDDFA(dreg(32), g1, g0, asi::BLK_P)});
    CHECK_EQ(r.load64(0x3038), 0x8888888888888888ull);
    CHECK_EQ(r.cpu.freg64(dreg(32)), 0x1111111111111111ull);
    CHECK_EQ(r.cpu.freg64(dreg(46)), 0x8888888888888888ull);
}
