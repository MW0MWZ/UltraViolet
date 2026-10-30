// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
#include "devices/mach64.h"

#include <algorithm>

namespace uv {

namespace {

constexpr u32 kHalf = 8u << 20;

// Block 0 registers (dword index).
constexpr unsigned kCrtcHTotalDisp = 0x00, kCrtcVTotalDisp = 0x02, kCrtcVline = 0x04, kCrtcOffPitch = 0x05,
                   kCrtcIntCntl = 0x06, kCrtcGenCntl = 0x07, kCurClr0 = 0x18, kCurClr1 = 0x19, kCurOffset = 0x1A,
                   kCurPosn = 0x1B, kCurOff = 0x1C, kGpIo = 0x1E, kClockCntl = 0x24, kBusCntl = 0x28,
                   kMemCntl = 0x2C, kDacRegs = 0x30, kDacCntl = 0x31, kGenTestCntl = 0x34, kConfigCntl = 0x37,
                   kConfigChipId = 0x38, kConfigStat0 = 0x39, kMppConfig = 0x3B, kFifoStat = 0xC4, kGuiStat = 0xCE;
// Block 1 (0x100 + dword index).
constexpr unsigned kGuiCntl = 0x100 + 0x5E;

// CRTC_GEN_CNTL.
constexpr u32 kGenDblScan = 1u << 0, kGenDisplayDis = 1u << 6, kGenExtDisp = 1u << 24, kGenCrtcEn = 1u << 25,
              kGenDispReqDis = 1u << 26;
// CRTC_INT_CNTL: enables (stored), interrupt status (written 1 to clear),
// live status.
constexpr u32 kIntVblank = 1u << 0, kIntVblankEn = 1u << 1, kIntVblankInt = 1u << 2, kIntVlineEn = 1u << 3,
              kIntVlineInt = 1u << 4, kIntOddLine = 1u << 5;
constexpr u32 kIntEnables = (1u << 1) | (1u << 3) | (1u << 7) | (1u << 9) | (1u << 16) | (1u << 18) | (1u << 20) |
                            (1u << 22) | (1u << 24) | (1u << 26);
constexpr u32 kIntStatus = (1u << 2) | (1u << 4) | (1u << 8) | (1u << 10) | (1u << 17) | (1u << 19) | (1u << 21) |
                           (1u << 23) | (1u << 25) | (1u << 27) | (1u << 31);
// BUS_CNTL.
constexpr u32 kBusApertureRegsDis = 1u << 4, kBusExtRegEn = 1u << 27, kBusErrorStatus = (1u << 21) | (1u << 23);
// CLOCK_CNTL.
constexpr u32 kPllWrEn = 1u << 9;
// DAC_CNTL.
constexpr u32 kDac8Bit = 1u << 8, kDacTypeShift = 16;
// GEN_TEST_CNTL.
constexpr u32 kGenCursorEn = 1u << 7, kGenGuiEn = 1u << 8;
// The 2D engine's registers start at dword 0x40 (0x100).
constexpr unsigned kEngineFirst = 0x40;

// The clock synthesiser's reference crystal (the value Linux assumes for
// these boards).
constexpr double kXtalHz = 14.31818e6;
// With no usable pixel clock: the frame rate the scan-out runs at.
constexpr double kFallbackHz = 66.0;

constexpr u32 lane_mask(unsigned lanes) {
    u32 m = 0;
    for (unsigned i = 0; i < 4; ++i)
        if (lanes & (1u << i)) m |= 0xFFu << (8 * i);
    return m;
}

}  // namespace

Mach64::Mach64(Scheduler& scheduler, double cpu_hz)
    : Function(0x1002, 0x4750, 0x5C, 0x030000), sched_(scheduler), cpu_hz_(cpu_hz),
      crtc_timer_(scheduler, [this] {
          latch_interrupts();
          update_irq();
          schedule_interrupt();
      }),
      vram_(kVramSize) {
    // As a real Ultra 10's: medium DEVSEL, fast back-to-back; interrupt pin
    // A, min-grant 8, max-latency 0.
    set_cfg16(pci::kStatus, 0x0280);
    set_cfg8(pci::kInterruptPin, 1);
    set_cfg8(pci::kMinGrant, 0x08);
    define_bar(0, 16u << 20, false);
    define_bar(1, 0x100, true);
    define_bar(2, 0x1000, false);
    save_reset_state();
    reset();
}

void Mach64::reset() {
    pci::Function::reset();
    crtc_timer_.cancel();
    latched_at_ = sched_.now();
    regs_ = {};
    regs_[kCrtcGenCntl] = kGenDispReqDis;  // display requests off until a mode is set
    regs_[kMemCntl] = 7;                   // 4 MiB
    regs_[kDacCntl] = 1u << kDacTypeShift;  // the internal DAC
    regs_[kConfigStat0] = 0x1D;            // SGRAM (the value OBP's FCode writes)
    regs_[kConfigChipId] = 0x7C004750;     // revision 0x7C, as a real Ultra 10's reads
    regs_[kGuiCntl] = 1;                   // a 128-entry command FIFO
    pll_ = {};
    static constexpr u8 kPllDefaults[] = {0, 0, 0x24, 0xCF, 0xF6, 0x04, 0x00, 0xFD, 0x8E, 0x9E, 0x65, 0x05, 0x40};
    std::copy(std::begin(kPllDefaults), std::end(kPllDefaults), pll_.begin());
    engine_reset();
    palette_ = {};
    dac_windex_ = dac_rindex_ = dac_wphase_ = dac_rphase_ = 0;
    dac_mask_ = 0xFF;
    update_irq();
}

// ---- registers ----

u32 Mach64::reg_read(unsigned index, unsigned lanes) {
    index &= 0x1FF;
    switch (index) {
        case kCrtcVline: return (regs_[index] & 0x7FF) | static_cast<u32>(current_line() & 0x7FF) << 16;
        case kCrtcIntCntl: {
            latch_interrupts();
            const Timing t = timing();
            const unsigned line = current_line();
            return (regs_[index] & ~(kIntVblank | kIntOddLine)) | (line >= t.visible ? kIntVblank : 0) |
                   ((line & 1) ? kIntOddLine : 0);
        }
        case kGpIo: {
            // Pins driven by the chip read what it drives; the others are
            // pulled up (nothing on the monitor's DDC lines answers).
            const u32 dir = regs_[index] >> 16;
            return (regs_[index] & 0xFFFF0000) | ((regs_[index] & dir) | (~dir & 0xFFFF));
        }
        case kClockCntl: return (regs_[index] & 0xFF00FFFF) | u32{pll_[(regs_[index] >> 10) & 0x3F]} << 16;
        case kDacRegs: {
            u32 v = 0;
            if (lanes & 1) v |= dac_windex_;
            if (lanes & 2) {
                v |= u32{palette_[dac_rindex_][dac_rphase_]} << 8;
                if (++dac_rphase_ == 3) dac_rphase_ = 0, ++dac_rindex_;
            }
            if (lanes & 4) v |= u32{dac_mask_} << 16;
            if (lanes & 8) v |= u32{dac_rindex_} << 24;
            return v;
        }
        case kConfigCntl: return (regs_[index] & ~0x3FF3u) | 2 | ((bar_address(0) >> 22) & 0x3FF) << 4;
        case kMppConfig: return regs_[index] & ~(1u << 30);  // never busy
        case kFifoStat: return 0;                              // (the engine is never behind)
        case kGuiStat: {
            // Free command FIFO entries, and busy while an operation waits
            // for host data.
            static constexpr u32 kFifoSize[4] = {192, 128, 64, 128};
            return kFifoSize[regs_[kGuiCntl] & 3] << 16 | (op_.active ? 1 : 0);
        }
        default: return regs_[index];
    }
}

void Mach64::reg_write(unsigned index, u32 value, unsigned lanes) {
    index &= 0x1FF;
    ++version_;
    const u32 mask = lane_mask(lanes), old = regs_[index], merged = (old & ~mask) | (value & mask);
    switch (index) {
        case kCrtcIntCntl:
            latch_interrupts();
            regs_[index] = (old & ~kIntEnables & ~(value & mask & kIntStatus)) | (merged & kIntEnables);
            update_irq();
            schedule_interrupt();
            break;
        case kClockCntl:
            regs_[index] = merged & 0xFF00FFFF;
            if ((lanes & 4) && (merged & kPllWrEn)) pll_[(merged >> 10) & 0x3F] = static_cast<u8>(merged >> 16);
            schedule_interrupt();
            break;
        case kBusCntl: regs_[index] = (merged & ~kBusErrorStatus) | (old & ~(value & mask) & kBusErrorStatus); break;
        case kDacRegs:
            // Bytes in address order: write index, data, mask, read index.
            if (lanes & 1) dac_windex_ = static_cast<u8>(value), dac_wphase_ = 0;
            if (lanes & 2) {
                const bool wide = regs_[kDacCntl] & kDac8Bit;
                dac_latch_[dac_wphase_] = static_cast<u8>((value >> 8) & (wide ? 0xFF : 0x3F));
                if (++dac_wphase_ == 3) palette_[dac_windex_++] = dac_latch_, dac_wphase_ = 0;
            }
            if (lanes & 4) dac_mask_ = static_cast<u8>(value >> 16);
            if (lanes & 8) dac_rindex_ = static_cast<u8>(value >> 24), dac_rphase_ = 0;
            break;
        case kDacCntl:
            regs_[index] = (merged & ~(0x7u << kDacTypeShift) & ~0x80u) | (old & (0x7u << kDacTypeShift));
            break;
        case kConfigChipId:
        case kFifoStat:
        case kGuiStat: break;  // read-only
        case kGenTestCntl:
            regs_[index] = merged;
            if ((old & kGenGuiEn) && !(merged & kGenGuiEn)) engine_reset();  // (on the falling edge)
            break;
        default:
            if (index >= kEngineFirst && index < 0x100) {  // the 2D engine's registers
                engine_write(index, merged);
                break;
            }
            regs_[index] = merged;
            if (index <= kCrtcGenCntl) schedule_interrupt();  // (the timing may have changed)
            break;
    }
}

// A block pair: block 1 at 0, block 0 at 0x400.
u64 Mach64::regs_read(u32 offset, unsigned size) {
    u64 v = 0;
    for (unsigned done = 0; done < size;) {
        const u32 at = offset + done, in = at & 3;
        const unsigned n = std::min(size - done, 4 - in);
        const unsigned index = (at & 0x400) ? ((at >> 2) & 0xFF) : 0x100 + ((at >> 2) & 0xFF);
        const unsigned lanes = ((1u << n) - 1) << in;
        const u32 r = reg_read(index, lanes);
        v |= static_cast<u64>((r >> (8 * in)) & (n == 4 ? 0xFFFFFFFFu : ((1u << (8 * n)) - 1))) << (8 * done);
        done += n;
    }
    return v;
}

void Mach64::regs_write(u32 offset, unsigned size, u64 value) {
    for (unsigned done = 0; done < size;) {
        const u32 at = offset + done, in = at & 3;
        const unsigned n = std::min(size - done, 4 - in);
        const unsigned index = (at & 0x400) ? ((at >> 2) & 0xFF) : 0x100 + ((at >> 2) & 0xFF);
        const unsigned lanes = ((1u << n) - 1) << in;
        reg_write(index, static_cast<u32>(value >> (8 * done)) << (8 * in), lanes);
        done += n;
    }
}

// Where an offset in the lower aperture falls in the register windows: the
// block pair offset + 1, or 0 for memory.
unsigned Mach64::register_window(u32 offset) const {
    const u32 bus = regs_[kBusCntl];
    if (bus & kBusApertureRegsDis) return 0;
    if (offset >= kHalf - 0x400) return offset - (kHalf - 0x400) + 0x400 + 1;
    if (offset >= kHalf - 0x800 && (bus & kBusExtRegEn)) return offset - (kHalf - 0x800) + 1;
    return 0;
}

// The frame buffer through an aperture: each byte lane swapped within 16
// or 32 bits as MEM_CNTL sets for that half. Memory past what is fitted
// reads as zero and keeps nothing.
u64 Mach64::aperture_read(u32 offset, unsigned size, bool upper) const {
    const u32 swap = (regs_[kMemCntl] >> (upper ? 26 : 24)) & 3, x = swap == 1 ? 1 : swap == 2 ? 3 : 0;
    u64 v = 0;
    for (unsigned i = 0; i < size; ++i) {
        const u32 a = (offset + i) ^ x;
        if (a < vram_.size()) v |= static_cast<u64>(vram_[a]) << (8 * i);
    }
    return v;
}

void Mach64::aperture_write(u32 offset, unsigned size, u64 value, bool upper) {
    ++version_;
    const u32 swap = (regs_[kMemCntl] >> (upper ? 26 : 24)) & 3, x = swap == 1 ? 1 : swap == 2 ? 3 : 0;
    for (unsigned i = 0; i < size; ++i) {
        const u32 a = (offset + i) ^ x;
        if (a < vram_.size()) vram_[a] = static_cast<u8>(value >> (8 * i));
    }
}

bool Mach64::bar_read(unsigned bar, u32 offset, unsigned size, u64& value) {
    switch (bar) {
        case 0: {
            const bool upper = offset >= kHalf;
            const u32 in = offset % kHalf;
            if (const unsigned w = upper ? 0 : register_window(in)) value = regs_read(w - 1, size);
            else value = aperture_read(in, size, upper);
            return true;
        }
        case 1: value = regs_read(0x400 + (offset & 0xFF), size); return true;  // block 0, 0x00-0xFF
        case 2: value = offset < 0x800 ? regs_read(offset, size) : 0; return true;
        default: return false;
    }
}

bool Mach64::bar_write(unsigned bar, u32 offset, unsigned size, u64 value) {
    switch (bar) {
        case 0: {
            const bool upper = offset >= kHalf;
            const u32 in = offset % kHalf;
            if (const unsigned w = upper ? 0 : register_window(in)) regs_write(w - 1, size, value);
            else aperture_write(in, size, value, upper);
            return true;
        }
        case 1: regs_write(0x400 + (offset & 0xFF), size, value); return true;
        case 2:
            if (offset < 0x800) regs_write(offset, size, value);
            return true;
        default: return false;
    }
}

// ---- display timing ----

// The pixel clock from the clock synthesiser (VCLK0-3 as CLOCK_CNTL
// selects), or 0 if it is not the source.
double Mach64::pixel_clock() const {
    if ((pll_[0x05] & 3) != 3) return 0;
    const unsigned n = regs_[kClockCntl] & 3, fb = pll_[0x07 + n], ref = pll_[0x02];
    static constexpr unsigned kPost[8] = {1, 2, 4, 8, 3, 5, 6, 12};
    const unsigned post = kPost[((pll_[0x06] >> (2 * n)) & 3) | (((pll_[0x0B] >> (4 + n)) & 1) << 2)];
    if (!fb || !ref) return 0;
    return 2.0 * kXtalHz * fb / ref / post;
}

Mach64::Timing Mach64::timing() const {
    const u32 h = regs_[kCrtcHTotalDisp], v = regs_[kCrtcVTotalDisp];
    const unsigned htotal = ((h & 0x1FF) + 1) * 8, vvisible = ((v >> 16) & 0x7FF) + 1;
    unsigned vtotal = (v & 0x7FF) + 1;
    if (vtotal <= vvisible) vtotal = vvisible + 1;
    double clock = pixel_clock();
    if (clock <= 0 || !(regs_[kCrtcGenCntl] & kGenCrtcEn)) clock = kFallbackHz * htotal * vtotal;
    return {std::max(cpu_hz_ * htotal / clock, 1.0), vtotal, vvisible};
}

unsigned Mach64::current_line() const {
    const Timing t = timing();
    return static_cast<unsigned>(t.line_at(sched_.now()) % t.total);
}

// The chosen line's interrupt comes as the scan finishes the line, at the
// start of the next: Solaris's driver asks for it at the last displayed
// line and loads the palette from it only if vertical blank has begun.
// Past the frame: never.
static unsigned vline_event(u32 vline, unsigned total) {
    const unsigned line = vline & 0x7FF;
    return line < total ? (line + 1) % total : total;
}

// Vertical blank and the chosen line latch their interrupt bits each time
// the scan passes them.
void Mach64::latch_interrupts() {
    const u64 now = sched_.now();
    const Timing t = timing();
    const u64 from = t.line_at(latched_at_), to = t.line_at(now);  // absolute lines
    latched_at_ = now;
    if (to <= from) return;
    auto passed = [&](unsigned line) {  // a line k * total + `line` in (from, to]
        const u64 first = (from / t.total) * t.total + line;
        return (first > from ? first : first + t.total) <= to;
    };
    if (passed(t.visible)) regs_[kCrtcIntCntl] |= kIntVblankInt;
    if (const unsigned v = vline_event(regs_[kCrtcVline], t.total); v < t.total && passed(v))
        regs_[kCrtcIntCntl] |= kIntVlineInt;
}

void Mach64::schedule_interrupt() {
    crtc_timer_.cancel();
    const u32 en = regs_[kCrtcIntCntl] & (kIntVblankEn | kIntVlineEn);
    if (!en) return;
    const Timing t = timing();
    const u64 now = sched_.now(), line = t.line_at(now), frame = line / t.total;
    u64 next = ~0ull;
    auto consider = [&](unsigned at) {
        u64 l = frame * t.total + at;
        if (l <= line) l += t.total;
        next = std::min(next, l);
    };
    if (en & kIntVblankEn) consider(t.visible);
    if (const unsigned v = vline_event(regs_[kCrtcVline], t.total); (en & kIntVlineEn) && v < t.total) consider(v);
    if (next != ~0ull) crtc_timer_.schedule_at(t.start_of(next));
}

void Mach64::update_irq() {
    const u32 i = regs_[kCrtcIntCntl];
    set_interrupt(((i & kIntVblankInt) && (i & kIntVblankEn)) || ((i & kIntVlineInt) && (i & kIntVlineEn)));
}

// ---- scan-out ----

bool Mach64::cursor_position(int& x, int& y) const {
    const u32 gen = regs_[kCrtcGenCntl];
    if (!(regs_[kGenTestCntl] & kGenCursorEn) || !(gen & kGenExtDisp) || !(gen & kGenCrtcEn)) return false;
    x = static_cast<int>(regs_[kCurPosn] & 0x7FF) - static_cast<int>(regs_[kCurOff] & 0x3F);
    y = static_cast<int>((regs_[kCurPosn] >> 16) & 0x7FF) / ((gen & kGenDblScan) ? 2 : 1) -
        static_cast<int>((regs_[kCurOff] >> 16) & 0x3F);
    return true;
}

bool Mach64::render(std::vector<u32>& pixels, unsigned& width, unsigned& height) const {
    const u32 gen = regs_[kCrtcGenCntl], h = regs_[kCrtcHTotalDisp], v = regs_[kCrtcVTotalDisp];
    if (!(gen & kGenExtDisp)) return false;  // VGA display: not used here
    const bool dbl = gen & kGenDblScan;
    width = (((h >> 16) & 0x1FF) + 1) * 8;
    height = (((v >> 16) & 0x7FF) + 1) / (dbl ? 2 : 1);
    pixels.assign(static_cast<std::size_t>(width) * height, 0);
    if (!(gen & kGenCrtcEn) || (gen & (kGenDisplayDis | kGenDispReqDis))) return true;  // blank

    const bool wide = regs_[kDacCntl] & kDac8Bit;
    auto lut = [&](unsigned i, unsigned c) -> u32 {
        const u8 e = palette_[i & 0xFF][c];
        return wide ? e : static_cast<u32>((e << 2) | (e >> 4));
    };
    auto rgb = [&](unsigned r, unsigned g, unsigned b) { return lut(r, 0) << 16 | lut(g, 1) << 8 | lut(b, 2); };
    auto byte = [&](u32 a) -> u32 { return a < vram_.size() ? vram_[a] : 0; };
    const u32 base = (regs_[kCrtcOffPitch] & 0xFFFFF) * 8, pitch = (regs_[kCrtcOffPitch] >> 22) * 8;
    const unsigned depth = (gen >> 8) & 7;
    static constexpr unsigned kBits[8] = {0, 4, 8, 16, 16, 24, 32, 0};
    if (!kBits[depth]) return true;
    for (unsigned y = 0; y < height; ++y) {
        const u32 row = base + y * pitch * kBits[depth] / 8;
        u32* out = &pixels[static_cast<std::size_t>(y) * width];
        for (unsigned x = 0; x < width; ++x) {
            switch (depth) {
                case 1: {  // 4 bpp: the first pixel in the high nibble (unless CRTC_BYTE_PIX_ORDER)
                    const u32 b = byte(row + x / 2);
                    const bool high = ((x & 1) == 0) != ((gen >> 11) & 1);
                    const unsigned i = (high ? b >> 4 : b) & 0xF & dac_mask_;
                    out[x] = rgb(i, i, i);
                    break;
                }
                case 2: {
                    const unsigned i = byte(row + x) & dac_mask_;
                    out[x] = rgb(i, i, i);
                    break;
                }
                case 3: {
                    const u32 p = byte(row + 2 * x) | byte(row + 2 * x + 1) << 8;
                    out[x] = rgb(((p >> 10) & 31) << 3, ((p >> 5) & 31) << 3, (p & 31) << 3);
                    break;
                }
                case 4: {
                    const u32 p = byte(row + 2 * x) | byte(row + 2 * x + 1) << 8;
                    out[x] = rgb(((p >> 11) & 31) << 3, ((p >> 5) & 63) << 2, (p & 31) << 3);
                    break;
                }
                case 5: out[x] = rgb(byte(row + 3 * x + 2), byte(row + 3 * x + 1), byte(row + 3 * x)); break;
                default: out[x] = rgb(byte(row + 4 * x + 2), byte(row + 4 * x + 1), byte(row + 4 * x)); break;
            }
        }
    }

    // The hardware cursor: 64 x 64, two bits a pixel (the first in the low
    // bits of each byte), 16 bytes a row: colour 0, colour 1, transparent,
    // or the screen inverted.
    if (regs_[kGenTestCntl] & kGenCursorEn) {
        const u32 posn = regs_[kCurPosn], off = regs_[kCurOff], bits = (regs_[kCurOffset] & 0xFFFFF) * 8;
        const int cx = posn & 0x7FF, cy = static_cast<int>((posn >> 16) & 0x7FF) / (dbl ? 2 : 1);
        const unsigned hoff = off & 0x3F, voff = (off >> 16) & 0x3F;
        const u32 colour[2] = {regs_[kCurClr0] >> 8, regs_[kCurClr1] >> 8};
        for (unsigned j = 0; j + voff < 64; ++j) {
            const int y = cy + static_cast<int>(j);
            if (y < 0 || y >= static_cast<int>(height)) continue;
            for (unsigned c = hoff; c < 64; ++c) {
                const int x = cx + static_cast<int>(c - hoff);
                if (x < 0 || x >= static_cast<int>(width)) continue;
                const unsigned code = (byte(bits + j * 16 + c / 4) >> ((c % 4) * 2)) & 3;
                u32& p = pixels[static_cast<std::size_t>(y) * width + static_cast<unsigned>(x)];
                if (code < 2) p = colour[code];
                else if (code == 3) p = ~p & 0xFFFFFF;
            }
        }
    }
    return true;
}

}  // namespace uv
