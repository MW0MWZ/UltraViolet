// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
// The Rage Pro's 2D engine. An operation starts when its last register is
// written (a rectangle: DST_WIDTH, DST_HEIGHT_WIDTH, DST_X_WIDTH or
// DST_WIDTH_HEIGHT; a line: DST_BRES_LNTH) and is drawn at once, pixel by
// pixel in the order the hardware visits them (so overlapping copies come
// out as on the chip); one that takes host data draws as HOST_DATA brings
// it. For each pixel a mono bit (always 1, the 8x8 pattern, host data or a
// 1-bit blit source) chooses the foreground or background source and mix;
// the colour comes from the colour registers, host data, the blit source or
// a colour pattern; then the colour compare, the scissors and the write
// mask decide what reaches the destination.
#include <algorithm>

#include "core/endian.h"
#include "devices/mach64.h"

namespace uv {

namespace {

// Registers (dword index).
constexpr unsigned kDstOffPitch = 0x40, kDstX = 0x41, kDstY = 0x42, kDstYX = 0x43, kDstWidth = 0x44,
                   kDstHeight = 0x45, kDstHeightWidth = 0x46, kDstXWidth = 0x47, kDstBresLnth = 0x48,
                   kDstBresErr = 0x49, kDstBresInc = 0x4A, kDstBresDec = 0x4B, kDstCntl = 0x4C, kDstYX2 = 0x4D,
                   kLeadBresLnth = 0x51, kSrcOffPitch = 0x60, kSrcX = 0x61, kSrcY = 0x62, kSrcYX = 0x63,
                   kSrcWidth1 = 0x64, kSrcHeight1 = 0x65, kSrcHeight1Width1 = 0x66, kSrcXStart = 0x67,
                   kSrcYStart = 0x68, kSrcYXStart = 0x69, kSrcWidth2 = 0x6A, kSrcHeight2 = 0x6B,
                   kSrcHeight2Width2 = 0x6C, kSrcCntl = 0x6D, kHostData0 = 0x80, kHostData15 = 0x8F,
                   kHostCntl = 0x90, kPatReg0 = 0xA0, kPatCntl = 0xA2, kScLeft = 0xA8, kScRight = 0xA9,
                   kScLeftRight = 0xAA, kScTop = 0xAB, kScBottom = 0xAC, kScTopBottom = 0xAD, kDpBkgdClr = 0xB0,
                   kDpFrgdClr = 0xB1, kDpWriteMask = 0xB2, kDpChainMask = 0xB3, kDpPixWidth = 0xB4, kDpMix = 0xB5,
                   kDpSrc = 0xB6, kDpFrgdBkgdClr = 0xB8, kDstXY = 0xBA, kDstWidthHeight = 0xBB, kClrCmpClr = 0xC0,
                   kClrCmpMsk = 0xC1, kClrCmpCntl = 0xC2, kGuiTrajCntl = 0xCC;

// DST_CNTL.
constexpr u32 kDstXDir = 1u << 0, kDstYDir = 1u << 1, kDstYMajor = 1u << 2, kDstXTile = 1u << 3,
              kDstYTile = 1u << 4, kDstLastPel = 1u << 5, kDstBresSign = 1u << 11;
// SRC_CNTL.
constexpr u32 kSrcPattEn = 1u << 0, kSrcPattRotEn = 1u << 1, kSrcLinearEn = 1u << 2, kSrcByteAlign = 1u << 3,
              kSrcLineXDir = 1u << 4;
// HOST_CNTL.
constexpr u32 kHostByteAlign = 1u << 0, kHostBigEndian = 1u << 1;
// DP_PIX_WIDTH.
constexpr u32 kDpHostTriple = 1u << 13, kDpLsbFirst = 1u << 24;
// PAT_CNTL.
constexpr u32 kPatMono = 1u << 0, kPatClr4x2 = 1u << 1, kPatClr8x1 = 1u << 2;
// Sources (DP_SRC).
constexpr unsigned kSrcBkgdClr = 0, kSrcFrgdClr = 1, kSrcHost = 2, kSrcBlit = 3, kSrcPattern = 4;

// Bits a pixel for a DP_PIX_WIDTH code (packed 24 bpp: the engine draws its
// bytes as 8-bit pixels).
unsigned bpp_of(unsigned code) {
    switch (code) {
        case 0: return 1;
        case 1: return 4;
        case 3:
        case 4:
        case 15: return 16;
        case 6:
        case 11:
        case 12:
        case 14: return 32;
        default: return 8;
    }
}

constexpr int sext(u32 v, unsigned bits) {
    const u32 m = 1u << (bits - 1);
    v &= (1u << bits) - 1;
    return static_cast<int>(v ^ m) - static_cast<int>(m);
}

constexpr u32 width_mask(unsigned bpp) { return bpp >= 32 ? 0xFFFFFFFFu : (1u << bpp) - 1; }

// The mixes (DP_MIX codes): S the source colour, D the destination.
u32 mix(unsigned code, u32 s, u32 d, u32 chain, unsigned bpp) {
    switch (code) {
        case 0x00: return ~d;
        case 0x01: return 0;
        case 0x02: return ~0u;
        case 0x03: return d;
        case 0x04: return ~s;
        case 0x05: return d ^ s;
        case 0x06: return ~d ^ s;
        case 0x07: return s;
        case 0x08: return ~d | ~s;
        case 0x09: return d | ~s;
        case 0x0A: return ~d | s;
        case 0x0B: return d | s;
        case 0x0C: return d & s;
        case 0x0D: return ~d & s;
        case 0x0E: return d & ~s;
        case 0x0F: return ~d & ~s;
        case 0x17: {
            // The average, component by component: DP_CHAIN_MASK marks each
            // component's top bit.
            u32 r = 0;
            unsigned lo = 0;
            for (unsigned b = 0; b < bpp; ++b) {
                if (!((chain >> b) & 1) && b + 1 < bpp) continue;
                const u32 m = width_mask(b - lo + 1);
                r |= ((((d >> lo) & m) + ((s >> lo) & m)) >> 1) << lo;
                lo = b + 1;
            }
            return r;
        }
        default: return s;  // (reserved)
    }
}

}  // namespace

void Mach64::engine_reset() {
    op_ = {};
    host_.clear();
    host_word_ = 0, host_bit_ = 0;
}

void Mach64::engine_write(unsigned index, u32 v) {
    if (index >= kHostData0 && index <= kHostData15) {  // one register at sixteen addresses
        if (!op_.active) return;                        // (nothing waiting for it: lost)
        host_.push_back(v);
        engine_run();
        return;
    }
    // Anything else while an operation waits for host data ends it there.
    if (op_.active) engine_reset();
    u32* r = regs_.data();
    r[index] = v;
    switch (index) {
        case kDstYX:
        case kDstYX2: r[kDstY] = v & 0xFFFF, r[kDstX] = v >> 16; break;
        case kDstXY: r[kDstX] = v & 0xFFFF, r[kDstY] = v >> 16; break;
        case kDstWidth:
            if (!(v >> 31)) engine_start(false);  // (unless DST_WIDTH_FILL_DIS)
            break;
        case kDstHeightWidth:
            r[kDstHeight] = v & 0xFFFF, r[kDstWidth] = v >> 16;
            engine_start(false);
            break;
        case kDstXWidth:
            r[kDstX] = v & 0xFFFF, r[kDstWidth] = v >> 16;
            engine_start(false);
            break;
        case kDstWidthHeight:
            r[kDstWidth] = v & 0xFFFF, r[kDstHeight] = v >> 16;
            engine_start(false);
            break;
        case kDstBresLnth:
        case kLeadBresLnth:
            r[kDstBresLnth] = r[kLeadBresLnth] = v;
            if (!(v & 0x80008000)) engine_start(true);  // (not LINE_DIS, not a trapezoid)
            break;
        case kSrcYX: r[kSrcY] = v & 0xFFFF, r[kSrcX] = v >> 16; break;
        case kSrcHeight1Width1: r[kSrcHeight1] = v & 0xFFFF, r[kSrcWidth1] = v >> 16; break;
        case kSrcYXStart: r[kSrcYStart] = v & 0xFFFF, r[kSrcXStart] = v >> 16; break;
        case kSrcHeight2Width2: r[kSrcHeight2] = v & 0xFFFF, r[kSrcWidth2] = v >> 16; break;
        case kScLeftRight: r[kScLeft] = v & 0xFFFF, r[kScRight] = v >> 16; break;
        case kScTopBottom: r[kScTop] = v & 0xFFFF, r[kScBottom] = v >> 16; break;
        case kDpFrgdBkgdClr: r[kDpFrgdClr] = v & 0xFFFF, r[kDpBkgdClr] = v >> 16; break;
        case kGuiTrajCntl:
            // DST_CNTL, SRC_CNTL's low byte, PAT_CNTL and HOST_CNTL at once.
            r[kDstCntl] = (r[kDstCntl] & ~0xFFFFu) | (v & 0xFFFF);
            r[kSrcCntl] = (r[kSrcCntl] & ~0xFFu) | ((v >> 16) & 0xFF);
            r[kPatCntl] = (v >> 24) & 7;
            r[kHostCntl] = ((v >> 28) & 1) | ((v >> 29) & 1) << 1;
            break;
        default: break;
    }
}

void Mach64::engine_start(bool line) {
    const u32* r = regs_.data();
    op_ = {};
    host_.clear();
    host_word_ = 0, host_bit_ = 0;
    op_.line = line;
    op_.x = op_.x0 = sext(r[kDstX], 14);
    op_.y = sext(r[kDstY], 15);
    if (line) {
        op_.count = r[kDstBresLnth] & 0x7FFF;
        op_.last = r[kDstCntl] & kDstLastPel;
        op_.err = sext(r[kDstBresErr], 18), op_.inc = sext(r[kDstBresInc], 18), op_.dec = sext(r[kDstBresDec], 18);
        if (!op_.count) return;
    } else {
        op_.width = r[kDstWidth] & 0x3FFF;
        op_.height = r[kDstHeight] & 0x7FFF;
        if (!op_.width || !op_.height) return;  // (an invalid draw: nothing)
    }
    op_.sx = sext(r[kSrcX], 14);
    op_.sy = sext(r[kSrcY], 15);
    op_.sbit = static_cast<u64>(r[kSrcOffPitch] & 0xFFFFF) * 64;
    op_.active = true;
    engine_run();
}

// Draws until the operation is done or needs host data that has not come.
void Mach64::engine_run() {
    const u32 dc = regs_[kDstCntl];
    const int xdir = (dc & kDstXDir) ? 1 : -1, ydir = (dc & kDstYDir) ? 1 : -1;
    while (op_.active) {
        if (op_.line) {
            const bool draw = op_.count > 1 || op_.last;
            if (draw && !engine_pixel(op_.x, op_.y, false)) return;
            if (op_.count == 1) {
                engine_finish();
                return;
            }
            --op_.count;
            // The major axis steps every pixel, the minor one when the
            // error says.
            const bool ymajor = dc & kDstYMajor;
            if (ymajor) op_.y += ydir;
            else op_.x += xdir;
            if (op_.err < 0 || (op_.err == 0 && (dc & kDstBresSign))) {
                op_.err += op_.inc;
            } else {
                op_.err += op_.dec;
                if (ymajor) op_.x += xdir;
                else op_.y += ydir;
            }
            continue;
        }
        if (!engine_pixel(op_.x, op_.y, op_.col == 0)) return;
        if (++op_.col < op_.width) {
            op_.x += xdir;
            continue;
        }
        op_.col = 0, op_.x = op_.x0, op_.y += ydir;
        if (++op_.row == op_.height) engine_finish();
    }
}

void Mach64::engine_finish() {
    u32* r = regs_.data();
    const u32 dc = r[kDstCntl];
    if (op_.line) {
        // DST_X and DST_Y rest on the last pixel.
        r[kDstX] = static_cast<u32>(op_.x) & 0x3FFF;
        r[kDstY] = static_cast<u32>(op_.y) & 0x7FFF;
    } else {
        // Tiling: the next rectangle beside or below this one.
        if (dc & kDstXTile) {
            const int w = static_cast<int>(op_.width) * ((dc & kDstXDir) ? 1 : -1);
            r[kDstX] = static_cast<u32>(sext(r[kDstX], 14) + w) & 0x3FFF;
        }
        if (dc & kDstYTile) {
            const int h = static_cast<int>(op_.height) * ((dc & kDstYDir) ? 1 : -1);
            r[kDstY] = static_cast<u32>(sext(r[kDstY], 15) + h) & 0x7FFF;
        }
    }
    op_.active = false;
    host_.clear();  // (what is left of the last word is not used)
    host_word_ = 0, host_bit_ = 0;
}

// The next `bits` of host data (1, 4, 8, 16 or 32), in the order the
// hardware takes them: words in order; within a word, bytes in address
// order, and for 1 and 4 bits a pixel the leftmost pixel in the high bits
// of each byte unless DP_BYTE_PIX_ORDER; 16- and 32-bit pixels byte
// swapped with HOST_BIG_ENDIAN_EN.
bool Mach64::host_take(unsigned bits, u32& value) {
    const std::size_t have = (host_.size() - host_word_) * 32 - host_bit_;
    if (have < bits) return false;
    const u32 w = host_[host_word_];
    if (bits < 8) {
        const unsigned in = host_bit_ % 8;
        const unsigned shift = (regs_[kDpPixWidth] & kDpLsbFirst) ? in : 8 - bits - in;
        value = (w >> ((host_bit_ / 8) * 8 + shift)) & width_mask(bits);
    } else {
        value = (w >> host_bit_) & width_mask(bits);
        if (regs_[kHostCntl] & kHostBigEndian) {
            if (bits == 16) value = ((value & 0xFF) << 8) | (value >> 8);
            if (bits == 32) value = byteswap(value);
        }
    }
    host_bit_ += bits;
    if (host_bit_ >= 32) host_bit_ = 0, ++host_word_;
    return true;
}

u32 Mach64::vram_get(u64 bit, unsigned bpp, bool lsb_first) const {
    const u64 byte = bit / 8;
    if (byte >= vram_.size()) return 0;
    if (bpp < 8) {
        const unsigned in = bit % 8, shift = lsb_first ? in : 8 - bpp - in;
        return (vram_[byte] >> shift) & width_mask(bpp);
    }
    u32 v = 0;
    for (unsigned i = 0; i < bpp / 8 && byte + i < vram_.size(); ++i) v |= u32{vram_[byte + i]} << (8 * i);
    return v;
}

void Mach64::vram_put(u64 bit, unsigned bpp, bool lsb_first, u32 value) {
    ++version_;
    const u64 byte = bit / 8;
    if (byte >= vram_.size()) return;
    if (bpp < 8) {
        const unsigned in = bit % 8, shift = lsb_first ? in : 8 - bpp - in;
        const u8 m = static_cast<u8>(width_mask(bpp) << shift);
        vram_[byte] = static_cast<u8>((vram_[byte] & ~m) | ((value << shift) & m));
        return;
    }
    for (unsigned i = 0; i < bpp / 8 && byte + i < vram_.size(); ++i) vram_[byte + i] = static_cast<u8>(value >> (8 * i));
}

// The blit source's current pixel.
u32 Mach64::source_pixel() {
    const u32* r = regs_.data();
    const unsigned bpp = bpp_of((r[kDpPixWidth] >> 8) & 0xF);
    const bool lsb = r[kDpPixWidth] & kDpLsbFirst;
    if (r[kSrcCntl] & kSrcLinearEn) return vram_get(op_.sbit, bpp, lsb);
    if (op_.sx < 0 || op_.sy < 0) return 0;
    const u64 pitch = static_cast<u64>(r[kSrcOffPitch] >> 22) * 8;  // pixels
    const u64 base = static_cast<u64>(r[kSrcOffPitch] & 0xFFFFF) * 64;
    return vram_get(base + (static_cast<u64>(op_.sy) * pitch + static_cast<u64>(op_.sx)) * bpp, bpp, lsb);
}

// Moves the blit source on after a pixel (`row_end`: that pixel ended a
// destination row): along a linear source; for lines, along x only; for
// rectangles, across SRC_WIDTH1 pixels then down a row (a plain copy), or
// round the pattern's width and height (a general pattern, the first span
// and rows from SRC_X/SRC_Y and SRC_WIDTH1/HEIGHT1, the rest from the
// START registers and WIDTH2/HEIGHT2 when rotating).
void Mach64::source_next(bool row_end) {
    const u32* r = regs_.data();
    const u32 sc = r[kSrcCntl], dc = r[kDstCntl];
    const unsigned bpp = bpp_of((r[kDpPixWidth] >> 8) & 0xF);
    if (sc & kSrcLinearEn) {
        op_.sbit += bpp;
        if (row_end && (sc & kSrcByteAlign)) op_.sbit = (op_.sbit + 7) & ~u64{7};
        return;
    }
    const int xdir = (dc & kDstXDir) ? 1 : -1, ydir = (dc & kDstYDir) ? 1 : -1;
    if (op_.line) {
        op_.sx += (sc & kSrcLineXDir) ? 1 : -1;
        return;
    }
    const bool rotate = (sc & kSrcPattEn) && (sc & kSrcPattRotEn);
    unsigned w = op_.second_span ? (r[kSrcWidth2] & 0x3FFF) : (r[kSrcWidth1] & 0x3FFF);
    if (!w) w = op_.width;
    ++op_.scol;
    op_.sx += xdir;
    if (!(sc & kSrcPattEn)) {
        // A plain copy: rows of SRC_WIDTH1, as many as it takes.
        if (op_.scol >= w || row_end) op_.scol = 0, op_.sx = sext(r[kSrcX], 14), op_.sy += ydir;
        return;
    }
    if (op_.scol >= w && !row_end) {  // round the pattern's width
        op_.scol = 0;
        op_.sx = rotate ? sext(r[kSrcXStart], 14) : sext(r[kSrcX], 14);
        op_.second_span = rotate;
    }
    if (row_end) {
        op_.scol = 0, op_.sx = sext(r[kSrcX], 14), op_.second_span = false;
        unsigned h = op_.second_rows ? (r[kSrcHeight2] & 0x7FFF) : (r[kSrcHeight1] & 0x7FFF);
        if (!h) h = op_.height;
        op_.sy += ydir;
        if (++op_.srow >= h) {
            op_.srow = 0;
            op_.sy = rotate ? sext(r[kSrcYStart], 15) : sext(r[kSrcY], 15);
            op_.second_rows = rotate;
        }
    }
}

// One pixel at (x, y); false (and nothing used) if it needs host data that
// has not come yet.
bool Mach64::engine_pixel(int x, int y, bool new_row) {
    const u32* r = regs_.data();
    const u32 pw = r[kDpPixWidth], src = r[kDpSrc];
    const unsigned dcode = pw & 0xF, dbpp = bpp_of(dcode), hbpp = bpp_of((pw >> 16) & 0xF);
    const bool packed24 = dcode == 5, lsb = pw & kDpLsbFirst;
    const unsigned mono_src = (src >> 16) & 3, fsrc = (src >> 8) & 7, bsrc = src & 7;
    const bool colour_host = fsrc == kSrcHost || bsrc == kSrcHost;
    const bool triple = (pw & kDpHostTriple) && mono_src == 2;

    // Host data: to the next byte at each row (HOST_BYTE_ALIGN, 1 and 4
    // bits a pixel), then what this pixel takes, all there or wait.
    if (new_row && (r[kHostCntl] & kHostByteAlign) && ((mono_src == 2 && !triple) || (colour_host && hbpp < 8)) &&
        host_bit_ % 8) {
        host_bit_ = (host_bit_ + 7) & ~7u;
        if (host_bit_ >= 32) host_bit_ = 0, ++host_word_;
    }
    const unsigned need = (mono_src == 2 && op_.triple == 0 ? 1 : 0) + (colour_host ? hbpp : 0);
    if ((host_.size() - host_word_) * 32 - host_bit_ < need) return false;

    // The mono bit.
    bool m = true;
    const unsigned px = packed24 ? static_cast<unsigned>(x) / 3 : static_cast<unsigned>(x);
    switch (mono_src) {
        case 1: {  // the 8x8 pattern, at the destination's position
            const unsigned row = static_cast<unsigned>(y) & 7, col = px & 7;
            const u32 bits = r[kPatReg0 + row / 4];
            m = (r[kPatCntl] & kPatMono) ? (bits >> ((row % 4) * 8 + (lsb ? col : 7 - col))) & 1 : true;
            break;
        }
        case 2:
            if (op_.triple == 0) host_take(1, op_.triple_bit);
            m = op_.triple_bit & 1;
            if (triple) op_.triple = (op_.triple + 1) % 3;
            break;
        case 3: m = source_pixel() & 1; break;
        default: break;
    }
    // The colour sources: host data and the blit source move on for every
    // pixel, whichever is chosen.
    u32 host_colour = 0, blit_colour = 0;
    if (colour_host) host_take(hbpp, host_colour);
    const bool blit = fsrc == kSrcBlit || bsrc == kSrcBlit || mono_src == 3;
    if (blit) blit_colour = source_pixel();
    auto component = [&](u32 v) { return packed24 ? (v >> (8 * (static_cast<unsigned>(x) % 3))) & 0xFF : v; };
    auto colour = [&](unsigned which) -> u32 {
        switch (which) {
            case kSrcBkgdClr: return component(r[kDpBkgdClr]);
            case kSrcFrgdClr: return component(r[kDpFrgdClr]);
            case kSrcHost: return host_colour;
            case kSrcBlit: return blit_colour;
            case kSrcPattern:
                if (r[kPatCntl] & kPatClr4x2)
                    return (r[kPatReg0 + (static_cast<unsigned>(y) & 1)] >> (8 * (px & 3))) & 0xFF;
                if (r[kPatCntl] & kPatClr8x1) return (r[kPatReg0 + ((px >> 2) & 1)] >> (8 * (px & 3))) & 0xFF;
                return 0;
            default: return 0;
        }
    };
    const u32 s = colour(m ? fsrc : bsrc) & width_mask(dbpp);
    if (blit) source_next(!op_.line && op_.col + 1 == op_.width);

    // Where it goes.
    const int left = sext(r[kScLeft], 14), right = sext(r[kScRight], 14);
    const int top = sext(r[kScTop], 15), bottom = sext(r[kScBottom], 15);
    if (x < left || x > right || y < top || y > bottom || x < 0 || y < 0) return true;
    const u64 pitch = static_cast<u64>(r[kDstOffPitch] >> 22) * 8;
    const u64 at = static_cast<u64>(r[kDstOffPitch] & 0xFFFFF) * 64 +
                   (static_cast<u64>(y) * pitch + static_cast<u64>(x)) * dbpp;
    const u32 d = vram_get(at, dbpp, lsb);
    const unsigned code = m ? (r[kDpMix] >> 16) & 0x1F : r[kDpMix] & 0x1F;
    const u32 result = mix(code, s, d, r[kDpChainMask], dbpp);
    // The colour compare: true leaves the destination as it is.
    const u32 cmp = r[kClrCmpCntl], mask = r[kClrCmpMsk];
    const u32 against = (((cmp >> 24) & 3) == 1 ? s : d) & mask;
    switch (cmp & 7) {
        case 1: return true;
        case 4:
            if (against != (r[kClrCmpClr] & mask)) return true;
            break;
        case 5:
            if (against == (r[kClrCmpClr] & mask)) return true;
            break;
        default: break;
    }
    const u32 wm = component(r[kDpWriteMask]) & width_mask(dbpp);
    vram_put(at, dbpp, lsb, (d & ~wm) | (result & wm));
    return true;
}

}  // namespace uv
