// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
// The Creator3D's raster processor and pixel processor: the operations
// software starts by writing their registers (the 2D ones from integer
// coordinates, the 3D ones from the setup unit's vertices), and what each
// pixel they (and the smart frame buffer views) write goes through on its
// way to the frame buffer: the colour, X, Y and Z sources, the area
// pattern, the view clip (with its Z bounds), the X clip, the buffer
// compares (the depth test among them), picking, depth cueing, blending,
// the raster operation, the plane masks and the plane groups enabled, into
// the buffers the frame buffer control selects.
#include <algorithm>
#include <climits>
#include <cmath>

#include "core/log.h"
#include "devices/ffb.h"

namespace uv {

namespace {

// Register offsets (words of the FBC).
constexpr u32 kBy = 0x60, kBx = 0x64, kDy = 0x68, kDx = 0x6C, kBh = 0x70, kBw = 0x74;
constexpr u32 kPpc = 0x200, kWid = 0x204, kFg = 0x208, kBg = 0x20C, kVclipMin = 0x220, kVclipMax = 0x224,
              kFbcCtl = 0x254, kRop = 0x258, kPmask = 0x290, kXpmask = 0x294, kDrawop = 0x300, kFontXy = 0x314,
              kFontW = 0x318, kFontInc = 0x31C, kPattern = 0x380, kBlendc = 0x244, kWidpmask = 0x35C, kAuxClip = 0x2A0;

constexpr u32 kLpat = 0x30C, kEbxi = 0x54;
constexpr u32 kConstY = 0x210, kConstZ = 0x214, kXclip = 0x218, kDcss = 0x21C, kVclipZmin = 0x228, kVclipZmax = 0x22C,
              kDcsf = 0x230, kDcsb = 0x234, kDczf = 0x238, kDczb = 0x23C, kBlendc1 = 0x248, kBlendc2 = 0x24C,
              kCmp = 0x25C, kMatchAb = 0x260, kMatchC = 0x264, kMagnAb = 0x268, kMagnC = 0x26C, kYpmask = 0x298,
              kZpmask = 0x29C, kDcss1 = 0x350, kDcs2 = 0x360, kDcd2 = 0x370;
constexpr unsigned kDrawDot = 0x00, kDrawAaDot = 0x01, kDrawLineCap = 0x02, kDrawLineOpen = 0x03, kDrawDdLine = 0x04,
                   kDrawAaLine = 0x05, kDrawTriangle = 0x06, kDrawRectangle = 0x08, kDrawFastFill = 0x09,
                   kDrawBcopy = 0x0A, kDrawVscroll = 0x0B;
// The vertex registers: alpha, red, green, blue, z; y and x of the next
// (strip) vertex, the restart vertex and the fan vertex. Writing an x
// register completes the vertex.
constexpr u32 kVAlpha = 0x0C, kVRed = 0x10, kVGreen = 0x14, kVBlue = 0x18, kVZ = 0x1C, kVY = 0x20, kVX = 0x24,
              kVRy = 0x30, kVRx = 0x34, kVFy = 0x40, kVFx = 0x44;
// The pixel processor control's fields.
constexpr u32 kPpcVce = 0x3000, kPpcVce3d = 0x3000, kPpcDce = 0x30000, kPpcForceWid = 0xC00000;
// Buffer C: depth in bits 27:0, the Y (stencil) planes in 31:28.
constexpr u32 kZBits = 0x0FFFFFFF, kYBits = 0xF0000000;
// The compare register's AB bytes: bit 6 passes the test for buffer A
// whatever it gives, bit 5 for buffer B.
constexpr u32 kCmpAAlways = 0x40, kCmpBAlways = 0x20;
// Fast fill: by holds the step (1 page fill, 2 block fill, 3 the colour
// block set up; 4 the page's block).
constexpr u32 kFastFillPage = 1, kFastFillBlock = 2, kFastFillColourBlock = 3;
// The frame buffer's organisation (FBCFG0's resolution field, which the
// card's FCode sets for the mode), and the block a block fill counts from
// in each, as Xsun's block fills take them: 16 x 4 in the high
// resolutions, 8 x 2 in stereo, 8 x 4 in the standard ones.
constexpr u32 kFbcfg0 = 0x270, kFbcfg0Res = 0x30, kResHigh = 0x30, kResStereo = 0x10;

// A raster operation (the X11 functions' order: bit 3 for neither source
// nor destination set, 2 destination only, 1 source only, 0 both).
u32 rop(unsigned code, u32 s, u32 d) {
    u32 r = 0;
    if (code & 8) r |= ~s & ~d;
    if (code & 4) r |= ~s & d;
    if (code & 2) r |= s & ~d;
    if (code & 1) r |= s & d;
    return r;
}

// Alpha blending (the pixel processor's blend enable), as the open drivers
// name the blend control: a source and a destination fraction (0, 1,
// 1 - alpha, alpha) of each colour, the alpha the pixel's X source value,
// 0xFF taken as 1.0 when forced. The colours' 24 bits.
constexpr u32 kPpcBlend = 0xC000, kBlendForceOne = 0x10;

u32 blend(u32 blendc, unsigned a, u32 src, u32 dst) {
    const double alpha = (blendc & kBlendForceOne) && a == 0xFF ? 1.0 : a / 256.0;
    auto fraction = [&](unsigned f) { return f == 0 ? 0.0 : f == 1 ? 1.0 : f == 2 ? 1.0 - alpha : alpha; };
    const double sf = fraction(blendc & 3), df = fraction((blendc >> 2) & 3);
    u32 out = 0;
    for (unsigned shift = 0; shift < 24; shift += 8) {
        const double c = ((src >> shift) & 0xFF) * sf + ((dst >> shift) & 0xFF) * df;
        out |= static_cast<u32>(std::min(255.0, std::floor(c + 0.5))) << shift;
    }
    return out;
}

// A magnitude test (the X clip's and the compares'): 0 always, 1 greater,
// 2 equal, 3 greater or equal, 4 never, 5 less or equal, 6 not equal,
// 7 less; the new value against the reference (or the stored one).
bool magnitude(unsigned code, u32 n, u32 ref) {
    switch (code & 7) {
        case 0: return true;
        case 1: return n > ref;
        case 2: return n == ref;
        case 3: return n >= ref;
        case 4: return false;
        case 5: return n <= ref;
        case 6: return n != ref;
        default: return n < ref;
    }
}

// A match test: 0 always, 1 never, 2 equal, 3 not equal.
bool match(unsigned code, u32 n, u32 ref) {
    switch (code & 3) {
        case 0: return true;
        case 1: return false;
        case 2: return n == ref;
        default: return n != ref;
    }
}

// A vertex's colour or alpha (2.30, 1.0 = 2^30) as the pixel's 8 bits:
// bits 29:22, 255 from 1.0 up.
u32 component8(double v) { return static_cast<u32>(std::clamp(std::floor(v / 4194304.0), 0.0, 255.0)); }
// A vertex's depth (1.0 = 2^30) as the stored 28 bits: bits 29:2, the
// largest from 1.0 up.
u32 depth28(double v) { return static_cast<u32>(std::clamp(std::floor(v / 4.0), 0.0, static_cast<double>(kZBits))); }

// The line pattern (lpat): 16 bits from bit 0, each repeated by the scale,
// of the pattern's length (0: 16), clear bits not drawn; where the pattern
// and the scale stand is kept for the next line (written back), so a
// pattern runs on from one line to the next until lpat is written.
struct LinePattern {
    u32 lpat;
    unsigned len, scale, ptr, sptr;
    explicit LinePattern(u32 v)
        : lpat(v), len((v >> 16) & 0xF ? (v >> 16) & 0xF : 16), scale((v >> 20) & 0xF ? (v >> 20) & 0xF : 1),
          ptr((v >> 24) & 0xF), sptr(v >> 28) {}
    // Whether this pixel is drawn; on to the next.
    bool next() {
        const bool on = !lpat || ((lpat >> ptr) & 1);
        if (lpat && ++sptr >= scale) sptr = 0, ptr = (ptr + 1) % len;
        return on;
    }
    u32 value() const { return lpat ? (lpat & 0x00FFFFFF) | (ptr & 0xF) << 24 | (sptr & 0xF) << 28 : lpat; }
};

// The rows a primitive drew in.
struct Rows {
    int top = INT_MAX, bottom = -1;
    void add(int y) { top = std::min(top, y), bottom = std::max(bottom, y); }
};

// --- Rasterisation rules (each in one place) ---
//
// Triangles sample each pixel at its integer coordinates (i.0, j.0) of the
// 16.16 vertex space; vertices are taken to kTriangleSubBits of fraction.
// A sample exactly on an edge is drawn when the edge is a top or a left one
// (the interior below or to the right of it), so the shared edges of a
// strip or fan are drawn once.
constexpr unsigned kTriangleSubBits = 12;
bool edge_covers(s64 e, s64 dx, s64 dy) { return e > 0 || (e == 0 && (dy < 0 || (dy == 0 && dx > 0))); }
// Lines (DDLINE, AALINE) step one pixel per integer major coordinate from
// the start (drawn) to the end (not drawn); the minor coordinate is the
// exact line's there, rounded (halves up).
int line_minor_pixel(double n) { return static_cast<int>(std::floor(n + 0.5)); }
// Antialiasing: a pixel's coverage (0-1) scales its alpha (the X source).
u32 aa_alpha(u32 alpha, double coverage) { return static_cast<u32>(std::clamp(std::floor(alpha * coverage + 0.5), 0.0, 255.0)); }

}  // namespace

void Ffb::engine_unmodelled(unsigned what, const char* text, u32 detail) {
    if (engine_warned_ & (1u << what)) return;
    engine_warned_ |= 1u << what;
    UV_LOG(Warn, "ffb", "not modelled: %s (%#x)", text, detail);
}

// The view clip (the pixel processor's view clip enable, 2D or 3D) and the
// auxiliary clip: four rectangles (0x2A0-0x2BC, a minimum and a maximum
// each, inclusive, y in the top half) that add to the view clip (a pixel
// in any of them is drawn too) or are taken out of it, as the auxiliary
// clip enable says. Xsun draws a window's text with the view clip a row
// and the rest of the window in an added rectangle.
bool Ffb::clipped(int x, int y) const {
    if (x < 0 || y < 0 || x >= static_cast<int>(kLine) || y >= static_cast<int>(kLines)) return true;
    auto inside = [&](u32 lo, u32 hi) {
        return x >= static_cast<int>(lo & 0xFFFF) && y >= static_cast<int>(lo >> 16) && x <= static_cast<int>(hi & 0xFFFF) &&
               y <= static_cast<int>(hi >> 16);
    };
    const u32 ppc = regs_[kPpc / 4], vce = ppc & 0x3000, ace = (ppc >> 18) & 3;
    const bool in_view = !(vce == 0x2000 || vce == 0x3000) || inside(regs_[kVclipMin / 4], regs_[kVclipMax / 4]);
    if (ace != 2 && ace != 3) return !in_view;
    bool in_aux = false;
    for (u32 r = kAuxClip; r < kAuxClip + 0x20 && !in_aux; r += 8) in_aux = inside(regs_[r / 4], regs_[(r + 4) / 4]);
    return ace == 3 ? !(in_view || in_aux) : (!in_view || in_aux);
}

// The depth cue (the pixel processor's depth cue enable): each colour
// channel c (not X) becomes ((c + k1) * s) / 256 + k2, clamped, where k1
// and k2 are the channel's 9-bit two's-complement values in BLENDC1 and
// BLENDC2 (red bits 8:0, green 17:9, blue 26:18) and s the scale (0-256)
// at the pixel's depth: DCSF at and before the front bound DCZF, DCSB at
// and past the back bound DCZB, and between them falling from the front
// scale along the slope DCSS, s = DCSF - M * 2^E * (z - DCZF) / 2^28 (M the
// slope's 10-bit signed mantissa, bits 31:22, E its exponent, bits 4:0).
// Up to three more breakpoints (DCD2-DCD4, each used when it lies above the
// one before and below the back bound) start segments of their own from
// their scales (DCS2-DCS4) along their slopes (DCSS1-DCSS3). With k1 = -C
// and k2 = C this fades the colour toward C.
u32 Ffb::depth_cue(u32 colour, u32 z) const {
    auto reg = [&](u32 off) { return regs_[off / 4]; };
    auto scale = [](u32 v) { return static_cast<s64>(std::min<u32>(v & 0x1FF, 256)); };
    const u32 front = reg(kDczf) & kZBits, back = reg(kDczb) & kZBits;
    s64 s;
    if (z <= front) {
        s = scale(reg(kDcsf));
    } else if (z >= back) {
        s = scale(reg(kDcsb));
    } else {
        u32 from = front, slope = reg(kDcss);
        s64 start = scale(reg(kDcsf));
        for (unsigned k = 0; k < 3; ++k) {
            const u32 d = reg(kDcd2 + 4 * k) & kZBits;
            if (d <= from || d >= back) break;
            if (z < d) break;
            from = d, start = scale(reg(kDcs2 + 4 * k)), slope = reg(kDcss1 + 4 * k);
        }
        const s64 m = static_cast<s32>(slope) >> 22;  // signed 10 bits
        const unsigned e = std::min<u32>(slope & 0x1F, 19);
        const s64 step = m * (s64{1} << e) * static_cast<s64>(z - from);
        s = start - (step >= 0 ? step >> 28 : -((-step + (s64{1} << 28) - 1) >> 28));
    }
    s = std::clamp<s64>(s, 0, 256);
    const u32 k1 = reg(kBlendc1), k2 = reg(kBlendc2);
    u32 out = 0;
    for (unsigned ch = 0; ch < 3; ++ch) {
        auto term = [&](u32 v) { return static_cast<s64>(static_cast<s32>(((v >> (9 * ch)) & 0x1FF) << 23) >> 23); };
        const s64 c = (colour >> (8 * ch)) & 0xFF;
        const s64 v = (c + term(k1)) * s;
        const s64 r = (v >= 0 ? v / 256 : -((-v + 255) / 256)) + term(k2);
        out |= static_cast<u32>(std::clamp<s64>(r, 0, 255)) << (8 * ch);
    }
    return out;
}

// A pixel past the clips: the X clip, the buffer compares, picking, then
// written. In order:
//   X clip (0x218): the X source value against XREF (bits 7:0) by the
//     test in bits 18:16 (a magnitude code); the alpha test.
//   Compares (CMP, 0x25C, a byte each): buffer C's match (31:24) and
//     magnitude (23:16) tests, the new Y and Z in their places
//     ((y << 28) | z) against buffer C's word, both through their masks
//     (MATCHC, MAGNC); the depth test is the magnitude test with MAGNC
//     0x0FFFFFFF. The AB match (15:8) and magnitude (7:0) tests, the
//     current window ID in the X byte with the colour, against each colour
//     buffer's word through MATCHAB / MAGNAB; bit 6 of a byte passes its
//     test for buffer A, bit 5 for buffer B. The AB tests are made for
//     each colour buffer written (one failing them is not written), or,
//     when none is, for the one read (A when C is read).
//   Picking: with picking enabled, a pixel passing every test sets the
//     pick status to hit, whatever is then written.
//   The write: the depth cue then the blend on the colour (the blend's
//     alpha the X source value); the X planes take the X source value, or
//     the current window ID when forced. Into each colour buffer written
//     through the raster operation (bits 3:0 colour, 11:8 X) and the plane
//     groups: each group's enable 01 off, 10 on (all its planes), 11 on
//     through its plane mask (pmask for red, green and blue, xpmask for X),
//     as Xsun uses them: its 24-bit images go through the 32-bit view with
//     the colour groups on and a plane mask of 0, its 8-bit drawing through
//     red's mask. With the window enable "use window mask", the X plane is
//     written through the window ID plane mask (widpmask) too: Xsun paints a
//     window's ID into its low six planes and the overlay selection into
//     the top two that way. Into buffer C through its raster operation
//     (bits 19:16) and its own groups: Z (ZE, through ZPMASK bits 27:0) and
//     Y (YE, through YPMASK bits 3:0 for planes 31:28); the colour and X
//     enables do not reach it.
void Ffb::pixel_ops(unsigned x, unsigned y, const Fragment& f) {
    const u32 ppc = regs_[kPpc / 4], fbc = regs_[kFbcCtl / 4], cmp = regs_[kCmp / 4], xclip = regs_[kXclip / 4];
    if (!magnitude(xclip >> 16, f.x & 0xFF, xclip & 0xFF)) return;
    const std::size_t i = static_cast<std::size_t>(y) * kLine + x;
    const u32 c_new = (f.y & 0xF) << 28 | (f.z & kZBits), c_old = buf_[2][i];
    if (!match(cmp >> 24, c_new & regs_[kMatchC / 4], c_old & regs_[kMatchC / 4]) ||
        !magnitude(cmp >> 16, c_new & regs_[kMagnC / 4], c_old & regs_[kMagnC / 4]))
        return;
    const u32 ab_new = (regs_[kWid / 4] & 0xFF) << 24 | (f.colour & 0xFFFFFF);
    auto ab_passes = [&](unsigned b) {
        const u32 old = buf_[b][i], mt = (cmp >> 8) & 0xFF, mg = cmp & 0xFF;
        const u32 always = b == 0 ? kCmpAAlways : kCmpBAlways;
        const u32 mm = regs_[kMatchAb / 4], gm = regs_[kMagnAb / 4];
        return ((mt & always) || match(mt, ab_new & mm, old & mm)) && ((mg & always) || magnitude(mg, ab_new & gm, old & gm));
    };
    const unsigned wb = write_buffers();
    const bool pass[2] = {(wb & 1) && ab_passes(0), (wb & 2) && ab_passes(1)};
    if ((wb & 3) ? !(pass[0] || pass[1]) : !ab_passes(read_buffer() == 1 ? 1 : 0)) return;
    if ((ucsr_ & kUcsrPickField) == kUcsrPickOn) ucsr_ |= kUcsrPickHit;

    const u32 r = regs_[kRop / 4];
    auto group = [&](unsigned shift, u32 planes, u32 pmask) {
        const unsigned e = (fbc >> shift) & 3;
        return e == 1 ? 0u : e == 2 ? planes : planes & pmask;  // (0, no change written, as 11)
    };
    if (wb & 3) {
        const bool window_mask = ((fbc >> 20) & 3) == 3;
        const u32 pmask = regs_[kPmask / 4];
        u32 xmask = group(6, 0xFF, regs_[kXpmask / 4]);
        if (window_mask) xmask &= regs_[kWidpmask / 4];
        const u32 mask = (group(0, 0xFFu, pmask) | group(2, 0xFF00u, pmask) | group(4, 0xFF0000u, pmask) | (xmask & 0xFF) << 24) &
                         f.lanes;
        u32 colour = f.colour & 0xFFFFFF;
        if ((ppc & kPpcDce) == kPpcDce) colour = depth_cue(colour, f.z);
        const u32 xw = (ppc & kPpcForceWid) == kPpcForceWid ? regs_[kWid / 4] & 0xFF : f.x & 0xFF;
        for (unsigned b = 0; b < 2; ++b) {
            if (!pass[b]) continue;
            const u32 old = buf_[b][i];
            const u32 src = xw << 24 | ((ppc & kPpcBlend) == kPpcBlend ? blend(regs_[kBlendc / 4], f.x & 0xFF, colour, old) : colour);
            const u32 n = (rop(r & 0xF, src, old) & 0xFFFFFF) | (rop((r >> 8) & 0xF, src, old) & 0xFF000000);
            buf_[b][i] = (old & ~mask) | (n & mask);
        }
    }
    if (wb & 4) {
        const u32 mask = (group(10, kZBits, regs_[kZpmask / 4]) | group(8, kYBits, regs_[kYpmask / 4] << 28)) & f.c_lanes;
        const u32 n = rop((r >> 16) & 0xF, c_new, c_old);
        buf_[2][i] = (c_old & ~mask) | (n & mask);
    }
}

// A pixel written through a smart view: the colour the value written, or
// the foreground when the pixel processor's colour source is constant
// (then written whole); the X value the window ID or the foreground's when
// the X source says so, else the value's top byte (NetBSD draws its
// antialiased console font this way, writing only each pixel's alpha);
// buffer C's Z and Y the value written (bits 27:0, 31:28) or the constant
// Z and Y when their sources are constant. (Through the 8-bit views the
// colour and X are the bytes written whatever the sources, as NetBSD's
// driver notes.) No view clip: the pixel goes from the X clip on.
void Ffb::smart_view_write(unsigned x, unsigned y, u32 value, u32 lanes, u32 c_value, u32 c_lanes, bool byte_view) {
    const u32 ppc = regs_[kPpc / 4], fg = regs_[kFg / 4];
    Fragment f;
    f.colour = value & 0xFFFFFF;
    f.x = value >> 24;
    if (!byte_view) {
        if ((ppc & 3) == 3) f.colour = fg & 0xFFFFFF, lanes |= 0xFFFFFF;
        if ((ppc & 0xC) == 0x4) f.x = regs_[kWid / 4] & 0xFF;
        else if ((ppc & 0xC) == 0xC) f.x = fg >> 24;
    }
    f.z = (ppc & 0xC0) == 0xC0 ? regs_[kConstZ / 4] & kZBits : c_value & kZBits;
    f.y = (ppc & 0x30) == 0x30 ? regs_[kConstY / 4] & 0xF : c_value >> 28;
    f.lanes = lanes;
    f.c_lanes = c_lanes;
    pixel_ops(x, y, f);
}

// A pixel the 2D operations draw: its colour the foreground (or the
// background for the pattern's or font's clear bits); its X value the
// current window ID (WID source) or the colour's top byte; its Z the
// constant Z (Z source constant) or the last vertex Z latched; its Y the
// constant Y (Y source constant) or 0.
void Ffb::draw_pixel(unsigned x, unsigned y, u32 colour, bool background) {
    const u32 ppc = regs_[kPpc / 4];
    Fragment f;
    f.colour = colour & 0xFFFFFF;
    f.x_from_colour = (ppc & 0xC) != 0x4;
    f.x = f.x_from_colour ? colour >> 24 : regs_[kWid / 4] & 0xFF;
    f.z = (ppc & 0xC0) == 0xC0 ? regs_[kConstZ / 4] & kZBits : depth28(latch_.z);
    f.y = (ppc & 0x30) == 0x30 ? regs_[kConstY / 4] & 0xF : 0;
    draw_fragment(static_cast<int>(x), static_cast<int>(y), f, background);
}

// A pixel any operation draws: the view clip and auxiliary clips, the area
// pattern (32 x 32, screen aligned, the leftmost pixel in bit 31: its clear
// bits in the background, or not drawn when the background is
// transparent), the view clip's Z bounds in its 3D mode (VCLIPZMIN,
// VCLIPZMAX, inclusive), then the pixel processor.
void Ffb::draw_fragment(int x, int y, Fragment f, bool background) {
    if (clipped(x, y)) return;
    const u32 ppc = regs_[kPpc / 4];
    if ((ppc & 0xC00) == 0xC00) {
        const bool set = (regs_[kPattern / 4 + static_cast<unsigned>(y) % 32] >> (31 - static_cast<unsigned>(x) % 32)) & 1;
        if (!set) {
            if ((ppc & 0x300) == 0x300) return;  // transparent background
            background = true;
        }
    }
    if (background) {
        if ((ppc & 0x300) == 0x300) return;
        const u32 bg = regs_[kBg / 4];
        f.colour = bg & 0xFFFFFF;
        if (f.x_from_colour) f.x = bg >> 24;
    }
    if ((ppc & kPpcVce) == kPpcVce3d &&
        (f.z < (regs_[kVclipZmin / 4] & kZBits) || f.z > (regs_[kVclipZmax / 4] & kZBits)))
        return;
    pixel_ops(static_cast<unsigned>(x), static_cast<unsigned>(y), f);
}

void Ffb::rectangle(int x, int y, int w, int h, u32 fg) {
    const int x0 = std::max(x, 0), y0 = std::max(y, 0);
    const int x1 = std::min(x + w, static_cast<int>(kLine)), y1 = std::min(y + h, static_cast<int>(kLines));
    if (x0 >= x1 || y0 >= y1) return;
    for (int j = y0; j < y1; ++j)
        for (int i = x0; i < x1; ++i) draw_pixel(static_cast<unsigned>(i), static_cast<unsigned>(j), fg);
    changed(static_cast<unsigned>(y0), static_cast<unsigned>(y1));
}

// A block of pixels moved within the frame buffer, as they are (the planes
// the frame buffer control enables), from the buffer read to those
// written; overlapping blocks move as if copied whole.
void Ffb::vscroll(int sx, int sy, int dx, int dy, int w, int h) {
    if (w <= 0 || h <= 0) return;
    const u32 fbc = regs_[kFbcCtl / 4];
    auto on = [&](unsigned shift) { return ((fbc >> shift) & 3) != 1; };
    const u32 mask = (on(0) ? 0xFFu : 0) | (on(2) ? 0xFF00u : 0) | (on(4) ? 0xFF0000u : 0) | (on(6) ? 0xFF000000u : 0);
    const unsigned rb = read_buffer(), wb = write_buffers();
    std::vector<u32> row(static_cast<std::size_t>(w));
    const bool up = dy <= sy;
    int top = static_cast<int>(kLines), bottom = 0;
    for (int k = 0; k < h; ++k) {
        const int j = up ? k : h - 1 - k;
        const int ys = sy + j, yd = dy + j;
        if (ys < 0 || yd < 0 || ys >= static_cast<int>(kLines) || yd >= static_cast<int>(kLines)) continue;
        for (int i = 0; i < w; ++i) {
            const int xs = sx + i;
            row[static_cast<std::size_t>(i)] =
                xs >= 0 && xs < static_cast<int>(kLine) ? buf_[rb][static_cast<std::size_t>(ys) * kLine + static_cast<unsigned>(xs)] : 0;
        }
        for (unsigned b = 0; b < 3; ++b) {
            if (!(wb & (1u << b))) continue;
            for (int i = 0; i < w; ++i) {
                const int xd = dx + i;
                if (xd < 0 || xd >= static_cast<int>(kLine)) continue;
                u32& p = buf_[b][static_cast<std::size_t>(yd) * kLine + static_cast<unsigned>(xd)];
                p = (p & ~mask) | (row[static_cast<std::size_t>(i)] & mask);
            }
        }
        top = std::min(top, yd);
        bottom = std::max(bottom, yd + 1);
    }
    if (top < bottom) changed(static_cast<unsigned>(top), static_cast<unsigned>(bottom));
}

// The operation in the draw operation register, started by the write of
// its last register (bw).
void Ffb::start_drawop(u32 written) {
    ++debug_ops_[{regs_[kFbcCtl / 4], regs_[kPpc / 4] | (regs_[kDrawop / 4] & 0xF) << 28}];
    const auto reg = [&](u32 off) { return static_cast<int>(static_cast<s32>(regs_[off / 4])); };
    switch (regs_[kDrawop / 4] & 0xF) {
        case kDrawDot: {
            const int x = reg(kBw), y = reg(kBh);
            if (x >= 0 && y >= 0 && x < static_cast<int>(kLine) && y < static_cast<int>(kLines)) {
                draw_pixel(static_cast<unsigned>(x), static_cast<unsigned>(y), regs_[kFg / 4]);
                changed(static_cast<unsigned>(y), static_cast<unsigned>(y) + 1);
            }
            break;
        }
        case kDrawLineCap:
        case kDrawLineOpen: line(reg(kBx), reg(kBy), reg(kBw), reg(kBh), (regs_[kDrawop / 4] & 0xF) == kDrawLineCap); break;
        case kDrawFastFill: fast_fill(written); break;
        case kDrawRectangle: rectangle(reg(kBx), reg(kBy), reg(kBw), reg(kBh), regs_[kFg / 4]); break;
        case kDrawVscroll:
        case kDrawBcopy: vscroll(reg(kBx), reg(kBy), reg(kDx), reg(kDy), reg(kBw), reg(kBh)); break;
        default: engine_unmodelled(8 + (regs_[kDrawop / 4] & 0xF), "a draw operation", regs_[kDrawop / 4]); break;
    }
}

// A Bresenham line from (x0, y0) to (x1, y1), its last pixel drawn or not
// (the cap and open operations), through the line pattern when one is
// set. The line's end is the next one's start.
void Ffb::line(int x0, int y0, int x1, int y1, bool last) {
    LinePattern pattern(regs_[kLpat / 4]);
    const u32 fg = regs_[kFg / 4];
    const int dx = std::abs(x1 - x0), dy = std::abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx - dy, x = x0, y = y0, top = y0, bottom = y0;
    for (;;) {
        const bool end = x == x1 && y == y1;
        if (end && !last) break;
        if (pattern.next()) {
            if (x >= 0 && y >= 0 && x < static_cast<int>(kLine) && y < static_cast<int>(kLines)) {
                draw_pixel(static_cast<unsigned>(x), static_cast<unsigned>(y), fg);
                top = std::min(top, y), bottom = std::max(bottom, y);
            }
        }
        if (end) break;
        const int e2 = 2 * err;
        if (e2 > -dy) err -= dy, x += sx;
        if (e2 < dx) err += dx, y += sy;
    }
    regs_[kLpat / 4] = pattern.value();
    regs_[kBy / 4] = static_cast<u32>(y1), regs_[kBx / 4] = static_cast<u32>(x1);
    top = std::max(top, 0), bottom = std::min(bottom, static_cast<int>(kLines) - 1);
    if (top <= bottom) changed(static_cast<unsigned>(top), static_cast<unsigned>(bottom) + 1);
}

// --- The setup unit and the 3D operations -------------------------------------
//
// A vertex is given as alpha, red, green, blue (2.30, 1.0 = 0x40000000),
// z (1.0 = 2^30), y and x (16.16 two's complement, frame buffer
// coordinates); each value is latched until written again, and the write
// of an x register completes the vertex. Which x register says how the
// vertex joins the last three kept:
//   rxf (0x34) restart: they are only the new one; nothing is drawn;
//   x (0x24) strip: it is added, the oldest of three dropped;
//   dmxf (0x44) fan: it is added, the middle one of three dropped (the
//     first, the restart vertex, kept).
// Then, by the draw operation: DOT and AADOT draw a dot at it; DDLINE and
// AALINE a line from the vertex before it; TRIANGLE, once three are held,
// the triangle they make. A restart draws nothing; nor does a line with no
// vertex before it. The pixel processor's state (foreground, sources) is
// taken as it stands when the x register is written.
void Ffb::vertex_register(u32 off, u32 v) {
    switch (off) {
        case kVAlpha: latch_.a = v; return;
        case kVRed: latch_.r = v; return;
        case kVGreen: latch_.g = v; return;
        case kVBlue: latch_.b = v; return;
        case kVZ: latch_.z = v; return;
        case kVY:
        case kVRy:
        case kVFy: latch_.y = static_cast<s32>(v); return;
        case kVX:
        case kVRx:
        case kVFx: break;
        default: return;
    }
    Vertex nv = latch_;
    nv.x = static_cast<s32>(v);
    if (off == kVRx) {
        history_[0] = nv, history_size_ = 1;
        return;
    }
    if (history_size_ < 3) {
        history_[history_size_++] = nv;
    } else if (off == kVFx) {
        history_[1] = history_[2], history_[2] = nv;
    } else {
        history_[0] = history_[1], history_[1] = history_[2], history_[2] = nv;
    }
    const unsigned op = regs_[kDrawop / 4] & 0xF;
    if (op == kDrawDot || op == kDrawAaDot || op == kDrawDdLine || op == kDrawAaLine || op == kDrawTriangle)
        ++debug_ops_[{regs_[kFbcCtl / 4], regs_[kPpc / 4] | op << 28}];
    switch (op) {
        case kDrawDot: vertex_dot(nv); break;
        case kDrawAaDot: vertex_aadot(nv); break;
        case kDrawDdLine:
        case kDrawAaLine:
            if (history_size_ >= 2) vertex_line(history_[history_size_ - 2], nv, op == kDrawAaLine);
            break;
        case kDrawTriangle:
            if (history_size_ == 3) triangle(history_[0], history_[1], history_[2]);
            break;
        default: break;
    }
}

// The setup unit's state as 32 words (0x100-0x17C), for software to save
// and restore across a context switch: the latched alpha, red, green,
// blue, z and y; the number of vertices kept; then the three kept, oldest
// first, each x, y, z, alpha, red, green, blue. (The words past them read
// as 0.)
u32 Ffb::setup_state_read(unsigned word) const {
    const Vertex& l = latch_;
    switch (word) {
        case 0: return l.a;
        case 1: return l.r;
        case 2: return l.g;
        case 3: return l.b;
        case 4: return l.z;
        case 5: return static_cast<u32>(l.y);
        case 6: return history_size_;
        default: break;
    }
    if (word < 7 || word >= 7 + 3 * 7) return 0;
    const Vertex& v = history_[(word - 7) / 7];
    const u32 fields[7] = {static_cast<u32>(v.x), static_cast<u32>(v.y), v.z, v.a, v.r, v.g, v.b};
    return fields[(word - 7) % 7];
}

void Ffb::setup_state_write(unsigned word, u32 value) {
    Vertex& l = latch_;
    switch (word) {
        case 0: l.a = value; return;
        case 1: l.r = value; return;
        case 2: l.g = value; return;
        case 3: l.b = value; return;
        case 4: l.z = value; return;
        case 5: l.y = static_cast<s32>(value); return;
        case 6: history_size_ = std::min<u32>(value, 3); return;
        default: break;
    }
    if (word < 7 || word >= 7 + 3 * 7) return;
    Vertex& v = history_[(word - 7) / 7];
    switch ((word - 7) % 7) {
        case 0: v.x = static_cast<s32>(value); break;
        case 1: v.y = static_cast<s32>(value); break;
        case 2: v.z = value; break;
        case 3: v.a = value; break;
        case 4: v.r = value; break;
        case 5: v.g = value; break;
        default: v.b = value; break;
    }
}

// A pixel of a 3D operation from its (interpolated) vertex values, by the
// pixel processor control's sources: the colour the vertex colour (VAR) or
// the foreground's (CONST); the X value the current window ID (WID), the
// vertex alpha (VAR) or the foreground's top byte (CONST); the Z the
// vertex z (VAR) or the constant Z (CONST); the Y the constant Y (CONST),
// else 0 (no vertex gives one).
Ffb::Fragment Ffb::vertex_fragment(const Attributes& at) const {
    const u32 ppc = regs_[kPpc / 4], fg = regs_[kFg / 4];
    Fragment f;
    f.colour = (ppc & 3) == 3 ? fg & 0xFFFFFF : component8(at.b) << 16 | component8(at.g) << 8 | component8(at.r);
    const u32 xs = ppc & 0xC;
    f.x = xs == 0x4 ? regs_[kWid / 4] & 0xFF : xs == 0x8 ? component8(at.a) : fg >> 24;
    f.x_from_colour = xs != 0x4 && xs != 0x8;
    f.z = (ppc & 0xC0) == 0xC0 ? regs_[kConstZ / 4] & kZBits : depth28(at.z);
    f.y = (ppc & 0x30) == 0x30 ? regs_[kConstY / 4] & 0xF : 0;
    return f;
}

namespace {

Ffb::Attributes attributes(const Ffb::Vertex& v) { return {double(v.z), double(v.a), double(v.r), double(v.g), double(v.b)}; }

Ffb::Attributes mix(const Ffb::Attributes& p, double wp, const Ffb::Attributes& q, double wq, const Ffb::Attributes& r = {}, double wr = 0) {
    return {p.z * wp + q.z * wq + r.z * wr, p.a * wp + q.a * wq + r.a * wr, p.r * wp + q.r * wq + r.r * wr,
            p.g * wp + q.g * wq + r.g * wr, p.b * wp + q.b * wq + r.b * wr};
}

}  // namespace

// DOT from a vertex: the pixel holding it (the integer parts of x and y).
void Ffb::vertex_dot(const Vertex& v) {
    const int x = v.x >> 16, y = v.y >> 16;
    if (x < 0 || y < 0 || x >= static_cast<int>(kLine) || y >= static_cast<int>(kLines)) return;
    draw_fragment(x, y, vertex_fragment(attributes(v)));
    changed(static_cast<unsigned>(y), static_cast<unsigned>(y) + 1);
}

// AADOT: the up to 2 x 2 pixels around the vertex, each with the bilinear
// weight of its distance from it as coverage.
void Ffb::vertex_aadot(const Vertex& v) {
    const double fx = v.x / 65536.0, fy = v.y / 65536.0;
    const int i0 = static_cast<int>(std::floor(fx)), j0 = static_cast<int>(std::floor(fy));
    const double dx = fx - i0, dy = fy - j0;
    const Fragment base = vertex_fragment(attributes(v));
    Rows rows;
    for (int k = 0; k < 4; ++k) {
        const int i = i0 + (k & 1), j = j0 + (k >> 1);
        const double w = ((k & 1) ? dx : 1 - dx) * ((k >> 1) ? dy : 1 - dy);
        if (w <= 0 || i < 0 || j < 0 || i >= static_cast<int>(kLine) || j >= static_cast<int>(kLines)) continue;
        Fragment f = base;
        f.x = aa_alpha(f.x, w);
        draw_fragment(i, j, f);
        rows.add(j);
    }
    if (rows.bottom >= 0) changed(static_cast<unsigned>(rows.top), static_cast<unsigned>(rows.bottom) + 1);
}

// DDLINE and AALINE: from a to b in 16.16, colour, alpha and z interpolated
// along the line, through the line pattern (a step a pixel along the major
// axis; the pattern runs on into the next line). Antialiased, each step
// draws the two pixels either side of the exact line on the minor axis,
// with coverage 1 - d and d (d the line's fractional distance past the
// first).
void Ffb::vertex_line(const Vertex& a, const Vertex& b, bool antialiased) {
    const double x0 = a.x / 65536.0, y0 = a.y / 65536.0, x1 = b.x / 65536.0, y1 = b.y / 65536.0;
    const double dx = x1 - x0, dy = y1 - y0;
    if (dx == 0 && dy == 0) return;
    const bool xmajor = std::abs(dx) >= std::abs(dy);
    const double m0 = xmajor ? x0 : y0, m1 = xmajor ? x1 : y1, n0 = xmajor ? y0 : x0, n1 = xmajor ? y1 : x1;
    const double dm = m1 - m0;
    // The integer major coordinates from the start (inclusive) to the end (exclusive).
    const int step = dm > 0 ? 1 : -1;
    const int first = static_cast<int>(dm > 0 ? std::ceil(m0) : std::floor(m0));
    const int end = static_cast<int>(dm > 0 ? std::ceil(m1) : std::floor(m1));
    const Attributes pa = attributes(a), pb = attributes(b);
    LinePattern pattern(regs_[kLpat / 4]);
    Rows rows;
    auto plot = [&](int m, int n, const Fragment& f) {
        const int x = xmajor ? m : n, y = xmajor ? n : m;
        if (x < 0 || y < 0 || x >= static_cast<int>(kLine) || y >= static_cast<int>(kLines)) return;
        draw_fragment(x, y, f);
        rows.add(y);
    };
    for (int m = first; m != end; m += step) {
        if (!pattern.next()) continue;
        const double t = (m - m0) / dm;
        const double n = n0 + t * (n1 - n0);
        const Fragment f = vertex_fragment(mix(pa, 1 - t, pb, t));
        if (!antialiased) {
            plot(m, line_minor_pixel(n), f);
            continue;
        }
        const int k = static_cast<int>(std::floor(n));
        const double d = n - k;
        for (int s = 0; s < 2; ++s) {
            const double w = s ? d : 1 - d;
            if (w <= 0) continue;
            Fragment g = f;
            g.x = aa_alpha(f.x, w);
            plot(m, k + s, g);
        }
    }
    regs_[kLpat / 4] = pattern.value();
    if (rows.bottom >= 0) changed(static_cast<unsigned>(rows.top), static_cast<unsigned>(rows.bottom) + 1);
}

// TRIANGLE: each pixel whose sample point (its integer coordinates) lies
// inside the triangle, by the edge rule above; colour, alpha and z
// interpolated linearly in screen space. A triangle of no area draws
// nothing; either orientation draws.
void Ffb::triangle(const Vertex& v0, const Vertex& v1, const Vertex& v2) {
    constexpr unsigned kShift = 16 - kTriangleSubBits;
    struct P {
        s64 x, y;
    };
    P p[3] = {{v0.x >> kShift, v0.y >> kShift}, {v1.x >> kShift, v1.y >> kShift}, {v2.x >> kShift, v2.y >> kShift}};
    const Vertex* vs[3] = {&v0, &v1, &v2};
    s64 area = (p[1].x - p[0].x) * (p[2].y - p[0].y) - (p[1].y - p[0].y) * (p[2].x - p[0].x);
    if (area == 0) return;
    if (area < 0) std::swap(p[1], p[2]), std::swap(vs[1], vs[2]), area = -area;
    const s64 one = s64{1} << kTriangleSubBits;
    auto floor_px = [&](s64 v) { return v >= 0 ? v / one : -((-v + one - 1) / one); };
    auto ceil_px = [&](s64 v) { return -floor_px(-v); };
    const s64 xmin = std::max<s64>(ceil_px(std::min({p[0].x, p[1].x, p[2].x})), 0);
    const s64 xmax = std::min<s64>(floor_px(std::max({p[0].x, p[1].x, p[2].x})), kLine - 1);
    const s64 ymin = std::max<s64>(ceil_px(std::min({p[0].y, p[1].y, p[2].y})), 0);
    const s64 ymax = std::min<s64>(floor_px(std::max({p[0].y, p[1].y, p[2].y})), kLines - 1);
    if (xmin > xmax || ymin > ymax) return;
    // Edge k runs from vertex k+1 to k+2 (opposite vertex k): its function
    // is positive inside, and k's weight is it over the area.
    struct Edge {
        s64 dx, dy, ax, ay;
        s64 at(s64 x, s64 y) const { return dx * (y - ay) - dy * (x - ax); }
    };
    Edge e[3];
    for (int k = 0; k < 3; ++k) {
        const P& a = p[(k + 1) % 3];
        const P& b = p[(k + 2) % 3];
        e[k] = {b.x - a.x, b.y - a.y, a.x, a.y};
    }
    const Attributes at[3] = {attributes(*vs[0]), attributes(*vs[1]), attributes(*vs[2])};
    const double inv = 1.0 / static_cast<double>(area);
    Rows rows;
    for (s64 j = ymin; j <= ymax; ++j) {
        const s64 sy = j * one;
        s64 w[3];
        for (int k = 0; k < 3; ++k) w[k] = e[k].at(xmin * one, sy);
        for (s64 i = xmin; i <= xmax; ++i) {
            if (edge_covers(w[0], e[0].dx, e[0].dy) && edge_covers(w[1], e[1].dx, e[1].dy) &&
                edge_covers(w[2], e[2].dx, e[2].dy)) {
                const Attributes a = mix(at[0], w[0] * inv, at[1], w[1] * inv, at[2], w[2] * inv);
                draw_fragment(static_cast<int>(i), static_cast<int>(j), vertex_fragment(a));
                rows.add(static_cast<int>(j));
            }
            for (int k = 0; k < 3; ++k) w[k] -= e[k].dy * one;
        }
    }
    if (rows.bottom >= 0) changed(static_cast<unsigned>(rows.top), static_cast<unsigned>(rows.bottom) + 1);
}

// A fast fill, a step at a time as by says: the colour block set up (the
// foreground taken as the fill's colour) and the page's block, then a block
// fill or a page fill from (dx, dy), its width (bx or bw) and height (bh)
// counted from the block (or page) the start is in, in the colour taken.
// The 3D-RAM fills whole blocks at once; here it is drawn as a rectangle.
void Ffb::fast_fill(u32 written) {
    const u32 step = regs_[kBy / 4];
    if (step == kFastFillColourBlock) fast_fill_colour_ = regs_[kFg / 4];
    if (step != kFastFillBlock && step != kFastFillPage) return;
    const int x = static_cast<s32>(regs_[kDx / 4]), y = static_cast<s32>(regs_[kDy / 4]);
    const int w = static_cast<s32>(regs_[written / 4]), h = static_cast<s32>(regs_[kBh / 4]);
    const u32 res = kregs_[kFbcfg0 / 4] & kFbcfg0Res;
    const int bw = res == kResHigh ? 16 : 8, bh = res == kResStereo ? 2 : 4;
    const int x0 = step == kFastFillBlock ? x & ~(bw - 1) : x, y0 = step == kFastFillBlock ? y & ~(bh - 1) : y;
    rectangle(x, y, x0 + w - x, y0 + h - y, fast_fill_colour_);
}

// A polygon's vertex: x in the register written, y in the one before it.
// The last (ebxi) draws the polygon.
void Ffb::polygon_vertex(u32 written) {
    polygon_.emplace_back(static_cast<s32>(regs_[written / 4]), static_cast<s32>(regs_[(written - 4) / 4]));
    if (written != kEbxi) return;
    ++debug_ops_[{regs_[kFbcCtl / 4], regs_[kPpc / 4] | (regs_[kDrawop / 4] & 0xF) << 28}];
    polygon();
    polygon_.clear();
}

// A polygon filled in the foreground (through the area pattern and the view
// clip as any pixel drawn): each row from the top vertex to the bottom one
// (not drawn), from where its left edge crosses the row, rounded up, to
// where its right edge does (not drawn), as X fills a polygon; taken for
// the hardware's rule. Xsun gives it only polygons convex in y.
void Ffb::polygon() {
    const std::size_t n = polygon_.size();
    if (n < 3) return;
    int top = polygon_[0].second, bottom = top;
    for (const auto& p : polygon_) top = std::min(top, p.second), bottom = std::max(bottom, p.second);
    top = std::max(top, 0), bottom = std::min(bottom, static_cast<int>(kLines));
    const u32 fg = regs_[kFg / 4];
    std::vector<double> xs;
    for (int y = top; y < bottom; ++y) {
        xs.clear();
        for (std::size_t i = 0; i < n; ++i) {
            const auto [ax, ay] = polygon_[i];
            const auto [bx, by] = polygon_[(i + 1) % n];
            if (ay == by || y < std::min(ay, by) || y >= std::max(ay, by)) continue;
            xs.push_back(ax + static_cast<double>(y - ay) * (bx - ax) / (by - ay));
        }
        std::sort(xs.begin(), xs.end());
        for (std::size_t i = 0; i + 1 < xs.size(); i += 2) {
            const int x0 = std::max(static_cast<int>(std::ceil(xs[i])), 0);
            const int x1 = std::min(static_cast<int>(std::ceil(xs[i + 1])), static_cast<int>(kLine));
            for (int x = x0; x < x1; ++x) draw_pixel(static_cast<unsigned>(x), static_cast<unsigned>(y), fg);
        }
    }
    if (top < bottom) changed(static_cast<unsigned>(top), static_cast<unsigned>(bottom));
}

// A row of a font glyph: the font width's bits from bit 31 at the font
// position, set bits in the foreground, clear ones in the background
// (unless it is transparent); the position then moves on by the font
// increment (y in the high half, x in the low).
void Ffb::font_row(u32 bits) {
    ++debug_ops_[{regs_[kFbcCtl / 4], regs_[kPpc / 4] | 0xF0000000}];
    const u32 xy = regs_[kFontXy / 4], inc = regs_[kFontInc / 4];
    const int x = static_cast<s16>(xy & 0xFFFF), y = static_cast<s16>(xy >> 16);
    const unsigned w = std::min<u32>(regs_[kFontW / 4] & 0x3F, 32);
    const u32 fg = regs_[kFg / 4];
    for (unsigned i = 0; i < w; ++i) {
        const int px = x + static_cast<int>(i);
        if (px < 0 || y < 0 || px >= static_cast<int>(kLine) || y >= static_cast<int>(kLines)) continue;
        draw_pixel(static_cast<unsigned>(px), static_cast<unsigned>(y), fg, !((bits >> (31 - i)) & 1));
    }
    if (y >= 0 && y < static_cast<int>(kLines)) changed(static_cast<unsigned>(y), static_cast<unsigned>(y) + 1);
    const int ny = y + static_cast<s16>(inc >> 16), nx = x + static_cast<s16>(inc & 0xFFFF);
    regs_[kFontXy / 4] = (static_cast<u32>(ny) & 0xFFFF) << 16 | (static_cast<u32>(nx) & 0xFFFF);
}

}  // namespace uv
