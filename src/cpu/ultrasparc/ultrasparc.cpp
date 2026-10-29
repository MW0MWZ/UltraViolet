// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
// UltraSPARC IIi integer unit: fetch, decode and execute, register windows,
// traps, interrupts and the privileged and ancillary state registers.
#include "cpu/ultrasparc/ultrasparc.h"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <limits>

#include "core/endian.h"
#include "core/log.h"
#include "cpu/ultrasparc/jit_loops.h"
#include "jit/exec_buffer.h"

namespace uv::usparc {

namespace {

constexpr u64 sext(u64 v, unsigned bits) {
    const u64 m = u64{1} << (bits - 1);
    v &= (m << 1) - 1;
    return (v ^ m) - m;
}

// Condition codes as a nibble: N Z V C in bits 3..0.
constexpr unsigned kN = 8, kZ = 4, kV = 2, kC = 1;

inline unsigned logic_cc32(u64 r) { return ((r >> 28) & kN) | (static_cast<u32>(r) == 0 ? kZ : 0); }
inline unsigned logic_cc64(u64 r) { return ((r >> 60) & kN) | (r == 0 ? kZ : 0); }
inline u8 logic_ccr(u64 r) { return static_cast<u8>((logic_cc64(r) << 4) | logic_cc32(r)); }

inline u8 add_ccr(u64 a, u64 b, u64 carry, u64 r) {
    const u64 c32 = ((a & 0xFFFFFFFFull) + (b & 0xFFFFFFFFull) + carry) >> 32;
    const u64 v32 = (((a ^ r) & (b ^ r)) >> 31) & 1;
    const u64 t = a + b;
    const u64 c64 = (t < a || t + carry < t) ? 1 : 0;
    const u64 v64 = ((a ^ r) & (b ^ r)) >> 63;
    const unsigned icc = logic_cc32(r) | (v32 ? kV : 0) | (c32 ? kC : 0);
    const unsigned xcc = logic_cc64(r) | (v64 ? kV : 0) | (c64 ? kC : 0);
    return static_cast<u8>((xcc << 4) | icc);
}

inline u8 sub_ccr(u64 a, u64 b, u64 borrow, u64 r) {
    const u64 c32 = (a & 0xFFFFFFFFull) < (b & 0xFFFFFFFFull) + borrow ? 1 : 0;
    const u64 v32 = (((a ^ b) & (a ^ r)) >> 31) & 1;
    const u64 c64 = (a < b || a - b < borrow) ? 1 : 0;
    const u64 v64 = ((a ^ b) & (a ^ r)) >> 63;
    const unsigned icc = logic_cc32(r) | (v32 ? kV : 0) | (c32 ? kC : 0);
    const unsigned xcc = logic_cc64(r) | (v64 ? kV : 0) | (c64 ? kC : 0);
    return static_cast<u8>((xcc << 4) | icc);
}

// Floating-point condition masks for FBfcc/FMOVcc, indexed by cond: bit n
// set if the condition holds when fcc = n (0 E, 1 L, 2 G, 3 U).
constexpr u8 kFccMask[16] = {0x0, 0xE, 0x6, 0xA, 0x2, 0xC, 0x4, 0x8, 0xF, 0x1, 0x9, 0x5, 0xD, 0x3, 0xB, 0x7};

bool rcond_holds(unsigned rcond, u64 v, bool& valid) {
    const s64 s = static_cast<s64>(v);
    valid = true;
    switch (rcond) {
        case 1: return s == 0;
        case 2: return s <= 0;
        case 3: return s < 0;
        case 5: return s != 0;
        case 6: return s > 0;
        case 7: return s >= 0;
        default: valid = false; return false;
    }
}

}  // namespace

UltraSparc::UltraSparc(Bus& bus, const Options& options)
    : bus_(bus), opt_(options), ecache_data_(options.ecache_bytes / 8), ecache_tag_(options.ecache_bytes / 64) {
    blocks_.resize(std::size_t{1} << kBlockBits);
    bus_.set_code_write_handler([this](const u8* page) { code_written(page); });
    bus_.set_remap_handler([this] { flush_blocks(); });
    set_jit(true);
    reset();
}

UltraSparc::~UltraSparc() {
    bus_.set_code_write_handler(nullptr);
    bus_.set_remap_handler(nullptr);
}

void UltraSparc::reset() {
    tl_ = kMaxTl;
    trap_[tl_].tt = tt::kPowerOnReset;
    trap_[tl_].tpc = pc_;
    trap_[tl_].tnpc = npc_;
    pstate_ = pstate::RED | pstate::PEF | pstate::PRIV | pstate::AG;
    cwp_ = 0;
    pil_ = 0;
    tick_npt_ = true;
    tick_offset_ = 0 - cycles();
    tick_cmpr_ = u64{1} << 63;
    tick_match_ = ~u64{0};
    softint_ = 0;
    lsu_control_ = 0;
    estate_error_en_ = 0;
    intr_receive_ = 0;
    fprs_ = 0;
    fsr_ = 0;
    halted_ = false;
    halt_reason_.clear();
    attention_ = true;
    update_globals();
    set_pc(kRstvAddr + 0x20);
}

void UltraSparc::update_globals() {
    unsigned set = 0;
    if (pstate_ & pstate::AG) set = 1;
    else if (pstate_ & pstate::MG) set = 2;
    else if (pstate_ & pstate::IG) set = 3;
    base_[0] = globals_[set].data();
    base_[1] = &windows_[window(1) * 16];  // the outs of window w are the ins of window w+1
    base_[2] = &windows_[cwp_ * 16 + 8];
    base_[3] = &windows_[cwp_ * 16];
}

void UltraSparc::set_pstate(u16 v) {
    pstate_ = v & pstate::kWritable;
    update_globals();
}

void UltraSparc::set_cwp(unsigned v) {
    cwp_ = v % kWindows;
    update_globals();
}

void UltraSparc::set_window_state(unsigned cansave, unsigned canrestore, unsigned cleanwin, unsigned otherwin,
                                  unsigned wstate) {
    cansave_ = cansave % kWindows;
    canrestore_ = canrestore % kWindows;
    cleanwin_ = cleanwin % kWindows;
    otherwin_ = otherwin % kWindows;
    wstate_ = wstate & 0x3F;
}

u64 UltraSparc::freg64(unsigned r) const {
    const unsigned n = (r & 0x1E) | ((r & 1) << 5);
    return (u64{f_[n]} << 32) | f_[n + 1];
}

void UltraSparc::set_freg64(unsigned r, u64 v) {
    const unsigned n = (r & 0x1E) | ((r & 1) << 5);
    f_[n] = static_cast<u32>(v >> 32);
    f_[n + 1] = static_cast<u32>(v);
}

u64 UltraSparc::tick() const {
    return ((cycles() + tick_offset_) & ~(u64{1} << 63)) | (tick_npt_ ? u64{1} << 63 : 0);
}

void UltraSparc::set_cycles_per_step(double cpi) {
    time_base_ = cycles();
    step_base_ = cycles_;
    cpi_fx_ = std::max<u64>(1, static_cast<u64>(cpi * 65536.0 + 0.5));
    update_tick_interrupt();
}

void UltraSparc::set_delay_cycles_per_step(double cpi) {
    delay_fx_ = cpi > 0 ? std::max<u64>(1, static_cast<u64>(cpi * 32768.0 + 0.5)) : 0;  // (half: see the header)
}

namespace {

// The condition codes of a - b (subcc).
u8 sub_codes(u64 a, u64 b) {
    auto codes = [](u64 x, u64 y, unsigned bits) {
        const u64 msb = u64{1} << (bits - 1), mask = (msb << 1) - 1;
        x &= mask, y &= mask;
        const u64 r = (x - y) & mask;
        return ((r & msb) ? 8u : 0u) | (r == 0 ? 4u : 0u) | (((x ^ y) & (x ^ r) & msb) ? 2u : 0u) | (x < y ? 1u : 0u);
    };
    return static_cast<u8>(codes(a, b, 64) << 4 | codes(a, b, 32));
}

}  // namespace

u64 UltraSparc::loop_at_once(u32 loop, u64 room, u64 next) {
    if ((loop >> 30) == 2) return wait_for_tick(loop, room, next);
    if (loop >> 30) return cycle_at_once(blocks_[loop & ((1u << kBlockBits) - 1)], (loop >> 16) & 0x3FFF, room, next);
    return count_down(loop, room);
}

u64 UltraSparc::tick_at_step(u64 step) const {
    const u64 now = time_base_ + (((step - step_base_) * cpi_fx_) >> 16);
    return ((now + tick_offset_) & ~(u64{1} << 63)) | (tick_npt_ ? u64{1} << 63 : 0);
}

u64 UltraSparc::cycle_at_once(const Block& b, unsigned back, u64 room, u64 next) {
    const int first = static_cast<int>(b.page_off) - static_cast<int>(back);  // (in its code page)
    constexpr int kPage = 1 << (Bus::kCodePageShift - 2);
    const u8* const host = b.host - 4 * back;
    u32 code[kPage];
    for (int i = 0; i < kPage - first; ++i) code[i] = load_be<u32>(host + 4 * i);
    const PollCycle c = poll_cycle(code, 0, kPage - first);
    const unsigned n = c.count;
    if (n == 0 || static_cast<unsigned>(c.last_start) != back) return 0;
    int last_cc = -1;  // the condition codes as an iteration leaves them come from here
    for (unsigned i = 0; i < n; ++i)
        if (c.p[i].writes_cc) last_cc = static_cast<int>(i);
    const bool clock_cc = last_cc >= 0 && (c.clock_cmp >> last_cc & 1);
    // Did this iteration leave the registers and codes (but for clocks')
    // as the one before?
    PollSnapshot now{b.host, next, clock_cc ? u8{0} : ccr_, 0, {}};
    for (unsigned i = 0; i < n && now.count < now.regs.size(); ++i)
        if (!(c.clock >> i & 1) && c.p[i].dst) now.regs[now.count++] = reg(c.p[i].dst);
    const bool same = poll_snap_.host == b.host && poll_snap_.next + n == next && poll_snap_.ccr == now.ccr &&
                      poll_snap_.count == now.count &&
                      std::equal(now.regs.begin(), now.regs.begin() + now.count, poll_snap_.regs.begin());
    poll_snap_ = now;
    if (!same || !(pstate_ & pstate::PRIV)) return 0;  // (reading TICK may trap)
    // The loads: each holds still until `until` (the last in an iteration
    // at `last`), or is a clock, read at the time of any step by reading it
    // with the step count set to that step.
    struct Load {
        Access acc;
        unsigned size = 0;
        bool sign = false, clock = false;
        u64 value = 0;  // (one that holds still)
    } loads[32];
    u64 until = Device::kForever;
    unsigned last = 0;
    for (unsigned i = 0; i < n; ++i) {
        if (c.p[i].kind != PollInsn::Load) continue;
        const u32 insn = code[c.off[i]];
        const unsigned op3 = (insn >> 19) & 0x3F, rs1 = (insn >> 14) & 31;
        const bool imm = (insn >> 13) & 1;
        u64 va = reg(rs1) + (imm ? static_cast<u64>(static_cast<s64>(static_cast<s32>(insn << 19) >> 19)) : reg(insn & 31));
        if (pstate_ & pstate::AM) va &= 0xFFFFFFFF;
        const u8 a = op3 & 0x10 ? static_cast<u8>(imm ? asi_ : (insn >> 5) & 0xFF) : implicit_asi();
        if (!asi_is_memory(a)) return 0;
        static constexpr unsigned kSizes[16] = {4, 1, 2, 0, 0, 0, 0, 0, 4, 1, 2, 8};
        Load& l = loads[i];
        l.size = kSizes[op3 & 0x0F];
        l.sign = (op3 & 0x0F) >= 0x08 && (op3 & 0x0F) <= 0x0A;
        if (!fast_device(va, a, AccessKind::Load, l.acc) && !translate_data(va, a, AccessKind::Load, l.size, l.acc)) return 0;
        const u64 st = bus_.stable_until(l.acc.pa, l.size);
        if (st == 0) return 0;
        if (st == Device::kClock || st == Device::kClockLittle) {
            // (a clock read as any other value, or in the byte order in which it does not count up)
            if (!(c.clock >> i & 1) || l.acc.little != (st == Device::kClockLittle)) return 0;
            l.clock = true;
            continue;
        }
        until = std::min(until, st);
        last = i;
        if ((c.clock >> i & 1) && !phys_read(l.acc, l.size, l.value)) return 0;
    }
    auto extend = [](const Load& l, u64 v) {
        if (!l.sign) return v;
        const unsigned bits = 8 * l.size;
        return static_cast<u64>((static_cast<s64>(v << (64 - bits))) >> (64 - bits));
    };
    // What instruction i's clock reads in iteration j (0: the one just run).
    auto clock_at = [&](unsigned i, u64 j) -> u64 {
        const u64 step = next + (j - 1) * n + i;
        if (c.p[i].kind == PollInsn::Tick) return tick_at_step(step);
        const Load& l = loads[i];
        if (!l.clock) return extend(l, l.value);
        const u64 saved = cycles_;
        cycles_ = step;
        u64 v = 0;
        phys_read(l.acc, l.size, v);
        cycles_ = saved;
        return extend(l, v);
    };
    auto def = [&](unsigned i, unsigned r) -> int {
        for (int k = static_cast<int>(i) - 1; k >= 0; --k)
            if (c.p[k].dst == r) return k;
        for (int k = static_cast<int>(n) - 1; k >= static_cast<int>(i); --k)
            if (c.p[k].dst == r) return k;
        return -1;
    };
    // The codes clock compare i sets in iteration j.
    auto cmp_codes = [&](unsigned i, u64 j) {
        u64 v[2];
        for (int s = 0; s < 2; ++s) {
            const unsigned r = c.p[i].src[s];
            const int d = r ? def(i, r) : -1;
            if (d >= 0 && (c.clock >> d & 1)) v[s] = clock_at(static_cast<unsigned>(d), d < static_cast<int>(i) ? j : j - 1);
            else v[s] = reg(r);  // (settled, from one instruction or none)
        }
        return sub_codes(v[0], v[1]);
    };
    // Iteration j goes all the way round: each clock compare's branch is
    // taken, and the loads that hold still are read before `until`.
    auto round = [&](u64 j) {
        if (until != Device::kForever && time_base_ + (((next + (j - 1) * n + last - step_base_) * cpi_fx_) >> 16) >= until)
            return false;
        for (unsigned i = 0; i < n; ++i) {
            if (!(c.clock_cmp >> i & 1)) continue;
            unsigned br = i + 1;
            while (c.p[br].kind != PollInsn::Branch) ++br;
            if (!cond_icc(c.cond[br] & 15, cmp_codes(i, j) >> 4)) return false;
        }
        return true;
    };
    u64 k = room / n;
    if (k == 0 || !round(1)) return 0;
    u64 lo = 1, hi = k;
    while (lo < hi) {
        const u64 mid = lo + (hi - lo + 1) / 2;
        if (round(mid)) lo = mid;
        else hi = mid - 1;
    }
    k = lo;
    // The state after k iterations: clocks' readings and their codes as
    // the last of them leaves them.
    for (unsigned i = 0; i < n; ++i)
        if ((c.clock >> i & 1) && c.p[i].dst) set_reg(c.p[i].dst, clock_at(i, k));
    if (clock_cc) ccr_ = cmp_codes(static_cast<unsigned>(last_cc), k);
    delay_steps_ += k * n;
    delay_time_fx_ += k * n * cpi_fx_;
    poll_snap_.next += k * n;
    return k * n;
}


u64 UltraSparc::wait_for_tick(u32 loop, u64 room, u64 next) {
    if (!(pstate_ & pstate::PRIV)) return 0;  // (reading TICK may trap)
    const unsigned xr = loop & 31, tr = (loop >> 5) & 31, cond = (loop >> 10) & 15;
    const bool t_first = loop & (1u << 14);
    const u64 x = reg(xr);
    // TICK as the read in iteration j (from 1) would see it: its step is
    // next + 4j - 3.
    auto tick_at = [&](u64 j) { return tick_at_step(next + 4 * j - 3); };
    auto codes = [&](u64 t) { return t_first ? sub_codes(t, x) : sub_codes(x, t); };
    auto taken = [&](u64 t) { return cond_icc(cond, codes(t) >> 4); };
    // Iteration j is taken if the TICK read in iteration j - 1 passes the
    // test (iteration 0 is the one just run); the test holds until TICK
    // passes x, so the iterations taken are a run from the first.
    const u64 cap = room / 4;
    u64 t_prev = reg(tr);
    if (cap == 0 || !taken(t_prev)) return 0;
    u64 lo = 1, hi = cap;  // iteration lo is taken; find the last one that is
    while (lo < hi) {
        const u64 mid = lo + (hi - lo + 1) / 2;
        if (taken(tick_at(mid - 1))) lo = mid;
        else hi = mid - 1;
    }
    const u64 k = lo;
    set_reg(tr, tick_at(k));
    ccr_ = codes(k > 1 ? tick_at(k - 1) : t_prev);
    delay_steps_ += 4 * k;
    delay_time_fx_ += 4 * k * cpi_fx_;
    return 4 * k;
}

u64 UltraSparc::count_down(u32 loop, u64 room) {
    const unsigned r = loop & 31;
    const bool sign = loop & 32, word = loop & 64;
    const u64 v = reg(r);
    const u64 c = !word ? v : sign ? static_cast<u64>(static_cast<s64>(static_cast<s32>(v))) : static_cast<u32>(v);
    if (sign ? static_cast<s64>(c) <= 0 : c == 0) return 0;
    if (sign && c == (word ? u64{0x7FFFFFFF} : u64{0x7FFFFFFFFFFFFFFF})) return 0;  // (from an overflow: V set, not taken)
    // The iterations that fit in the steps left, and at a charge above the
    // usual one, in the guest time those steps would take.
    const u64 cost = delay_fx_ ? delay_fx_ : cpi_fx_;
    u64 fit = room / 2;
    if (cost > cpi_fx_) fit = std::min<u64>(room, u64{1} << 40) * cpi_fx_ / (2 * cost);
    const u64 k = std::min(c, fit);
    if (k < 2) return 0;
    const u64 steps = 2 * (k - 1), res = v - (k - 1), x = res + 1;
    set_reg(r, res);
    if (loop & 128) {  // the codes the last of them set: x - 1 or x + -1
        const bool add = loop & 256;
        auto codes = [add](u64 res_, u64 x_, unsigned bits) {
            const u64 msb = u64{1} << (bits - 1), mask = (msb << 1) - 1;
            res_ &= mask, x_ &= mask;
            const unsigned cn = (res_ & msb) ? 8 : 0, cz = res_ == 0 ? 4 : 0, cv = x_ == msb ? 2 : 0;
            const unsigned cc = add ? (x_ != 0 ? 1 : 0) : (x_ == 0 ? 1 : 0);
            return cn | cz | cv | cc;
        };
        ccr_ = static_cast<u8>(codes(res, x, 64) << 4 | codes(res, x, 32));
    }
    delay_steps_ += steps;
    delay_time_fx_ += steps * cost;
    if (cost != cpi_fx_) {  // (the steps are charged the usual rate through cycles_: the difference goes in time_base_)
        const s64 t = static_cast<s64>(steps) * (static_cast<s64>(cost) - static_cast<s64>(cpi_fx_)) + static_cast<s64>(delay_frac_);
        time_base_ += static_cast<u64>(t >> 16);
        delay_frac_ = static_cast<u64>(t & 0xFFFF);
        delay_charged_ = true;
    }
    return steps;
}

u64 UltraSparc::steps_until(u64 when) const {
    if (when <= cycles()) return 1;
    // The first step whose time reaches `when`, counted from the base.
    const u64 d = when - time_base_;
    if (d >= u64{1} << 47) return u64{1} << 62;  // (beyond any run)
    const u64 from_base = ((d << 16) + cpi_fx_ - 1) / cpi_fx_;
    return std::max<u64>(1, from_base - (cycles_ - step_base_));
}

void UltraSparc::update_tick_interrupt() {
    if (tick_cmpr_ >> 63) {
        tick_match_ = ~u64{0};
        return;
    }
    const u64 now = (cycles() + tick_offset_) & ~(u64{1} << 63);
    const u64 cmp = tick_cmpr_ & ~(u64{1} << 63);
    tick_match_ = cmp > now ? cycles_ + steps_until(cycles() + (cmp - now)) : ~u64{0};
}

bool UltraSparc::deliver_interrupt_vector(u64 d0, u64 d1, u64 d2) {
    if (intr_receive_ & 0x20) return false;
    intr_data_ = {d0, d1, d2};
    intr_receive_ |= 0x20;
    attention_ = true;
    return true;
}

unsigned UltraSparc::fcc(unsigned n) const {
    if (n == 0) return static_cast<unsigned>(fsr_ >> 10) & 3;
    return static_cast<unsigned>(fsr_ >> (30 + 2 * n)) & 3;
}

bool UltraSparc::cond_icc(unsigned cond, unsigned cc) const {
    const bool n = cc & kN, z = cc & kZ, v = cc & kV, c = cc & kC;
    bool r;
    switch (cond & 7) {
        case 0: r = false; break;
        case 1: r = z; break;
        case 2: r = z || (n != v); break;
        case 3: r = n != v; break;
        case 4: r = c || z; break;
        case 5: r = c; break;
        case 6: r = n; break;
        default: r = v; break;
    }
    return (cond & 8) ? !r : r;
}

bool UltraSparc::cond_fcc(unsigned cond, unsigned f) const { return (kFccMask[cond & 15] >> f) & 1; }

void UltraSparc::enter_error_state(const char* why) {
    UV_LOG(Error, "cpu", "error_state: %s at pc %016llx", why, static_cast<unsigned long long>(pc_));
    halted_ = true;
    halt_reason_ = std::string("error_state: ") + why;
}

void UltraSparc::take_trap(unsigned type) {
    trapped_ = true;
    if (tl_ == kMaxTl && type > tt::kRedState) {
        enter_error_state("trap at TL = MAXTL");
        return;
    }
    const unsigned old_tl = tl_;
    if (type >= tt::kPowerOnReset && type <= tt::kSoftwareInitiatedReset) {
        tl_ = type == tt::kPowerOnReset ? kMaxTl : (tl_ < kMaxTl ? tl_ + 1 : kMaxTl);
    } else {
        ++tl_;
    }
    TrapLevel& t = trap_[tl_];
    t.tstate = (u64{ccr_} << 32) | (u64{asi_} << 24) | (u64{pstate_} << 8) | cwp_;
    t.tpc = pc_;
    t.tnpc = npc_;
    t.tt = type;

    u16 ps = pstate_ & (pstate::MM | pstate::RED | pstate::TLE);
    ps |= pstate::PEF | pstate::PRIV;
    if (ps & pstate::TLE) ps |= pstate::CLE;
    if (type == tt::kInterruptVector) ps |= pstate::IG;
    else if (type >= tt::kFastInstructionAccessMmuMiss && type <= tt::kFastDataAccessProtection + 3) ps |= pstate::MG;
    else ps |= pstate::AG;
    const bool reset = type >= tt::kPowerOnReset && type <= tt::kSoftwareInitiatedReset;
    if (reset || tl_ == kMaxTl) ps |= pstate::RED;
    pstate_ = ps;

    if (type == tt::kCleanWindow) {
        cwp_ = window(1);
    } else if ((type & 0x1C0) == tt::kSpillNormal) {
        cwp_ = window(static_cast<int>(cansave_) + 2);
    } else if ((type & 0x1C0) == tt::kFillNormal) {
        cwp_ = window(-1);
    }
    update_globals();

    u64 vector;
    if (reset) {
        vector = kRstvAddr + (type << 5);
    } else if (tl_ == kMaxTl) {
        vector = kRstvAddr + (tt::kRedState << 5);
    } else {
        vector = (tba_ & ~u64{0x7FFF}) | (old_tl > 0 ? 0x4000 : 0) | (u64{type} << 5);
    }
    pc_ = vector;
    npc_ = vector + 4;
    if (trap_hook) trap_hook(*this, type);
}

void UltraSparc::check_interrupts() {
    if (!(pstate_ & pstate::IE)) return;
    if (intr_receive_ & 0x20) {
        take_trap(tt::kInterruptVector);
        return;
    }
    const u64 levels = (softint_ | ((softint_ & 1) << 14)) & 0xFFFE;
    if (levels == 0) return;
    const unsigned level = 63 - static_cast<unsigned>(std::countl_zero(levels));
    if (level > pil_) take_trap(tt::kInterruptLevel1 + level - 1);
}

void UltraSparc::run(u64 instructions) {
    if (block_cache_ && !trace_hook) return run_blocks(instructions);
    for (u64 i = 0; i < instructions && !halted_; ++i) step();
}

// The same as step() repeated, except that the instructions of a block are
// fetched once. Inside a block, interrupts and TICK_COMPARE are not looked
// at: any instruction that could change them sets attention_, which ends
// the block, as does any change of control flow.
void UltraSparc::run_blocks(u64 instructions) {
    u64 done = 0;
    while (done < instructions && !halted_) {
        trapped_ = false;
        if (UV_UNLIKELY(cycles_ >= tick_match_)) {
            softint_ |= 1;
            tick_match_ = ~u64{0};
        }
        check_interrupts();
        ++done;
        if (trapped_) {
            trapped_ = false;
            continue;
        }
        ++cycles_;
        const u64 page = pc_ >> 13, off = pc_ & 0x1FFF;
        FetchEntry& f = fetch_cache_[page & (kFetchEntries - 1)];
        const u16 mode = fetch_mode();
        u64 pa;
        const u8* host;
        if (f.va_page == page && f.gen == soft_gen_ && f.mode == mode) {
            pa = f.pa + off;
            host = f.host ? f.host + off : nullptr;
        } else {
            if (!translate_fetch(pc_, pa)) continue;
            host = bus_.host_read_ptr(pa);
            f = {page, soft_gen_, mode, host ? host - off : nullptr, pa - off};
        }
        if (!host) {
            u32 insn;
            if (!bus_.read(pa, insn)) {
                take_trap(tt::kInstructionAccessError);
                continue;
            }
            execute(insn);
            if (!trapped_) ++instret_;
            continue;
        }
        Block& b = blocks_[block_index(host)];
        if (b.host != host) decode_block(b, host, pa);
        if (b.code) {
            // The translation runs the whole block, so it must fit in what
            // is left of the run and before TICK_COMPARE matches; it starts
            // with npc = pc + 4 (not in a delay slot).
            const u64 room = std::min(instructions - done, tick_match_ > cycles_ ? tick_match_ - cycles_ : 0);
            if (b.length <= room + 1 && npc_ == pc_ + 4 && b.jit_mode == jit_mode()) {
                attention_ = false;
                const u64 cycles0 = cycles_, instret0 = instret_;
                const u32 k = jit_check_ && b.jit_self ? jit_run_checked(b) : b.code(this, pc_, room + 1);
                done += k - 1;
                cycles_ = cycles0 + k - 1;
                instret_ = instret0 + k - (trapped_ ? 1 : 0);
                if (UV_UNLIKELY(delay_charged_)) {  // (see set_delay_cycles_per_step)
                    delay_charged_ = false;
                    update_tick_interrupt();
                    return;
                }
                continue;
            }
        } else if (jit_buf_ && ++b.runs == kJitThreshold) {
            jit_compile(b);
        }
        u64 next = pc_;
        for (unsigned i = 0;;) {
            attention_ = false;
            execute(b.insns[i]);
            if (trapped_) break;
            ++instret_;
            next += 4;
            if (attention_ || ++i == b.length || pc_ != next || done == instructions || halted_ || cycles_ >= tick_match_)
                break;
            ++done;
            ++cycles_;
        }
        // A delay loop, back at its start (with no recompiler: one
        // translates the loop, which then does this itself).
        const u64 loop_back = (b.loop >> 30) == 3 ? 4 * ((b.loop >> 16) & 0x3FFF) : 0;
        if (b.loop && !jit_buf_ && !trapped_ && !attention_ && pc_ == next - 4 * b.length - loop_back && npc_ == pc_ + 4) {
            const u64 room = std::min(instructions - done, tick_match_ > cycles_ ? tick_match_ - cycles_ : 0);
            const u64 k = loop_at_once(b.loop, room, cycles_ + 1);
            done += k;
            cycles_ += k;
            instret_ += k;
            if (UV_UNLIKELY(delay_charged_)) {
                delay_charged_ = false;
                update_tick_interrupt();
                return;
            }
        }
    }
}

void UltraSparc::decode_block(Block& b, const u8* host, u64 pa) {
    // Up to the end of the code page, or the delay slot of the first
    // control transfer (DONE, RETRY and Tcc have none).
    const unsigned room = static_cast<unsigned>(((u64{1} << Bus::kCodePageShift) - (pa & ((u64{1} << Bus::kCodePageShift) - 1))) >> 2);
    const unsigned max = room < kMaxBlockLength ? room : kMaxBlockLength;
    unsigned n = 0, end = max;
    while (n < end) {
        const u32 insn = load_be<u32>(host + 4 * n);
        b.insns[n++] = insn;
        const unsigned op = insn >> 30, op2 = (insn >> 22) & 7, op3 = (insn >> 19) & 0x3F;
        if (op == 1 || (op == 0 && op2 != 0 && op2 != 4 && op2 != 7) || (op == 2 && (op3 == 0x38 || op3 == 0x39))) {
            end = n + 1 < max ? n + 1 : max;  // and its delay slot
        } else if (op == 2 && (op3 == 0x3A || op3 == 0x3E)) {
            break;
        }
    }
    b.host = host;
    b.length = n;
    b.code = nullptr;
    b.runs = 0;
    b.loop = counted_loop(b.insns.data(), n).code() | timed_loop(b.insns.data(), n).code();
    b.page_off = static_cast<u16>((pa & ((u64{1} << Bus::kCodePageShift) - 1)) >> 2);
    // The last block of a polling loop: its branch goes back to its own
    // start or earlier in its code page, to a loop that comes round to it.
    if (!b.loop && n >= 2) {
        const u32 br = b.insns[n - 2];
        const unsigned op2 = (br >> 22) & 7;
        s64 disp = 0;
        if (br >> 30 == 0 && op2 == 2) disp = static_cast<s32>(br << 10) >> 10;
        else if (br >> 30 == 0 && op2 == 1) disp = static_cast<s32>(br << 13) >> 13;
        else if (br >> 30 == 0 && op2 == 3) disp = static_cast<s32>((((br >> 20) & 3) << 14 | (br & 0x3FFF)) << 16) >> 16;
        const s64 back = -(static_cast<s64>(n) - 2 + disp);
        if (disp != 0 && back >= 0 && back <= b.page_off) {
            constexpr int kPage = 1 << (Bus::kCodePageShift - 2);
            const int first = b.page_off - static_cast<int>(back);
            u32 code[kPage];
            for (int i = 0; i < kPage - first; ++i) code[i] = load_be<u32>(host - 4 * back + 4 * i);
            const PollCycle c = poll_cycle(code, 0, kPage - first);
            if (c.count && c.last_start == back) b.loop = 3u << 30 | static_cast<u32>(back) << 16 | block_index(host);
        }
    }
    if (u8* const flag = bus_.code_flag(host)) *flag = 1;
}

void UltraSparc::code_written(const u8* page) {
    constexpr std::size_t kPage = std::size_t{1} << Bus::kCodePageShift;
    for (std::size_t off = 0; off < kPage; off += 4) {
        Block& b = blocks_[block_index(page + off)];
        if (b.host >= page && b.host < page + kPage) b.host = nullptr, b.code = nullptr;
    }
    if (u8* const flag = bus_.code_flag(page)) *flag = 0;
    attention_ = true;
}

// Host memory was remapped: decoded blocks and cached fetch translations
// may point at memory that is no longer there.
void UltraSparc::flush_blocks() {
    for (Block& b : blocks_) b.host = nullptr, b.code = nullptr;
    flush_soft_tlb();
}

void UltraSparc::set_jit(bool enabled, std::size_t buffer) {
    for (Block& b : blocks_) b.code = nullptr, b.runs = 0;
    jit_buf_.reset();
    jit_stub_seq_ = jit_stub_dyn_ = nullptr;
    if (!enabled || !jit_compile_supported()) return;
    jit_buf_ = std::make_unique<jit::ExecBuffer>(buffer);
    if (!jit_buf_->ok()) jit_buf_.reset();
}

// Runs a translated block, then the same instructions through execute()
// from the same state, and compares the two.
u32 UltraSparc::jit_run_checked(Block& b) {
    const auto globals = globals_;
    const auto windows = windows_;
    const u8 ccr = ccr_;
    const u64 pc = pc_, npc = npc_;
    const u32 k = b.code(this, pc_, b.length);  // (this block only)
    if (trapped_) return k;
    const auto j_globals = globals_;
    const auto j_windows = windows_;
    const u8 j_ccr = ccr_;
    const u64 j_pc = pc_, j_npc = npc_;
    globals_ = globals, windows_ = windows, ccr_ = ccr, pc_ = pc, npc_ = npc;
    for (u32 i = 0; i < k; ++i) execute(b.insns[i]);
    if (j_pc == pc_ && j_npc == npc_ && j_ccr == ccr_ && j_globals == globals_ && j_windows == windows_) return k;
    std::string diff;
    char buf[160];
    auto note = [&](const char* what, u64 jit, u64 interp) {
        if (jit == interp) return;
        std::snprintf(buf, sizeof buf, " %s jit %llx interp %llx;", what, static_cast<unsigned long long>(jit),
                      static_cast<unsigned long long>(interp));
        diff += buf;
    };
    note("pc", j_pc, pc_);
    note("npc", j_npc, npc_);
    note("ccr", j_ccr, ccr_);
    for (unsigned s = 0; s < globals_.size(); ++s)
        for (unsigned r = 0; r < 8; ++r) note(("g" + std::to_string(r) + "/" + std::to_string(s)).c_str(), j_globals[s][r], globals_[s][r]);
    for (unsigned r = 0; r < windows_.size(); ++r) note(("w" + std::to_string(r)).c_str(), j_windows[r], windows_[r]);
    {
        std::snprintf(buf, sizeof buf, "jit check: block at pc %llx (%u of %u instructions):", static_cast<unsigned long long>(pc),
                      k, b.length);
        halt_reason_ = buf + diff;
        for (unsigned i = 0; i < b.length; ++i) {
            std::snprintf(buf, sizeof buf, " [%08x]", b.insns[i]);
            halt_reason_ += buf;
        }
        UV_LOG(Error, "cpu", "%s", halt_reason_.c_str());
        halted_ = true;
    }
    return k;
}

bool UltraSparc::jit_execute(UltraSparc* cpu, u32 insn) {
    cpu->execute(insn);
    return cpu->trapped_ || cpu->attention_ || cpu->halted_;
}

void UltraSparc::step() {
    trapped_ = false;
    if (UV_UNLIKELY(cycles_ >= tick_match_)) {
        softint_ |= 1;
        tick_match_ = ~u64{0};
    }
    check_interrupts();
    if (trapped_) {
        trapped_ = false;
        return;
    }
    if (trace_hook) trace_hook(*this);
    ++cycles_;
    u64 pa;
    if (!translate_fetch(pc_, pa)) return;
    u32 insn;
    if (!bus_.read(pa, insn)) {
        take_trap(tt::kInstructionAccessError);
        return;
    }
    execute(insn);
    if (!trapped_) ++instret_;
}

bool UltraSparc::read_pr(unsigned r, u64& v) {
    if (!(pstate_ & pstate::PRIV)) {
        take_trap(tt::kPrivilegedOpcode);
        return false;
    }
    if (r <= 3 && tl_ == 0) {
        take_trap(tt::kIllegalInstruction);
        return false;
    }
    switch (r) {
        case 0: v = trap_[tl_].tpc; return true;
        case 1: v = trap_[tl_].tnpc; return true;
        case 2: v = trap_[tl_].tstate; return true;
        case 3: v = trap_[tl_].tt; return true;
        case 4: v = tick(); return true;
        case 5: v = tba_; return true;
        case 6: v = pstate_; return true;
        case 7: v = tl_; return true;
        case 8: v = pil_; return true;
        case 9: v = cwp_; return true;
        case 10: v = cansave_; return true;
        case 11: v = canrestore_; return true;
        case 12: v = cleanwin_; return true;
        case 13: v = otherwin_; return true;
        case 14: v = wstate_; return true;
        case 31: v = opt_.version; return true;
        default: take_trap(tt::kIllegalInstruction); return false;
    }
}

bool UltraSparc::write_pr(unsigned r, u64 v) {
    attention_ = true;
    if (!(pstate_ & pstate::PRIV)) {
        take_trap(tt::kPrivilegedOpcode);
        return false;
    }
    if (r <= 3 && tl_ == 0) {
        take_trap(tt::kIllegalInstruction);
        return false;
    }
    switch (r) {
        case 0: trap_[tl_].tpc = v; return true;
        case 1: trap_[tl_].tnpc = v; return true;
        case 2: trap_[tl_].tstate = v & 0xFFFF0FFF1Full; return true;
        case 3: trap_[tl_].tt = static_cast<unsigned>(v) & 0x1FF; return true;
        case 4:
            tick_npt_ = v >> 63;
            tick_offset_ = (v & ~(u64{1} << 63)) - cycles();
            update_tick_interrupt();
            return true;
        case 5: tba_ = v & ~u64{0x7FFF}; return true;
        case 6: set_pstate(static_cast<u16>(v)); return true;
        case 7: tl_ = static_cast<unsigned>(v & 7) > kMaxTl ? kMaxTl : static_cast<unsigned>(v & 7); return true;
        case 8: pil_ = static_cast<unsigned>(v) & 15; return true;
        case 9: set_cwp(static_cast<unsigned>(v) & 7); return true;
        case 10: cansave_ = static_cast<unsigned>(v) & 7; return true;
        case 11: canrestore_ = static_cast<unsigned>(v) & 7; return true;
        case 12: cleanwin_ = static_cast<unsigned>(v) & 7; return true;
        case 13: otherwin_ = static_cast<unsigned>(v) & 7; return true;
        case 14: wstate_ = static_cast<unsigned>(v) & 0x3F; return true;
        default: take_trap(tt::kIllegalInstruction); return false;
    }
}

bool UltraSparc::read_asr(unsigned r, u64& v) {
    const bool priv = pstate_ & pstate::PRIV;
    switch (r) {
        case 0: v = y_; return true;
        case 2: v = ccr_; return true;
        case 3: v = asi_; return true;
        case 4:
            if (tick_npt_ && !priv) {
                take_trap(tt::kPrivilegedAction);
                return false;
            }
            v = tick();
            return true;
        case 5: v = mask_address(pc_); return true;
        case 6: v = fprs_; return true;
        case 16:  // PCR
            if (!priv) {
                take_trap(tt::kPrivilegedAction);
                return false;
            }
            v = pcr_;
            return true;
        case 17:  // PIC; privileged when PCR.PRIV
            if ((pcr_ & 1) && !priv) {
                take_trap(tt::kPrivilegedAction);
                return false;
            }
            v = pic_;
            return true;
        case 18:
        case 22:
        case 23:
            if (!priv) {
                take_trap(tt::kPrivilegedOpcode);
                return false;
            }
            v = r == 18 ? dcr_ : r == 22 ? softint_ : tick_cmpr_;
            return true;
        case 19:
            if (!fp_enabled()) {
                take_trap(tt::kFpDisabled);
                return false;
            }
            v = gsr_;
            return true;
        default: take_trap(tt::kIllegalInstruction); return false;
    }
}

bool UltraSparc::write_asr(unsigned r, u64 v) {
    attention_ = true;
    const bool priv = pstate_ & pstate::PRIV;
    switch (r) {
        case 0: y_ = v & 0xFFFFFFFFull; return true;
        case 2: ccr_ = static_cast<u8>(v); return true;
        case 3: asi_ = static_cast<u8>(v); return true;
        case 6: fprs_ = static_cast<u8>(v & 7); return true;
        case 16:
            if (!priv) {
                take_trap(tt::kPrivilegedAction);
                return false;
            }
            pcr_ = v;
            return true;
        case 17:
            if ((pcr_ & 1) && !priv) {
                take_trap(tt::kPrivilegedAction);
                return false;
            }
            pic_ = v;
            return true;
        case 19:
            if (!fp_enabled()) {
                take_trap(tt::kFpDisabled);
                return false;
            }
            gsr_ = v & 0x7F;
            return true;
        case 18:
        case 20:
        case 21:
        case 22:
        case 23:
            if (!priv) {
                take_trap(tt::kPrivilegedOpcode);
                return false;
            }
            if (r == 18) dcr_ = v;
            else if (r == 20) softint_ |= v & 0x1FFFF;
            else if (r == 21) softint_ &= ~(v & 0x1FFFF);
            else if (r == 22) softint_ = v & 0x1FFFF;
            else {
                tick_cmpr_ = v;
                update_tick_interrupt();
            }
            return true;
        default: take_trap(tt::kIllegalInstruction); return false;
    }
}

void UltraSparc::execute(u32 insn) {
    const unsigned op = insn >> 30;
    const unsigned rd = (insn >> 25) & 31;
    u64 next_pc = npc_, next_npc = npc_ + 4;

    // Branch: `taken`, annul bit, and whether the condition is "always".
    auto branch = [&](bool taken, bool annul, bool always, s64 disp) {
        const u64 target = mask_address(pc_ + static_cast<u64>(disp));
        if (taken) {
            if (annul && always) {
                next_pc = target;
                next_npc = target + 4;
            } else {
                next_npc = target;
            }
        } else if (annul) {
            next_pc = npc_ + 4;
            next_npc = npc_ + 8;
        }
    };

    switch (op) {
        case 1: {  // CALL
            const s64 disp = static_cast<s64>(sext(insn, 30) << 2);
            set_reg(15, mask_address(pc_));
            next_npc = mask_address(pc_ + static_cast<u64>(disp));
            break;
        }
        case 0: {
            const unsigned op2 = (insn >> 22) & 7;
            const bool annul = (insn >> 29) & 1;
            const unsigned cond = (insn >> 25) & 15;
            switch (op2) {
                case 1: {  // BPcc
                    const unsigned cc = (insn >> 20) & 3;
                    if (cc & 1) return take_trap(tt::kIllegalInstruction);
                    const unsigned nibble = cc == 2 ? ccr_ >> 4 : ccr_ & 15;
                    branch(cond_icc(cond, nibble), annul, cond == 8, static_cast<s64>(sext(insn, 19) << 2));
                    break;
                }
                case 2:  // Bicc
                    branch(cond_icc(cond, ccr_ & 15), annul, cond == 8, static_cast<s64>(sext(insn, 22) << 2));
                    break;
                case 3: {  // BPr
                    if ((insn >> 28) & 1) return take_trap(tt::kIllegalInstruction);
                    bool valid;
                    const bool taken = rcond_holds((insn >> 25) & 7, reg((insn >> 14) & 31), valid);
                    if (!valid) return take_trap(tt::kIllegalInstruction);
                    const u64 d16 = (((insn >> 20) & 3) << 14) | (insn & 0x3FFF);
                    branch(taken, annul, false, static_cast<s64>(sext(d16, 16) << 2));
                    break;
                }
                case 4:  // SETHI
                    set_reg(rd, u64{insn & 0x3FFFFF} << 10);
                    break;
                case 5:    // FBPfcc
                case 6: {  // FBfcc
                    if (!fp_enabled()) return take_trap(tt::kFpDisabled);
                    const unsigned n = op2 == 5 ? (insn >> 20) & 3 : 0;
                    const s64 disp = op2 == 5 ? static_cast<s64>(sext(insn, 19) << 2) : static_cast<s64>(sext(insn, 22) << 2);
                    branch(cond_fcc(cond, fcc(n)), annul, cond == 8, disp);
                    break;
                }
                default: return take_trap(tt::kIllegalInstruction);  // ILLTRAP and the unused op2 values
            }
            break;
        }
        case 2: {
            const unsigned op3 = (insn >> 19) & 0x3F;
            const unsigned rs1 = (insn >> 14) & 31;
            const bool imm = (insn >> 13) & 1;
            const u64 a = reg(rs1);
            const u64 b = imm ? sext(insn, 13) : reg(insn & 31);
            switch (op3) {
                case 0x00: set_reg(rd, a + b); break;
                case 0x01: set_reg(rd, a & b); break;
                case 0x02: set_reg(rd, a | b); break;
                case 0x03: set_reg(rd, a ^ b); break;
                case 0x04: set_reg(rd, a - b); break;
                case 0x05: set_reg(rd, a & ~b); break;
                case 0x06: set_reg(rd, a | ~b); break;
                case 0x07: set_reg(rd, ~(a ^ b)); break;
                case 0x08: set_reg(rd, a + b + (ccr_ & kC)); break;
                case 0x09: set_reg(rd, a * b); break;
                case 0x0A: {  // UMUL
                    const u64 r = (a & 0xFFFFFFFFull) * (b & 0xFFFFFFFFull);
                    y_ = r >> 32;
                    set_reg(rd, r);
                    break;
                }
                case 0x0B: {  // SMUL
                    const u64 r = static_cast<u64>(static_cast<s64>(static_cast<s32>(a)) * static_cast<s32>(b));
                    y_ = r >> 32;
                    set_reg(rd, r);
                    break;
                }
                case 0x0C: set_reg(rd, a - b - (ccr_ & kC)); break;
                case 0x0D:  // UDIVX
                    if (b == 0) return take_trap(tt::kDivisionByZero);
                    set_reg(rd, a / b);
                    break;
                case 0x0E:
                case 0x1E: {  // UDIV, UDIVcc
                    const u64 divisor = b & 0xFFFFFFFFull;
                    if (divisor == 0) return take_trap(tt::kDivisionByZero);
                    u64 q = ((y_ << 32) | (a & 0xFFFFFFFFull)) / divisor;
                    const bool overflow = q > 0xFFFFFFFFull;
                    if (overflow) q = 0xFFFFFFFFull;
                    if (op3 == 0x1E) ccr_ = static_cast<u8>((logic_cc64(q) << 4) | logic_cc32(q) | (overflow ? kV : 0));
                    set_reg(rd, q);
                    break;
                }
                case 0x0F:
                case 0x1F: {  // SDIV, SDIVcc
                    const s64 divisor = static_cast<s32>(b);
                    if (divisor == 0) return take_trap(tt::kDivisionByZero);
                    const s64 dividend = static_cast<s64>((y_ << 32) | (a & 0xFFFFFFFFull));
                    s64 q;
                    if (dividend == std::numeric_limits<s64>::min() && divisor == -1) q = std::numeric_limits<s64>::max();
                    else q = dividend / divisor;
                    bool overflow = false;
                    if (q > 0x7FFFFFFFll) {
                        q = 0x7FFFFFFFll;
                        overflow = true;
                    } else if (q < -0x80000000ll) {
                        q = -0x80000000ll;
                        overflow = true;
                    }
                    const u64 r = static_cast<u64>(q);
                    if (op3 == 0x1F) ccr_ = static_cast<u8>((logic_cc64(r) << 4) | logic_cc32(r) | (overflow ? kV : 0));
                    set_reg(rd, r);
                    break;
                }
                case 0x10: {
                    const u64 r = a + b;
                    ccr_ = add_ccr(a, b, 0, r);
                    set_reg(rd, r);
                    break;
                }
                case 0x11: ccr_ = logic_ccr(a & b); set_reg(rd, a & b); break;
                case 0x12: ccr_ = logic_ccr(a | b); set_reg(rd, a | b); break;
                case 0x13: ccr_ = logic_ccr(a ^ b); set_reg(rd, a ^ b); break;
                case 0x14: {
                    const u64 r = a - b;
                    ccr_ = sub_ccr(a, b, 0, r);
                    set_reg(rd, r);
                    break;
                }
                case 0x15: ccr_ = logic_ccr(a & ~b); set_reg(rd, a & ~b); break;
                case 0x16: ccr_ = logic_ccr(a | ~b); set_reg(rd, a | ~b); break;
                case 0x17: ccr_ = logic_ccr(~(a ^ b)); set_reg(rd, ~(a ^ b)); break;
                case 0x18: {
                    const u64 c = ccr_ & kC;
                    const u64 r = a + b + c;
                    ccr_ = add_ccr(a, b, c, r);
                    set_reg(rd, r);
                    break;
                }
                case 0x1A: {  // UMULcc
                    const u64 r = (a & 0xFFFFFFFFull) * (b & 0xFFFFFFFFull);
                    y_ = r >> 32;
                    ccr_ = logic_ccr(r);
                    set_reg(rd, r);
                    break;
                }
                case 0x1B: {  // SMULcc
                    const u64 r = static_cast<u64>(static_cast<s64>(static_cast<s32>(a)) * static_cast<s32>(b));
                    y_ = r >> 32;
                    ccr_ = logic_ccr(r);
                    set_reg(rd, r);
                    break;
                }
                case 0x1C: {
                    const u64 c = ccr_ & kC;
                    const u64 r = a - b - c;
                    ccr_ = sub_ccr(a, b, c, r);
                    set_reg(rd, r);
                    break;
                }
                case 0x20:
                case 0x22: {  // TADDcc, TADDccTV
                    const u64 r = a + b;
                    u8 cc = add_ccr(a, b, 0, r);
                    if ((a | b) & 3) cc |= kV;
                    if (op3 == 0x22 && (cc & kV)) return take_trap(tt::kTagOverflow);
                    ccr_ = cc;
                    set_reg(rd, r);
                    break;
                }
                case 0x21:
                case 0x23: {  // TSUBcc, TSUBccTV
                    const u64 r = a - b;
                    u8 cc = sub_ccr(a, b, 0, r);
                    if ((a | b) & 3) cc |= kV;
                    if (op3 == 0x23 && (cc & kV)) return take_trap(tt::kTagOverflow);
                    ccr_ = cc;
                    set_reg(rd, r);
                    break;
                }
                case 0x24: {  // MULScc
                    const unsigned icc = ccr_ & 15;
                    const u64 n_xor_v = ((icc & kN) != 0) != ((icc & kV) != 0) ? 1 : 0;
                    const u64 op1 = (n_xor_v << 31) | ((a & 0xFFFFFFFFull) >> 1);
                    const u64 op2v = (y_ & 1) ? (b & 0xFFFFFFFFull) : 0;
                    const u64 r = (op1 + op2v) & 0xFFFFFFFFull;
                    ccr_ = add_ccr(op1, op2v, 0, op1 + op2v);
                    y_ = (y_ >> 1) | ((a & 1) << 31);
                    set_reg(rd, r);
                    break;
                }
                case 0x25: {  // SLL, SLLX
                    const bool x = (insn >> 12) & 1;
                    const unsigned n = static_cast<unsigned>(b) & (x ? 63 : 31);
                    set_reg(rd, a << n);
                    break;
                }
                case 0x26: {  // SRL, SRLX
                    const bool x = (insn >> 12) & 1;
                    if (x) set_reg(rd, a >> (b & 63));
                    else set_reg(rd, (a & 0xFFFFFFFFull) >> (b & 31));
                    break;
                }
                case 0x27: {  // SRA, SRAX
                    const bool x = (insn >> 12) & 1;
                    if (x) set_reg(rd, static_cast<u64>(static_cast<s64>(a) >> (b & 63)));
                    else set_reg(rd, static_cast<u64>(static_cast<s64>(static_cast<s32>(a)) >> (b & 31)));
                    break;
                }
                case 0x28: {  // RDASR, STBAR, MEMBAR
                    if (rs1 == 15 && rd == 0) break;  // STBAR / MEMBAR: memory is always ordered here
                    if (rs1 == 1 || (rs1 >= 7 && rs1 <= 14) || rs1 == 15) return take_trap(tt::kIllegalInstruction);
                    u64 v;
                    if (!read_asr(rs1, v)) return;
                    set_reg(rd, v);
                    break;
                }
                case 0x2A: {  // RDPR
                    u64 v;
                    if (!read_pr(rs1, v)) return;
                    set_reg(rd, v);
                    break;
                }
                case 0x2B:  // FLUSHW
                    if (cansave_ != kWindows - 2) {
                        const unsigned type = otherwin_ ? tt::kSpillOther | ((wstate_ >> 3 & 7) << 2)
                                                        : tt::kSpillNormal | ((wstate_ & 7) << 2);
                        return take_trap(type);
                    }
                    break;
                case 0x2C: {  // MOVcc
                    const bool cc2 = (insn >> 18) & 1;
                    const unsigned cc = (insn >> 11) & 3;
                    const unsigned c = (insn >> 14) & 15;
                    const u64 src = imm ? sext(insn, 11) : reg(insn & 31);
                    bool take;
                    if (cc2) {
                        if (cc & 1) return take_trap(tt::kIllegalInstruction);
                        take = cond_icc(c, cc == 2 ? ccr_ >> 4 : ccr_ & 15);
                    } else {
                        if (!fp_enabled()) return take_trap(tt::kFpDisabled);
                        take = cond_fcc(c, fcc(cc));
                    }
                    if (take) set_reg(rd, src);
                    break;
                }
                case 0x2D: {  // SDIVX
                    if (b == 0) return take_trap(tt::kDivisionByZero);
                    const s64 sa = static_cast<s64>(a), sb = static_cast<s64>(b);
                    set_reg(rd, (sa == std::numeric_limits<s64>::min() && sb == -1) ? a : static_cast<u64>(sa / sb));
                    break;
                }
                case 0x2E:  // POPC
                    if (rs1 != 0) return take_trap(tt::kIllegalInstruction);
                    set_reg(rd, static_cast<u64>(std::popcount(b)));
                    break;
                case 0x2F: {  // MOVr
                    bool valid;
                    const bool take = rcond_holds((insn >> 10) & 7, a, valid);
                    if (!valid) return take_trap(tt::kIllegalInstruction);
                    if (take) set_reg(rd, imm ? sext(insn, 10) : reg(insn & 31));
                    break;
                }
                case 0x30: {  // WRASR, SIR
                    if (rd == 15) {
                        if (rs1 == 0 && imm) {
                            if (!(pstate_ & pstate::PRIV)) return take_trap(tt::kIllegalInstruction);
                            return take_trap(tt::kSoftwareInitiatedReset);
                        }
                        return take_trap(tt::kIllegalInstruction);
                    }
                    if (rd == 1 || rd == 4 || rd == 5 || (rd >= 7 && rd <= 14)) return take_trap(tt::kIllegalInstruction);
                    if (!write_asr(rd, a ^ b)) return;
                    break;
                }
                case 0x31:  // SAVED, RESTORED
                    if (!(pstate_ & pstate::PRIV)) return take_trap(tt::kPrivilegedOpcode);
                    if (rd == 0) {
                        cansave_ = (cansave_ + 1) % kWindows;
                        if (otherwin_ == 0) canrestore_ = (canrestore_ + kWindows - 1) % kWindows;
                        else otherwin_ = (otherwin_ + kWindows - 1) % kWindows;
                    } else if (rd == 1) {
                        canrestore_ = (canrestore_ + 1) % kWindows;
                        if (cleanwin_ < kWindows - 1) ++cleanwin_;
                        if (otherwin_ == 0) cansave_ = (cansave_ + kWindows - 1) % kWindows;
                        else otherwin_ = (otherwin_ + kWindows - 1) % kWindows;
                    } else {
                        return take_trap(tt::kIllegalInstruction);
                    }
                    break;
                case 0x32:  // WRPR
                    if (!write_pr(rd, a ^ b)) return;
                    break;
                case 0x34:
                case 0x35:
                case 0x36:
                    if (!fp_enabled()) return take_trap(tt::kFpDisabled);
                    if (op3 == 0x34) fpop1(insn);
                    else if (op3 == 0x35) fpop2(insn);
                    else impdep1(insn);
                    if (trapped_) return;
                    break;
                case 0x37:  // IMPDEP2: not used by the UltraSPARC IIi
                    return take_trap(tt::kIllegalInstruction);
                case 0x38: {  // JMPL
                    const u64 target = mask_address(a + b);
                    if (target & 3) return take_trap(tt::kMemAddressNotAligned);
                    set_reg(rd, mask_address(pc_));
                    next_npc = target;
                    break;
                }
                case 0x39: {  // RETURN
                    const u64 target = mask_address(a + b);
                    if (target & 3) return take_trap(tt::kMemAddressNotAligned);
                    if (canrestore_ == 0) {
                        const unsigned type = otherwin_ ? tt::kFillOther | ((wstate_ >> 3 & 7) << 2)
                                                        : tt::kFillNormal | ((wstate_ & 7) << 2);
                        return take_trap(type);
                    }
                    cansave_ = (cansave_ + 1) % kWindows;
                    canrestore_ = (canrestore_ + kWindows - 1) % kWindows;
                    set_cwp(window(-1));
                    next_npc = target;
                    break;
                }
                case 0x3A: {  // Tcc
                    const unsigned cc = (insn >> 11) & 3;
                    if (cc & 1) return take_trap(tt::kIllegalInstruction);
                    if (cond_icc((insn >> 25) & 15, cc == 2 ? ccr_ >> 4 : ccr_ & 15)) {
                        const u64 n = a + (imm ? (insn & 0x7F) : reg(insn & 31));
                        return take_trap(tt::kTrapInstruction + static_cast<unsigned>(n & 0x7F));
                    }
                    break;
                }
                case 0x3B:  // FLUSH
                    break;
                case 0x3C: {  // SAVE
                    const u64 r = a + b;
                    if (cansave_ == 0) {
                        const unsigned type = otherwin_ ? tt::kSpillOther | ((wstate_ >> 3 & 7) << 2)
                                                        : tt::kSpillNormal | ((wstate_ & 7) << 2);
                        return take_trap(type);
                    }
                    if (cleanwin_ == canrestore_) return take_trap(tt::kCleanWindow);
                    cansave_ = (cansave_ + kWindows - 1) % kWindows;
                    canrestore_ = (canrestore_ + 1) % kWindows;
                    set_cwp(window(1));
                    set_reg(rd, r);
                    break;
                }
                case 0x3D: {  // RESTORE
                    const u64 r = a + b;
                    if (canrestore_ == 0) {
                        const unsigned type = otherwin_ ? tt::kFillOther | ((wstate_ >> 3 & 7) << 2)
                                                        : tt::kFillNormal | ((wstate_ & 7) << 2);
                        return take_trap(type);
                    }
                    cansave_ = (cansave_ + 1) % kWindows;
                    canrestore_ = (canrestore_ + kWindows - 1) % kWindows;
                    set_cwp(window(-1));
                    set_reg(rd, r);
                    break;
                }
                case 0x3E: {  // DONE, RETRY
                    if (!(pstate_ & pstate::PRIV)) return take_trap(tt::kPrivilegedOpcode);
                    if (rd > 1 || tl_ == 0) return take_trap(tt::kIllegalInstruction);
                    const TrapLevel& t = trap_[tl_];
                    ccr_ = static_cast<u8>(t.tstate >> 32);
                    asi_ = static_cast<u8>(t.tstate >> 24);
                    pstate_ = static_cast<u16>(t.tstate >> 8) & pstate::kWritable;
                    cwp_ = static_cast<unsigned>(t.tstate) & 7;
                    if (rd == 0) {
                        next_pc = t.tnpc;
                        next_npc = t.tnpc + 4;
                    } else {
                        next_pc = t.tpc;
                        next_npc = t.tnpc;
                    }
                    --tl_;
                    attention_ = true;
                    update_globals();
                    break;
                }
                default: return take_trap(tt::kIllegalInstruction);
            }
            break;
        }
        case 3: {
            const unsigned op3 = (insn >> 19) & 0x3F;
            const unsigned rs1 = (insn >> 14) & 31;
            const bool imm = (insn >> 13) & 1;
            const u64 va = mask_address(reg(rs1) + (imm ? sext(insn, 13) : reg(insn & 31)));
            const bool alternate = (op3 & 0x10) != 0;
            u8 a = implicit_asi();
            if (alternate) {
                a = imm ? asi_ : static_cast<u8>(insn >> 5);
                if (!asi_restricted_check(a)) return;
            }
            auto aligned = [&](unsigned size) {
                if (va & (size - 1)) {
                    take_trap(tt::kMemAddressNotAligned);
                    return false;
                }
                return true;
            };
            u64 v = 0;
            switch (op3 & 0x2F) {  // the alternate forms share the plain ones' cases
                case 0x00:  // LDUW(A)
                    if (!aligned(4) || !load(va, a, 4, v)) return;
                    set_reg(rd, v);
                    break;
                case 0x01:  // LDUB(A)
                    if (!load(va, a, 1, v)) return;
                    set_reg(rd, v);
                    break;
                case 0x02:  // LDUH(A)
                    if (!aligned(2) || !load(va, a, 2, v)) return;
                    set_reg(rd, v);
                    break;
                case 0x03: {  // LDD(A), and the 128-bit atomic quad load
                    if (rd & 1) return take_trap(tt::kIllegalInstruction);
                    if (alternate && (a == asi::NQUAD_LDD || a == asi::NQUAD_LDD_L)) {
                        if (!aligned(16)) return;
                        u64 lo;
                        if (!load(va, a, 8, v, AccessKind::Atomic) || !load(va + 8, a, 8, lo, AccessKind::Atomic)) return;
                        set_reg(rd, v);
                        set_reg(rd + 1, lo);
                        break;
                    }
                    u64 lo;
                    if (!aligned(8) || !load(va, a, 4, v) || !load(va + 4, a, 4, lo)) return;
                    set_reg(rd, v);
                    set_reg(rd + 1, lo);
                    break;
                }
                case 0x04:  // STW(A)
                    if (!aligned(4) || !store(va, a, 4, reg(rd))) return;
                    break;
                case 0x05:  // STB(A)
                    if (!store(va, a, 1, reg(rd))) return;
                    break;
                case 0x06:  // STH(A)
                    if (!aligned(2) || !store(va, a, 2, reg(rd))) return;
                    break;
                case 0x07:  // STD(A)
                    if (rd & 1) return take_trap(tt::kIllegalInstruction);
                    if (!aligned(8) || !store(va, a, 4, reg(rd)) || !store(va + 4, a, 4, reg(rd + 1))) return;
                    break;
                case 0x08:  // LDSW(A)
                    if (!aligned(4) || !load(va, a, 4, v)) return;
                    set_reg(rd, sext(v, 32));
                    break;
                case 0x09:  // LDSB(A)
                    if (!load(va, a, 1, v)) return;
                    set_reg(rd, sext(v, 8));
                    break;
                case 0x0A:  // LDSH(A)
                    if (!aligned(2) || !load(va, a, 2, v)) return;
                    set_reg(rd, sext(v, 16));
                    break;
                case 0x0B:  // LDX(A)
                    if (!aligned(8) || !load(va, a, 8, v)) return;
                    set_reg(rd, v);
                    break;
                case 0x0D:  // LDSTUB(A)
                    if (!load(va, a, 1, v, AccessKind::Atomic) || !store(va, a, 1, 0xFF)) return;
                    set_reg(rd, v);
                    break;
                case 0x0E:  // STX(A)
                    if (!aligned(8) || !store(va, a, 8, reg(rd))) return;
                    break;
                case 0x0F:  // SWAP(A)
                    if (!aligned(4) || !load(va, a, 4, v, AccessKind::Atomic) || !store(va, a, 4, reg(rd))) return;
                    set_reg(rd, v);
                    break;
                case 0x20:  // LDF(A)
                    if (!fp_enabled()) return take_trap(tt::kFpDisabled);
                    if (!aligned(4) || !load(va, a, 4, v)) return;
                    set_f32(rd, static_cast<u32>(v));
                    break;
                case 0x21:  // LDFSR, LDXFSR
                    if (alternate) return take_trap(tt::kIllegalInstruction);
                    if (!fp_enabled()) return take_trap(tt::kFpDisabled);
                    if (rd == 0) {
                        if (!aligned(4) || !load(va, a, 4, v)) return;
                        fsr_ = (fsr_ & ~u64{0xFFFFFFFF}) | (v & 0xCFC00FFFull) | (fsr_ & 0x000FE000ull);
                    } else if (rd == 1) {
                        if (!aligned(8) || !load(va, a, 8, v)) return;
                        fsr_ = (v & 0x3FCFC00FFFull) | (fsr_ & 0x000FE000ull);
                    } else {
                        return take_trap(tt::kIllegalInstruction);
                    }
                    break;
                case 0x23:  // LDDF(A); block loads and short loads come through here too
                    if (!fp_enabled()) return take_trap(tt::kFpDisabled);
                    if (alternate && asi_is_block(a)) {
                        if (!block_load(va, a, rd)) return;
                        break;
                    }
                    if (alternate && asi_is_short_fp(a)) {
                        if (!short_fp_load(va, a, rd)) return;
                        break;
                    }
                    if (va & 3) return take_trap(tt::kMemAddressNotAligned);
                    if (va & 4) return take_trap(tt::kLddfMemAddressNotAligned);
                    if (!load(va, a, 8, v)) return;
                    set_f64(rd, v);
                    break;
                case 0x24:  // STF(A)
                    if (!fp_enabled()) return take_trap(tt::kFpDisabled);
                    if (!aligned(4) || !store(va, a, 4, f_[rd])) return;
                    break;
                case 0x25:  // STFSR, STXFSR
                    if (alternate) return take_trap(tt::kIllegalInstruction);
                    if (!fp_enabled()) return take_trap(tt::kFpDisabled);
                    if (rd == 0) {
                        if (!aligned(4) || !store(va, a, 4, fsr_ & 0xFFFFFFFFull)) return;
                    } else if (rd == 1) {
                        if (!aligned(8) || !store(va, a, 8, fsr_)) return;
                    } else {
                        return take_trap(tt::kIllegalInstruction);
                    }
                    fsr_ &= ~u64{0x1C000};  // ftt is cleared by the store
                    break;
                case 0x27:  // STDF(A); block, partial and short stores come through here too
                    if (!fp_enabled()) return take_trap(tt::kFpDisabled);
                    if (alternate && asi_is_block(a)) {
                        if (!block_store(va, a, rd)) return;
                        break;
                    }
                    if (alternate && asi_is_partial(a)) {
                        if (!partial_store(mask_address(reg(rs1)), a, rd, reg(insn & 31))) return;
                        break;
                    }
                    if (alternate && asi_is_short_fp(a)) {
                        if (!short_fp_store(va, a, rd)) return;
                        break;
                    }
                    if (va & 3) return take_trap(tt::kMemAddressNotAligned);
                    if (va & 4) return take_trap(tt::kStdfMemAddressNotAligned);
                    if (!store(va, a, 8, freg64(rd))) return;
                    break;
                case 0x2D:  // PREFETCH(A)
                    break;
                case 0x22:  // LDQF(A), STQF(A): not implemented by the UltraSPARC IIi
                case 0x26:
                    if (!fp_enabled()) return take_trap(tt::kFpDisabled);
                    return take_trap(tt::kIllegalInstruction);
                case 0x2C:
                case 0x2E: {  // CASA, CASXA
                    if (!alternate) return take_trap(tt::kIllegalInstruction);
                    const unsigned size = op3 == 0x3C ? 4 : 8;
                    const u64 addr = mask_address(reg(rs1));
                    if (addr & (size - 1)) return take_trap(tt::kMemAddressNotAligned);
                    const u8 casasi = imm ? asi_ : static_cast<u8>(insn >> 5);
                    if (!load(addr, casasi, size, v, AccessKind::Atomic)) return;
                    const u64 cmp = size == 4 ? reg(insn & 31) & 0xFFFFFFFFull : reg(insn & 31);
                    if (v == cmp && !store(addr, casasi, size, reg(rd))) return;
                    set_reg(rd, v);
                    break;
                }
                default: return take_trap(tt::kIllegalInstruction);
            }
            break;
        }
    }
    pc_ = next_pc;
    npc_ = next_npc;
}

}  // namespace uv::usparc
