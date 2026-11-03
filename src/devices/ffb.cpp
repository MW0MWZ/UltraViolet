// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
#include "devices/ffb.h"

#include <cmath>
#include <map>

#include "core/text.h"

#include "core/log.h"

namespace uv {

namespace {

// UPA space regions (offsets from the port's base).
constexpr u64 kProm = 0x0000000, kExp = 0x0200000, kDac = 0x0400000, kFbc = 0x0600000, kFbcEnd = 0x0612000,
              kFbViews = 0x1000000;

// The PROM holds a byte at each fourth address; the other three bytes of
// each word read as these on a real card.
constexpr u8 kPromLanes[3] = {0x00, 0x01, 0x2C};

// The expansion space: the board's strapping in byte 0 (0x33: FFB2+,
// double-buffered, as a real card reads), and the monitor's sense
// lines in bits 24-26 of the word at 0x10. With nothing to sense (all three
// lines high) the FCode takes 1152 x 900 at 66 Hz, as it does on a real
// card with a monitor that has no sense or DDC lines.
constexpr u32 kStrap = 0x33;
constexpr u32 kMonitorSense = 7;

constexpr u32 kFbcDevId = 0x3241906D;  // FBC_reg_ID, as a real card reads
// UCSR when idle: the FIFO's free entries in bits 0-11. (A real card's
// count when its FIFO is empty is not known; the X driver's smallest
// known FIFO is 124 entries, and it keeps 4 back.)
constexpr u32 kUcsrFifoEmpty = 0x80;  // as the card reads when idle
constexpr u32 kUcsrPickEnable = 0x000C0000, kUcsrPickNoHit = 0x00020000;
constexpr u32 kUcsrErrors = 0xC0000000;
// The FBC's interrupt register (+0x10900), as Solaris's driver uses it:
// bit 3 enables the vertical retrace interrupt and bit 2 says it came;
// the driver acknowledges with 5, clearing both, and enables it again
// for each wait. Bits 1 and 0 are a second enable and its flag (written
// with the retrace's enable as 0xA, cleared with it); what raises that one
// is not known.
constexpr u32 kFbcInt = 0x10900;
constexpr u32 kIntVertEnable = 8, kIntVertPending = 4, kIntEnables = 0xA, kIntPendings = 0x5;

// DAC (BT498) indirect addresses.
constexpr u32 kDacPpll = 0x0000, kDacGpll = 0x0001, kDacPfctrl = 0x1000, kDacUctrl = 0x1001, kDacClut = 0x2000,
              kDacClutEnd = 0x2400, kDacSovwlut = 0x3100, kDacAovwlut = 0x3120, kDacWtctrl = 0x3150, kDacTmctrl = 0x3151,
              kDacTcolorkey = 0x3152, kDacWamask = 0x3153, kDacSpwlut = 0x3200, kDacApwlut = 0x3240,
              kDacSanal = 0x5000, kDacDacctrl = 0x5001, kDacTgen = 0x6000, kDacTregs = 0x6001, kDacTgvc = 0x600F,
              kDacTghc = 0x6010, kDacDid = 0x8000, kDacMpdata = 0x8001, kDacMpsense = 0x8002;
constexpr u32 kDacDidValue = 0xA236E1AD;  // BT498 (part 0x236E), revision 0xA
constexpr u32 kDacManRev = 1;             // UCTRL bits 8-11 (RamDAC_rev 1, as a real card's)
constexpr u32 kTgenVideo = 1, kTgenTiming = 2;
// Window transfer control (the X driver's names): DS busy, TCMD transfer,
// TE at the next frame (else field).
constexpr u32 kWtctrlBusy = 1, kWtctrlTransfer = 2;
constexpr double kDacRefHz = 13.5e6;  // the PLLs' reference: the FCode's PLL settings give its mode clocks from it

// Window lookup entries (PAC2).
constexpr u32 kWlutTable = 0x30, kWlutLookup = 0x100, kWlutSource = 0x3000, kWlutTrue = 0x4000, kWlutBufferB = 0x8000;

}  // namespace

Ffb::Ffb(Scheduler& scheduler, double cpu_hz, std::vector<u8> fcode)
    : sched_(scheduler), cpu_hz_(cpu_hz), fcode_(std::move(fcode)), retrace_timer_(scheduler, [this] { retrace(); }) {
    for (auto& b : buf_) b.assign(static_cast<std::size_t>(kLine) * kLines, 0);
    regs_.assign(0x1000 / 4, 0);
    kregs_.assign(0x2000 / 4, 0);
    row_version_.assign(kLines, 0);
    reset();
}

void Ffb::reset() {
    std::fill(regs_.begin(), regs_.end(), 0);
    std::fill(kregs_.begin(), kregs_.end(), 0);
    ucsr_ = 0;
    interrupts_ = 0;
    transfer_pending_ = false;
    retrace_timer_.cancel();
    fast_fill_colour_ = 0;
    polygon_.clear();
    dac_ptr_ = cur_ptr_ = 0;
    ppll_ = gpll_ = pfctrl_ = uctrl_ = wtctrl_ = tmctrl_ = tcolorkey_ = wamask_ = 0;
    sanal_ = dacctrl_ = tgen_ = 0;
    mpdata_ = 3;
    tregs_.fill(0);
    cursor_ctrl_ = 3;
    cursor_pos_ = 0;
    changed_all();
}

// --- UPA space -------------------------------------------------------------------

bool Ffb::read(u64 offset, unsigned size, u64& value) {
    if (offset >= kFbViews) return fb_read(offset - kFbViews, size, value);
    value = 0;
    if (offset < kExp) {
        for (unsigned i = 0; i < size; ++i) {
            const u64 a = offset + i;
            const u64 n = (a - kProm) / 4;
            const u8 b = a % 4 ? kPromLanes[a % 4 - 1] : n < fcode_.size() ? fcode_[n] : 0xFF;
            value = value << 8 | b;
        }
        return true;
    }
    // Registers: words (a doubleword is two, the first in its high half).
    auto word = [&](u64 a) -> u32 {
        if (a < kDac) {
            const u64 e = a - kExp;
            return e == 0 ? kStrap << 24 : e == 0x10 ? kMonitorSense << 24 : 0;
        }
        if (a < kFbc) {
            switch ((a - kDac) & 0xC) {
                case 0x0: return dac_ptr_;
                case 0x4: ++debug_dac_reads_[dac_ptr_ & 0xFFFF]; return dac_read(dac_ptr_++ & 0xFFFF);
                case 0x8: return cur_ptr_;
                default: ++debug_dac_reads_[0x10000 | (cur_ptr_ & 0xFFFF)]; return cursor_read(cur_ptr_++ & 0xFFFF);
            }
        }
        if (a < kFbcEnd) return fbc_read(static_cast<u32>(a - kFbc));
        engine_unmodelled(0, "a read of the card's space outside its registers and frame buffer", static_cast<u32>(a));
        return 0;
    };
    if (size == 8) {
        value = u64{word(offset)} << 32 | word(offset + 4);
    } else {
        const u32 w = word(offset & ~u64{3});
        const unsigned shift = static_cast<unsigned>(4 - size - (offset & 3)) * 8;
        value = (w >> shift) & (size == 4 ? 0xFFFFFFFFu : (1u << (8 * size)) - 1);
    }
    return true;
}

bool Ffb::write(u64 offset, unsigned size, u64 value) {
    if (offset >= kFbViews) return fb_write(offset - kFbViews, size, value);
    if (offset < kExp) return true;  // (the PROM)
    auto word = [&](u64 a, u32 v) {
        if (a < kDac) return;  // (the strapping and sense: read only)
        if (a < kFbc) {
            switch ((a - kDac) & 0xC) {
                case 0x0: dac_ptr_ = v & 0xFFFF; break;
                case 0x4: dac_write(dac_ptr_++ & 0xFFFF, v); break;
                case 0x8: cur_ptr_ = v & 0xFFFF; break;
                default: cursor_write(cur_ptr_++ & 0xFFFF, v); break;
            }
            return;
        }
        if (a < kFbcEnd) return fbc_write(static_cast<u32>(a - kFbc), v);
        engine_unmodelled(5, "a write to the card's space outside its registers and frame buffer", static_cast<u32>(a));
    };
    if (size == 8) {
        word(offset, static_cast<u32>(value >> 32));
        word(offset + 4, static_cast<u32>(value));
    } else if (size == 4) {
        word(offset, static_cast<u32>(value));
    } else {
        engine_unmodelled(1, "a register write narrower than a word");
    }
    return true;
}

// --- DAC -----------------------------------------------------------------------

u32 Ffb::dac_read(u32 a) {
    if (a >= kDacClut && a < kDacClutEnd) return clut_[a - kDacClut];
    if (a >= kDacSpwlut && a < kDacSpwlut + 64) return pwlut_shadow_[a - kDacSpwlut];
    if (a >= kDacApwlut && a < kDacApwlut + 64) return pwlut_[a - kDacApwlut];
    if (a >= kDacSovwlut && a < kDacSovwlut + 4) return owlut_shadow_[a - kDacSovwlut];
    if (a >= kDacAovwlut && a < kDacAovwlut + 4) return owlut_[a - kDacAovwlut];
    if (a >= kDacTregs && a < kDacTregs + tregs_.size()) return tregs_[a - kDacTregs];
    switch (a) {
        case kDacPpll: return ppll_;
        case kDacGpll: return gpll_;
        case kDacPfctrl: return pfctrl_;
        case kDacUctrl: return (uctrl_ & ~0xF00u) | kDacManRev << 8;
        case kDacWtctrl: return (wtctrl_ & ~kWtctrlBusy) | (transfer_pending_ ? kWtctrlBusy : 0);
        case kDacTmctrl: return tmctrl_;
        case kDacTcolorkey: return tcolorkey_;
        case kDacWamask: return wamask_;
        case kDacSanal: return sanal_;
        case kDacDacctrl: return dacctrl_;
        case kDacTgen: return tgen_;
        case kDacTgvc: return current_line();
        case kDacTghc: {
            const Timing t = timing();
            if (!t.on) return 0;
            const double in_line = std::fmod(static_cast<double>(sched_.now()), t.line_cycles) / t.line_cycles;
            return static_cast<u32>(in_line * (tregs_[8] + 1));
        }
        case kDacDid: return kDacDidValue;
        case kDacMpdata: return mpdata_;
        // The monitor port's lines as they are: what the card drives, with
        // nothing on the other end pulling either low (no DDC monitor).
        case kDacMpsense: return mpdata_ & 3;
        default: break;
    }
    engine_unmodelled(2, "a DAC register read", a);
    return 0;
}

void Ffb::dac_write(u32 a, u32 v) {
    if (a >= kDacClut && a < kDacClutEnd) {
        clut_[a - kDacClut] = v & 0xFFFFFF;
    } else if (a >= kDacSpwlut && a < kDacSpwlut + 64) {
        pwlut_shadow_[a - kDacSpwlut] = v;
        return;  // (shown once transferred)
    } else if (a >= kDacApwlut && a < kDacApwlut + 64) {
        pwlut_[a - kDacApwlut] = v;
    } else if (a >= kDacSovwlut && a < kDacSovwlut + 4) {
        owlut_shadow_[a - kDacSovwlut] = v;
        return;
    } else if (a >= kDacTregs && a < kDacTregs + tregs_.size()) {
        tregs_[a - kDacTregs] = v & 0xFFF;
    } else {
        switch (a) {
            case kDacPpll: ppll_ = v; break;
            case kDacGpll: gpll_ = v; break;
            case kDacPfctrl: pfctrl_ = v; break;
            case kDacUctrl: uctrl_ = v; break;
            case kDacWtctrl:
                // The shadow window lookups to the active ones, at the next
                // vertical blank (frame and field are the same, not
                // interlaced); busy till then.
                wtctrl_ = v & ~(kWtctrlTransfer | kWtctrlBusy);
                if (v & kWtctrlTransfer) {
                    transfer_pending_ = true;
                    schedule_retrace();
                }
                return;
            case kDacTmctrl: tmctrl_ = v; break;
            case kDacTcolorkey: tcolorkey_ = v; break;
            case kDacWamask: wamask_ = v; break;
            case kDacSanal: sanal_ = v; return;
            case kDacDacctrl: dacctrl_ = v; break;
            case kDacTgen: tgen_ = v; break;
            case kDacMpdata: mpdata_ = v & 3; return;
            case kDacTgvc:
            case kDacTghc:
            case kDacDid: return;  // (read only)
            default: engine_unmodelled(4, "a DAC register written", a); return;
        }
    }
    changed_all();
}

// The cursor: two 64 x 64 planes (0x00-0x7F and 0x80-0xFF, two words a
// row, the leftmost pixel in bit 31), control (a plane shown while its bit
// is clear), three colours and the position.
u32 Ffb::cursor_read(u32 a) const {
    if (a < 0x100) return cursor_bits_[a];
    if (a == 0x100) return cursor_ctrl_;
    if (a >= 0x101 && a <= 0x103) return cursor_colour_[a - 0x101];
    if (a == 0x104) return cursor_pos_;
    return 0;
}

void Ffb::cursor_write(u32 a, u32 v) {
    if (a < 0x100) cursor_bits_[a] = v;
    else if (a == 0x100) cursor_ctrl_ = v & 3;
    else if (a >= 0x101 && a <= 0x103) cursor_colour_[a - 0x101] = v & 0xFFFFFF;
    else if (a == 0x104) cursor_pos_ = v;
    else return;
    cursor_version_ = ++version_;
}

// --- FBC registers -----------------------------------------------------------------

u32 Ffb::fbc_read(u32 off) {
    if (off == kFbcInt) return interrupts_;
    if (off >= 0x10000) {
        const u32 i = (off - 0x10000) / 4;
        return i < kregs_.size() ? kregs_[i] : 0;
    }
    if (off >= 0x1000) return 0;
    switch (off) {
        case 0x800: return kFbcDevId;
        // Drawing is done when it is started: the raster processor is never
        // busy, and the FIFO always empty. Picking is not modelled, so a pick
        // never hits.
        case 0x900: return (ucsr_ & ~0xFFFu) | kUcsrPickNoHit | kUcsrFifoEmpty;
        default: return regs_[off / 4];
    }
}

namespace {

// Registers made of fields that a write changes only where it gives a
// value: two-bit fields (three for the buffers written) left alone by 0
// (the pixel processor control, the frame buffer control), and eight-bit
// fields whose top bit says they are written (the raster operation, the
// compare).
constexpr u32 kPpcFields[] = {0xC00000, 0x0C0000, 0x030000, 0x00C000, 0x003000, 0x000C00,
                              0x000300, 0x0000C0, 0x000030, 0x00000C, 0x000003};
constexpr u32 kFbcFields[] = {0xE0000000, 0x00300000, 0x000C0000, 0x0000C000, 0x00003000, 0x00000C00,
                              0x00000300, 0x000000C0, 0x00000030, 0x0000000C, 0x00000003};

u32 merge_edit_bytes(u32 old, u32 v, unsigned count) {
    for (unsigned i = 0; i < count; ++i) {
        const u32 m = 0xFFu << (8 * i);
        if (v & (0x80u << (8 * i))) old = (old & ~m) | (v & m);
    }
    return old;
}

}  // namespace

void Ffb::fbc_write(u32 off, u32 v) {
    if (off == kFbcInt) {
        interrupts_ = (v & kIntEnables) | (interrupts_ & kIntPendings & ~v);
        schedule_retrace();
        return;
    }
    if (off >= 0x10000) {
        const u32 i = (off - 0x10000) / 4;
        if (i < kregs_.size()) kregs_[i] = v;
        return;
    }
    if (off >= 0x1000) return;
    u32& r = regs_[off / 4];
    switch (off) {
        case 0x200: {  // ppc
            u32 old = r;
            for (u32 m : kPpcFields)
                if (v & m) old = (old & ~m) | (v & m);
            r = old;
            return;
        }
        case 0x254: {  // fbc
            u32 old = r;
            for (u32 m : kFbcFields)
                if (v & m) old = (old & ~m) | (v & m);
            r = old;
            return;
        }
        case 0x258: r = merge_edit_bytes(r, v, 3); return;  // rop
        case 0x25C: r = merge_edit_bytes(r, v, 4); return;  // cmp
        case 0x320: font_row(v); return;
        case 0x800: return;  // (devid: read only)
        case 0x900:  // errors cleared by writing them; the pick enable an edit field (0 no change)
            ucsr_ &= ~(v & kUcsrErrors);
            if (v & kUcsrPickEnable) ucsr_ = (ucsr_ & ~kUcsrPickEnable) | (v & kUcsrPickEnable);
            return;
        default: break;
    }
    r = v;
    // A polygon is given vertex by vertex from the draw operation's write:
    // each through by/bx, one through bh/bw, the last through ebyi/ebxi,
    // which draws it.
    if (off == 0x300) polygon_.clear();
    if ((regs_[0x300 / 4] & 0xF) == 0x07) {
        if (off == 0x64 || off == 0x74 || off == 0x54) polygon_vertex(off);
        return;
    }
    // An operation starts with the write of its last register: bw, or for a
    // fast fill's block and page fills bx (its "go").
    if (off == 0x74 || (off == 0x64 && (regs_[0x300 / 4] & 0xF) == 0x09)) start_drawop(off);
}

// --- Frame buffer views ------------------------------------------------------------

namespace {

// The dumb views are buffer A's whatever the frame buffer control selects
// (a rectangle the engine draws into A alone reads back through DFB32 with
// the read buffer B); DFB24B is buffer B's.
enum class View { None, Dumb8, Dumb24, Dumb32, Smart8, Smart32, Dumb24B };
constexpr bool dumb_view(View v) { return v == View::Dumb8 || v == View::Dumb24 || v == View::Dumb32 || v == View::Dumb24B; }

// The view at an offset from 0x1000000, the channel of an 8-bit view (0 R,
// 1 G, 2 B, 3 X) and the offset into the view.
View view_at(u64 off, unsigned& channel, u64& in) {
    channel = 0;
    if (off < 0x1000000) {
        channel = static_cast<unsigned>(off >> 22);
        in = off & 0x3FFFFF;
        return View::Dumb8;
    }
    if (off < 0x2000000) return in = off - 0x1000000, View::Dumb24;
    if (off < 0x3000000) return in = off - 0x2000000, View::Dumb32;
    if (off < 0x4000000) {
        channel = static_cast<unsigned>((off - 0x3000000) >> 22);
        in = off & 0x3FFFFF;
        return View::Smart8;
    }
    if (off < 0x5000000) return in = off - 0x4000000, View::Smart32;
    if (off >= 0x9000000 && off < 0xA000000) return in = off - 0x9000000, View::Dumb24B;
    return View::None;
}

}  // namespace

bool Ffb::fb_read(u64 off, unsigned size, u64& value) {
    unsigned channel;
    u64 in;
    const View v = view_at(off, channel, in);
    value = 0;
    if (v == View::None) {
        engine_unmodelled(3, "a frame buffer view", static_cast<u32>(off + 0x1000000));
        return true;
    }
    // A pixel from the buffer the view reads: the dumb views' own, the
    // smart views' by the frame buffer control.
    const unsigned rb = !dumb_view(v) ? read_buffer() : v == View::Dumb24B ? 1 : 0;
    auto pixel = [&](u64 x, u64 y) { return x < kLine && y < kLines ? buf_[rb][y * kLine + x] : 0u; };
    for (unsigned i = 0; i < size; ++i) {
        const u64 a = in + i;
        u32 b;
        if (v == View::Dumb8 || v == View::Smart8) {
            const u32 p = pixel(a % kLine, a / kLine);
            b = (p >> (channel == 3 ? 24 : 8 * channel)) & 0xFF;
        } else {
            const u64 n = a / 4;
            const u32 p = pixel(n % kLine, n / kLine);
            b = (p >> (8 * (3 - a % 4))) & 0xFF;
        }
        value = value << 8 | b;
    }
    return true;
}

bool Ffb::fb_write(u64 off, unsigned size, u64 value) {
    unsigned channel;
    u64 in;
    const View v = view_at(off, channel, in);
    if (v == View::None) {
        engine_unmodelled(3, "a frame buffer view", static_cast<u32>(off + 0x1000000));
        return true;
    }
    const unsigned db = v == View::Dumb24B ? 1 : 0;  // the dumb views' buffer
    // (Smart views with the pixel processor control, dumb ones with the channel.)
    ++debug_ops_[{regs_[0x254 / 4], v == View::Smart8 || v == View::Smart32
                                        ? 0xE0000000u | static_cast<u32>(v) << 24 | (regs_[0x200 / 4] & 0xFFFFFF)
                                        : 0xD0000000u | static_cast<u32>(v) << 8 | channel}];
    auto dumb = [&](unsigned x, unsigned y, u32 bits, u32 mask) {
        const std::size_t i = static_cast<std::size_t>(y) * kLine + x;
        buf_[db][i] = (buf_[db][i] & ~mask) | (bits & mask);
        changed(y, y + 1);
    };
    if (v == View::Dumb8 || v == View::Smart8) {
        const unsigned shift = channel == 3 ? 24 : 8 * channel;
        for (unsigned i = 0; i < size; ++i) {
            const u64 a = in + i;
            const u32 b = static_cast<u32>(value >> (8 * (size - 1 - i))) & 0xFF;
            const unsigned x = static_cast<unsigned>(a % kLine), y = static_cast<unsigned>(a / kLine);
            if (y >= kLines) continue;
            if (v == View::Dumb8) {
                dumb(x, y, b << shift, 0xFFu << shift);
            } else {
                smart_write(x, y, b << shift, 0xFFu << shift);
                changed(y, y + 1);
            }
        }
        return true;
    }
    // 32-bit views: whole pixels, or the bytes of one.
    for (unsigned i = 0; i < size;) {
        const u64 a = in + i;
        const u64 n = a / 4;
        const unsigned x = static_cast<unsigned>(n % kLine), y = static_cast<unsigned>(n / kLine);
        const unsigned first = static_cast<unsigned>(a % 4), count = std::min(4 - first, size - i);
        u32 bits = 0, mask = 0;
        for (unsigned k = 0; k < count; ++k) {
            const unsigned shift = 8 * (3 - (first + k));
            bits |= (static_cast<u32>(value >> (8 * (size - 1 - (i + k)))) & 0xFF) << shift;
            mask |= 0xFFu << shift;
        }
        i += count;
        if (y >= kLines) continue;
        if (v == View::Dumb24 || v == View::Dumb24B) mask &= 0xFFFFFF;  // (the X plane kept)
        if (v == View::Smart32) {
            smart_view_write(x, y, bits, mask);
            changed(y, y + 1);
        } else {
            dumb(x, y, bits, mask);
        }
    }
    return true;
}

// --- Picture ---------------------------------------------------------------------

Ffb::Timing Ffb::timing() const {
    Timing t;
    // tregs_: VBNP, VBAP, VSNP, VSAP, HSNP, HBNP, HBAP, HSYNCNP, HSYNCAP, ...
    // Vertical in lines, horizontal in pairs of pixels (the DAC takes two
    // a clock): blank ends at VBNP/HBNP and starts at VBAP/HBAP; the line
    // ends at HSYNCAP, the frame at VSAP.
    const unsigned m = ppll_ & 0x7F, d = (ppll_ >> 7) & 0xF, pfd = (ppll_ >> 11) & 3;
    if (!(tgen_ & kTgenTiming) || !m || !d || tregs_[1] <= tregs_[0] || tregs_[6] <= tregs_[5] || !tregs_[3] ||
        !tregs_[8])
        return t;
    t.on = true;
    t.width = std::min((tregs_[6] - tregs_[5]) * 2, kLine);
    t.height = std::min(tregs_[1] - tregs_[0], kLines);
    t.total = tregs_[3] + 1;
    const double pclk = kDacRefHz * m / d / (1u << pfd);
    t.line_cycles = cpu_hz_ * 2.0 * (tregs_[8] + 1) / pclk;
    return t;
}

// The vertical blank's start (the line the DAC's VBAP gives, counted as
// current_line counts): a window transfer waiting is made, and the
// vertical retrace interrupt raised if enabled. With no video timing a
// transfer is made at once.
void Ffb::schedule_retrace() {
    const Timing t = timing();
    if (!t.on && transfer_pending_) window_transfer();
    if (!t.on || !(transfer_pending_ || (interrupts_ & kIntVertEnable))) {
        retrace_timer_.cancel();
        return;
    }
    const double frame = t.line_cycles * t.total, now = static_cast<double>(sched_.now());
    double at = (std::floor(now / frame) * t.total + tregs_[1]) * t.line_cycles;
    if (at <= now) at += frame;
    retrace_timer_.schedule_at(static_cast<u64>(std::ceil(at)));
}

void Ffb::window_transfer() {
    transfer_pending_ = false;
    pwlut_ = pwlut_shadow_;
    owlut_ = owlut_shadow_;
    changed_all();
}

void Ffb::retrace() {
    if (transfer_pending_) window_transfer();
    if ((interrupts_ & kIntVertEnable) && !(interrupts_ & kIntVertPending)) {
        interrupts_ |= kIntVertPending;
        ++debug_retraces_;
        if (on_interrupt_) on_interrupt_();
    }
    if (timing().on) schedule_retrace();
}

unsigned Ffb::current_line() const {
    const Timing t = timing();
    if (!t.on) return 0;
    return static_cast<unsigned>(static_cast<u64>(static_cast<double>(sched_.now()) / t.line_cycles) % t.total);
}

bool Ffb::cursor_position(int& x, int& y) const {
    // (With the video off the DAC shows nothing, the cursor included.)
    if ((cursor_ctrl_ & 3) == 3 || !(tgen_ & kTgenVideo)) return false;
    auto field = [](u32 v, bool sign) { return sign ? static_cast<int>(v) - 4096 : static_cast<int>(v); };
    x = field(cursor_pos_ & 0xFFF, cursor_pos_ & 0x8000);
    y = field((cursor_pos_ >> 16) & 0xFFF, cursor_pos_ & 0x80000000);
    return true;
}

bool Ffb::render(Frame& f) const {
    FrameState& st = frame_state<FrameState>(f);
    f.changed_top = f.changed_bottom = 0;
    if (st.made && st.seen == version_) return st.shown;
    const Timing t = timing();
    const bool all = !st.made || dac_version_ > st.seen || t.width != st.width || t.height != st.height;
    const u64 since = st.seen;
    st.made = true;
    st.seen = version_;
    st.shown = t.on;
    if (!t.on) return false;
    const unsigned width = t.width, height = t.height;
    if (all) {
        st.width = f.width = width;
        st.height = f.height = height;
        st.base.assign(static_cast<std::size_t>(width) * height, 0);
        f.pixels.assign(st.base.size(), 0);
    }

    // Each pixel through its window ID's lookup entry. In the Combined
    // window mode the window ID is buffer A's X planes. In Separate_8 it is
    // buffer B's X planes, and A's are an 8-bit overlay: shown through an
    // overlay lookup (chosen by the window ID's top bits), unless that says
    // none, or transparent and the overlay pixel is the key.
    const bool video = tgen_ & kTgenVideo;
    const u32 wid_mask = (wamask_ & 0x3F) ? (wamask_ & 0x3F) : 0x3F;
    const bool separate8 = (uctrl_ & 0x30) == 0x20, overlay = separate8 && (uctrl_ & 8);
    const u32 ov_mask = (wamask_ >> 8) & 3, key_mask = tmctrl_ & 0xFF, key = tcolorkey_ & key_mask;
    auto make_row = [&](unsigned y) {
        u32* out = &st.base[static_cast<std::size_t>(y) * width];
        if (!video) {
            std::fill(out, out + width, 0);
            return;
        }
        const u32* a = &buf_[0][static_cast<std::size_t>(y) * kLine];
        const u32* b = &buf_[1][static_cast<std::size_t>(y) * kLine];
        for (unsigned x = 0; x < width; ++x) {
            const u32 wid = (separate8 ? b[x] : a[x]) >> 24;
            if (overlay) {
                const u32 o = owlut_[(wid >> 6) & ov_mask], ov = a[x] >> 24;
                const u32 type = o & 0xC00;  // 0 none, 0x400 transparent, 0x800 opaque
                if (type == 0x800 || (type == 0x400 && (ov & key_mask) != key)) {
                    const u32 c = (o & kWlutLookup) ? clut_[((o & kWlutTable) >> 4) * 256 + ov] : ov * 0x010101u;
                    out[x] = (c & 0xFF) << 16 | (c & 0xFF00) | (c >> 16 & 0xFF);
                    continue;
                }
            }
            const u32 e = pwlut_[wid & wid_mask];
            const u32 p = (e & kWlutBufferB) ? b[x] : a[x];
            const u32* table = &clut_[((e & kWlutTable) >> 4) * 256];
            u32 r, g, bl;
            if (e & kWlutTrue) {
                r = p & 0xFF, g = (p >> 8) & 0xFF, bl = (p >> 16) & 0xFF;
                if (e & kWlutLookup) r = table[r] & 0xFF, g = (table[g] >> 8) & 0xFF, bl = (table[bl] >> 16) & 0xFF;
            } else {
                static constexpr unsigned kShift[4] = {24, 0, 8, 16};  // X, R, G, B
                const u32 i = (p >> kShift[(e & kWlutSource) >> 12]) & 0xFF;
                if (e & kWlutLookup) {
                    const u32 c = table[i];
                    r = c & 0xFF, g = (c >> 8) & 0xFF, bl = (c >> 16) & 0xFF;
                } else {
                    r = g = bl = i;
                }
            }
            out[x] = r << 16 | g << 8 | bl;
        }
    };

    // The cursor: plane 0 the low bit of each pixel's colour number, plane
    // 1 the high; 0 shows the screen, 1-3 the cursor colours.
    int cx = 0, cy = 0;
    const bool cursor = cursor_position(cx, cy);
    const bool cursor_moved = all || cursor_version_ > since || cursor != st.cursor || cx != st.cx || cy != st.cy;
    auto draw_cursor = [&](unsigned y) {
        if (!cursor || static_cast<int>(y) < cy || static_cast<int>(y) >= cy + 64) return;
        const unsigned j = y - static_cast<unsigned>(cy);
        u32* out = &f.pixels[static_cast<std::size_t>(y) * width];
        for (unsigned col = 0; col < 64; ++col) {
            const int x = cx + static_cast<int>(col);
            if (x < 0 || x >= static_cast<int>(width)) continue;
            const unsigned w = j * 2 + col / 32, bit = 31 - col % 32;
            const unsigned p0 = (cursor_ctrl_ & 1) ? 0 : (cursor_bits_[w] >> bit) & 1;
            const unsigned p1 = (cursor_ctrl_ & 2) ? 0 : (cursor_bits_[0x80 + w] >> bit) & 1;
            if (const unsigned code = p1 << 1 | p0) {
                const u32 c = cursor_colour_[code - 1];
                out[x] = (c & 0xFF) << 16 | (c & 0xFF00) | (c >> 16 & 0xFF);
            }
        }
    };
    std::vector<u8> recompose(height, all ? 1 : 0), redo(height, all ? 1 : 0);
    if (!all)
        for (unsigned y = 0; y < height; ++y) recompose[y] = redo[y] = row_version_[y] > since;
    if (cursor_moved) {
        auto mark = [&](bool shown, int top) {
            if (!shown) return;
            for (int y = std::max(top, 0); y < std::min(top + 64, static_cast<int>(height)); ++y)
                recompose[static_cast<unsigned>(y)] = 1;
        };
        mark(st.cursor, st.cy);
        mark(cursor, cy);
    }
    st.cursor = cursor, st.cx = cx, st.cy = cy;

    unsigned top = height, bottom = 0;
    for (unsigned y = 0; y < height; ++y) {
        if (!recompose[y]) continue;
        if (redo[y]) make_row(y);
        std::copy_n(&st.base[static_cast<std::size_t>(y) * width], width, &f.pixels[static_cast<std::size_t>(y) * width]);
        draw_cursor(y);
        top = std::min(top, y);
        bottom = y + 1;
    }
    if (top < bottom) f.changed_top = top, f.changed_bottom = bottom;
    return true;
}

void Ffb::describe(std::string& out) const {
    appendf(out, "ffb: uctrl %x wamask %x tmctrl %x tcolorkey %x tgen %x ppll %x pfctrl %x interrupts %x retraces %llu\n",
            uctrl_, wamask_, tmctrl_, tcolorkey_, tgen_, ppll_, pfctrl_, interrupts_,
            static_cast<unsigned long long>(debug_retraces_));
    appendf(out, "ffb: owlut %x %x %x %x  cursor ctrl %x pos %x colours %x %x %x\n", owlut_[0], owlut_[1], owlut_[2],
            owlut_[3], cursor_ctrl_, cursor_pos_, cursor_colour_[0], cursor_colour_[1], cursor_colour_[2]);
    out += "ffb: pwlut";
    for (unsigned i = 0; i < 64; ++i) appendf(out, "%s%x", i % 16 ? " " : "\n  ", pwlut_[i]);
    out += "\nffb: fbc";
    for (u32 off = 0x200; off < 0x400; off += 4)
        if (regs_[off / 4] && (off < 0x380 || off >= 0x400)) appendf(out, " %03x=%08x", off, regs_[off / 4]);
    out += "\n";
    out += "ffb: dac reads";
    for (auto [a, n] : debug_dac_reads_) appendf(out, " %x(%u)", a, n);
    out += "\n";
    for (auto [k, n] : debug_ops_)
        if (k.second >> 28 == 0xE)
            appendf(out, "ffb: op e view %x fbc %08x ppc %06x x%u\n", (k.second >> 24) & 0xF, k.first, k.second & 0xFFFFFF, n);
        else
            appendf(out, "ffb: op %x fbc %08x ppc %06x x%u\n", k.second >> 28, k.first, k.second & 0xFFFFFF, n);
    // The X planes in use on the picture, by buffer.
    const Timing t = timing();
    for (unsigned b = 0; b < 3; ++b) {
        std::map<u32, unsigned> x;
        for (unsigned y = 0; y < t.height; y += 4)
            for (unsigned i = 0; i < t.width; i += 4) ++x[buf_[b][static_cast<std::size_t>(y) * kLine + i] >> (b == 2 ? 0 : 24)];
        appendf(out, "ffb: buffer %c %s:", 'A' + b, b == 2 ? "words" : "X");
        unsigned shown = 0;
        for (auto [v, n] : x)
            if (++shown <= 40) appendf(out, " %02x(%u)", v, n);
        out += "\n";
    }
    // Their colours (the most used), and the palettes' entries not black.
    for (unsigned b = 0; b < 2; ++b) {
        std::map<u32, unsigned> c;
        for (unsigned y = 0; y < t.height; y += 4)
            for (unsigned i = 0; i < t.width; i += 4) ++c[buf_[b][static_cast<std::size_t>(y) * kLine + i] & 0xFFFFFF];
        std::vector<std::pair<unsigned, u32>> by(c.size());
        std::transform(c.begin(), c.end(), by.begin(), [](auto e) { return std::pair{e.second, e.first}; });
        std::sort(by.rbegin(), by.rend());
        appendf(out, "ffb: buffer %c colours:", 'A' + b);
        for (std::size_t i = 0; i < std::min<std::size_t>(by.size(), 12); ++i) appendf(out, " %06x(%u)", by[i].second, by[i].first);
        out += "\n";
    }
    for (unsigned p = 0; p < 4; ++p) {
        unsigned lit = 0;
        for (unsigned i = 0; i < 256; ++i) lit += clut_[p * 256 + i] != 0;
        appendf(out, "ffb: palette %u: %u entries not black, [0] %06x [1] %06x [ff] %06x\n", p, lit, clut_[p * 256],
                clut_[p * 256 + 1], clut_[p * 256 + 255]);
    }
}

}  // namespace uv
