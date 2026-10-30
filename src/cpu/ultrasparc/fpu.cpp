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
#include "cpu/ultrasparc/vis.h"

namespace uv::usparc {

namespace {

// FSR fields
constexpr u64 kFsrCexcMask = 0x1F, kFsrAexcShift = 5, kFsrTemShift = 23, kFsrNs = u64{1} << 22,
              kFsrFttShift = 14, kFsrFttMask = u64{7} << 14;
constexpr unsigned kFttIeee = 1, kFttUnfinished = 2, kFttUnimplemented = 3;
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

// The value, as if read from memory here: an operation whose operands and
// result pass through this stays between the HostFp calls around it, which
// the compiler would otherwise feel free to move it across (it does not
// know that the calls change the rounding mode and read the flags).
template <typename T>
inline T opaque(T v) {
#if defined(__GNUC__) || defined(__clang__)
    asm volatile("" : "+m"(v) : : "memory");
#else
    volatile T t = v;
    v = t;
#endif
    return v;
}

// Nonstandard mode (FSR.NS): subnormal operands are read as zero (and
// signal inexact) and subnormal results flushed to zero.
inline u32 ns_in32(u32 v, bool ns, unsigned& exc) {
    if (!ns || !is_sub32(v)) return v;
    exc |= kNx;
    return v & 0x80000000;
}
inline u64 ns_in64(u64 v, bool ns, unsigned& exc) {
    if (!ns || !is_sub64(v)) return v;
    exc |= kNx;
    return v & 0x8000000000000000ull;
}
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

// The IIi's subnormal cases (UltraSPARC IIi User's Manual, 14.3): with
// FSR.NS clear, the hardware leaves these to the operating system with
// fp_exception_other (unfinished_FPop). ER is a result's biased exponent
// before rounding, as each operation works it out.
struct Format {
    int bias, max_exp, deep;  // the exponent bias, the biased exponent of infinity, the ER at or below which a tiny result is the hardware's
};
constexpr Format kSingle{127, 255, -25}, kDouble{1023, 2047, -54};

int field32(u32 v) { return static_cast<int>(v >> 23) & 0xFF; }
int field64(u64 v) { return static_cast<int>(v >> 52) & 0x7FF; }
bool ordinary32(u32 v) { return field32(v) != 0 && field32(v) != 0xFF; }  // normal: not zero, subnormal, infinite or NaN
bool ordinary64(u64 v) { return field64(v) != 0 && field64(v) != 0x7FF; }

enum class Arith { Add, Mul, Div };

// FMUL or FDIV with one subnormal operand: the exact result's ER (from
// the operands' true exponents and significands), and whether it
// overflows.
template <typename F>
int exact_er(F a, F b, Arith op, const Format& f) {
    int xa, xb;
    const F ma = std::frexp(a, &xa), mb = std::frexp(b, &xb);  // [0.5, 1) * 2^x, subnormals too
    const F pa = std::fabs(ma) * 2, pb = std::fabs(mb) * 2;    // [1, 2)
    const int l = op == Arith::Mul ? (xa - 1) + (xb - 1) + (pa * pb >= 2 ? 1 : 0)
                                   : (xa - 1) - (xb - 1) - (pa < pb ? 1 : 0);
    return l + f.bias;
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

void UltraSparc::fp_unfinished() {
    fsr_ = (fsr_ & ~kFsrFttMask) | (u64{kFttUnfinished} << kFsrFttShift);
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

    // Two-operand single and double arithmetic. With NS clear, the cases
    // the IIi does not finish itself (subnormal operands; results whose ER
    // lies between the format's deep-underflow bound and 1; inexact divides
    // that it predicts pessimistically when that trap is enabled) take
    // unfinished_FPop.
    const unsigned tem = static_cast<unsigned>(fsr_ >> kFsrTemShift) & 0x1F;
    auto unfinished_operands = [&](bool sa, bool sb, Arith op, auto fa, auto fb, const Format& f) {
        if (!sa && !sb) return false;
        if (op == Arith::Add || (sa && sb)) return true;
        // FMUL, FDIV with one: unless the result overflows or is deep below the subnormals.
        if (fa == 0 || fb == 0 || std::isinf(fa) || std::isinf(fb)) return false;
        const int er = exact_er(fa, fb, op, f);
        return er < f.max_exp && er > f.deep;
    };
    // ER as the hardware forms it for ordinary operands: an add's exact
    // result; the sum of the exponents plus one; their difference.
    auto unfinished_result = [&](int ea, int eb, Arith op, long double exact_add, const Format& f) {
        int er;
        if (op == Arith::Add) {
            if (exact_add == 0) return false;
            er = std::ilogb(exact_add) + f.bias;
        } else if (op == Arith::Mul) {
            er = ea + eb - f.bias + 1;
        } else {
            er = ea - eb + f.bias;
            // (an overflow it cannot tell from the exponents, when that trap is enabled)
            if ((tem & kOf) && er == f.max_exp) return true;
            return er > f.deep && er <= 1;
        }
        return er > f.deep && er < 1;
    };
    auto arith32 = [&](Arith kind, auto op) {
        u32 a = f_[rs1], b = f_[rs2];
        if (!ns && !is_nan32(a) && !is_nan32(b)) {
            const bool sa = is_sub32(a), sb = is_sub32(b);
            if (unfinished_operands(sa, sb, kind, f32(a), f32(b), kSingle)) return fp_unfinished();
            if (ordinary32(a) && ordinary32(b)) {
                if (kind == Arith::Div && (tem & kNx)) return fp_unfinished();  // inexact predicted
                const long double exact = kind == Arith::Add ? static_cast<long double>(op(static_cast<double>(f32(a)),
                                                                                         static_cast<double>(f32(b))))
                                                             : 0;
                if (unfinished_result(field32(a), field32(b), kind, exact, kSingle)) return fp_unfinished();
            }
        }
        a = ns_in32(a, ns, exc), b = ns_in32(b, ns, exc);
        u32 r;
        if (is_nan32(a) || is_nan32(b)) {
            r = nan_result32(a, b, exc);
        } else {
            HostFp h(rnd);
            const float v = opaque(op(opaque(f32(a)), opaque(f32(b))));
            exc |= h.exceptions();
            r = std::isnan(v) ? kDefaultNan32 : ns_out32(bits(v), ns, exc);
        }
        if (fp_finish(exc)) set_f32(rd, r);
    };
    auto arith64 = [&](Arith kind, auto op) {
        u64 a = freg64(rs1), b = freg64(rs2);
        if (!ns && !is_nan64(a) && !is_nan64(b)) {
            const bool sa = is_sub64(a), sb = is_sub64(b);
            if (unfinished_operands(sa, sb, kind, f64(a), f64(b), kDouble)) return fp_unfinished();
            if (ordinary64(a) && ordinary64(b)) {
                if (kind == Arith::Div && (tem & kNx)) return fp_unfinished();
                const long double exact =
                    kind == Arith::Add ? op(static_cast<long double>(f64(a)), static_cast<long double>(f64(b))) : 0;
                if (unfinished_result(field64(a), field64(b), kind, exact, kDouble)) return fp_unfinished();
            }
        }
        a = ns_in64(a, ns, exc), b = ns_in64(b, ns, exc);
        u64 r;
        if (is_nan64(a) || is_nan64(b)) {
            r = nan_result64(a, b, exc);
        } else {
            HostFp h(rnd);
            const double v = opaque(op(opaque(f64(a)), opaque(f64(b))));
            exc |= h.exceptions();
            r = std::isnan(v) ? kDefaultNan64 : ns_out64(bits(v), ns, exc);
        }
        if (fp_finish(exc)) set_f64(rd, r);
    };
    // One operand: a subnormal one is unfinished (NS clear) or zero (NS set).
    auto sub_operand32 = [&](u32 v) { return !ns && is_sub32(v); };
    auto sub_operand64 = [&](u64 v) { return !ns && is_sub64(v); };

    switch (opf) {
        case 0x041: return arith32(Arith::Add, [](auto a, auto b) { return a + b; });
        case 0x042: return arith64(Arith::Add, [](auto a, auto b) { return a + b; });
        case 0x045: return arith32(Arith::Add, [](auto a, auto b) { return a - b; });
        case 0x046: return arith64(Arith::Add, [](auto a, auto b) { return a - b; });
        case 0x049: return arith32(Arith::Mul, [](auto a, auto b) { return a * b; });
        case 0x04A: return arith64(Arith::Mul, [](auto a, auto b) { return a * b; });
        case 0x04D: return arith32(Arith::Div, [](auto a, auto b) { return a / b; });
        case 0x04E: return arith64(Arith::Div, [](auto a, auto b) { return a / b; });
        case 0x069: {  // FsMULd: the exact product of two singles
            if (sub_operand32(f_[rs1]) || sub_operand32(f_[rs2])) return fp_unfinished();
            const u32 a = ns_in32(f_[rs1], ns, exc), b = ns_in32(f_[rs2], ns, exc);
            u64 r;
            if (is_nan32(a) || is_nan32(b)) {
                const u32 n = nan_result32(a, b, exc);
                r = bits(static_cast<double>(f32(n | 0x00400000)));
            } else {
                HostFp h(rnd);
                const double v = opaque(static_cast<double>(opaque(f32(a))) * static_cast<double>(opaque(f32(b))));
                exc |= h.exceptions();
                r = std::isnan(v) ? kDefaultNan64 : bits(v);
            }
            if (fp_finish(exc)) set_f64(rd, r);
            return;
        }
        case 0x029: {  // FSQRTs (inexact predicted pessimistically, as a divide's)
            if (sub_operand32(f_[rs2])) return fp_unfinished();
            if (!ns && (tem & kNx) && ordinary32(f_[rs2])) return fp_unfinished();
            const u32 b = ns_in32(f_[rs2], ns, exc);
            u32 r;
            if (is_nan32(b)) {
                r = nan_result32(b, b, exc);
            } else if (f32(b) < 0) {  // (invalid, whatever the host library's sqrt raises: some raise nothing)
                exc |= kNv;
                r = kDefaultNan32;
            } else {
                HostFp h(rnd);
                const float v = opaque(std::sqrt(opaque(f32(b))));
                exc |= h.exceptions();
                r = std::isnan(v) ? kDefaultNan32 : bits(v);
            }
            if (fp_finish(exc)) set_f32(rd, r);
            return;
        }
        case 0x02A: {  // FSQRTd
            if (sub_operand64(freg64(rs2))) return fp_unfinished();
            if (!ns && (tem & kNx) && ordinary64(freg64(rs2))) return fp_unfinished();
            const u64 b = ns_in64(freg64(rs2), ns, exc);
            u64 r;
            if (is_nan64(b)) {
                r = nan_result64(b, b, exc);
            } else if (f64(b) < 0) {  // (as FSQRTs)
                exc |= kNv;
                r = kDefaultNan64;
            } else {
                HostFp h(rnd);
                const double v = opaque(std::sqrt(opaque(f64(b))));
                exc |= h.exceptions();
                r = std::isnan(v) ? kDefaultNan64 : bits(v);
            }
            if (fp_finish(exc)) set_f64(rd, r);
            return;
        }
        case 0x081: {  // FsTOx
            if (sub_operand32(f_[rs2])) return fp_unfinished();
            const s64 v = to_int<s64>(f32(ns_in32(f_[rs2], ns, exc)), exc);
            if (fp_finish(exc)) set_f64(rd, static_cast<u64>(v));
            return;
        }
        case 0x082: {  // FdTOx
            if (sub_operand64(freg64(rs2))) return fp_unfinished();
            const s64 v = to_int<s64>(f64(ns_in64(freg64(rs2), ns, exc)), exc);
            if (fp_finish(exc)) set_f64(rd, static_cast<u64>(v));
            return;
        }
        case 0x0D1: {  // FsTOi
            if (sub_operand32(f_[rs2])) return fp_unfinished();
            const s32 v = to_int<s32>(f32(ns_in32(f_[rs2], ns, exc)), exc);
            if (fp_finish(exc)) set_f32(rd, static_cast<u32>(v));
            return;
        }
        case 0x0D2: {  // FdTOi
            if (sub_operand64(freg64(rs2))) return fp_unfinished();
            const s32 v = to_int<s32>(f64(ns_in64(freg64(rs2), ns, exc)), exc);
            if (fp_finish(exc)) set_f32(rd, static_cast<u32>(v));
            return;
        }
        case 0x084: {  // FxTOs
            HostFp h(rnd);
            const float v = opaque(static_cast<float>(opaque(static_cast<s64>(freg64(rs2)))));
            exc |= h.exceptions();
            if (fp_finish(exc)) set_f32(rd, bits(v));
            return;
        }
        case 0x088: {  // FxTOd
            HostFp h(rnd);
            const double v = opaque(static_cast<double>(opaque(static_cast<s64>(freg64(rs2)))));
            exc |= h.exceptions();
            if (fp_finish(exc)) set_f64(rd, bits(v));
            return;
        }
        case 0x0C4: {  // FiTOs
            HostFp h(rnd);
            const float v = opaque(static_cast<float>(opaque(static_cast<s32>(f_[rs2]))));
            exc |= h.exceptions();
            if (fp_finish(exc)) set_f32(rd, bits(v));
            return;
        }
        case 0x0C8:  // FiTOd: always exact
            if (fp_finish(0)) set_f64(rd, bits(static_cast<double>(static_cast<s32>(f_[rs2]))));
            return;
        case 0x0C9: {  // FsTOd
            if (sub_operand32(f_[rs2])) return fp_unfinished();
            const u32 b = ns_in32(f_[rs2], ns, exc);
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
        case 0x0C6: {  // FdTOs: unfinished for a subnormal operand, or a result in the subnormal window
            if (sub_operand64(freg64(rs2))) return fp_unfinished();
            if (!ns && ordinary64(freg64(rs2))) {
                const int er = std::ilogb(f64(freg64(rs2))) + kSingle.bias;
                if (er > kSingle.deep && er < 1) return fp_unfinished();
            }
            const u64 b = ns_in64(freg64(rs2), ns, exc);
            u32 r;
            if (is_nan64(b)) {
                if (is_snan64(b)) exc |= kNv;
                r = static_cast<u32>((b >> 32) & 0x80000000u) | 0x7FC00000u | static_cast<u32>((b >> 29) & 0x003FFFFFu);
            } else {
                HostFp h(rnd);
                const float v = opaque(static_cast<float>(opaque(f64(b))));
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

    if (const vis::Op* o = vis::op(opf)) {
        auto operand = [&](vis::Src k, unsigned r) -> u64 {
            switch (k) {
                case vis::Src::R: return o->edge ? mask_address(reg(r)) : reg(r);
                case vis::Src::S: return f_[r];
                case vis::Src::D: return freg64(r);
                case vis::Src::Gsr: return gsr_;
                case vis::Src::Rd: return freg64(rd);
                default: return 0;
            }
        };
        const u64 r = o->fn(operand(o->a, rs1), operand(o->b, rs2), operand(o->c, rd));
        if (o->edge) {  // the condition codes as by SUBcc rs1, rs2
            const u64 ra = reg(rs1), rb = reg(rs2), diff = ra - rb;
            const u64 c32 = (ra & 0xFFFFFFFFull) < (rb & 0xFFFFFFFFull) ? 1 : 0, c64 = ra < rb ? 1 : 0;
            const u64 v32 = (((ra ^ rb) & (ra ^ diff)) >> 31) & 1, v64 = ((ra ^ rb) & (ra ^ diff)) >> 63;
            const unsigned icc =
                ((diff >> 28) & 8) | (static_cast<u32>(diff) == 0 ? 4 : 0) | (v32 ? 2 : 0) | (c32 ? 1 : 0);
            const unsigned xcc = ((diff >> 60) & 8) | (diff == 0 ? 4 : 0) | (v64 ? 2 : 0) | (c64 ? 1 : 0);
            ccr_ = static_cast<u8>((xcc << 4) | icc);
        }
        switch (o->dst) {
            case vis::Src::R: set_reg(rd, r); break;
            case vis::Src::S: set_f32(rd, static_cast<u32>(r)); break;
            default: set_f64(rd, r); break;
        }
        return;
    }

    switch (opf) {
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
        case 0x080:  // SHUTDOWN: privileged; there is no power-down to enter
            if (!(pstate_ & pstate::PRIV)) return take_trap(tt::kPrivilegedOpcode);
            return;
        default:
            // Unassigned opf values.
            UV_LOG(Warn, "cpu", "unimplemented VIS opf %03x at %016llx", opf, static_cast<unsigned long long>(pc_));
            return take_trap(tt::kIllegalInstruction);
    }
}

}  // namespace uv::usparc
