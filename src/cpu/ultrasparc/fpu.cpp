// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
// UltraSPARC IIi floating point (FPop1, FPop2) and the VIS 1.0 instructions
// (IMPDEP1). Arithmetic runs on the host FPU under the guest rounding mode
// and the exceptions it raises are read back from the host; NaNs are handled
// here, to SPARC's rules: a signaling NaN operand wins over a quiet one, rs2
// over rs1, and an invalid operation with no NaN operand gives the default
// NaN (all ones but the sign). The IIi has no quad arithmetic: those
// instructions take fp_exception_other (unimplemented_FPop) for the
// operating system to emulate.
#include <bit>
#include <cfenv>
#include <cmath>
#include <cstring>
#include <limits>

#include "core/log.h"
#include "cpu/ultrasparc/ultrasparc.h"

namespace uv::usparc {

namespace {

// FSR fields
constexpr u64 kFsrCexcMask = 0x1F, kFsrAexcShift = 5, kFsrTemShift = 23, kFsrNs = u64{1} << 22,
              kFsrFttShift = 14, kFsrFttMask = u64{7} << 14;
constexpr unsigned kFttIeee = 1, kFttUnimplemented = 3;
// cexc/aexc/TEM bits
constexpr unsigned kNv = 0x10, kOf = 0x08, kUf = 0x04, kDz = 0x02, kNx = 0x01;

constexpr u32 kDefaultNan32 = 0x7FFFFFFF;
constexpr u64 kDefaultNan64 = 0x7FFFFFFFFFFFFFFFull;

inline float f32(u32 v) { return std::bit_cast<float>(v); }
inline double f64(u64 v) { return std::bit_cast<double>(v); }
inline u32 bits(float v) { return std::bit_cast<u32>(v); }
inline u64 bits(double v) { return std::bit_cast<u64>(v); }

inline bool is_nan32(u32 v) { return (v & 0x7F800000) == 0x7F800000 && (v & 0x007FFFFF); }
inline bool is_snan32(u32 v) { return is_nan32(v) && !(v & 0x00400000); }
inline bool is_nan64(u64 v) { return (v & 0x7FF0000000000000ull) == 0x7FF0000000000000ull && (v & 0x000FFFFFFFFFFFFFull); }
inline bool is_snan64(u64 v) { return is_nan64(v) && !(v & 0x0008000000000000ull); }
inline bool is_sub32(u32 v) { return (v & 0x7F800000) == 0 && (v & 0x007FFFFF); }
inline bool is_sub64(u64 v) { return (v & 0x7FF0000000000000ull) == 0 && (v & 0x000FFFFFFFFFFFFFull); }

// The NaN result of a two-operand operation with at least one NaN operand.
u32 nan_result32(u32 a, u32 b, unsigned& exc) {
    if (is_snan32(a) || is_snan32(b)) exc |= kNv;
    if (is_snan32(b)) return b | 0x00400000;
    if (is_snan32(a)) return a | 0x00400000;
    return is_nan32(b) ? b : a;
}
u64 nan_result64(u64 a, u64 b, unsigned& exc) {
    if (is_snan64(a) || is_snan64(b)) exc |= kNv;
    if (is_snan64(b)) return b | 0x0008000000000000ull;
    if (is_snan64(a)) return a | 0x0008000000000000ull;
    return is_nan64(b) ? b : a;
}

// Runs `op` on the host FPU in the guest rounding mode and collects the
// IEEE exceptions it raised.
class HostFp {
public:
    explicit HostFp(unsigned rd) {
        static constexpr int kModes[4] = {FE_TONEAREST, FE_TOWARDZERO, FE_UPWARD, FE_DOWNWARD};
        saved_ = std::fegetround();
        std::fesetround(kModes[rd & 3]);
        std::feclearexcept(FE_ALL_EXCEPT);
    }
    ~HostFp() { std::fesetround(saved_); }
    unsigned exceptions() const {
        const int e = std::fetestexcept(FE_ALL_EXCEPT);
        unsigned r = 0;
        if (e & FE_INVALID) r |= kNv;
        if (e & FE_OVERFLOW) r |= kOf;
        if (e & FE_UNDERFLOW) r |= kUf;
        if (e & FE_DIVBYZERO) r |= kDz;
        if (e & FE_INEXACT) r |= kNx;
        return r;
    }

private:
    int saved_;
};

// Nonstandard mode (FSR.NS): subnormal operands are read as zero and
// subnormal results flushed to zero.
inline u32 ns_in32(u32 v, bool ns) { return ns && is_sub32(v) ? (v & 0x80000000) : v; }
inline u64 ns_in64(u64 v, bool ns) { return ns && is_sub64(v) ? (v & 0x8000000000000000ull) : v; }
inline u32 ns_out32(u32 v, bool ns, unsigned& exc) {
    if (ns && is_sub32(v)) {
        exc |= kUf | kNx;
        return v & 0x80000000;
    }
    return v;
}
inline u64 ns_out64(u64 v, bool ns, unsigned& exc) {
    if (ns && is_sub64(v)) {
        exc |= kUf | kNx;
        return v & 0x8000000000000000ull;
    }
    return v;
}

// Conversions to integers round toward zero. NaN and out-of-range values
// are invalid and give the largest integer of the value's sign.
template <typename I, typename F>
I to_int(F v, unsigned& exc) {
    constexpr F lo = static_cast<F>(std::numeric_limits<I>::min());
    constexpr F hi = -lo;  // 2^(n-1), exactly representable
    if (std::isnan(v)) {
        exc |= kNv;
        return std::numeric_limits<I>::max();
    }
    if (v >= hi) {
        exc |= kNv;
        return std::numeric_limits<I>::max();
    }
    if (v < lo) {
        exc |= kNv;
        return std::numeric_limits<I>::min();
    }
    const F t = std::trunc(v);
    if (t != v) exc |= kNx;
    return static_cast<I>(t);
}

// fcc for a comparison: 0 equal, 1 rs1 < rs2, 2 rs1 > rs2, 3 unordered.
template <typename F>
unsigned compare(F a, F b) {
    if (std::isnan(a) || std::isnan(b)) return 3;
    if (a == b) return 0;
    return a < b ? 1 : 2;
}

}  // namespace

void UltraSparc::fp_unimplemented() {
    fsr_ = (fsr_ & ~kFsrFttMask) | (u64{kFttUnimplemented} << kFsrFttShift);
    take_trap(tt::kFpExceptionOther);
}

bool UltraSparc::fp_finish(unsigned cexc) {
    const unsigned tem = static_cast<unsigned>(fsr_ >> kFsrTemShift) & 0x1F;
    // Underflow is signalled for an inexact tiny result, as the host
    // reports it.
    if (cexc & tem) {
        // Of the exceptions raised, only the trapping ones are reported:
        // overflow or underflow with inexact report just the former.
        unsigned shown = cexc;
        if ((cexc & (kOf | kUf)) && (tem & (kOf | kUf))) shown &= ~kNx;
        fsr_ = (fsr_ & ~(kFsrCexcMask | kFsrFttMask)) | shown | (u64{kFttIeee} << kFsrFttShift);
        take_trap(tt::kFpExceptionIeee754);
        return false;
    }
    fsr_ = (fsr_ & ~kFsrCexcMask) | cexc | (u64{cexc} << kFsrAexcShift);
    return true;
}

void UltraSparc::fpop1(u32 insn) {
    const unsigned rd = (insn >> 25) & 31, rs1 = (insn >> 14) & 31, rs2 = insn & 31;
    const unsigned opf = (insn >> 5) & 0x1FF;
    const unsigned rnd = static_cast<unsigned>(fsr_ >> 30) & 3;
    const bool ns = fsr_ & kFsrNs;
    unsigned exc = 0;

    // Moves, negation and absolute value: no exceptions, cexc cleared.
    auto move32 = [&](u32 v) {
        fsr_ &= ~kFsrCexcMask;
        set_f32(rd, v);
    };
    auto move64 = [&](u64 v) {
        fsr_ &= ~kFsrCexcMask;
        set_f64(rd, v);
    };
    switch (opf) {
        case 0x001: return move32(f_[rs2]);
        case 0x002: return move64(freg64(rs2));
        case 0x005: return move32(f_[rs2] ^ 0x80000000u);
        case 0x006: return move64(freg64(rs2) ^ 0x8000000000000000ull);
        case 0x009: return move32(f_[rs2] & 0x7FFFFFFFu);
        case 0x00A: return move64(freg64(rs2) & 0x7FFFFFFFFFFFFFFFull);
        default: break;
    }

    // Two-operand single and double arithmetic.
    auto arith32 = [&](auto op) {
        const u32 a = ns_in32(f_[rs1], ns), b = ns_in32(f_[rs2], ns);
        u32 r;
        if (is_nan32(a) || is_nan32(b)) {
            r = nan_result32(a, b, exc);
        } else {
            HostFp h(rnd);
            const float v = op(f32(a), f32(b));
            exc |= h.exceptions();
            r = std::isnan(v) ? kDefaultNan32 : ns_out32(bits(v), ns, exc);
        }
        if (fp_finish(exc)) set_f32(rd, r);
    };
    auto arith64 = [&](auto op) {
        const u64 a = ns_in64(freg64(rs1), ns), b = ns_in64(freg64(rs2), ns);
        u64 r;
        if (is_nan64(a) || is_nan64(b)) {
            r = nan_result64(a, b, exc);
        } else {
            HostFp h(rnd);
            const double v = op(f64(a), f64(b));
            exc |= h.exceptions();
            r = std::isnan(v) ? kDefaultNan64 : ns_out64(bits(v), ns, exc);
        }
        if (fp_finish(exc)) set_f64(rd, r);
    };

    switch (opf) {
        case 0x041: return arith32([](float a, float b) { return a + b; });
        case 0x042: return arith64([](double a, double b) { return a + b; });
        case 0x045: return arith32([](float a, float b) { return a - b; });
        case 0x046: return arith64([](double a, double b) { return a - b; });
        case 0x049: return arith32([](float a, float b) { return a * b; });
        case 0x04A: return arith64([](double a, double b) { return a * b; });
        case 0x04D: return arith32([](float a, float b) { return a / b; });
        case 0x04E: return arith64([](double a, double b) { return a / b; });
        case 0x069: {  // FsMULd: the exact product of two singles
            const u32 a = ns_in32(f_[rs1], ns), b = ns_in32(f_[rs2], ns);
            u64 r;
            if (is_nan32(a) || is_nan32(b)) {
                const u32 n = nan_result32(a, b, exc);
                r = bits(static_cast<double>(f32(n | 0x00400000)));
            } else {
                HostFp h(rnd);
                const double v = static_cast<double>(f32(a)) * static_cast<double>(f32(b));
                exc |= h.exceptions();
                r = std::isnan(v) ? kDefaultNan64 : bits(v);
            }
            if (fp_finish(exc)) set_f64(rd, r);
            return;
        }
        case 0x029: {  // FSQRTs
            const u32 b = ns_in32(f_[rs2], ns);
            u32 r;
            if (is_nan32(b)) {
                r = nan_result32(b, b, exc);
            } else {
                HostFp h(rnd);
                const float v = std::sqrt(f32(b));
                exc |= h.exceptions();
                r = std::isnan(v) ? kDefaultNan32 : bits(v);
            }
            if (fp_finish(exc)) set_f32(rd, r);
            return;
        }
        case 0x02A: {  // FSQRTd
            const u64 b = ns_in64(freg64(rs2), ns);
            u64 r;
            if (is_nan64(b)) {
                r = nan_result64(b, b, exc);
            } else {
                HostFp h(rnd);
                const double v = std::sqrt(f64(b));
                exc |= h.exceptions();
                r = std::isnan(v) ? kDefaultNan64 : bits(v);
            }
            if (fp_finish(exc)) set_f64(rd, r);
            return;
        }
        case 0x081: {  // FsTOx
            const s64 v = to_int<s64>(f32(ns_in32(f_[rs2], ns)), exc);
            if (fp_finish(exc)) set_f64(rd, static_cast<u64>(v));
            return;
        }
        case 0x082: {  // FdTOx
            const s64 v = to_int<s64>(f64(ns_in64(freg64(rs2), ns)), exc);
            if (fp_finish(exc)) set_f64(rd, static_cast<u64>(v));
            return;
        }
        case 0x0D1: {  // FsTOi
            const s32 v = to_int<s32>(f32(ns_in32(f_[rs2], ns)), exc);
            if (fp_finish(exc)) set_f32(rd, static_cast<u32>(v));
            return;
        }
        case 0x0D2: {  // FdTOi
            const s32 v = to_int<s32>(f64(ns_in64(freg64(rs2), ns)), exc);
            if (fp_finish(exc)) set_f32(rd, static_cast<u32>(v));
            return;
        }
        case 0x084: {  // FxTOs
            HostFp h(rnd);
            const float v = static_cast<float>(static_cast<s64>(freg64(rs2)));
            exc |= h.exceptions();
            if (fp_finish(exc)) set_f32(rd, bits(v));
            return;
        }
        case 0x088: {  // FxTOd
            HostFp h(rnd);
            const double v = static_cast<double>(static_cast<s64>(freg64(rs2)));
            exc |= h.exceptions();
            if (fp_finish(exc)) set_f64(rd, bits(v));
            return;
        }
        case 0x0C4: {  // FiTOs
            HostFp h(rnd);
            const float v = static_cast<float>(static_cast<s32>(f_[rs2]));
            exc |= h.exceptions();
            if (fp_finish(exc)) set_f32(rd, bits(v));
            return;
        }
        case 0x0C8:  // FiTOd: always exact
            if (fp_finish(0)) set_f64(rd, bits(static_cast<double>(static_cast<s32>(f_[rs2]))));
            return;
        case 0x0C9: {  // FsTOd
            const u32 b = ns_in32(f_[rs2], ns);
            u64 r;
            if (is_nan32(b)) {
                if (is_snan32(b)) exc |= kNv;
                r = (u64{b & 0x80000000u} << 32) | 0x7FF8000000000000ull | (u64{b & 0x003FFFFFu} << 29);
            } else {
                r = bits(static_cast<double>(f32(b)));
            }
            if (fp_finish(exc)) set_f64(rd, r);
            return;
        }
        case 0x0C6: {  // FdTOs
            const u64 b = ns_in64(freg64(rs2), ns);
            u32 r;
            if (is_nan64(b)) {
                if (is_snan64(b)) exc |= kNv;
                r = static_cast<u32>((b >> 32) & 0x80000000u) | 0x7FC00000u | static_cast<u32>((b >> 29) & 0x003FFFFFu);
            } else {
                HostFp h(rnd);
                const float v = static_cast<float>(f64(b));
                exc |= h.exceptions();
                r = ns_out32(bits(v), ns, exc);
            }
            if (fp_finish(exc)) set_f32(rd, r);
            return;
        }
        // Quad: FMOVq, FNEGq, FABSq, FSQRTq, FADDq, FSUBq, FMULq, FDIVq,
        // FdMULq, FqTOx, FxTOq, FqTOs, FqTOd, FiTOq, FsTOq, FdTOq, FqTOi.
        default: return fp_unimplemented();
    }
}

void UltraSparc::fpop2(u32 insn) {
    const unsigned rd = (insn >> 25) & 31, rs1 = (insn >> 14) & 31, rs2 = insn & 31;
    const unsigned opf = (insn >> 5) & 0x1FF;
    unsigned exc = 0;

    // FCMP{E}{s,d}: rd<1:0> names the fcc to set.
    if ((opf & 0x1F8) == 0x050) {
        const unsigned n = rd & 3;
        const bool signaling = opf & 4;
        unsigned c;
        switch (opf & 3) {
            case 1: {
                const u32 a = f_[rs1], b = f_[rs2];
                if (is_snan32(a) || is_snan32(b) || (signaling && (is_nan32(a) || is_nan32(b)))) exc |= kNv;
                c = compare(f32(a), f32(b));
                break;
            }
            case 2: {
                const u64 a = freg64(rs1), b = freg64(rs2);
                if (is_snan64(a) || is_snan64(b) || (signaling && (is_nan64(a) || is_nan64(b)))) exc |= kNv;
                c = compare(f64(a), f64(b));
                break;
            }
            default: return fp_unimplemented();
        }
        if (!fp_finish(exc)) return;
        if (n == 0) fsr_ = (fsr_ & ~(u64{3} << 10)) | (u64{c} << 10);
        else fsr_ = (fsr_ & ~(u64{3} << (30 + 2 * n))) | (u64{c} << (30 + 2 * n));
        return;
    }

    // FMOVr{s,d}: opf<4:0> = 5 or 6, rcond in opf<7:5>.
    if (!(opf & 0x100) && ((opf & 0x1F) == 0x05 || (opf & 0x1F) == 0x06)) {
        const unsigned rcond = (opf >> 5) & 7;
        const s64 v = static_cast<s64>(reg(rs1));
        bool take;
        switch (rcond) {
            case 1: take = v == 0; break;
            case 2: take = v <= 0; break;
            case 3: take = v < 0; break;
            case 5: take = v != 0; break;
            case 6: take = v > 0; break;
            case 7: take = v >= 0; break;
            default: return take_trap(tt::kIllegalInstruction);
        }
        fsr_ &= ~kFsrCexcMask;
        if (take) {
            if ((opf & 0x1F) == 0x05) set_f32(rd, f_[rs2]);
            else set_f64(rd, freg64(rs2));
        }
        return;
    }

    // FMOVcc{s,d}: opf<5:0> = 1 or 2, opf_cc in opf<8:6>, cond in insn<17:14>.
    if ((opf & 0x3F) == 0x01 || (opf & 0x3F) == 0x02) {
        const unsigned occ = (opf >> 6) & 7;
        const unsigned cond = (insn >> 14) & 15;
        bool take;
        if (occ < 4) take = cond_fcc(cond, fcc(occ));
        else if (occ == 4) take = cond_icc(cond, ccr_ & 15);
        else if (occ == 6) take = cond_icc(cond, ccr_ >> 4);
        else return take_trap(tt::kIllegalInstruction);
        fsr_ &= ~kFsrCexcMask;
        if (take) {
            if ((opf & 0x3F) == 0x01) set_f32(rd, f_[rs2]);
            else set_f64(rd, freg64(rs2));
        }
        return;
    }
    fp_unimplemented();  // the quad forms and unassigned opf values
}

void UltraSparc::impdep1(u32 insn) {
    const unsigned rd = (insn >> 25) & 31, rs1 = (insn >> 14) & 31, rs2 = insn & 31;
    const unsigned opf = (insn >> 5) & 0x1FF;
    const u64 a = freg64(rs1), b = freg64(rs2);
    const u32 as = f_[rs1], bs = f_[rs2];
    const unsigned scale = static_cast<unsigned>(gsr_ >> 3) & 15;

    // Logical operations: even opf is the double form, odd the single.
    if (opf >= 0x060 && opf <= 0x07F) {
        u64 r;
        const u64 x = (opf & 1) ? as : a, y = (opf & 1) ? bs : b;
        switch ((opf - 0x060) >> 1) {
            case 0: r = 0; break;           // FZERO
            case 1: r = ~(x | y); break;    // FNOR
            case 2: r = x & ~y; break;      // FANDNOT2
            case 3: r = ~y; break;          // FNOT2
            case 4: r = ~x & y; break;      // FANDNOT1
            case 5: r = ~x; break;          // FNOT1
            case 6: r = x ^ y; break;       // FXOR
            case 7: r = ~(x & y); break;    // FNAND
            case 8: r = x & y; break;       // FAND
            case 9: r = ~(x ^ y); break;    // FXNOR
            case 10: r = x; break;          // FSRC1
            case 11: r = x | ~y; break;     // FORNOT2
            case 12: r = y; break;          // FSRC2
            case 13: r = ~x | y; break;     // FORNOT1
            case 14: r = x | y; break;      // FOR
            default: r = ~u64{0}; break;    // FONE
        }
        if (opf & 1) set_f32(rd, static_cast<u32>(r));
        else set_f64(rd, r);
        return;
    }

    auto lanes16 = [](u64 v, unsigned i) { return static_cast<s16>(v >> (48 - 16 * i)); };
    auto lanes32 = [](u64 v, unsigned i) { return static_cast<s32>(v >> (32 - 32 * i)); };

    switch (opf) {
        case 0x000:  // EDGE8, EDGE8L, EDGE16, EDGE16L, EDGE32, EDGE32L
        case 0x002:
        case 0x004:
        case 0x006:
        case 0x008:
        case 0x00A: {
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
            const unsigned k = opf >> 1;
            const unsigned shift = k / 2;  // element size: 1, 2 or 4 bytes
            const u64 x = mask_address(reg(rs1)), y = mask_address(reg(rs2));
            const unsigned li = static_cast<unsigned>(x & 7) >> shift, ri = static_cast<unsigned>(y & 7) >> shift;
            u64 mask = kLeft[k][li];
            if ((x & ~u64{7}) == (y & ~u64{7})) mask &= kRight[k][ri];
            // The condition codes are set as by SUBcc rs1, rs2.
            const u64 ra = reg(rs1), rb = reg(rs2), r = ra - rb;
            const u64 c32 = (ra & 0xFFFFFFFFull) < (rb & 0xFFFFFFFFull) ? 1 : 0, c64 = ra < rb ? 1 : 0;
            const u64 v32 = (((ra ^ rb) & (ra ^ r)) >> 31) & 1, v64 = ((ra ^ rb) & (ra ^ r)) >> 63;
            const unsigned icc = ((r >> 28) & 8) | (static_cast<u32>(r) == 0 ? 4 : 0) | (v32 ? 2 : 0) | (c32 ? 1 : 0);
            const unsigned xcc = ((r >> 60) & 8) | (r == 0 ? 4 : 0) | (v64 ? 2 : 0) | (c64 ? 1 : 0);
            ccr_ = static_cast<u8>((xcc << 4) | icc);
            set_reg(rd, mask);
            return;
        }
        case 0x018:    // ALIGNADDRESS
        case 0x01A: {  // ALIGNADDRESS_LITTLE
            const u64 sum = reg(rs1) + reg(rs2);
            const u64 align = opf == 0x018 ? (sum & 7) : ((0 - sum) & 7);
            gsr_ = (gsr_ & ~u64{7}) | align;
            set_reg(rd, sum & ~u64{7});
            return;
        }
        case 0x048: {  // FALIGNDATA
            const unsigned off = static_cast<unsigned>(gsr_ & 7);
            set_f64(rd, off == 0 ? a : (a << (8 * off)) | (b >> (64 - 8 * off)));
            return;
        }
        case 0x04B: {  // FPMERGE
            u64 r = 0;
            for (unsigned i = 0; i < 4; ++i) {
                r = (r << 8) | ((as >> (24 - 8 * i)) & 0xFF);
                r = (r << 8) | ((bs >> (24 - 8 * i)) & 0xFF);
            }
            set_f64(rd, r);
            return;
        }
        case 0x04D: {  // FEXPAND
            u64 r = 0;
            for (unsigned i = 0; i < 4; ++i) r = (r << 16) | (u64{(bs >> (24 - 8 * i)) & 0xFF} << 4);
            set_f64(rd, r);
            return;
        }
        case 0x03B: {  // FPACK16: four signed 16-bit to unsigned bytes
            u32 r = 0;
            for (unsigned i = 0; i < 4; ++i) {
                const s64 v = (static_cast<s64>(lanes16(b, i)) << scale) >> 7;
                r = (r << 8) | static_cast<u32>(v < 0 ? 0 : v > 255 ? 255 : v);
            }
            set_f32(rd, r);
            return;
        }
        case 0x03A: {  // FPACK32: shift rs1 left a byte, insert two packed bytes from rs2
            u64 r = 0;
            for (unsigned i = 0; i < 2; ++i) {
                const s64 v = (static_cast<s64>(lanes32(b, i)) << scale) >> 23;
                const u64 byte = static_cast<u64>(v < 0 ? 0 : v > 255 ? 255 : v);
                const u64 word = ((a >> (32 - 32 * i)) << 8) & 0xFFFFFF00ull;
                r = (r << 32) | word | byte;
            }
            set_f64(rd, r);
            return;
        }
        case 0x03D: {  // FPACKFIX: two signed 32-bit to signed 16-bit
            u32 r = 0;
            for (unsigned i = 0; i < 2; ++i) {
                const s64 v = (static_cast<s64>(lanes32(b, i)) << scale) >> 16;
                const s64 c = v < -32768 ? -32768 : v > 32767 ? 32767 : v;
                r = (r << 16) | (static_cast<u32>(c) & 0xFFFF);
            }
            set_f32(rd, r);
            return;
        }
        case 0x03E: {  // PDIST
            u64 sum = freg64(rd);
            for (unsigned i = 0; i < 8; ++i) {
                const int x = static_cast<int>((a >> (8 * i)) & 0xFF), y = static_cast<int>((b >> (8 * i)) & 0xFF);
                sum += static_cast<u64>(x > y ? x - y : y - x);
            }
            set_f64(rd, sum);
            return;
        }
        case 0x031:    // FMUL8x16: unsigned bytes of rs1 (single) by signed 16-bit lanes of rs2
        case 0x033:    // FMUL8x16AU: by the upper 16 bits of rs2 (single)
        case 0x035: {  // FMUL8x16AL: by the lower 16 bits
            u64 r = 0;
            for (unsigned i = 0; i < 4; ++i) {
                const s32 x = static_cast<s32>((as >> (24 - 8 * i)) & 0xFF);
                const s32 y = opf == 0x031 ? lanes16(b, i) : opf == 0x033 ? static_cast<s16>(bs >> 16) : static_cast<s16>(bs);
                const s32 p = (x * y + 128) >> 8;
                r = (r << 16) | (static_cast<u32>(p) & 0xFFFF);
            }
            set_f64(rd, r);
            return;
        }
        case 0x036:    // FMUL8SUx16: upper signed byte of each 16-bit lane of rs1
        case 0x037: {  // FMUL8ULx16: lower unsigned byte
            u64 r = 0;
            for (unsigned i = 0; i < 4; ++i) {
                const s32 lane = lanes16(a, i);
                const s32 y = lanes16(b, i);
                s32 p;
                if (opf == 0x036) p = ((lane >> 8) * y + 128) >> 8;
                else p = ((lane & 0xFF) * y + 128) >> 8;
                r = (r << 16) | (static_cast<u32>(p) & 0xFFFF);
            }
            set_f64(rd, r);
            return;
        }
        case 0x038:    // FMULD8SUx16
        case 0x039: {  // FMULD8ULx16
            u64 r = 0;
            for (unsigned i = 0; i < 2; ++i) {
                const s32 lane = static_cast<s16>(as >> (16 - 16 * i));
                const s32 y = static_cast<s16>(bs >> (16 - 16 * i));
                const s64 p = opf == 0x038 ? static_cast<s64>((lane >> 8) * y) << 8 : static_cast<s64>((lane & 0xFF) * y);
                r = (r << 32) | (static_cast<u64>(p) & 0xFFFFFFFFull);
            }
            set_f64(rd, r);
            return;
        }
        case 0x050:  // FPADD16, FPADD16S, FPADD32, FPADD32S, FPSUB16, FPSUB16S, FPSUB32, FPSUB32S
        case 0x051:
        case 0x052:
        case 0x053:
        case 0x054:
        case 0x055:
        case 0x056:
        case 0x057: {
            const bool sub = opf & 4, wide = opf & 2, single = opf & 1;
            const u64 x = single ? as : a, y = single ? bs : b;
            const unsigned esize = wide ? 32 : 16, count = (single ? 32 : 64) / esize;
            const u64 emask = (u64{1} << esize) - 1;
            u64 r = 0;
            for (unsigned i = 0; i < count; ++i) {
                const unsigned sh = esize * i;
                const u64 e = ((sub ? ((x >> sh) - (y >> sh)) : ((x >> sh) + (y >> sh))) & emask) << sh;
                r |= e;
            }
            if (single) set_f32(rd, static_cast<u32>(r));
            else set_f64(rd, r);
            return;
        }
        case 0x020:  // FCMPLE16, FCMPNE16, FCMPLE32, FCMPNE32, FCMPGT16, FCMPEQ16, FCMPGT32, FCMPEQ32
        case 0x022:
        case 0x024:
        case 0x026:
        case 0x028:
        case 0x02A:
        case 0x02C:
        case 0x02E: {
            const bool wide = opf & 4;
            const unsigned kind = ((opf >> 1) & 1) | ((opf >> 2) & 2);  // 0 LE, 1 NE, 2 GT, 3 EQ
            const unsigned count = wide ? 2 : 4;
            u64 r = 0;
            for (unsigned i = 0; i < count; ++i) {
                // Bit 0 of the result is the least significant element.
                const s64 x = wide ? lanes32(a, count - 1 - i) : lanes16(a, count - 1 - i);
                const s64 y = wide ? lanes32(b, count - 1 - i) : lanes16(b, count - 1 - i);
                bool t;
                switch (kind) {
                    case 0: t = x <= y; break;
                    case 1: t = x != y; break;
                    case 2: t = x > y; break;
                    default: t = x == y; break;
                }
                if (t) r |= u64{1} << i;
            }
            set_reg(rd, r);
            return;
        }
        case 0x080:  // SHUTDOWN: privileged; there is no power-down to enter
            if (!(pstate_ & pstate::PRIV)) return take_trap(tt::kPrivilegedOpcode);
            return;
        default:
            // ARRAY8/16/32 (0x010/0x012/0x014) and unassigned opf values.
            UV_LOG(Warn, "cpu", "unimplemented VIS opf %03x at %016llx", opf, static_cast<unsigned long long>(pc_));
            return take_trap(tt::kIllegalInstruction);
    }
}

}  // namespace uv::usparc
