// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
// The Creator3D's raster processor and pixel processor: the operations
// software starts by writing their registers, and what each pixel they
// (and the smart frame buffer views) write goes through on its way to the
// frame buffer: the colour and X sources, the area pattern, the view clip,
// the raster operation, the plane masks and the plane groups enabled, into
// the buffers the frame buffer control selects.
#include "core/log.h"
#include "devices/ffb.h"

namespace uv {

namespace {

// Register offsets (words of the FBC).
constexpr u32 kBy = 0x60, kBx = 0x64, kDy = 0x68, kDx = 0x6C, kBh = 0x70, kBw = 0x74;
constexpr u32 kPpc = 0x200, kWid = 0x204, kFg = 0x208, kBg = 0x20C, kVclipMin = 0x220, kVclipMax = 0x224,
              kFbcCtl = 0x254, kRop = 0x258, kPmask = 0x290, kXpmask = 0x294, kDrawop = 0x300, kFontXy = 0x314,
              kFontW = 0x318, kFontInc = 0x31C, kPattern = 0x380;

constexpr u32 kLpat = 0x30C;
constexpr unsigned kDrawDot = 0x00, kDrawLineCap = 0x02, kDrawLineOpen = 0x03, kDrawRectangle = 0x08, kDrawFastFill = 0x09,
                   kDrawBcopy = 0x0A, kDrawVscroll = 0x0B;
// Fast fill: by holds the step (1 page fill, 2 block fill; 3 and 4 set up
// the colour block and the page's block); the block size, which the frame
// buffer's organisation sets, for the standard resolution (as the X driver
// takes it).
constexpr u32 kFastFillPage = 1, kFastFillBlock = 2;
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

}  // namespace

void Ffb::engine_unmodelled(unsigned what, const char* text, u32 detail) {
    if (engine_warned_ & (1u << what)) return;
    engine_warned_ |= 1u << what;
    UV_LOG(Warn, "ffb", "not modelled: %s (%#x)", text, detail);
}

u32 Ffb::read_pixel(unsigned x, unsigned y) const {
    if (x >= kLine || y >= kLines) return 0;
    return buf_[read_buffer()][static_cast<std::size_t>(y) * kLine + x];
}

bool Ffb::clipped(int x, int y) const {
    if (x < 0 || y < 0 || x >= static_cast<int>(kLine) || y >= static_cast<int>(kLines)) return true;
    const u32 vce = regs_[kPpc / 4] & 0x3000;
    if (vce == 0x2000 || vce == 0x3000) {
        const u32 lo = regs_[kVclipMin / 4], hi = regs_[kVclipMax / 4];
        if (x < static_cast<int>(lo & 0xFFFF) || y < static_cast<int>(lo >> 16) || x > static_cast<int>(hi & 0xFFFF) ||
            y > static_cast<int>(hi >> 16))
            return true;
    }
    return false;
}

// A pixel into the buffers written: the raster operation on the colour
// planes and on the X plane, then the plane masks and the plane groups
// enabled (a group is written unless its enable is off), and `lanes` (the
// bytes a smart view's write gives).
void Ffb::smart_write(unsigned x, unsigned y, u32 value, u32 lanes) {
    const u32 fbc = regs_[kFbcCtl / 4], r = regs_[kRop / 4];
    auto on = [&](unsigned shift) { return ((fbc >> shift) & 3) != 1; };
    u32 mask = (on(0) ? 0xFFu : 0) | (on(2) ? 0xFF00u : 0) | (on(4) ? 0xFF0000u : 0);
    mask &= regs_[kPmask / 4] & 0xFFFFFF;
    if (on(6)) mask |= (regs_[kXpmask / 4] & 0xFF) << 24;
    mask &= lanes;
    const unsigned wb = write_buffers();
    const std::size_t i = static_cast<std::size_t>(y) * kLine + x;
    for (unsigned b = 0; b < 3; ++b) {
        if (!(wb & (1u << b))) continue;
        const u32 old = buf_[b][i];
        const u32 n = (rop(r & 0xF, value, old) & 0xFFFFFF) | (rop((r >> 8) & 0xF, value, old) & 0xFF000000);
        buf_[b][i] = (old & ~mask) | (n & mask);
    }
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

void Ffb::rectangle(int x, int y, int w, int h) {
    const u32 fg = regs_[kFg / 4];
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
        case kDrawRectangle: rectangle(reg(kBx), reg(kBy), reg(kBw), reg(kBh)); break;
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

// A fast fill, a step at a time as by says: the colour block and the
// page's block set up (nothing drawn), then a block fill or a page fill
// from (dx, dy), its width (bx or bw) and height (bh) counted from the
// block (or page) the start is in. The 3D-RAM fills whole blocks at once;
// here it is drawn as a rectangle in the foreground.
void Ffb::fast_fill(u32 written) {
    const u32 step = regs_[kBy / 4];
    if (step != kFastFillBlock && step != kFastFillPage) return;  // (a set-up step)
    const int x = static_cast<s32>(regs_[kDx / 4]), y = static_cast<s32>(regs_[kDy / 4]);
    const int w = static_cast<s32>(regs_[written / 4]), h = static_cast<s32>(regs_[kBh / 4]);
    const int x0 = step == kFastFillBlock ? x & ~(kFastFillBlockW - 1) : x, y0 = step == kFastFillBlock ? y & ~(kFastFillBlockH - 1) : y;
    rectangle(x, y, x0 + w - x, y0 + h - y);
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
