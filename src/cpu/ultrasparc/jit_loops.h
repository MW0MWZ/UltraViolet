// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
// Delay loops the processor runs many iterations of at once, each
// leaving exactly the state running them one by one would.
//
// A block of two instructions, a branch to itself and a delay slot that
// counts a register down by one, is a delay loop:
//
//     1:  bg,pt  %xcc, 1b          (or bne, on icc or xcc; or brgz/brnz r)
//         subcc  r, 1, r           (or addcc r, -1, r; sub/add for brgz/brnz)
//
// With r above zero after an iteration (by the branch's own test: signed
// or nonzero, 32 or 64 bits), the next r iterations are all taken and only
// count r down: k of them, short of the last, can be done at once as
// r -= k, k * 2 instructions started and the condition codes as the last of
// them would set them.
//
// A block of four that waits for TICK to pass a value is a timed loop:
//
//     1:  cmp    x, t              (or cmp t, x)
//         rdpr   %tick, t          (or rd %tick, t)
//         bcc,pt %xcc, 1b          (or another test of x against t that
//         nop                       holds until t passes x: see timed_loop)
//
// Each iteration tests the TICK the one before read, and TICK grows with
// the steps run, so the iterations still taken follow from the time each
// read would see: those that fit are done at once, with t the TICK the last
// of them read and the condition codes its compare set.
#pragma once

#include "core/types.h"

namespace uv::usparc {

struct CountedLoop {
    unsigned reg = 0;     // the register counted down (0: not a counted loop)
    bool sign = false;    // taken while r > 0 (signed); else while r != 0
    bool word = false;    // on the low 32 bits of r (icc); else all 64
    bool cc = false;      // the delay slot sets the condition codes
    bool add = false;     // it adds -1 (else subtracts 1)
    // As Block::loop holds it.
    u16 code() const {
        return static_cast<u16>(reg ? reg | (sign ? 32 : 0) | (word ? 64 : 0) | (cc ? 128 : 0) | (add ? 256 : 0) : 0);
    }
};

inline CountedLoop counted_loop(const u32* insns, unsigned n) {
    if (n != 2) return {};
    const u32 br = insns[0], ds = insns[1];
    if (br >> 30 != 0) return {};
    const unsigned op2 = (br >> 22) & 7;
    unsigned reg = 0;
    bool sign = false, word = false, cc = false;
    if (op2 == 2 || op2 == 1) {  // Bicc, BPcc
        const unsigned cond = (br >> 25) & 15;
        if (cond != 9 && cond != 10) return {};  // bne, bg
        if (op2 == 2) {
            if (br & 0x3FFFFF) return {};
            word = true;
        } else {
            const unsigned ccn = (br >> 20) & 3;
            if ((br & 0x7FFFF) || (ccn != 0 && ccn != 2)) return {};
            word = ccn == 0;
        }
        sign = cond == 10;
        cc = true;
    } else if (op2 == 3) {  // BPr
        const unsigned rcond = (br >> 25) & 7;
        if ((rcond != 5 && rcond != 6) || (br & 0x10303FFF)) return {};  // brnz, brgz (to itself)
        sign = rcond == 6;
        reg = (br >> 14) & 31;
    } else {
        return {};
    }
    // The delay slot: r - 1 or r + -1 into r (setting the codes a cc branch tests).
    if (ds >> 30 != 2 || !((ds >> 13) & 1)) return {};
    const unsigned op3 = (ds >> 19) & 0x3F, rd = (ds >> 25) & 31, rs1 = (ds >> 14) & 31;
    const u32 simm = ds & 0x1FFF;
    const bool sub = (op3 & 0xF) == 0x04 && simm == 1, add = (op3 & 0xF) == 0x00 && simm == 0x1FFF;
    if (!(sub || add) || (op3 & ~0x10u) > 0x04 || rd != rs1 || rd == 0) return {};
    if (cc ? !(op3 & 0x10) : reg != rd) return {};
    return {rd, sign, word, (op3 & 0x10) != 0, add};
}

struct TimedLoop {
    unsigned x = 0, t = 0;  // the value waited for and the register TICK is read into (t 0: not a timed loop)
    unsigned cond = 0;      // the branch's condition, on xcc
    bool t_first = false;   // the compare is cmp t, x
    // As Block::loop holds it (bit 31: a timed loop).
    u32 code() const { return t ? 0x80000000u | x | t << 5 | cond << 10 | (t_first ? 1u << 14 : 0) : 0; }
};

inline TimedLoop timed_loop(const u32* insns, unsigned n) {
    if (n != 4) return {};
    const u32 cmp = insns[0], rd = insns[1], br = insns[2], ds = insns[3];
    // cmp: subcc %g0 <- rs1 - rs2 (registers).
    if (cmp >> 30 != 2 || ((cmp >> 19) & 0x3F) != 0x14 || ((cmp >> 25) & 31) || ((cmp >> 13) & 1)) return {};
    const unsigned a = (cmp >> 14) & 31, b = cmp & 31;
    // rdpr %tick or rd %tick into t.
    const unsigned rop3 = (rd >> 19) & 0x3F, t = (rd >> 25) & 31;
    if (rd >> 30 != 2 || (rop3 != 0x2A && rop3 != 0x28) || ((rd >> 14) & 31) != 4 || t == 0) return {};
    // BPcc on xcc back to the compare, with a nop in its delay slot.
    if (br >> 30 != 0 || ((br >> 22) & 7) != 1 || ((br >> 20) & 3) != 2 || (br & 0x7FFFF) != 0x7FFFE) return {};
    if (ds != 0x01000000) return {};
    const unsigned cond = (br >> 25) & 15;
    // The test holds while t is below x (or at it), and so fails for good once t passes it.
    if (b == t && a != t && a != 0 && (cond == 13 || cond == 12 || cond == 11 || cond == 10))  // cc, gu, ge, g
        return {a, t, cond, false};
    if (a == t && b != t && b != 0 && (cond == 5 || cond == 4 || cond == 3 || cond == 2))  // cs, leu, l, le
        return {b, t, cond, true};
    return {};
}

// A polling loop: blocks (up to 4, 32 instructions in all), each ending
// in a branch whose target is the next one's start and the last's the
// first's, made of integer operations, loads, reads of TICK and the
// branches, in which everything an iteration writes, every load's address
// and what each branch tests derive only from loads and from registers the
// loop does not write, with one exception: a clock (TICK, or a load from a
// device register that counts guest time) may be read if its reading is
// only compared, by the compare its block's branch tests on xcc, with such
// a value, the loop going on while the clock is short of it.
//
// Once an iteration leaves the registers and condition codes as the one
// before did (but for the clocks' readings and what their compares set),
// every later one does too while the loads read the same, and while they
// read memory or device registers that hold still (Bus::stable_until) the
// iterations that still go round, by the clocks, can be done at once.
struct PollInsn {
    enum Kind : u8 { Alu, Load, Tick, Branch } kind = Alu;
    u8 dst = 0;                 // the register written (0: none)
    u8 src[3] = {};             // the registers read (0: none)
    bool reads_cc = false, writes_cc = false;
};

// Decodes instruction `insn` for a polling loop; false if it may not be in one.
inline bool poll_insn(u32 insn, PollInsn& p) {
    p = {};
    const unsigned op = insn >> 30, rd = (insn >> 25) & 31, rs1 = (insn >> 14) & 31, rs2 = insn & 31;
    const bool imm = (insn >> 13) & 1;
    if (op == 0 && ((insn >> 22) & 7) == 4) {  // SETHI (and NOP)
        p.dst = static_cast<u8>(rd);
        return true;
    }
    const unsigned op3 = (insn >> 19) & 0x3F;
    if (op == 2) {
        if (op3 <= 0x07 || (op3 >= 0x10 && op3 <= 0x17) || op3 == 0x09 || (op3 >= 0x25 && op3 <= 0x27)) {
            p.dst = static_cast<u8>(rd), p.src[0] = static_cast<u8>(rs1), p.src[1] = static_cast<u8>(imm ? 0 : rs2);
            p.writes_cc = op3 >= 0x10 && op3 <= 0x17;
            return true;
        }
        if (op3 == 0x2C && ((insn >> 18) & 1)) {  // MOVcc on icc/xcc: keeps rd if not
            p.dst = static_cast<u8>(rd), p.src[0] = static_cast<u8>(rd), p.src[1] = static_cast<u8>(imm ? 0 : rs2);
            p.reads_cc = true;
            return true;
        }
        if (op3 == 0x2F) {  // MOVr
            p.dst = static_cast<u8>(rd), p.src[0] = static_cast<u8>(rs1), p.src[1] = static_cast<u8>(imm ? 0 : rs2);
            p.src[2] = static_cast<u8>(rd);
            return true;
        }
        if ((op3 == 0x28 || op3 == 0x2A) && rs1 == 4) {  // RD %tick, RDPR %tick
            p.kind = PollInsn::Tick, p.dst = static_cast<u8>(rd);
            return true;
        }
        return false;
    }
    if (op == 3) {
        const unsigned base = op3 & 0x0F;
        if (op3 >= 0x20 || (base != 0x00 && base != 0x01 && base != 0x02 && base != 0x08 && base != 0x09 && base != 0x0A &&
                            base != 0x0B))
            return false;
        p.kind = PollInsn::Load, p.dst = static_cast<u8>(rd), p.src[0] = static_cast<u8>(rs1);
        p.src[1] = static_cast<u8>(imm ? 0 : rs2);
        return true;
    }
    return false;
}

struct PollCycle {
    unsigned count = 0;        // instructions in one time round (0: not a polling loop)
    int last_start = 0;        // where the last block starts (instructions from the first's)
    int off[32] = {};          // each one's place, in instructions from the first block's start
    PollInsn p[32];
    u32 clock = 0;             // those reading a clock
    u32 clock_cmp = 0;         // the compares of a clock's reading
    u32 cond[32] = {};         // a branch's condition (Bicc/BPcc)
};

// The polling loop starting at code[0], its other blocks within
// code[lo, hi) (in instructions); count 0 if there is none.
inline PollCycle poll_cycle(const u32* code, int lo, int hi) {
    PollCycle c;
    int start = 0;
    unsigned n = 0;
    int starts[4];
    for (unsigned blk = 0;; ++blk) {
        if (blk == 4) return {};
        starts[blk] = start;
        int at = start;
        for (;; ++at) {  // to the branch
            if (at < lo || at + 1 >= hi || n + (at - start) + 2 > 32) return {};
            const u32 insn = code[at];
            const unsigned op = insn >> 30, op2 = (insn >> 22) & 7;
            if (op == 1 || (op == 0 && op2 != 0 && op2 != 4 && op2 != 7) || (op == 2 && (((insn >> 19) & 0x3F) == 0x38 ||
                                                                                          ((insn >> 19) & 0x3F) == 0x39)))
                break;
        }
        for (int i = start; i < at; ++i) {
            if (!poll_insn(code[i], c.p[n])) return {};
            c.off[n++] = i;
        }
        // The branch, and where it goes.
        const u32 br = code[at];
        if (br >> 30 != 0) return {};
        const unsigned op2 = (br >> 22) & 7;
        PollInsn b;
        b.kind = PollInsn::Branch;
        s64 disp;
        if (op2 == 2) {  // Bicc
            disp = static_cast<s64>(static_cast<s32>(br << 10) >> 10);
            c.cond[n] = (br >> 25) & 15;
            if (c.cond[n] == 0) return {};
            b.reads_cc = c.cond[n] != 8;
        } else if (op2 == 1) {  // BPcc
            if ((br >> 20) & 1) return {};
            disp = static_cast<s64>(static_cast<s32>(br << 13) >> 13);
            c.cond[n] = ((br >> 25) & 15) | (((br >> 21) & 1) ? 16 : 0);  // (16: on xcc)
            if ((c.cond[n] & 15) == 0) return {};
            b.reads_cc = (c.cond[n] & 15) != 8;
        } else if (op2 == 3) {  // BPr
            if ((br & 0x10000000) || ((br >> 25) & 3) == 0) return {};
            disp = static_cast<s64>(static_cast<s32>((((br >> 20) & 3) << 14 | (br & 0x3FFF)) << 16) >> 16);
            b.src[0] = static_cast<u8>((br >> 14) & 31);
        } else {
            return {};
        }
        c.p[n] = b;
        c.off[n++] = at;
        if (!poll_insn(code[at + 1], c.p[n])) return {};  // the delay slot
        c.off[n++] = at + 1;
        const int target = at + static_cast<int>(disp);
        if (target == 0) {  // back to the first
            c.last_start = start;
            break;
        }
        for (unsigned k = 0; k <= blk; ++k)
            if (starts[k] == target) return {};
        start = target;
    }
    c.count = n;
    // The instruction whose result a read at i sees: the last writer before
    // i, else the last in the loop (the previous iteration's), else none.
    auto def = [&](unsigned i, unsigned r) -> int {
        if (r == 0) return -1;
        for (int j = static_cast<int>(i) - 1; j >= 0; --j)
            if (c.p[j].dst == r) return j;
        for (int j = static_cast<int>(n) - 1; j >= static_cast<int>(i); --j)
            if (c.p[j].dst == r) return j;
        return -1;
    };
    auto def_cc = [&](unsigned i) -> int {
        for (int j = static_cast<int>(i) - 1; j >= 0; --j)
            if (c.p[j].writes_cc) return j;
        for (int j = static_cast<int>(n) - 1; j >= static_cast<int>(i); --j)
            if (c.p[j].writes_cc) return j;
        return -1;
    };
    auto is_cmp = [&](unsigned i) {  // subcc %g0, rs1, rs2 (registers)
        const u32 insn = code[c.off[i]];
        return insn >> 30 == 2 && ((insn >> 19) & 0x3F) == 0x14 && ((insn >> 25) & 31) == 0 && !((insn >> 13) & 1);
    };
    // Clock readings: TICK's, and loads read only by compares.
    for (unsigned i = 0; i < n; ++i) {
        if (c.p[i].kind == PollInsn::Tick) c.clock |= 1u << i;
        if (c.p[i].kind != PollInsn::Load || !c.p[i].dst) continue;
        bool only_cmp = true, read = false;
        for (unsigned j = 0; j < n; ++j)
            for (unsigned r : c.p[j].src)
                if (r && def(j, r) == static_cast<int>(i)) read = true, only_cmp = only_cmp && is_cmp(j);
        if (read && only_cmp) c.clock |= 1u << i;
    }
    // Their compares: of a clock's reading with a value not from a clock,
    // tested only by the branch that ends the block, on xcc, going round
    // while the clock is short of the value. A load compared otherwise is
    // read as any other (its device must then hold still); TICK may not be.
    auto cmp_ok = [&](unsigned i, bool clock_second) {
        unsigned readers = 0, branch = 0;
        for (unsigned j = 0; j < n; ++j)
            if (c.p[j].reads_cc && def_cc(j) == static_cast<int>(i)) ++readers, branch = j;
        if (readers != 1 || c.p[branch].kind != PollInsn::Branch) return false;
        for (unsigned j = i + 1; j < branch; ++j)
            if (c.p[j].writes_cc) return false;
        const unsigned cond = c.cond[branch];
        if (!(cond & 16)) return false;  // (icc: the low half of a clock wraps)
        const unsigned cc = cond & 15;
        return clock_second ? (cc == 13 || cc == 12 || cc == 11 || cc == 10)   // cmp x, clock: cc gu ge g
                            : (cc == 5 || cc == 4 || cc == 3 || cc == 2);      // cmp clock, x: cs leu l le
    };
    for (bool again = true; again;) {
        again = false;
        for (unsigned i = 0; i < n && !again; ++i) {
            if (!is_cmp(i)) continue;
            const int d0 = def(i, c.p[i].src[0]), d1 = def(i, c.p[i].src[1]);
            const bool k0 = d0 >= 0 && (c.clock >> d0 & 1), k1 = d1 >= 0 && (c.clock >> d1 & 1);
            if (!k0 && !k1) continue;
            if ((k0 && k1) || !cmp_ok(i, k1)) {  // not a clock's compare: its loads are read as any other
                for (const int d : {d0, d1}) {
                    if (d < 0 || !(c.clock >> d & 1)) continue;
                    if (c.p[d].kind == PollInsn::Tick) return {};
                    c.clock &= ~(1u << d);
                }
                again = true;
            }
        }
    }
    for (unsigned i = 0; i < n; ++i) {
        if (!is_cmp(i)) continue;
        const int d0 = def(i, c.p[i].src[0]), d1 = def(i, c.p[i].src[1]);
        if ((d0 >= 0 && (c.clock >> d0 & 1)) || (d1 >= 0 && (c.clock >> d1 & 1))) c.clock_cmp |= 1u << i;
    }
    // A clock's reading, and a value a clock is compared with, each come
    // from one instruction in the loop (or none).
    auto writers = [&](unsigned r) {
        unsigned w = 0;
        for (unsigned j = 0; j < n; ++j) w += r && c.p[j].dst == r;
        return w;
    };
    for (unsigned i = 0; i < n; ++i) {
        if ((c.clock >> i & 1) && writers(c.p[i].dst) > 1) return {};
        if (c.clock_cmp >> i & 1)
            for (unsigned r : c.p[i].src)
                if (writers(r) > 1) return {};
    }
    // A clock's reading goes nowhere else (a TICK reading may go nowhere).
    for (unsigned j = 0; j < n; ++j)
        for (unsigned r : c.p[j].src)
            if (const int d = def(j, r); d >= 0 && (c.clock >> d & 1) && !(c.clock_cmp >> j & 1)) return {};
    // Everything else settles: reads only settled values (none, loads'
    // results, settled instructions' results).
    bool settled[32] = {};
    for (bool more = true; more;) {
        more = false;
        for (unsigned i = 0; i < n; ++i) {
            if (settled[i] || (c.clock >> i & 1) || (c.clock_cmp >> i & 1)) continue;
            bool ok = true;
            for (unsigned r : c.p[i].src)
                if (const int d = def(i, r); d >= 0 && !settled[d] && c.p[d].kind != PollInsn::Load) ok = false;
            if (c.p[i].reads_cc)
                if (const int d = def_cc(i); d >= 0 && !settled[d] && !(c.clock_cmp >> d & 1)) ok = false;
            if (ok) settled[i] = more = true;
        }
    }
    for (unsigned i = 0; i < n; ++i) {
        if (c.clock >> i & 1) {  // (a clock load's address settles)
            if (c.p[i].kind != PollInsn::Load) continue;
            for (unsigned r : c.p[i].src)
                if (const int d = def(i, r); d >= 0 && !settled[d] && c.p[d].kind != PollInsn::Load) return {};
            continue;
        }
        if (c.clock_cmp >> i & 1) {  // (its other operand settles)
            for (unsigned r : c.p[i].src)
                if (const int d = def(i, r); d >= 0 && !(c.clock >> d & 1) && !settled[d] && c.p[d].kind != PollInsn::Load)
                    return {};
            continue;
        }
        if (!settled[i]) return {};
    }
    return c;
}

}  // namespace uv::usparc
