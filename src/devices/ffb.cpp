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
// lines high, the default) the FCode takes 1152 x 900 at 66 Hz, as it does
// on a real card with a monitor that has no sense or DDC lines.
constexpr u32 kStrap = 0x33;

constexpr u32 kFbcDevId = 0x3241906D;  // FBC_reg_ID, as a real card reads
// UCSR when idle: the FIFO's free entries in bits 0-11. (A real card's
// count when its FIFO is empty is not known; the X driver's smallest
// known FIFO is 124 entries, and it keeps 4 back.)
constexpr u32 kUcsrFifoEmpty = 0x80;  // as the card reads when idle
constexpr u32 kUcsrErrors = 0xC0000000;
// The raster operation register after reset: "new" in each plane group's
// field (colour, X, Y and Z), so a group whose field software never writes
// takes the pixel as given.
constexpr u32 kRopReset = 0x00838383;
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
constexpr u32 kTgenVideo = 1, kTgenTiming = 2, kTgenInterlaced = 0x40;
// Window transfer control (the X driver's names): DS busy, TCMD transfer,
// TE at the next frame (else field).
constexpr u32 kWtctrlBusy = 1, kWtctrlTransfer = 2, kWtctrlFrame = 4;
// User control: the window mode (bits 5:4: 0 Combined, 1 Separate_4, 2
// Separate_8, 3 reserved, shown as Combined) and the overlay enable.
constexpr u32 kUctrlMode = 0x30, kUctrlSeparate4 = 0x10, kUctrlSeparate8 = 0x20,
              kUctrlOverlay = 8;
// Separate_4: buffer A's X byte holds a 4-bit overlay and a 4-bit window
// ID. True: the overlay is the low nibble and the window ID the high one;
// false: the other way round.
constexpr bool kSeparate4OverlayLowNibble = true;
constexpr double kDacRefHz = 13.5e6;  // the PLLs' reference: the FCode's PLL settings give its mode clocks from it

// Window lookup entries (PAC2).
constexpr u32 kWlutTable = 0x30, kWlutLookup = 0x100, kWlutSource = 0x3000, kWlutTrue = 0x4000, kWlutBufferB = 0x8000;
// Overlay window lookup entries: the palette section (Separate_4) and the
// overlay type (0 none, 1 transparent, 2 opaque, 3 reserved: shown as none).
constexpr u32 kOwlutSection = 0xF, kOwlutType = 0xC00, kOwlutTransparent = 0x400, kOwlutOpaque = 0x800;

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
    regs_[0x258 / 4] = kRopReset;
    ucsr_ = kUcsrPickOff | kUcsrPickNoHit;
    latch_ = {};
    history_ = {};
    history_size_ = 0;
    interrupts_ = 0;
    transfer_pending_ = false;
    retrace_timer_.cancel();
    fast_fill_colour_ = 0;
    polygon_.clear();
    dac_ptr_ = cur_ptr_ = 0;
    ppll_ = gpll_ = pfctrl_ = uctrl_ = wtctrl_ = tmctrl_ = tcolorkey_ = wamask_ = 0;
    sanal_ = dacctrl_ = tgen_ = 0;
    mpdata_ = 3;
    monitor_.drive(true, true);
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
            return e == 0 ? kStrap << 24 : e == 0x10 ? monitor_.sense() << 24 : 0;
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
        // The monitor port's lines as they are: what the card drives, and
        // the monitor's pulling SDA low.
        case kDacMpsense: return (monitor_.scl() ? 1 : 0) | (monitor_.sda() ? 2 : 0);
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
    } else if (a >= kDacAovwlut && a < kDacAovwlut + 4) {
        owlut_[a - kDacAovwlut] = v;  // (the active entries: at once)
    } else if (a >= kDacTregs && a < kDacTregs + tregs_.size()) {
        tregs_[a - kDacTregs] = v & 0xFFF;
        schedule_retrace();
    } else {
        switch (a) {
            case kDacPpll: ppll_ = v; schedule_retrace(); break;
            case kDacGpll: gpll_ = v; break;
            case kDacPfctrl: pfctrl_ = v; schedule_retrace(); break;
            case kDacUctrl: uctrl_ = v; break;
            case kDacWtctrl:
                // The shadow window lookups to the active ones, at the next
                // vertical blank (interlaced, with TE the next frame's: the
                // blank after the second field; without, the next field's);
                // busy till then. The command bit reads 0; the others as
                // written.
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
            case kDacTgen: tgen_ = v; schedule_retrace(); break;
            case kDacMpdata:
                mpdata_ = v & 3;
                monitor_.drive(v & 1, v & 2);
                return;
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

// --- The monitor --------------------------------------------------------------

const std::vector<MonitorTiming>& Ffb::resolutions() {
    // The card's own timings for these modes (its FCode's PLL and timing
    // generator settings): pixel clock; front porch, sync and back porch.
    static const std::vector<MonitorTiming> kList = {
        {640, 480, 60, 25125, 16, 96, 48, 10, 2, 33, false},
        {800, 600, 75, 49500, 16, 80, 160, 1, 3, 21, true},
        {1024, 768, 60, 64800, 24, 136, 160, 3, 6, 29, false},
        {1024, 800, 84, 94500, 16, 128, 152, 2, 4, 31, false},
        {1152, 900, 66, 94500, 40, 128, 208, 2, 4, 31, false},
        {1280, 800, 76, 101250, 20, 114, 156, 3, 7, 36, false},
        {1280, 1024, 60, 108000, 48, 112, 248, 1, 3, 38, true},
        {1440, 900, 76, 135000, 40, 160, 240, 2, 3, 39, false},
        {1600, 1000, 66, 135000, 40, 136, 192, 2, 5, 32, false},
        {1600, 1200, 75, 202500, 64, 192, 304, 1, 3, 46, true},
        {1600, 1280, 76, 216000, 24, 216, 280, 2, 8, 50, false},
        {1920, 1080, 72, 216000, 48, 216, 376, 3, 3, 86, false},
        {1920, 1200, 70, 219375, 24, 228, 344, 1, 3, 36, false},
    };
    return kList;
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
        // Drawing is done when it is started: the raster processor and the
        // frame buffer are never busy, and the FIFO always empty; so the
        // pick status is final as soon as it is read.
        case 0x900: return (ucsr_ & ~0xFFFu) | kUcsrFifoEmpty;
        default:
            if (off >= 0x100 && off < 0x180) return setup_state_read((off - 0x100) / 4);
            return regs_[off / 4];
    }
}

namespace {

// Registers made of fields that a write changes only where it gives a
// value: two-bit fields (three for the buffers written) left alone by 0
// (the pixel processor control, the frame buffer control), and eight-bit
// fields whose top bit (the edit bit) says they are written: the raster
// operation (colour bits 7:0, X 15:8, Y and Z 23:16) and the compare.
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
        case 0x900: {
            // The errors cleared by writing them. The pick enable (19:18: 10
            // off, 11 on) and the pick status (17:16: 10 no hit, 11 hit, as
            // software restores a saved status) are edit fields: 00 leaves
            // them (01 too).
            ucsr_ &= ~(v & kUcsrErrors);
            const u32 en = v & kUcsrPickField, st = v & kUcsrPickStatus;
            if (en == kUcsrPickOn || en == kUcsrPickOff) ucsr_ = (ucsr_ & ~kUcsrPickField) | en;
            if (st == kUcsrPickHit || st == kUcsrPickNoHit) ucsr_ = (ucsr_ & ~kUcsrPickStatus) | st;
            return;
        }
        default: break;
    }
    r = v;
    if (off >= 0x100 && off < 0x180) return setup_state_write((off - 0x100) / 4, v);
    if (off >= 0x0C && off <= 0x44) return vertex_register(off, v);
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
enum class View { None, Dumb8, Dumb24, Dumb32, Smart8, Smart32, Dumb24B, Smart64, Video, Z16, Z8 };
constexpr bool dumb_view(View v) { return v == View::Dumb8 || v == View::Dumb24 || v == View::Dumb32 || v == View::Dumb24B; }
constexpr bool smart_view(View v) { return v == View::Smart8 || v == View::Smart32 || v == View::Smart64; }

// The 4:2:2 video views (8 MiB each: 2 bytes a pixel, 2048 a line), as the
// channel of View::Video: into buffer A, A line doubled, B, B line doubled,
// through the pixel processor, and that line doubled.
enum : unsigned { kVideoA, kVideoAD, kVideoB, kVideoBD, kVideoSmart, kVideoSmartD };

// The view at an offset from 0x1000000, the channel of an 8-bit view (0 R,
// 1 G, 2 B, 3 X) or of a video view, and the offset into the view.
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
    if (off < 0x7000000) return in = off - 0x5000000, View::Smart64;
    auto video = [&](u64 base, unsigned ch) { return channel = ch, in = off - base, View::Video; };
    if (off >= 0x8000000 && off < 0x8800000) return video(0x8000000, kVideoA);
    if (off >= 0x8800000 && off < 0x9000000) return video(0x8800000, kVideoAD);
    if (off >= 0x9000000 && off < 0xA000000) return in = off - 0x9000000, View::Dumb24B;
    if (off >= 0xA000000 && off < 0xA800000) return video(0xA000000, kVideoB);
    if (off >= 0xA800000 && off < 0xB000000) return video(0xA800000, kVideoBD);
    if (off >= 0xB000000 && off < 0xB800000) return in = off - 0xB000000, View::Z16;
    if (off >= 0xB800000 && off < 0xBC00000) return in = off - 0xB800000, View::Z8;
    if (off >= 0xC000000 && off < 0xC800000) return video(0xC000000, kVideoSmart);
    if (off >= 0xC800000 && off < 0xD000000) return video(0xC800000, kVideoSmartD);
    return View::None;
}

// --- Views whose layout is a guess (no software known to use them) ---
//
// SFB64: a pixel a 64-bit word, 2048 a line: the colour word (X, B, G, R)
// in the high half and buffer C's word (Y, Z) in the low, written as one
// pixel through the pixel processor (the colour to the colour buffers
// written, Y and Z to buffer C, compared with Z from the low half); read,
// the colour from the buffer read (A when that is C) and buffer C.
constexpr unsigned kSfb64Bytes = 8;
// The 4:2:2 views: a 32-bit word holds two pixels as Y0, Cb, Y1, Cr (from
// the top byte), turned into RGB as ITU-R BT.601 (Y 16-235, Cb and Cr
// 16-240) and written to the colour planes (the X planes kept); the line
// doubled views write each line given to two frame buffer lines. Only
// whole words are taken; reads give 0.
u32 ycbcr_to_rgb(int y, int cb, int cr) {
    const double l = 1.164 * (y - 16), u = cb - 128, v = cr - 128;
    auto c = [](double x) { return static_cast<u32>(std::clamp(std::floor(x + 0.5), 0.0, 255.0)); };
    return c(l + 2.017 * u) << 16 | c(l - 0.392 * u - 0.813 * v) << 8 | c(l + 1.596 * v);
}
// The Z views: 16 (0xC000000) or 8 (0xC800000) bits a pixel, the top bits
// of buffer C's depth (27:12 or 27:20); a write sets them and clears the
// depth's bits below, leaving the Y planes; written as they are (not
// through the pixel processor).
constexpr unsigned kZ16Shift = 12, kZ8Shift = 20;

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
    if (v == View::Video) return true;
    // A pixel from the buffer the view reads: the dumb views' own, the
    // smart views' by the frame buffer control.
    const unsigned rb = !dumb_view(v) ? read_buffer() : v == View::Dumb24B ? 1 : 0;
    auto pixel_of = [&](unsigned b, u64 x, u64 y) { return x < kLine && y < kLines ? buf_[b][y * kLine + x] : 0u; };
    auto pixel = [&](u64 x, u64 y) { return pixel_of(rb, x, y); };
    for (unsigned i = 0; i < size; ++i) {
        const u64 a = in + i;
        u32 b;
        if (v == View::Dumb8 || v == View::Smart8) {
            const u32 p = pixel(a % kLine, a / kLine);
            b = (p >> (channel == 3 ? 24 : 8 * channel)) & 0xFF;
        } else if (v == View::Smart64) {
            const u64 n = a / kSfb64Bytes, k = a % kSfb64Bytes;
            const u32 p = pixel_of(k < 4 ? (rb == 2 ? 0 : rb) : 2, n % kLine, n / kLine);
            b = (p >> (8 * (3 - k % 4))) & 0xFF;
        } else if (v == View::Z16) {
            const u64 n = a / 2;
            const u32 z = (pixel_of(2, n % kLine, n / kLine) >> kZ16Shift) & 0xFFFF;
            b = a % 2 ? z & 0xFF : z >> 8;
        } else if (v == View::Z8) {
            b = (pixel_of(2, a % kLine, a / kLine) >> kZ8Shift) & 0xFF;
        } else {
            const u64 n = a / 4;
            const u32 p = pixel(n % kLine, n / kLine);
            b = (p >> (8 * (3 - a % 4))) & 0xFF;
        }
        value = value << 8 | b;
    }
    return true;
}

// A pixel through SFB64: the high half the colour word, the low buffer C's.
void Ffb::smart64_write(unsigned x, unsigned y, u64 value, u64 lanes) {
    smart_view_write(x, y, static_cast<u32>(value >> 32), static_cast<u32>(lanes >> 32), static_cast<u32>(value),
                     static_cast<u32>(lanes));
    changed(y, y + 1);
}

// A word of a 4:2:2 view: two pixels.
void Ffb::video_write(unsigned view, u64 in, u32 word) {
    const unsigned line = static_cast<unsigned>(in / (u64{kLine} * 2)), x0 = static_cast<unsigned>(in % (u64{kLine} * 2) / 2);
    const bool doubled = view == kVideoAD || view == kVideoBD || view == kVideoSmartD;
    const int cb = (word >> 16) & 0xFF, cr = word & 0xFF;
    const u32 rgb[2] = {ycbcr_to_rgb(static_cast<int>(word >> 24), cb, cr), ycbcr_to_rgb(static_cast<int>((word >> 8) & 0xFF), cb, cr)};
    for (unsigned d = 0; d < (doubled ? 2u : 1u); ++d) {
        const unsigned y = doubled ? 2 * line + d : line;
        if (y >= kLines) continue;
        for (unsigned p = 0; p < 2; ++p) {
            const unsigned x = x0 + p;
            if (x >= kLine) continue;
            if (view == kVideoSmart || view == kVideoSmartD) {
                smart_view_write(x, y, rgb[p], 0xFFFFFF, 0, 0);
            } else {
                u32& px = buf_[view == kVideoA || view == kVideoAD ? 0 : 1][static_cast<std::size_t>(y) * kLine + x];
                px = (px & 0xFF000000) | rgb[p];
            }
        }
        changed(y, y + 1);
    }
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
    ++debug_ops_[{regs_[0x254 / 4], smart_view(v) ? 0xE0000000u | static_cast<u32>(v) << 24 | (regs_[0x200 / 4] & 0xFFFFFF)
                                                  : 0xD0000000u | static_cast<u32>(v) << 8 | channel}];
    auto byte = [&](unsigned i) { return static_cast<u32>(value >> (8 * (size - 1 - i))) & 0xFF; };
    auto dumb = [&](unsigned x, unsigned y, u32 bits, u32 mask) {
        const std::size_t i = static_cast<std::size_t>(y) * kLine + x;
        buf_[db][i] = (buf_[db][i] & ~mask) | (bits & mask);
        changed(y, y + 1);
    };
    if (v == View::Video) {
        if (in % 4 || size % 4) {
            engine_unmodelled(6, "a 4:2:2 view written but by whole words", static_cast<u32>(off + 0x1000000));
            return true;
        }
        for (unsigned i = 0; i < size; i += 4) video_write(channel, in + i, byte(i) << 24 | byte(i + 1) << 16 | byte(i + 2) << 8 | byte(i + 3));
        return true;
    }
    if (v == View::Z16 || v == View::Z8) {
        const unsigned per = v == View::Z16 ? 2 : 1, shift = v == View::Z16 ? kZ16Shift : kZ8Shift;
        for (unsigned i = 0; i < size; ++i) {
            const u64 a = in + i, n = a / per;
            if (n / kLine >= kLines) continue;
            const unsigned k = static_cast<unsigned>(a % per);
            const unsigned bit = shift + 8 * (per - 1 - k);  // where this byte goes
            u32& c = buf_[2][n / kLine * kLine + n % kLine];
            c = (c & ~((0xFFu << bit) | ((1u << shift) - 1))) | byte(i) << bit;
        }
        return true;
    }
    if (v == View::Smart64) {
        // Whole pixels, or the bytes of one.
        for (unsigned i = 0; i < size;) {
            const u64 a = in + i, n = a / kSfb64Bytes;
            const unsigned first = static_cast<unsigned>(a % kSfb64Bytes), count = std::min(kSfb64Bytes - first, size - i);
            u64 bits = 0, mask = 0;
            for (unsigned k = 0; k < count; ++k) {
                const unsigned shift = 8 * (7 - (first + k));
                bits |= u64{byte(i + k)} << shift;
                mask |= u64{0xFF} << shift;
            }
            i += count;
            if (n / kLine < kLines) smart64_write(static_cast<unsigned>(n % kLine), static_cast<unsigned>(n / kLine), bits, mask);
        }
        return true;
    }
    if (v == View::Dumb8 || v == View::Smart8) {
        const unsigned shift = channel == 3 ? 24 : 8 * channel;
        for (unsigned i = 0; i < size; ++i) {
            const u64 a = in + i;
            const u32 b = byte(i);
            const unsigned x = static_cast<unsigned>(a % kLine), y = static_cast<unsigned>(a / kLine);
            if (y >= kLines) continue;
            if (v == View::Dumb8) {
                dumb(x, y, b << shift, 0xFFu << shift);
            } else {
                smart_view_write(x, y, b << shift, 0xFFu << shift, b << shift, 0xFFu << shift, true);
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
            bits |= byte(i + k) << shift;
            mask |= 0xFFu << shift;
        }
        i += count;
        if (y >= kLines) continue;
        if (v == View::Dumb24 || v == View::Dumb24B) mask &= 0xFFFFFF;  // (the X plane kept)
        if (v == View::Smart32) {
            smart_view_write(x, y, bits, mask, bits, mask);
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
    // Vertical in lines, horizontal in pixels by two (4/2:1 pixel format)
    // or by four (8/2:1, the card's high resolutions): blank ends at
    // VBNP/HBNP and starts at VBAP/HBAP; the line ends at HSYNCAP, the
    // frame at VSAP. (The FCode's 1600 x 1200 at 75 Hz, 8/2:1, comes out as
    // VESA's timing only with four: 202.5 MHz, 2160 pixels a line.)
    const unsigned m = ppll_ & 0x7F, d = (ppll_ >> 7) & 0xF, pfd = (ppll_ >> 11) & 3;
    const unsigned per = (pfctrl_ & 3) == 3 ? 4 : 2;
    if (!(tgen_ & kTgenTiming) || !m || !d || tregs_[1] <= tregs_[0] || tregs_[6] <= tregs_[5] || !tregs_[3] ||
        !tregs_[8])
        return t;
    t.on = true;
    // Interlaced, the counters run in half-lines: a line is two of the
    // horizontal count, and a field the vertical count's half-lines (the
    // frame's lines, VBNP-VBAP the frame's picture). The picture starts
    // at HBNP of one half-line and ends at HBAP of the next (NTSC's 640
    // pixels in 780, PAL's 768 in 936).
    const bool interlaced = tgen_ & kTgenInterlaced;
    t.width = std::min((interlaced ? tregs_[8] + 1 - tregs_[5] + tregs_[6] : tregs_[6] - tregs_[5]) * per, kLine);
    t.height = std::min(tregs_[1] - tregs_[0], kLines);
    t.total = tregs_[3] + 1;
    const double pclk = kDacRefHz * m / d / (1u << pfd);
    t.line_cycles = cpu_hz_ * per * (tregs_[8] + 1) / pclk;
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
    // Interlaced, a transfer at the next frame waits out the frame's first
    // field (the even field count from time 0).
    const Timing t = timing();
    const bool first_field = t.on && (tgen_ & kTgenInterlaced) &&
                             (static_cast<u64>(static_cast<double>(sched_.now()) / (t.line_cycles * t.total)) & 1) == 0;
    if (transfer_pending_ && !((wtctrl_ & kWtctrlFrame) && first_field)) window_transfer();
    if ((interrupts_ & kIntVertEnable) && !(interrupts_ & kIntVertPending)) {
        interrupts_ |= kIntVertPending;
        ++debug_retraces_;
        if (on_interrupt_) on_interrupt_();
    }
    if (t.on) schedule_retrace();
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

    // Each pixel through its window ID's lookup entries. The window mode
    // says where the window ID (W) and the overlay pixel come from:
    //   Combined (and the reserved mode): buffer A's X planes are both.
    //   Separate_8: W is buffer B's X planes, the overlay A's (8 bits).
    //   Separate_4: A's X planes hold a 4-bit window ID and a 4-bit overlay;
    //     the window ID's low two bits pick the primary entry, its high two
    //     the overlay entry.
    // The primary entry is W & PMSK (0 leaves only entry 0); the overlay
    // entry W[7:6] & OMSK. With the overlay enabled that entry decides
    // first: none shows the primary entry; transparent shows it where A's X
    // planes under the transparent mask match the key, else the overlay;
    // opaque always the overlay. The overlay is a palette index (in
    // Separate_4, in the entry's palette section of 16) or, with no
    // lookup, grey.
    const bool video = tgen_ & kTgenVideo;
    const u32 mode = uctrl_ & kUctrlMode;
    const bool separate8 = mode == kUctrlSeparate8, separate4 = mode == kUctrlSeparate4,
               overlay = uctrl_ & kUctrlOverlay;
    const u32 pmsk = wamask_ & 0x3F, omsk = (wamask_ >> 8) & 3, key_mask = tmctrl_ & 0xFF, key = tcolorkey_ & key_mask;
    auto make_row = [&](unsigned y) {
        u32* out = &st.base[static_cast<std::size_t>(y) * width];
        if (!video) {
            std::fill(out, out + width, 0);
            return;
        }
        const u32* a = &buf_[0][static_cast<std::size_t>(y) * kLine];
        const u32* b = &buf_[1][static_cast<std::size_t>(y) * kLine];
        for (unsigned x = 0; x < width; ++x) {
            const u32 ax = a[x] >> 24;
            u32 primary, select, ov;
            if (separate4) {
                const u32 lo = ax & 0xF, hi = ax >> 4;
                const u32 w4 = kSeparate4OverlayLowNibble ? hi : lo;
                ov = kSeparate4OverlayLowNibble ? lo : hi;
                primary = (w4 & 3) & pmsk;
                select = (w4 >> 2) & omsk;
            } else {
                const u32 w = separate8 ? b[x] >> 24 : ax;
                ov = ax;
                primary = w & pmsk;
                select = (w >> 6) & omsk;
            }
            if (overlay) {
                const u32 o = owlut_[select], type = o & kOwlutType;
                if (type == kOwlutOpaque || (type == kOwlutTransparent && (ax & key_mask) != key)) {
                    const u32 i = separate4 ? (o & kOwlutSection) * 16 + ov : ov;
                    const u32 c = (o & kWlutLookup) ? clut_[((o & kWlutTable) >> 4) * 256 + i] : ov * 0x010101u;
                    out[x] = (c & 0xFF) << 16 | (c & 0xFF00) | (c >> 16 & 0xFF);
                    continue;
                }
            }
            const u32 e = pwlut_[primary];
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
    appendf(out, "ffb: timing (vbnp vbap vsnp vsap hsnp hbnp hbap hsyncnp hsyncap hscen hscenap epeqv eieqv eineqv)");
    for (u32 r : tregs_) appendf(out, " %u", r);
    appendf(out, "\nffb: dacctrl %x sanal %x\n", dacctrl_, sanal_);
    if (!monitor_.trace().empty()) out += "ffb: ddc" + monitor_.trace() + "\n";
    appendf(out, "ffb: owlut %x %x %x %x  cursor ctrl %x pos %x colours %x %x %x\n", owlut_[0], owlut_[1], owlut_[2],
            owlut_[3], cursor_ctrl_, cursor_pos_, cursor_colour_[0], cursor_colour_[1], cursor_colour_[2]);
    out += "ffb: pwlut";
    for (unsigned i = 0; i < 64; ++i) appendf(out, "%s%x", i % 16 ? " " : "\n  ", pwlut_[i]);
    out += "\nffb: fbc";
    for (u32 off = 0x200; off < 0x400; off += 4)
        if (regs_[off / 4] && (off < 0x380 || off >= 0x400)) appendf(out, " %03x=%08x", off, regs_[off / 4]);
    out += "\nffb: fbc kernel";
    for (u32 i = 0; i < kregs_.size(); ++i)
        if (kregs_[i]) appendf(out, " %05x=%08x", 0x10000 + i * 4, kregs_[i]);
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
    out += "ffb: palette 0 in full:";
    for (unsigned i = 0; i < 256; ++i) appendf(out, "%s%06x", i % 16 ? " " : "\n  ", clut_[i]);
    out += "\n";
    for (unsigned p = 0; p < 4; ++p) {
        unsigned lit = 0;
        for (unsigned i = 0; i < 256; ++i) lit += clut_[p * 256 + i] != 0;
        appendf(out, "ffb: palette %u: %u entries not black, [0] %06x [1] %06x [ff] %06x\n", p, lit, clut_[p * 256],
                clut_[p * 256 + 1], clut_[p * 256 + 255]);
    }
}

}  // namespace uv
