// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
// Minimal SPARC V9 instruction encoders for unit tests. Branch and call
// displacements are in instructions, relative to the branch itself.
#pragma once

#include "core/types.h"

namespace uv::test::sparc {

// Registers
enum : unsigned { g0 = 0, g1, g2, g3, g4, g5, g6, g7, o0, o1, o2, o3, o4, o5, sp, o7,
                  l0, l1, l2, l3, l4, l5, l6, l7, i0, i1, i2, i3, i4, i5, fp, i7 };

// Integer condition codes for Bicc/BPcc/Tcc/MOVcc
enum : unsigned { cN = 0, cE, cLE, cL, cLEU, cCS, cNEG, cVS, cA, cNE, cG, cGE, cGU, cCC, cPOS, cVC };
constexpr unsigned kIcc = 0, kXcc = 2;

constexpr u32 f3r(unsigned op, unsigned op3, unsigned rd, unsigned rs1, unsigned rs2) {
    return (op << 30) | (rd << 25) | (op3 << 19) | (rs1 << 14) | rs2;
}
constexpr u32 f3i(unsigned op, unsigned op3, unsigned rd, unsigned rs1, int simm13) {
    return (op << 30) | (rd << 25) | (op3 << 19) | (rs1 << 14) | (1u << 13) | (static_cast<u32>(simm13) & 0x1FFF);
}

// ALU, register and immediate forms
#define UV_ALU(name, op3)                                                                         \
    constexpr u32 name(unsigned rd, unsigned rs1, unsigned rs2) { return f3r(2, op3, rd, rs1, rs2); } \
    constexpr u32 name##i(unsigned rd, unsigned rs1, int imm) { return f3i(2, op3, rd, rs1, imm); }
UV_ALU(ADD, 0x00)
UV_ALU(AND, 0x01)
UV_ALU(OR, 0x02)
UV_ALU(XOR, 0x03)
UV_ALU(SUB, 0x04)
UV_ALU(ANDN, 0x05)
UV_ALU(ADDC, 0x08)
UV_ALU(MULX, 0x09)
UV_ALU(UMUL, 0x0A)
UV_ALU(SMUL, 0x0B)
UV_ALU(SUBC, 0x0C)
UV_ALU(UDIVX, 0x0D)
UV_ALU(UDIV, 0x0E)
UV_ALU(SDIV, 0x0F)
UV_ALU(ADDcc, 0x10)
UV_ALU(ANDcc, 0x11)
UV_ALU(ORcc, 0x12)
UV_ALU(SUBcc, 0x14)
UV_ALU(ADDCcc, 0x18)
UV_ALU(SUBCcc, 0x1C)
UV_ALU(UDIVcc, 0x1E)
UV_ALU(SDIVcc, 0x1F)
UV_ALU(TADDcc, 0x20)
UV_ALU(TADDccTV, 0x22)
UV_ALU(MULScc, 0x24)
UV_ALU(SDIVX, 0x2D)
UV_ALU(JMPL, 0x38)
UV_ALU(RETURN, 0x39)
UV_ALU(SAVE, 0x3C)
UV_ALU(RESTORE, 0x3D)
#undef UV_ALU

constexpr u32 SLLi(unsigned rd, unsigned rs1, unsigned n) { return f3i(2, 0x25, rd, rs1, static_cast<int>(n & 31)); }
constexpr u32 SRLi(unsigned rd, unsigned rs1, unsigned n) { return f3i(2, 0x26, rd, rs1, static_cast<int>(n & 31)); }
constexpr u32 SRAi(unsigned rd, unsigned rs1, unsigned n) { return f3i(2, 0x27, rd, rs1, static_cast<int>(n & 31)); }
constexpr u32 SLLXi(unsigned rd, unsigned rs1, unsigned n) { return f3i(2, 0x25, rd, rs1, static_cast<int>((n & 63) | 0x1000)); }
constexpr u32 SRLXi(unsigned rd, unsigned rs1, unsigned n) { return f3i(2, 0x26, rd, rs1, static_cast<int>((n & 63) | 0x1000)); }
constexpr u32 SRAXi(unsigned rd, unsigned rs1, unsigned n) { return f3i(2, 0x27, rd, rs1, static_cast<int>((n & 63) | 0x1000)); }

constexpr u32 SETHI(unsigned rd, u32 value) { return (rd << 25) | (4u << 22) | ((value >> 10) & 0x3FFFFF); }
constexpr u32 NOP = SETHI(0, 0);
constexpr u32 MOVi(unsigned rd, int imm) { return ORi(rd, g0, imm); }
constexpr u32 CMP(unsigned rs1, unsigned rs2) { return SUBcc(g0, rs1, rs2); }
constexpr u32 CMPi(unsigned rs1, int imm) { return SUBcci(g0, rs1, imm); }

constexpr u32 CALL(int disp) { return (1u << 30) | (static_cast<u32>(disp) & 0x3FFFFFFF); }
constexpr u32 Bicc(unsigned cond, int disp, bool annul = false) {
    return (annul ? 1u << 29 : 0) | (cond << 25) | (2u << 22) | (static_cast<u32>(disp) & 0x3FFFFF);
}
constexpr u32 BPcc(unsigned cond, unsigned cc, int disp, bool annul = false) {
    return (annul ? 1u << 29 : 0) | (cond << 25) | (1u << 22) | (cc << 20) | (1u << 19) | (static_cast<u32>(disp) & 0x7FFFF);
}
// rcond: 1 Z, 2 LEZ, 3 LZ, 5 NZ, 6 GZ, 7 GEZ
constexpr u32 BPr(unsigned rcond, unsigned rs1, int disp, bool annul = false) {
    const u32 d = static_cast<u32>(disp) & 0xFFFF;
    return (annul ? 1u << 29 : 0) | (rcond << 25) | (3u << 22) | ((d >> 14) << 20) | (rs1 << 14) | (d & 0x3FFF);
}
constexpr u32 MOVcc(unsigned cond, unsigned cc, unsigned rd, int simm11) {
    return f3i(2, 0x2C, rd, 0, 0) | (1u << 18) | (cond << 14) | (cc << 11) | (static_cast<u32>(simm11) & 0x7FF);
}
constexpr u32 MOVr(unsigned rcond, unsigned rd, unsigned rs1, int simm10) {
    return (2u << 30) | (rd << 25) | (0x2Fu << 19) | (rs1 << 14) | (1u << 13) | (rcond << 10) | (static_cast<u32>(simm10) & 0x3FF);
}
constexpr u32 Tcc(unsigned cond, unsigned cc, unsigned n) {
    return (2u << 30) | (cond << 25) | (0x3Au << 19) | (1u << 13) | (cc << 11) | (n & 0x7F);
}
constexpr u32 POPC(unsigned rd, unsigned rs2) { return f3r(2, 0x2E, rd, 0, rs2); }

constexpr u32 RDPR(unsigned rd, unsigned pr) { return f3r(2, 0x2A, rd, pr, 0); }
constexpr u32 WRPRi(unsigned pr, unsigned rs1, int imm) { return f3i(2, 0x32, pr, rs1, imm); }
constexpr u32 WRPR(unsigned pr, unsigned rs1, unsigned rs2) { return f3r(2, 0x32, pr, rs1, rs2); }
constexpr u32 RDASR(unsigned rd, unsigned asr) { return f3r(2, 0x28, rd, asr, 0); }
constexpr u32 WRASRi(unsigned asr, unsigned rs1, int imm) { return f3i(2, 0x30, asr, rs1, imm); }
constexpr u32 WRASR(unsigned asr, unsigned rs1, unsigned rs2) { return f3r(2, 0x30, asr, rs1, rs2); }
constexpr u32 DONE = f3r(2, 0x3E, 0, 0, 0);
constexpr u32 RETRY = f3r(2, 0x3E, 1, 0, 0);
constexpr u32 SAVED = f3r(2, 0x31, 0, 0, 0);
constexpr u32 RESTORED = f3r(2, 0x31, 1, 0, 0);
constexpr u32 FLUSHW = f3r(2, 0x2B, 0, 0, 0);

// privileged registers
enum : unsigned { prTPC = 0, prTNPC, prTSTATE, prTT, prTICK, prTBA, prPSTATE, prTL, prPIL, prCWP, prCANSAVE,
                  prCANRESTORE, prCLEANWIN, prOTHERWIN, prWSTATE, prVER = 31 };

// Loads and stores: [rs1 + simm13]; the alternate forms take an ASI.
#define UV_MEM(name, op3)                                                                                  \
    constexpr u32 name(unsigned rd, unsigned rs1, int imm) { return f3i(3, op3, rd, rs1, imm); }
UV_MEM(LDUW, 0x00)
UV_MEM(LDUB, 0x01)
UV_MEM(LDUH, 0x02)
UV_MEM(LDD, 0x03)
UV_MEM(STW, 0x04)
UV_MEM(STB, 0x05)
UV_MEM(STH, 0x06)
UV_MEM(STD, 0x07)
UV_MEM(LDSW, 0x08)
UV_MEM(LDSB, 0x09)
UV_MEM(LDSH, 0x0A)
UV_MEM(LDX, 0x0B)
UV_MEM(LDSTUB, 0x0D)
UV_MEM(STX, 0x0E)
UV_MEM(SWAP, 0x0F)
UV_MEM(LDF, 0x20)
UV_MEM(LDDF, 0x23)
UV_MEM(STF, 0x24)
UV_MEM(STDF, 0x27)
#undef UV_MEM
#define UV_MEMA(name, op3)                                                                                 \
    constexpr u32 name(unsigned rd, unsigned rs1, unsigned rs2, u8 asi) {                                  \
        return f3r(3, op3, rd, rs1, rs2) | (u32{asi} << 5);                                                \
    }
UV_MEMA(LDUWA, 0x10)
UV_MEMA(LDUBA, 0x11)
UV_MEMA(LDXA, 0x1B)
UV_MEMA(STWA, 0x14)
UV_MEMA(STXA, 0x1E)
UV_MEMA(LDDFA, 0x33)
UV_MEMA(STDFA, 0x37)
#undef UV_MEMA
constexpr u32 CASA(unsigned rd, unsigned rs1, unsigned rs2, u8 asi) { return f3r(3, 0x3C, rd, rs1, rs2) | (u32{asi} << 5); }
constexpr u32 CASXA(unsigned rd, unsigned rs1, unsigned rs2, u8 asi) { return f3r(3, 0x3E, rd, rs1, rs2) | (u32{asi} << 5); }

// Floating point: FPop1 (op3 0x34), FPop2 (0x35) and VIS (IMPDEP1, 0x36).
constexpr u32 FPOP1(unsigned rd, unsigned rs1, unsigned rs2, unsigned opf) {
    return (2u << 30) | (rd << 25) | (0x34u << 19) | (rs1 << 14) | (opf << 5) | rs2;
}
constexpr u32 FPOP2(unsigned rd, unsigned rs1, unsigned rs2, unsigned opf) {
    return (2u << 30) | (rd << 25) | (0x35u << 19) | (rs1 << 14) | (opf << 5) | rs2;
}
constexpr u32 VIS(unsigned rd, unsigned rs1, unsigned rs2, unsigned opf) {
    return (2u << 30) | (rd << 25) | (0x36u << 19) | (rs1 << 14) | (opf << 5) | rs2;
}
// Double registers are named by their encoding: %f(2n) for n < 16, and
// %f32..%f62 as odd encodings.
constexpr unsigned dreg(unsigned n) { return (n & 0x1E) | ((n >> 5) & 1); }

}  // namespace uv::test::sparc
