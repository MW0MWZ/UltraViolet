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
// (The Rage Pro's layout, as the Rage LT Pro reference has it: bit 21 is
// BUS_MSTR_RD_LINE, not the 264VT's error status; bit 23 LAT16X, read-only;
// bits 1 and 2 one-shots that read 0.)
constexpr u32 kBusApertureRegsDis = 1u << 4, kBusExtRegEn = 1u << 27, kBusReadOnly = 1u << 23,
              kBusOneShots = (1u << 1) | (1u << 2);
// GP_IO: what the pins the chip does not drive read; pin 7 reads low even
// when driven high (OpenBoot pulses it 1-0-1 and a real Ultra 10 then reads 0).
constexpr u32 kGpIoInputs = 0x3F5C, kGpIoHeldLow = 0x0080;
// CLOCK_CNTL.
constexpr u32 kPllWrEn = 1u << 9;
// DAC_CNTL.
// Read-only: the comparator output (7, high: nothing loads the outputs on
// a real Ultra 10), the DAC type (18:16) and the GIO pins' states (26:24, high).
constexpr u32 kDac8Bit = 1u << 8, kDacReadOnly = 0x07070080;
// GEN_TEST_CNTL.
// The EEPROM data input (3) reads high: no EEPROM on the Ultra 10's Rage.
constexpr u32 kGenEeDataIn = 1u << 3, kGenCursorEn = 1u << 7, kGenGuiEn = 1u << 8;
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
      vram_(kVramSize),
      written_(kVramSize >> kGranuleShift) {
    // As a real Ultra 10's at power-on (OpenBoot writes none of these):
    // the command's stepping bit (7) set and fixed; medium DEVSEL, fast
    // back-to-back; interrupt line 0x1B, pin A, min-grant 8, max-latency 0;
    // 0x0C at 0x40; and an AGP capability block (AGP 1.0, 1x, 256 requests)
    // at 0x50 that the status's capability list bit does not point to.
    set_cfg16(pci::kCommand, 0x0080);
    set_cfg16(pci::kStatus, 0x0280);
    set_cfg8(pci::kInterruptLine, 0x1B);
    set_cfg8(pci::kInterruptPin, 1);
    set_cfg8(pci::kMinGrant, 0x08);
    set_cfg32(0x40, 0x0000000C);
    set_cfg32(0x50, 0x00100002);
    set_cfg32(0x54, 0xFF000001);
    define_bar(0, 16u << 20, false);
    define_bar(1, 0x100, true);
    define_bar(2, 0x1000, false);
    define_rom(128u << 10);  // as a real Ultra 10 sizes it
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
    // The power-on values a real Ultra 10 reads: the internal DAC; SGRAM;
    // a FIFO error pending in BUS_CNTL.
    regs_[kDacCntl] = 0x87010182;
    regs_[kConfigStat0] = 0x0080001D;
    regs_[kBusCntl] = 0x7B33A040;
    regs_[kGenTestCntl] = 0x8;
    regs_[kConfigChipId] = 0x7C004750;     // revision 0x7C, as a real Ultra 10's reads
    regs_[kGuiCntl] = 1;                   // a 128-entry command FIFO
    pll_ = {};
    static constexpr u8 kPllDefaults[] = {0, 0, 0x24, 0xCF, 0xF6, 0x04, 0x00, 0xFD, 0x8E, 0x9E, 0x65, 0x05, 0x40};
    std::copy(std::begin(kPllDefaults), std::end(kPllDefaults), pll_.begin());
    pll_[0x12] = 0x80, pll_[0x14] = 0x10;  // as a real Ultra 10 reads them
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
            // Pins driven by the chip read what it drives, the others as
            // the Ultra 10's board leaves them (a real Ultra 10, nothing on the
            // DDC lines, 12 and 13).
            const u32 dir = regs_[index] >> 16;
            return (regs_[index] & 0xFFFF0000) | (((regs_[index] & dir) | (~dir & kGpIoInputs)) & ~kGpIoHeldLow);
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
        case kBusCntl: regs_[index] = (merged & ~kBusReadOnly & ~kBusOneShots) | (old & kBusReadOnly); break;
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
            regs_[index] = (merged & ~kDacReadOnly) | (old & kDacReadOnly);
            break;
        case kConfigChipId:
        case kFifoStat:
        case kGuiStat: break;  // read-only
        case kGenTestCntl:
            regs_[index] = (merged & ~kGenEeDataIn) | (old & kGenEeDataIn);
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
    written(offset & ~3u, (offset + size - 1) | 3u);  // (the swap stays within a word)
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

Mach64::Scanout Mach64::scanout() const {
    const u32 gen = regs_[kCrtcGenCntl], h = regs_[kCrtcHTotalDisp], v = regs_[kCrtcVTotalDisp];
    Scanout s;
    s.extended = gen & kGenExtDisp;
    if (!s.extended) return s;  // VGA display: not used here
    s.double_scan = gen & kGenDblScan;
    s.width = (((h >> 16) & 0x1FF) + 1) * 8;
    s.height = (((v >> 16) & 0x7FF) + 1) / (s.double_scan ? 2 : 1);
    static constexpr unsigned kBits[8] = {0, 4, 8, 16, 16, 24, 32, 0};
    s.depth = (gen >> 8) & 7;
    s.blank = !(gen & kGenCrtcEn) || (gen & (kGenDisplayDis | kGenDispReqDis)) || !kBits[s.depth];
    if (s.blank) return s;
    s.wide_dac = regs_[kDacCntl] & kDac8Bit;
    s.nibble_swap = (gen >> 11) & 1;
    s.base = (regs_[kCrtcOffPitch] & 0xFFFFF) * 8;
    s.pitch = (regs_[kCrtcOffPitch] >> 22) * 8 * kBits[s.depth] / 8;
    s.mask = dac_mask_;
    return s;
}

Mach64::Cursor Mach64::cursor() const {
    Cursor c;
    if (!(regs_[kGenTestCntl] & kGenCursorEn)) return c;
    const u32 posn = regs_[kCurPosn], off = regs_[kCurOff];
    c.shown = true;
    c.x = posn & 0x7FF;
    c.y = static_cast<int>((posn >> 16) & 0x7FF) / ((regs_[kCrtcGenCntl] & kGenDblScan) ? 2 : 1);
    c.hoff = off & 0x3F;
    c.voff = (off >> 16) & 0x3F;
    c.bits = (regs_[kCurOffset] & 0xFFFFF) * 8;
    c.colour[0] = regs_[kCurClr0] >> 8;
    c.colour[1] = regs_[kCurClr1] >> 8;
    return c;
}

bool Mach64::written_since(u32 from, u32 bytes, u64 version) const {
    if (!bytes || from >= kVramSize) return false;
    const u32 last = std::min<u32>(from + bytes - 1, kVramSize - 1) >> kGranuleShift;
    for (u32 g = from >> kGranuleShift; g <= last; ++g)
        if (written_[g] > version) return true;
    return false;
}

bool Mach64::render(Frame& f) const {
    FrameState& st = frame_state<FrameState>(f);
    f.changed_top = f.changed_bottom = 0;
    if (st.made && st.seen == version_) return st.shown;
    const Scanout s = scanout();
    const Cursor c = s.extended && !s.blank ? cursor() : Cursor{};
    const bool all = !st.made || !(s == st.scan) || st.palette != palette_;
    const u64 since = st.seen;
    st.made = true;
    st.shown = s.extended;
    st.seen = version_;
    if (!s.extended) {
        st.scan = s;
        return false;
    }
    const unsigned width = s.width, height = s.height;
    if (all) {
        f.width = width;
        f.height = height;
        st.base.assign(static_cast<std::size_t>(width) * height, 0);
        f.pixels.assign(st.base.size(), 0);
        st.palette = palette_;
    }

    // The rows to remake: those whose memory was written, and those the
    // cursor was on or is now on if it moved or changed.
    std::vector<u8> redo(height, all ? 1 : 0);
    static constexpr unsigned kBits[8] = {0, 4, 8, 16, 16, 24, 32, 0};
    const u32 row_bytes = s.blank ? 0 : (width * kBits[s.depth] + 7) / 8;
    if (!all && !s.blank)
        for (unsigned y = 0; y < height; ++y) redo[y] = written_since(s.base + y * s.pitch, row_bytes, since);
    std::vector<u8> recompose(redo);
    auto cursor_rows = [&](const Cursor& k, u8 mark) {
        if (!k.shown) return;
        for (unsigned j = 0; j + k.voff < 64; ++j) {
            const int y = k.y + static_cast<int>(j);
            if (y >= 0 && y < static_cast<int>(height)) recompose[static_cast<unsigned>(y)] |= mark;
        }
    };
    if (all || !(c == st.cursor) || (c.shown && written_since(c.bits, 64 * 16, since))) {
        cursor_rows(st.cursor, 2);
        cursor_rows(c, 2);
    }
    st.cursor = c;
    st.scan = s;

    // Palette lookups: the DAC's 6-bit entries widened unless it is in
    // 8-bit mode; and for indexed pixels, the colour an index shows
    // through the pixel mask.
    std::array<u32, 256> lut[3], index_lut;
    bool lut_made = false;
    auto make_lut = [&] {
        for (unsigned i = 0; i < 256; ++i)
            for (unsigned k = 0; k < 3; ++k) {
                const u8 e = palette_[i][k];
                lut[k][i] = (s.wide_dac ? e : static_cast<u32>((e << 2) | (e >> 4))) << (16 - 8 * k);
            }
        for (unsigned i = 0; i < 256; ++i) index_lut[i] = lut[0][i & s.mask] | lut[1][i & s.mask] | lut[2][i & s.mask];
        lut_made = true;
    };
    auto byte = [&](u32 a) -> u32 { return a < vram_.size() ? vram_[a] : 0; };
    auto indexed = [&](unsigned i) { return index_lut[i & 0xFF]; };
    auto rgb = [&](unsigned r, unsigned g, unsigned b) { return lut[0][r] | lut[1][g] | lut[2][b]; };
    auto make_row = [&](unsigned y) {
        u32* out = &st.base[static_cast<std::size_t>(y) * width];
        if (s.blank) {
            std::fill(out, out + width, 0);
            return;
        }
        if (!lut_made) make_lut();
        const u32 row = s.base + y * s.pitch;
        const bool inside = row + row_bytes <= vram_.size();
        const u8* m = vram_.data() + (inside ? row : 0);
        if (s.depth == 2 && inside) {  // (the usual mode, on its own)
            for (unsigned x = 0; x < width; ++x) out[x] = index_lut[m[x]];
            return;
        }
        for (unsigned x = 0; x < width; ++x) {
            switch (s.depth) {
                case 1: {  // 4 bpp: the first pixel in the high nibble (unless CRTC_BYTE_PIX_ORDER)
                    const u32 b = byte(row + x / 2);
                    const bool high = ((x & 1) == 0) != s.nibble_swap;
                    out[x] = indexed((high ? b >> 4 : b) & 0xF);
                    break;
                }
                case 2: out[x] = indexed(inside ? m[x] : byte(row + x)); break;
                case 3: {
                    const u32 p = inside ? (m[2 * x] | m[2 * x + 1] << 8) : (byte(row + 2 * x) | byte(row + 2 * x + 1) << 8);
                    out[x] = rgb(((p >> 10) & 31) << 3, ((p >> 5) & 31) << 3, (p & 31) << 3);
                    break;
                }
                case 4: {
                    const u32 p = inside ? (m[2 * x] | m[2 * x + 1] << 8) : (byte(row + 2 * x) | byte(row + 2 * x + 1) << 8);
                    out[x] = rgb(((p >> 11) & 31) << 3, ((p >> 5) & 63) << 2, (p & 31) << 3);
                    break;
                }
                case 5:
                    out[x] = inside ? rgb(m[3 * x + 2], m[3 * x + 1], m[3 * x])
                                    : rgb(byte(row + 3 * x + 2), byte(row + 3 * x + 1), byte(row + 3 * x));
                    break;
                default:
                    out[x] = inside ? rgb(m[4 * x + 2], m[4 * x + 1], m[4 * x])
                                    : rgb(byte(row + 4 * x + 2), byte(row + 4 * x + 1), byte(row + 4 * x));
                    break;
            }
        }
    };

    // The hardware cursor over a row: 64 x 64, two bits a pixel (the first
    // in the low bits of each byte), 16 bytes a row: colour 0, colour 1,
    // transparent, or the screen inverted.
    auto draw_cursor = [&](unsigned y) {
        if (!c.shown || static_cast<int>(y) < c.y) return;
        const unsigned j = y - static_cast<unsigned>(c.y);
        if (j + c.voff >= 64) return;
        u32* out = &f.pixels[static_cast<std::size_t>(y) * width];
        for (unsigned col = c.hoff; col < 64; ++col) {
            const int x = c.x + static_cast<int>(col - c.hoff);
            if (x < 0 || x >= static_cast<int>(width)) continue;
            const unsigned code = (byte(c.bits + j * 16 + col / 4) >> ((col % 4) * 2)) & 3;
            u32& p = out[x];
            if (code < 2) p = c.colour[code];
            else if (code == 3) p = ~p & 0xFFFFFF;
        }
    };

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

}  // namespace uv
