// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
// The Creator3D's raster processor and pixel processor: the operations
// software starts by writing their registers, and what each pixel they
// (and the smart frame buffer views) write goes through on its way to the
// frame buffer: the colour and X sources, the area pattern, the view clip,
// the raster operation, the plane masks and the plane groups enabled, into
// the buffers the frame buffer control selects.
#include <algorithm>
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
constexpr unsigned kDrawDot = 0x00, kDrawLineCap = 0x02, kDrawLineOpen = 0x03, kDrawRectangle = 0x08, kDrawFastFill = 0x09,
                   kDrawBcopy = 0x0A, kDrawVscroll = 0x0B;
// Fast fill: by holds the step (1 page fill, 2 block fill, 3 the colour
// block set up; 4 the page's block); the block size, which the frame
// buffer's organisation sets, for the standard resolution (as the X driver
// takes it).
constexpr u32 kFastFillPage = 1, kFastFillBlock = 2, kFastFillColourBlock = 3;
constexpr int kFastFillBlockW = 8, kFastFillBlockH = 4;

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
// 1 - alpha, alpha) of each colour, the alpha the source's X byte, 0xFF
// taken as 1.0 when forced.
constexpr u32 kPpcBlend = 0xC000, kBlendForceOne = 0x10;

u32 blend(u32 blendc, u32 src, u32 dst) {
    const unsigned a = src >> 24;
    const double alpha = (blendc & kBlendForceOne) && a == 0xFF ? 1.0 : a / 256.0;
    auto fraction = [&](unsigned f) { return f == 0 ? 0.0 : f == 1 ? 1.0 : f == 2 ? 1.0 - alpha : alpha; };
    const double sf = fraction(blendc & 3), df = fraction((blendc >> 2) & 3);
    u32 out = 0;
    for (unsigned shift = 0; shift < 24; shift += 8) {
        const double c = ((src >> shift) & 0xFF) * sf + ((dst >> shift) & 0xFF) * df;
        out |= static_cast<u32>(std::min(255.0, std::floor(c + 0.5))) << shift;
    }
    return out | (src & 0xFF000000);
}

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

// A pixel into the buffers written: the raster operation on the colour
// planes and on the X plane, then the plane groups the frame buffer control
// enables and `lanes` (the bytes a smart view's write gives). Each group's
// enable is 01 off, 10 on (all its planes), 11 on through its plane mask
// (pmask for red, green and blue, xpmask for the X plane), as Xsun uses
// them: its 24-bit images go through the 32-bit view with the colour
// groups on and a plane mask of 0, its 8-bit drawing through red's mask.
// With the window enable "use window mask", the X plane is written through
// the window ID plane mask (widpmask) too: Xsun paints a window's ID into
// its low six planes and the overlay selection into the top two that way.
void Ffb::smart_write(unsigned x, unsigned y, u32 value, u32 lanes) {
    const u32 fbc = regs_[kFbcCtl / 4], r = regs_[kRop / 4];
    const bool window_mask = ((fbc >> 20) & 3) == 3;
    auto group = [&](unsigned shift, u32 planes, u32 pmask) {
        const unsigned e = (fbc >> shift) & 3;
        return e == 1 ? 0u : e == 2 ? planes : planes & pmask;  // (0, no change written, as 11)
    };
    const u32 pmask = regs_[kPmask / 4];
    u32 xmask = group(6, 0xFF, regs_[kXpmask / 4]);
    if (window_mask) xmask &= regs_[kWidpmask / 4];
    u32 mask = group(0, 0xFFu, pmask) | group(2, 0xFF00u, pmask) | group(4, 0xFF0000u, pmask) | (xmask & 0xFF) << 24;
    mask &= lanes;
    const unsigned wb = write_buffers();
    const std::size_t i = static_cast<std::size_t>(y) * kLine + x;
    for (unsigned b = 0; b < 3; ++b) {
        if (!(wb & (1u << b))) continue;
        const u32 old = buf_[b][i];
        const u32 src = (regs_[kPpc / 4] & kPpcBlend) == kPpcBlend ? blend(regs_[kBlendc / 4], value, old) : value;
        const u32 n = (rop(r & 0xF, src, old) & 0xFFFFFF) | (rop((r >> 8) & 0xF, src, old) & 0xFF000000);
        buf_[b][i] = (old & ~mask) | (n & mask);
    }
}

// A pixel written through the smart 32-bit view: its colour the
// foreground when the pixel processor's colour source is constant, its X
// byte the current window ID or the foreground's when the X source says so
// (NetBSD draws its antialiased console font this way, writing only each
// pixel's alpha). (Through the 8-bit views the colour is the value written
// whatever the colour source, as NetBSD's driver notes.)
void Ffb::smart_view_write(unsigned x, unsigned y, u32 value, u32 lanes) {
    const u32 ppc = regs_[kPpc / 4], fg = regs_[kFg / 4];
    if ((ppc & 3) == 3) value = (value & 0xFF000000) | (fg & 0xFFFFFF), lanes |= 0xFFFFFF;
    if ((ppc & 0xC) == 0x4) value = (value & 0xFFFFFF) | (regs_[kWid / 4] & 0xFF) << 24;
    else if ((ppc & 0xC) == 0xC) value = (value & 0xFFFFFF) | (fg & 0xFF000000);
    smart_write(x, y, value, lanes);
}

// A pixel the raster processor draws: its colour (the foreground, or the
// background for the pattern's or font's clear bits), its X plane by the
// pixel processor control's X source, the area pattern and the view clip.
void Ffb::draw_pixel(unsigned x, unsigned y, u32 colour, bool background) {
    if (clipped(static_cast<int>(x), static_cast<int>(y))) return;
    const u32 ppc = regs_[kPpc / 4];
    if ((ppc & 0xC00) == 0xC00) {  // area pattern: 32 x 32, the leftmost pixel in bit 31
        const bool set = (regs_[kPattern / 4 + y % 32] >> (31 - x % 32)) & 1;
        if (!set) {
            if ((ppc & 0x300) == 0x300) return;  // transparent background
            background = true;
        }
    }
    if (background) {
        if ((ppc & 0x300) == 0x300) return;
        colour = regs_[kBg / 4];
    }
    // The X plane: the foreground's or background's (constant), or the
    // current window ID (WID source, or forced).
    if ((ppc & 0xC) == 0x4 || (ppc & 0xC00000) == 0xC00000) colour = (colour & 0xFFFFFF) | (regs_[kWid / 4] & 0xFF) << 24;
    smart_write(x, y, colour, 0xFFFFFFFF);
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
// set: 16 bits from bit 0, each repeated by the scale, of the pattern's
// length (0: 16), clear bits not drawn; where the pattern and scale stand
// is kept for the next line. The line's end is the next one's start.
void Ffb::line(int x0, int y0, int x1, int y1, bool last) {
    u32 lpat = regs_[kLpat / 4];
    const unsigned len = (lpat >> 16) & 0xF ? (lpat >> 16) & 0xF : 16, scale = (lpat >> 20) & 0xF ? (lpat >> 20) & 0xF : 1;
    unsigned ptr = (lpat >> 24) & 0xF, sptr = lpat >> 28;
    const u32 fg = regs_[kFg / 4];
    const int dx = std::abs(x1 - x0), dy = std::abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx - dy, x = x0, y = y0, top = y0, bottom = y0;
    for (;;) {
        const bool end = x == x1 && y == y1;
        if (end && !last) break;
        if (!lpat || ((lpat >> ptr) & 1)) {
            if (x >= 0 && y >= 0 && x < static_cast<int>(kLine) && y < static_cast<int>(kLines)) {
                draw_pixel(static_cast<unsigned>(x), static_cast<unsigned>(y), fg);
                top = std::min(top, y), bottom = std::max(bottom, y);
            }
        }
        if (lpat && ++sptr >= scale) sptr = 0, ptr = (ptr + 1) % len;
        if (end) break;
        const int e2 = 2 * err;
        if (e2 > -dy) err -= dy, x += sx;
        if (e2 < dx) err += dx, y += sy;
    }
    if (lpat) regs_[kLpat / 4] = (lpat & 0x00FFFFFF) | (ptr & 0xF) << 24 | (sptr & 0xF) << 28;
    regs_[kBy / 4] = static_cast<u32>(y1), regs_[kBx / 4] = static_cast<u32>(x1);
    top = std::max(top, 0), bottom = std::min(bottom, static_cast<int>(kLines) - 1);
    if (top <= bottom) changed(static_cast<unsigned>(top), static_cast<unsigned>(bottom) + 1);
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
    const int x0 = step == kFastFillBlock ? x & ~(kFastFillBlockW - 1) : x, y0 = step == kFastFillBlock ? y & ~(kFastFillBlockH - 1) : y;
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
