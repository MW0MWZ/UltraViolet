// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
// VIS 1.0 operations of the UltraSPARC IIi that are pure functions of their
// operands, shared by the interpreter (fpu.cpp) and the recompilers, which
// call them directly. Each takes (a, b, c) and returns the result: a and b
// are the rs1 and rs2 operands (a double's 64 bits, a single in the low 32,
// or an integer register), c what else the operation reads (GSR, or rd for
// PDIST).
//
// Reference: UltraSPARC Architecture 2007 (Draft D0.9.4), chapter 7; UltraSPARC
// I/II User's Manual, VIS instructions.
#pragma once

#include "core/types.h"

namespace uv::usparc::vis {

using Fn = u64 (*)(u64 a, u64 b, u64 c);

inline s32 lane16(u64 v, unsigned i) { return static_cast<s16>(v >> (48 - 16 * i)); }  // i = 0 is the leftmost
inline s32 lane32(u64 v, unsigned i) { return static_cast<s32>(v >> (32 - 32 * i)); }
inline unsigned scale(u64 gsr) { return static_cast<unsigned>(gsr >> 3) & 15; }

// FPMERGE: the bytes of two singles interleaved, rs1's first.
inline u64 fpmerge(u64 a, u64 b, u64) {
    u64 r = 0;
    for (unsigned i = 0; i < 4; ++i) {
        r = (r << 8) | ((a >> (24 - 8 * i)) & 0xFF);
        r = (r << 8) | ((b >> (24 - 8 * i)) & 0xFF);
    }
    return r;
}
// FEXPAND: the four bytes of a single, each into a 16-bit lane shifted left 4.
inline u64 fexpand(u64, u64 b, u64) {
    u64 r = 0;
    for (unsigned i = 0; i < 4; ++i) r = (r << 16) | (((b >> (24 - 8 * i)) & 0xFF) << 4);
    return r;
}
// FPACK16: four signed 16-bit lanes, scaled by GSR.SCALE, clipped to unsigned bytes.
inline u64 fpack16(u64, u64 b, u64 gsr) {
    u32 r = 0;
    for (unsigned i = 0; i < 4; ++i) {
        const s64 v = (static_cast<s64>(lane16(b, i)) << scale(gsr)) >> 7;
        r = (r << 8) | static_cast<u32>(v < 0 ? 0 : v > 255 ? 255 : v);
    }
    return r;
}
// FPACK32: rs1's two words shifted left a byte, each taking a byte packed from rs2's lane.
inline u64 fpack32(u64 a, u64 b, u64 gsr) {
    u64 r = 0;
    for (unsigned i = 0; i < 2; ++i) {
        const s64 v = (static_cast<s64>(lane32(b, i)) << scale(gsr)) >> 23;
        const u64 byte = static_cast<u64>(v < 0 ? 0 : v > 255 ? 255 : v);
        const u64 word = ((a >> (32 - 32 * i)) << 8) & 0xFFFFFF00ull;
        r = (r << 32) | word | byte;
    }
    return r;
}
// FPACKFIX: two signed 32-bit lanes to signed 16 bits.
inline u64 fpackfix(u64, u64 b, u64 gsr) {
    u32 r = 0;
    for (unsigned i = 0; i < 2; ++i) {
        const s64 v = (static_cast<s64>(lane32(b, i)) << scale(gsr)) >> 16;
        const s64 c = v < -32768 ? -32768 : v > 32767 ? 32767 : v;
        r = (r << 16) | (static_cast<u32>(c) & 0xFFFF);
    }
    return r;
}
// PDIST: rd plus the sum of the absolute differences of the eight bytes.
inline u64 pdist(u64 a, u64 b, u64 acc) {
    for (unsigned i = 0; i < 8; ++i) {
        const int x = static_cast<int>((a >> (8 * i)) & 0xFF), y = static_cast<int>((b >> (8 * i)) & 0xFF);
        acc += static_cast<u64>(x > y ? x - y : y - x);
    }
    return acc;
}
// FMUL8x16 (opf 0x031): rs1's unsigned bytes (single) by rs2's signed
// lanes; FMUL8x16AU/AL (0x033/0x035): by the upper or lower 16 bits of rs2
// (single). Rounded products' middle 16 bits.
template <unsigned Opf>
u64 fmul8x16(u64 a, u64 b, u64) {
    u64 r = 0;
    for (unsigned i = 0; i < 4; ++i) {
        const s32 x = static_cast<s32>((a >> (24 - 8 * i)) & 0xFF);
        const s32 y = Opf == 0x031 ? lane16(b, i) : Opf == 0x033 ? static_cast<s16>(b >> 16) : static_cast<s16>(b);
        const s32 p = (x * y + 128) >> 8;
        r = (r << 16) | (static_cast<u32>(p) & 0xFFFF);
    }
    return r;
}
// FMUL8SUx16 (0x036): the signed upper byte of each lane of rs1;
// FMUL8ULx16 (0x037): the unsigned lower byte.
template <unsigned Opf>
u64 fmul8x16_lanes(u64 a, u64 b, u64) {
    u64 r = 0;
    for (unsigned i = 0; i < 4; ++i) {
        const s32 lane = lane16(a, i), y = lane16(b, i);
        const s32 p = Opf == 0x036 ? ((lane >> 8) * y + 128) >> 8 : ((lane & 0xFF) * y + 128) >> 8;
        r = (r << 16) | (static_cast<u32>(p) & 0xFFFF);
    }
    return r;
}
// FMULD8SUx16 (0x038), FMULD8ULx16 (0x039): two 32-bit products of singles' lanes.
template <unsigned Opf>
u64 fmuld8x16(u64 a, u64 b, u64) {
    u64 r = 0;
    for (unsigned i = 0; i < 2; ++i) {
        const s32 lane = static_cast<s16>(a >> (16 - 16 * i)), y = static_cast<s16>(b >> (16 - 16 * i));
        const s64 p = Opf == 0x038 ? static_cast<s64>((lane >> 8) * y) << 8 : static_cast<s64>((lane & 0xFF) * y);
        r = (r << 32) | (static_cast<u64>(p) & 0xFFFFFFFFull);
    }
    return r;
}
// FPADD16/16S/32/32S, FPSUB16/16S/32/32S (opf 0x050-0x057).
template <unsigned Opf>
u64 fpaddsub(u64 a, u64 b, u64) {
    constexpr bool sub = Opf & 4, wide = Opf & 2, single = Opf & 1;
    constexpr unsigned esize = wide ? 32 : 16, count = (single ? 32 : 64) / esize;
    constexpr u64 emask = (u64{1} << esize) - 1;
    u64 r = 0;
    for (unsigned i = 0; i < count; ++i) {
        const unsigned sh = esize * i;
        r |= ((sub ? ((a >> sh) - (b >> sh)) : ((a >> sh) + (b >> sh))) & emask) << sh;
    }
    return r;
}
// FCMPLE16, FCMPNE16, FCMPLE32, FCMPNE32, FCMPGT16, FCMPEQ16, FCMPGT32,
// FCMPEQ32 (opf 0x020-0x02E, even): a bit per lane into an integer
// register, the least significant lane in bit 0.
template <unsigned Opf>
u64 fcmp(u64 a, u64 b, u64) {
    constexpr bool wide = Opf & 4;
    constexpr unsigned kind = ((Opf >> 1) & 1) | ((Opf >> 2) & 2);  // 0 LE, 1 NE, 2 GT, 3 EQ
    constexpr unsigned count = wide ? 2 : 4;
    u64 r = 0;
    for (unsigned i = 0; i < count; ++i) {
        const s64 x = wide ? lane32(a, count - 1 - i) : lane16(a, count - 1 - i);
        const s64 y = wide ? lane32(b, count - 1 - i) : lane16(b, count - 1 - i);
        const bool t = kind == 0 ? x <= y : kind == 1 ? x != y : kind == 2 ? x > y : x == y;
        if (t) r |= u64{1} << i;
    }
    return r;
}
// EDGE8, EDGE8L, EDGE16, EDGE16L, EDGE32, EDGE32L (opf 0x000-0x00A, even):
// the mask of the elements from address a to the end of its 8 bytes,
// stopping at address b when it is in the same 8 bytes (addresses already
// masked by PSTATE.AM). The condition codes are set separately, as by
// SUBcc.
template <unsigned Opf>
u64 edge(u64 a, u64 b, u64) {
    static constexpr u8 kLeft[6][8] = {{0xFF, 0x7F, 0x3F, 0x1F, 0x0F, 0x07, 0x03, 0x01},
                                       {0xFF, 0xFE, 0xFC, 0xF8, 0xF0, 0xE0, 0xC0, 0x80},
                                       {0xF, 0x7, 0x3, 0x1},
                                       {0xF, 0xE, 0xC, 0x8},
                                       {0x3, 0x1},
                                       {0x3, 0x2}};
    static constexpr u8 kRight[6][8] = {{0x80, 0xC0, 0xE0, 0xF0, 0xF8, 0xFC, 0xFE, 0xFF},
                                        {0x01, 0x03, 0x07, 0x0F, 0x1F, 0x3F, 0x7F, 0xFF},
                                        {0x8, 0xC, 0xE, 0xF},
                                        {0x1, 0x3, 0x7, 0xF},
                                        {0x2, 0x3},
                                        {0x1, 0x3}};
    constexpr unsigned k = Opf >> 1, shift = k / 2;  // element size: 1, 2 or 4 bytes
    const unsigned li = static_cast<unsigned>(a & 7) >> shift, ri = static_cast<unsigned>(b & 7) >> shift;
    u64 mask = kLeft[k][li];
    if ((a & ~u64{7}) == (b & ~u64{7})) mask &= kRight[k][ri];
    return mask;
}
// ARRAY8, ARRAY16, ARRAY32 (opf 0x010, 0x012, 0x014): the blocked-byte
// offset of the fixed-point (x, y, z) in a, for X and Y dimensions of
// 64 << n elements, n from b (b<63:3> ignored, 6 and 7 taken as 5, as the
// UltraSPARC I/II do), scaled by the element size.
template <unsigned Opf>
u64 array(u64 a, u64 b, u64) {
    const unsigned n = (b & 7) > 5 ? 5 : static_cast<unsigned>(b & 7);
    const u64 low = (u64{1} << n) - 1;
    const u64 r = ((a >> 11) & 3) | (((a >> 33) & 3) << 2) | (((a >> 55) & 1) << 4) | (((a >> 13) & 0xF) << 5) |
                  (((a >> 35) & 0xF) << 9) | (((a >> 56) & 0xF) << 13) | (((a >> 17) & low) << 17) |
                  (((a >> 39) & low) << (17 + n)) | (((a >> 60) & 0xF) << (17 + 2 * n));
    return r << ((Opf - 0x010) / 2);
}

// VIS partial stores (ASI 0xC0-0xC5, 0xC8-0xCD): the bytes of the 8 in
// memory that the element mask in rs2 selects, as a mask over the 64-bit
// word read from memory in host (little-endian) byte order. Element i of
// the value (i = 0 leftmost) is selected by mask bit count - 1 - i, and
// lies at memory offset i (big-endian ASIs) or count - 1 - i (little).
struct PartialMasks {
    u64 m[256];
};
constexpr PartialMasks make_partial_masks(unsigned esize, bool little) {
    PartialMasks t{};
    const unsigned count = 8 / esize;
    for (unsigned mask = 0; mask < (1u << count); ++mask)
        for (unsigned j = 0; j < 8; ++j) {
            const unsigned e = j / esize;  // the memory element
            const unsigned bit = little ? e : count - 1 - e;
            if ((mask >> bit) & 1) t.m[mask] |= u64{0xFF} << (8 * j);
        }
    return t;
}
inline const u64* partial_masks(unsigned esize, bool little) {
    static constexpr PartialMasks k[6] = {make_partial_masks(1, false), make_partial_masks(2, false), make_partial_masks(4, false),
                                          make_partial_masks(1, true),  make_partial_masks(2, true),  make_partial_masks(4, true)};
    return k[(little ? 3 : 0) + (esize == 1 ? 0 : esize == 2 ? 1 : 2)].m;
}

// How each of the operations above takes its operands and gives its
// result: from or to an integer register (R), a single (S) or a double (D)
// floating-point register; c from GSR or rd (as a double). Null for an opf
// not listed (the logical operations, ALIGNADDRESS, FALIGNDATA, SHUTDOWN
// and unassigned values are done elsewhere).
enum class Src : u8 { None, R, S, D, Gsr, Rd };
struct Op {
    Fn fn;
    Src a, b, c;
    Src dst;
    bool edge;  // also sets the condition codes as SUBcc rs1, rs2
};
inline const Op* op(unsigned opf) {
    using S = Src;
    static const Op kEdge[6] = {{edge<0x000>, S::R, S::R, S::None, S::R, true}, {edge<0x002>, S::R, S::R, S::None, S::R, true},
                                {edge<0x004>, S::R, S::R, S::None, S::R, true}, {edge<0x006>, S::R, S::R, S::None, S::R, true},
                                {edge<0x008>, S::R, S::R, S::None, S::R, true}, {edge<0x00A>, S::R, S::R, S::None, S::R, true}};
    static const Op kArray[3] = {{array<0x010>, S::R, S::R, S::None, S::R, false},
                                 {array<0x012>, S::R, S::R, S::None, S::R, false},
                                 {array<0x014>, S::R, S::R, S::None, S::R, false}};
    static const Op kFcmp[8] = {{fcmp<0x020>, S::D, S::D, S::None, S::R, false}, {fcmp<0x022>, S::D, S::D, S::None, S::R, false},
                                {fcmp<0x024>, S::D, S::D, S::None, S::R, false}, {fcmp<0x026>, S::D, S::D, S::None, S::R, false},
                                {fcmp<0x028>, S::D, S::D, S::None, S::R, false}, {fcmp<0x02A>, S::D, S::D, S::None, S::R, false},
                                {fcmp<0x02C>, S::D, S::D, S::None, S::R, false}, {fcmp<0x02E>, S::D, S::D, S::None, S::R, false}};
    static const Op kAddSub[8] = {{fpaddsub<0x050>, S::D, S::D, S::None, S::D, false}, {fpaddsub<0x051>, S::S, S::S, S::None, S::S, false},
                                  {fpaddsub<0x052>, S::D, S::D, S::None, S::D, false}, {fpaddsub<0x053>, S::S, S::S, S::None, S::S, false},
                                  {fpaddsub<0x054>, S::D, S::D, S::None, S::D, false}, {fpaddsub<0x055>, S::S, S::S, S::None, S::S, false},
                                  {fpaddsub<0x056>, S::D, S::D, S::None, S::D, false}, {fpaddsub<0x057>, S::S, S::S, S::None, S::S, false}};
    static const Op kMul8x16 = {fmul8x16<0x031>, S::S, S::D, S::None, S::D, false};
    static const Op kMul8x16Au = {fmul8x16<0x033>, S::S, S::S, S::None, S::D, false};
    static const Op kMul8x16Al = {fmul8x16<0x035>, S::S, S::S, S::None, S::D, false};
    static const Op kMul8Su = {fmul8x16_lanes<0x036>, S::D, S::D, S::None, S::D, false};
    static const Op kMul8Ul = {fmul8x16_lanes<0x037>, S::D, S::D, S::None, S::D, false};
    static const Op kMuld8Su = {fmuld8x16<0x038>, S::S, S::S, S::None, S::D, false};
    static const Op kMuld8Ul = {fmuld8x16<0x039>, S::S, S::S, S::None, S::D, false};
    static const Op kPack32 = {fpack32, S::D, S::D, S::Gsr, S::D, false};
    static const Op kPack16 = {fpack16, S::None, S::D, S::Gsr, S::S, false};
    static const Op kPackFix = {fpackfix, S::None, S::D, S::Gsr, S::S, false};
    static const Op kPdist = {pdist, S::D, S::D, S::Rd, S::D, false};
    static const Op kMerge = {fpmerge, S::S, S::S, S::None, S::D, false};
    static const Op kExpand = {fexpand, S::None, S::S, S::None, S::D, false};
    if (opf <= 0x00A && !(opf & 1)) return &kEdge[opf >> 1];
    if (opf == 0x010 || opf == 0x012 || opf == 0x014) return &kArray[(opf - 0x010) >> 1];
    if (opf >= 0x020 && opf <= 0x02E && !(opf & 1)) return &kFcmp[(opf - 0x020) >> 1];
    if (opf >= 0x050 && opf <= 0x057) return &kAddSub[opf - 0x050];
    switch (opf) {
        case 0x031: return &kMul8x16;
        case 0x033: return &kMul8x16Au;
        case 0x035: return &kMul8x16Al;
        case 0x036: return &kMul8Su;
        case 0x037: return &kMul8Ul;
        case 0x038: return &kMuld8Su;
        case 0x039: return &kMuld8Ul;
        case 0x03A: return &kPack32;
        case 0x03B: return &kPack16;
        case 0x03D: return &kPackFix;
        case 0x03E: return &kPdist;
        case 0x04B: return &kMerge;
        case 0x04D: return &kExpand;
        default: return nullptr;
    }
}

}  // namespace uv::usparc::vis
