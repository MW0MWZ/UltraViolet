// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
// ATI mach64 3D Rage Pro (215GP, pci1002,4750), the Ultra 5/10's on-board
// graphics (Sun PGX24, OBP SUNW,m64B), with 4 MiB of SGRAM.
//
// BAR0 is a 16 MiB aperture onto the frame buffer: the lower 8 MiB with the
// register blocks at its top (block 1 at 0x7FF800, block 0 at 0x7FFC00,
// as BUS_CNTL enables them), the upper 8 MiB the same memory again; each
// half can swap the bytes within 16- or 32-bit units (MEM_CNTL), for
// big-endian hosts. BAR1 is block 0's display and configuration registers
// in I/O space, BAR2 a 4 KiB window with block 1 then block 0.
//
// Block 0 holds the CRTC (timing, start, pitch, depth), the palette DAC,
// the clock synthesiser's registers (indexed through CLOCK_CNTL), memory
// and bus configuration, general-purpose pins (GP_IO: the monitor's DDC),
// the hardware cursor and the 2D engine; block 1 the multimedia and 3D
// registers. The scan-out position and vertical blank follow guest time
// at the rate the clock synthesiser is set to.
#pragma once

#include <array>
#include <cmath>
#include <vector>

#include "core/scheduler.h"
#include "devices/pci.h"

namespace uv {

class Mach64 final : public pci::Function {
public:
    static constexpr u32 kVramSize = 4u << 20;  // 4 MiB, as a real Ultra 10's

    Mach64(Scheduler& scheduler, double cpu_hz);
    const char* name() const override { return "mach64"; }
    void reset() override;

    const std::vector<u8>& vram() const { return vram_; }

    // The picture as the monitor would show it: `width` x `height` pixels of
    // 0x00RRGGBB (blank while the CRTC or display is off). False if no mode
    // is set (nothing to show).
    bool render(std::vector<u32>& pixels, unsigned& width, unsigned& height) const;
    // Where the hardware cursor's 64 x 64 image starts on the picture, if
    // it is shown.
    bool cursor_position(int& x, int& y) const;
    // Changes with anything that may change the picture (memory, registers,
    // the palette), so a display need not be remade while it stays put.
    u64 display_version() const { return version_; }

protected:
    bool bar_read(unsigned bar, u32 offset, unsigned size, u64& value) override;
    bool bar_write(unsigned bar, u32 offset, unsigned size, u64 value) override;

private:
    // Register access: `index` a dword of block 0 (0-0xFF) or block 1
    // (0x100-0x1FF), `lanes` the bytes accessed (bit n: byte n).
    u32 reg_read(unsigned index, unsigned lanes);
    void reg_write(unsigned index, u32 value, unsigned lanes);
    // An access of `size` bytes at a byte offset into a block pair (block 1
    // at 0, block 0 at 0x400).
    u64 regs_read(u32 offset, unsigned size);
    void regs_write(u32 offset, unsigned size, u64 value);
    // The frame buffer through an aperture half (`upper`).
    u64 aperture_read(u32 offset, unsigned size, bool upper) const;
    void aperture_write(u32 offset, unsigned size, u64 value, bool upper);
    unsigned register_window(u32 offset) const;  // 0: none, else a block pair offset + 1

    // Display timing: cycles per scan line, lines per frame, and the scan
    // line at `now`.
    struct Timing {
        double line_cycles;  // (exact: the scan does not drift from guest time)
        unsigned total, visible;
        u64 line_at(u64 cycle) const { return static_cast<u64>(static_cast<double>(cycle) / line_cycles); }
        u64 start_of(u64 line) const { return static_cast<u64>(std::ceil(static_cast<double>(line) * line_cycles)); }
    };
    Timing timing() const;
    unsigned current_line() const;
    void latch_interrupts();
    void schedule_interrupt();
    void update_irq();
    double pixel_clock() const;

    // The 2D engine (mach64_engine.cpp): register writes (dword `index`
    // 0x40-0xFF), starting an operation, drawing until it is done or the
    // host data runs out, and the engine reset.
    void engine_write(unsigned index, u32 value);
    void engine_start(bool line);
    void engine_run();
    void engine_finish();
    void engine_reset();
    bool engine_pixel(int x, int y, bool new_row);
    bool host_take(unsigned bits, u32& value);
    u32 source_pixel();
    void source_next(bool new_row);
    u32 vram_get(u64 bit, unsigned bpp, bool lsb_first) const;
    void vram_put(u64 bit, unsigned bpp, bool lsb_first, u32 value);

    // An operation under way: a rectangle (`col` of `width` in row `row`
    // of `height`) or a line (`count` pixels left), where the next pixel
    // goes, and where the blit source is.
    struct EngineOp {
        bool active = false, line = false;
        int x = 0, y = 0, x0 = 0;
        unsigned width = 0, height = 0, col = 0, row = 0;
        // Lines: Bresenham error and steps, pixels left, whether the last
        // is drawn.
        s32 err = 0, inc = 0, dec = 0;
        unsigned count = 0;
        bool last = false;
        // Blit source: position (x, y) and how far along its span and rows
        // it is, or a bit address for a linear source.
        int sx = 0, sy = 0;
        unsigned scol = 0, srow = 0;
        bool second_span = false, second_rows = false;
        u64 sbit = 0;
        unsigned triple = 0;  // (host mono bits used three times: which use)
        u32 triple_bit = 0;
    };
    EngineOp op_;
    std::vector<u32> host_;  // host data for the operation: used up to word host_word_, bit host_bit_
    std::size_t host_word_ = 0;
    unsigned host_bit_ = 0;

    Scheduler& sched_;
    double cpu_hz_;
    Timer crtc_timer_;
    u64 latched_at_ = 0;  // guest time interrupts were last latched to

    std::vector<u8> vram_;
    u64 version_ = 0;
    std::array<u32, 512> regs_{};
    std::array<u8, 64> pll_{};
    // Palette DAC: 256 entries of R, G, B; write and read index, which of
    // R/G/B is next.
    std::array<std::array<u8, 3>, 256> palette_{};
    u8 dac_windex_ = 0, dac_rindex_ = 0, dac_wphase_ = 0, dac_rphase_ = 0, dac_mask_ = 0xFF;
    std::array<u8, 3> dac_latch_{};
};

}  // namespace uv
