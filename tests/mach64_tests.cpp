// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
// The Rage Pro: configuration, the aperture and its register windows, byte
// swapping, the palette DAC, the clock synthesiser's registers, the CRTC's
// timing and scan-out, and the hardware cursor.
#include <vector>

#include "devices/mach64.h"
#include "test.h"

using namespace uv;

namespace {

constexpr u32 kBar0 = 0x10000000, kBar2 = 0x20000000, kRegs = kBar0 + 0x7FFC00;

struct Rig {
    u64 now = 0;
    Scheduler sched;
    Mach64 m{sched, 100e6};
    bool irq = false;
    Rig() {
        sched.set_clock([this] { return now; });
        m.set_interrupt_sink([this](bool a) { irq = a; });
        m.config_write(pci::kBar0, 4, kBar0);
        m.config_write(pci::kBar0 + 8, 4, kBar2);
        m.config_write(pci::kCommand, 2, pci::kCmdMemory);
    }
    u64 rd(u32 a, unsigned size = 4) {
        u64 v = 0;
        m.mem_read(a, size, v);
        return v;
    }
    void wr(u32 a, u64 v, unsigned size = 4) { m.mem_write(a, size, v); }
    void run(u64 cycles) {
        now += cycles;
        sched.run_due(now);
    }
    // 1152 x 900 at 8 bpp from offset 0, the way OpenBoot's FCode sets it.
    void mode_1152x900() {
        wr(kRegs + 0x00, 0x008F00BE);  // H: 1152 shown of 1528
        wr(kRegs + 0x08, 0x038303A8);  // V: 900 shown of 937
        wr(kRegs + 0x14, 144u << 22);  // pitch 1152, offset 0
        wr(kRegs + 0x1C, 0x03000200);  // CRTC and extended display on, 8 bpp
    }
};

}  // namespace

TEST(mach64_configuration_space_and_bars) {
    Rig r;
    CHECK_EQ(r.m.config_read(0x00, 4), 0x47501002u);
    CHECK_EQ(r.m.config_read(0x08, 4), 0x0300005Cu);
    CHECK_EQ(r.m.config_read(0x3D, 1), 1u);
    CHECK_EQ(r.m.config_read(0x3E, 1), 8u);
    r.m.config_write(pci::kBar0, 4, 0xFFFFFFFF);
    CHECK_EQ(r.m.config_read(pci::kBar0, 4), 0xFF000000u);
    r.m.config_write(pci::kBar0 + 4, 4, 0xFFFFFFFF);
    CHECK_EQ(r.m.config_read(pci::kBar0 + 4, 4), 0xFFFFFF01u);
    r.m.config_write(pci::kBar0 + 8, 4, 0xFFFFFFFF);
    CHECK_EQ(r.m.config_read(pci::kBar0 + 8, 4), 0xFFFFF000u);
}

// Identity, scratch registers, the aperture's place in CONFIG_CNTL, and the
// register windows: block 0 at the top of the lower half, block 1 below it
// only with BUS_EXT_REG_EN, none with BUS_APER_REG_DIS (memory shows
// through); none in the upper half; BAR2 always.
TEST(mach64_register_windows_and_identity) {
    Rig r;
    CHECK_EQ(r.rd(kRegs + 0xE0), 0x7C004750u);
    r.wr(kRegs + 0xE0, 0);
    CHECK_EQ(r.rd(kRegs + 0xE0), 0x7C004750u);
    r.wr(kRegs + 0x80, 0x55555555);
    CHECK_EQ(r.rd(kRegs + 0x80), 0x55555555u);
    r.wr(kRegs + 0x80, 0xAAAAAAAA);
    CHECK_EQ(r.rd(kBar2 + 0x480), 0xAAAAAAAAu);  // the same register through BAR2
    CHECK_EQ(r.rd(kRegs + 0xDC) & 0x3FF3, (kBar0 >> 22) << 4 | 2);
    CHECK_EQ(r.rd(kRegs + 0xB0) & 0xF, 7u);  // 4 MiB
    // Block 1 (GUI_CNTL at 0x178): through BAR2 always, through the
    // aperture only when enabled.
    r.wr(kBar2 + 0x178, 2);
    CHECK_EQ(r.rd(kBar0 + 0x7FF978), 0u);  // (frame buffer there: past the 4 MiB fitted)
    r.wr(kRegs + 0xA0, 1u << 27);
    CHECK_EQ(r.rd(kBar0 + 0x7FF978), 2u);
    CHECK_EQ(r.rd(kRegs + 0x338) >> 16, 64u);  // GUI_STAT: 64 free entries in that FIFO mode
    CHECK_EQ(r.rd(kRegs + 0x310), 0u);         // FIFO_STAT: nothing queued
    r.wr(kRegs + 0xA0, 1u << 4);
    r.wr(kRegs + 0x80, 0x12345678);  // (now memory, past the fitted 4 MiB: lost)
    CHECK_EQ(r.rd(kBar2 + 0x480), 0xAAAAAAAAu);
    CHECK_EQ(r.rd(kBar0 + 0xFFFC80), 0u);  // the upper half has no registers
}

// The aperture halves swap bytes within 16 or 32 bits as MEM_CNTL says,
// independently.
TEST(mach64_aperture_byte_swapping) {
    Rig r;
    r.wr(kBar0 + 0x100, 0x44332211);
    CHECK_EQ(r.m.vram()[0x100], 0x11u);
    CHECK_EQ(r.rd(kBar0 + 0x800100), 0x44332211u);
    r.wr(kRegs + 0xB0, 7u | 2u << 26);  // upper: 32-bit swap
    CHECK_EQ(r.rd(kBar0 + 0x800100), 0x11223344u);
    CHECK_EQ(r.rd(kBar0 + 0x800100, 1), 0x44u);
    CHECK_EQ(r.rd(kBar0 + 0x100), 0x44332211u);
    r.wr(kRegs + 0xB0, 7u | 1u << 24);  // lower: 16-bit swap
    CHECK_EQ(r.rd(kBar0 + 0x100), 0x33441122u);
    r.wr(kBar0 + 0x200, 0xBBAA, 2);
    CHECK_EQ(r.m.vram()[0x200], 0xBBu);
    CHECK_EQ(r.m.vram()[0x201], 0xAAu);
}

// The palette: write index, R G B with auto-increment, read back the same
// way; 6-bit values unless DAC_8BIT_EN; DAC_TYPE read-only.
TEST(mach64_palette_dac) {
    Rig r;
    r.wr(kRegs + 0xC4, 1u << 8);  // 8-bit
    r.wr(kRegs + 0xC0, 5, 1);
    for (u8 v : {0x10, 0x20, 0x30, 0xF0, 0xE0, 0xD0}) r.wr(kRegs + 0xC1, v, 1);
    r.wr(kRegs + 0xC3, 5, 1);
    std::vector<u8> back;
    for (int i = 0; i < 6; ++i) back.push_back(static_cast<u8>(r.rd(kRegs + 0xC1, 1)));
    CHECK(back == (std::vector<u8>{0x10, 0x20, 0x30, 0xF0, 0xE0, 0xD0}));
    CHECK_EQ(r.rd(kRegs + 0xC0, 1), 7u);
    r.wr(kRegs + 0xC4, 0);  // 6-bit
    r.wr(kRegs + 0xC0, 9, 1);
    for (int i = 0; i < 3; ++i) r.wr(kRegs + 0xC1, 0xFF, 1);
    r.wr(kRegs + 0xC3, 9, 1);
    CHECK_EQ(r.rd(kRegs + 0xC1, 1), 0x3Fu);
    CHECK_EQ((r.rd(kRegs + 0xC4) >> 16) & 7, 1u);
    r.wr(kRegs + 0xC4, 0);
    CHECK_EQ((r.rd(kRegs + 0xC4) >> 16) & 7, 1u);
}

// The clock synthesiser's registers through CLOCK_CNTL: address in byte 1,
// data in byte 2, written only with PLL_WR_EN; documented defaults.
TEST(mach64_pll_registers) {
    Rig r;
    r.wr(kRegs + 0x91, 0x02 << 2, 1);
    CHECK_EQ(r.rd(kRegs + 0x92, 1), 0x24u);  // PLL_REF_DIV
    r.wr(kRegs + 0x92, 0x55, 1);             // no write enable: ignored
    CHECK_EQ(r.rd(kRegs + 0x92, 1), 0x24u);
    r.wr(kRegs + 0x91, (0x07 << 2) | 2, 1);
    r.wr(kRegs + 0x92, 0xA6, 1);
    r.wr(kRegs + 0x91, 0x07 << 2, 1);
    CHECK_EQ(r.rd(kRegs + 0x92, 1), 0xA6u);
    // A whole dword: address and data together.
    r.wr(kRegs + 0x90, (0x2Cu << 10) | (0x77u << 16) | (1u << 9));
    r.wr(kRegs + 0x91, 0x2C << 2, 1);
    CHECK_EQ(r.rd(kRegs + 0x92, 1), 0x77u);
}

// GP_IO: pins driven read what is driven, released pins read high (no
// monitor answering on DDC).
TEST(mach64_gp_io_pins) {
    Rig r;
    CHECK_EQ(r.rd(kRegs + 0x78) & 0xFFFF, 0xFFFFu);
    r.wr(kRegs + 0x7B, 0x20, 1);  // pin 13 driven
    r.wr(kRegs + 0x79, 0x00, 1);  // low
    CHECK_EQ(r.rd(kRegs + 0x79, 1), 0xDFu);
    r.wr(kRegs + 0x7B, 0x00, 1);  // released
    CHECK_EQ(r.rd(kRegs + 0x79, 1), 0xFFu);
}

// Vertical blank follows the mode's timing: the live bit, the current line,
// the latched interrupt (written 1 to clear) and INTA when enabled.
TEST(mach64_vertical_blank_follows_the_mode) {
    Rig r;
    r.mode_1152x900();
    // VCLK0 = 2 x 14.31818 MHz x N / M / post: N 0xA6, M 0x0B... set a
    // known clock: N = 0xC8 (200), M = 0x24 (36), post 1: ~159 MHz.
    r.wr(kRegs + 0x90, (0x05u << 10) | (3u << 16) | (1u << 9));  // VCLK from the synthesiser
    r.wr(kRegs + 0x90, (0x07u << 10) | (200u << 16) | (1u << 9));
    r.wr(kRegs + 0x90, 0);
    const double clock = 2 * 14.31818e6 * 200 / 36;
    const double line_s = 1528 / clock, frame_s = line_s * 937;
    auto at = [&](double s) { r.now = static_cast<u64>(s * 100e6); };
    at(frame_s * 3 + line_s * 10.5);
    CHECK_EQ((r.rd(kRegs + 0x10) >> 16) & 0x7FF, 10u);
    CHECK_EQ(r.rd(kRegs + 0x18) & 1, 0u);
    at(frame_s * 3 + line_s * 899.5);
    CHECK_EQ(r.rd(kRegs + 0x18) & 1, 0u);  // the last line shown
    at(frame_s * 3 + line_s * 900.5);
    CHECK_EQ(r.rd(kRegs + 0x18) & 1, 1u);  // the first of the blank
    at(frame_s * 3 + line_s * 910.5);
    CHECK_EQ(r.rd(kRegs + 0x18) & 1, 1u);
    CHECK_EQ(r.rd(kRegs + 0x18) & 4, 4u);  // latched
    r.wr(kRegs + 0x18, 4);                 // acknowledged
    CHECK_EQ(r.rd(kRegs + 0x18) & 4, 0u);
    r.wr(kRegs + 0x18, 2);  // vblank interrupt enabled
    CHECK(!r.irq);
    r.run(static_cast<u64>(frame_s * 100e6));
    CHECK(r.irq);
    r.wr(kRegs + 0x18, 2 | 4);
    CHECK(!r.irq);
}

// Scan-out at 8 bpp through the palette and DAC mask, blank when the
// display is off, and the cursor's colours, transparency and inversion.
TEST(mach64_scans_out_the_frame_buffer_and_cursor) {
    Rig r;
    std::vector<u32> px;
    unsigned w = 0, h = 0;
    CHECK(!r.m.render(px, w, h));  // no extended display yet
    r.mode_1152x900();
    r.wr(kRegs + 0xC4, 1u << 8);
    r.wr(kRegs + 0xC0, 1, 1);
    for (u8 v : {0x80, 0x40, 0xC0}) r.wr(kRegs + 0xC1, v, 1);
    r.wr(kBar0 + 1152 * 2 + 3, 1, 1);
    CHECK(r.m.render(px, w, h));
    CHECK_EQ(w, 1152u);
    CHECK_EQ(h, 900u);
    CHECK_EQ(px[1152 * 2 + 3], 0x8040C0u);
    r.wr(kRegs + 0xC2, 0xFE, 1);  // DAC mask
    r.m.render(px, w, h);
    CHECK_EQ(px[1152 * 2 + 3], 0u);
    r.wr(kRegs + 0xC2, 0xFF, 1);
    // Cursor at (100, 50): bitmap at 0x300000, row 0 pixels: 0, 1, 2 (clear), 3 (invert).
    r.wr(kRegs + 0x68, 0x300000 / 8);
    r.wr(kRegs + 0x6C, 100u | 50u << 16);
    r.wr(kRegs + 0x60, 0x11223300);
    r.wr(kRegs + 0x64, 0xAABBCC00);
    r.wr(kBar0 + 0x300000, 0b11100100, 1);
    r.wr(kRegs + 0xD0, 1u << 7);
    r.m.render(px, w, h);
    CHECK_EQ(px[1152 * 50 + 100], 0x112233u);
    CHECK_EQ(px[1152 * 50 + 101], 0xAABBCCu);
    CHECK_EQ(px[1152 * 50 + 102], 0u);
    CHECK_EQ(px[1152 * 50 + 103], 0xFFFFFFu);
    r.wr(kRegs + 0x1C, 0x03000240);  // display disabled
    r.m.render(px, w, h);
    CHECK_EQ(px[1152 * 2 + 3], 0u);
}

// ---- the 2D engine ----

namespace {

// The engine as a console driver sets it up: 1152 pixels a row at 8 bits
// from offset 0, the whole screen in the scissors, every bit written,
// foreground mix "source", background "destination", foreground colour.
struct Engine : Rig {
    Engine() {
        wr(kRegs + 0x100, 144u << 22);           // DST_OFF_PITCH
        wr(kRegs + 0x180, 144u << 22);           // SRC_OFF_PITCH
        wr(kRegs + 0x2D0, 0x00000202);           // DP_PIX_WIDTH: 8 bpp (host 1 bpp)
        wr(kRegs + 0x2A8, 0 | (1151u << 16));    // SC_LEFT_RIGHT
        wr(kRegs + 0x2B4, 0 | (899u << 16));     // SC_TOP_BOTTOM
        wr(kRegs + 0x2C8, 0xFFFFFFFF);           // DP_WRITE_MASK
        wr(kRegs + 0x2D4, 0x00070003);           // DP_MIX
        wr(kRegs + 0x2D8, 0x00000100);           // DP_SRC: foreground colour
        wr(kRegs + 0x130, 0x00000003);           // DST_CNTL: left to right, top to bottom
        wr(kRegs + 0x304, 0xFFFFFFFF);           // CLR_CMP_MSK
    }
    u8 at(unsigned x, unsigned y) const { return m.vram()[y * 1152 + x]; }
    void rect(unsigned x, unsigned y, unsigned w, unsigned h) {
        wr(kRegs + 0x10C, y | (x << 16));  // DST_Y_X
        wr(kRegs + 0x118, h | (w << 16));  // DST_HEIGHT_WIDTH: go
    }
};

}  // namespace

// A solid fill, inside the scissors, through the write mask; tiling moves
// DST_X on.
TEST(mach64_engine_fills_rectangles) {
    Engine e;
    e.wr(kRegs + 0x2C4, 0x5A);  // DP_FRGD_CLR
    e.rect(10, 20, 4, 3);
    CHECK_EQ(e.at(10, 20), 0x5Au);
    CHECK_EQ(e.at(13, 22), 0x5Au);
    CHECK_EQ(e.at(14, 22), 0u);
    CHECK_EQ(e.at(10, 23), 0u);
    CHECK_EQ(e.at(9, 20), 0u);
    e.wr(kRegs + 0x2A8, 0 | (100u << 16));  // right edge at 100
    e.rect(99, 0, 4, 1);
    CHECK_EQ(e.at(100, 0), 0x5Au);
    CHECK_EQ(e.at(101, 0), 0u);
    e.wr(kRegs + 0x2A8, 0 | (1151u << 16));
    e.wr(kRegs + 0x2C8, 0x0F);  // write mask: the low nibble
    e.wr(kRegs + 0x2C4, 0xFF);
    e.rect(10, 20, 1, 1);
    CHECK_EQ(e.at(10, 20), 0x5Fu);
    e.wr(kRegs + 0x2C8, 0xFFFFFFFF);
    e.wr(kRegs + 0x130, 0x0B);  // + DST_X_TILE
    e.rect(200, 0, 8, 1);
    CHECK_EQ(e.rd(kRegs + 0x104) & 0x3FFF, 208u);  // DST_X
    CHECK(!(e.rd(kRegs + 0x338) & 1));             // GUI_STAT: idle
}

// Screen to screen copies in all four directions; an overlapping copy
// done bottom to top keeps its source intact; mixes.
TEST(mach64_engine_copies_and_mixes) {
    Engine e;
    for (unsigned y = 0; y < 8; ++y)
        for (unsigned x = 0; x < 8; ++x) e.wr(kBar0 + (100 + y) * 1152 + 100 + x, (y << 4) | x, 1);
    e.wr(kRegs + 0x2D8, 0x00000300);  // DP_SRC: blit
    e.wr(kRegs + 0x18C, 100 | (100u << 16));  // SRC_Y_X
    e.wr(kRegs + 0x198, 8 | (8u << 16));      // SRC_HEIGHT1_WIDTH1
    e.rect(300, 300, 8, 8);
    CHECK_EQ(e.at(300, 300), 0x00u);
    CHECK_EQ(e.at(307, 307), 0x77u);
    CHECK_EQ(e.at(303, 305), 0x53u);
    // Down by 2, overlapping: bottom to top, from the bottom rows.
    e.wr(kRegs + 0x130, 0x01);  // left to right, bottom to top
    e.wr(kRegs + 0x18C, 107 | (100u << 16));
    e.rect(100, 109, 8, 8);
    CHECK_EQ(e.at(100, 102), 0x00u);  // the old row 0
    CHECK_EQ(e.at(107, 109), 0x77u);  // the old row 7
    CHECK_EQ(e.at(103, 105), 0x33u);
    // Right to left, top to bottom: the mirror of the order, same result.
    e.wr(kRegs + 0x130, 0x02);
    e.wr(kRegs + 0x18C, 300 | (307u << 16));
    e.rect(507, 300, 8, 1);
    CHECK_EQ(e.at(500, 300), 0x00u);
    CHECK_EQ(e.at(507, 300), 0x07u);
    // XOR with the destination.
    e.wr(kRegs + 0x130, 0x03);
    e.wr(kRegs + 0x2D8, 0x00000100);
    e.wr(kRegs + 0x2D4, 0x00050003);
    e.wr(kRegs + 0x2C4, 0xFF);
    e.rect(300, 300, 1, 1);
    CHECK_EQ(e.at(300, 300), 0xFFu);
    e.rect(300, 300, 1, 1);
    CHECK_EQ(e.at(300, 300), 0x00u);
}

// Colour expansion of host data (text): 1 bit a pixel, leftmost in each
// byte's top bit, each row starting on a byte (HOST_BYTE_ALIGN); ones in
// the foreground colour, zeros in the background mix (here: left as they
// are). Busy until the data has come; extra data is not used.
TEST(mach64_engine_expands_host_data) {
    Engine e;
    e.wr(kRegs + 0x240, 1);           // HOST_CNTL: byte aligned rows
    e.wr(kRegs + 0x2D8, 0x00020100);  // mono from host, foreground colour
    e.wr(kRegs + 0x2C4, 0x11);
    e.rect(40, 50, 3, 2);             // 3 x 2: two bytes of host data
    CHECK_EQ(e.rd(kRegs + 0x338) & 1, 1u);  // waiting
    e.wr(kRegs + 0x200, 0x0000A0C0);  // row 0: 110..., row 1: 101...
    CHECK_EQ(e.rd(kRegs + 0x338) & 1, 0u);
    CHECK_EQ(e.at(40, 50), 0x11u);
    CHECK_EQ(e.at(41, 50), 0x11u);
    CHECK_EQ(e.at(42, 50), 0u);
    CHECK_EQ(e.at(40, 51), 0x11u);
    CHECK_EQ(e.at(41, 51), 0u);
    CHECK_EQ(e.at(42, 51), 0x11u);
    e.wr(kRegs + 0x204, 0xFFFFFFFF);  // nothing waiting for it
    CHECK_EQ(e.at(41, 51), 0u);
    // DP_BYTE_PIX_ORDER: the leftmost pixel in the low bit; the background
    // drawn in its colour.
    e.wr(kRegs + 0x2D0, 0x01000202);
    e.wr(kRegs + 0x2D4, 0x00070007);
    e.wr(kRegs + 0x2D8, 0x00020100);  // foreground colour, background colour
    e.wr(kRegs + 0x2C0, 0x22);
    e.rect(60, 50, 4, 1);
    e.wr(kRegs + 0x23C, 0x00000005);
    CHECK_EQ(e.at(60, 50), 0x11u);
    CHECK_EQ(e.at(61, 50), 0x22u);
    CHECK_EQ(e.at(62, 50), 0x11u);
    CHECK_EQ(e.at(63, 50), 0x22u);
    // A new operation while one waits: the one waiting ends there.
    e.rect(70, 50, 8, 1);
    e.wr(kRegs + 0x2C4, 0x33);
    CHECK(!(e.rd(kRegs + 0x338) & 1));
}

// Colour host data at 32 bits, byte swapped for a big-endian host.
TEST(mach64_engine_takes_colour_host_data) {
    Engine e;
    e.wr(kRegs + 0x100, (1152u / 8) << 22);
    e.wr(kRegs + 0x2D0, 0x00060006);  // 32 bpp destination and host data
    e.wr(kRegs + 0x2D8, 0x00000200);  // host colour
    e.wr(kRegs + 0x240, 2);           // HOST_BIG_ENDIAN_EN
    e.rect(1, 0, 1, 1);
    e.wr(kRegs + 0x200, 0x44332211);
    CHECK_EQ(e.m.vram()[4], 0x44u);
    CHECK_EQ(e.m.vram()[7], 0x11u);
}

// Bresenham lines (X major, down and right): the first pixel, the minor
// step when the error says, the last pixel only with DST_LAST_PEL; DST_X
// and DST_Y at the last pixel after.
TEST(mach64_engine_draws_lines) {
    Engine e;
    e.wr(kRegs + 0x2C4, 0x77);
    // From (10, 10) to (18, 13): major 8, minor 3.
    e.wr(kRegs + 0x10C, 10 | (10u << 16));
    e.wr(kRegs + 0x124, static_cast<u32>(2 * 3 - 8) & 0x3FFFF);  // ERR
    e.wr(kRegs + 0x128, 2 * 3);                                  // INC
    e.wr(kRegs + 0x12C, static_cast<u32>(2 * (3 - 8)) & 0x3FFFF);  // DEC
    e.wr(kRegs + 0x130, 0x23);                                   // right, down, X major, last pixel
    e.wr(kRegs + 0x120, 9);                                      // 9 pixels: go
    CHECK_EQ(e.at(10, 10), 0x77u);
    CHECK_EQ(e.at(18, 13), 0x77u);
    CHECK_EQ(e.at(11, 10), 0x77u);
    unsigned count = 0;
    for (unsigned y = 8; y < 16; ++y)
        for (unsigned x = 8; x < 20; ++x) count += e.at(x, y) == 0x77;
    CHECK_EQ(count, 9u);
    CHECK_EQ(e.rd(kRegs + 0x104) & 0x3FFF, 18u);
    CHECK_EQ(e.rd(kRegs + 0x108) & 0x7FFF, 13u);
    // The same without the last pixel.
    e.wr(kRegs + 0x10C, 30 | (10u << 16));
    e.wr(kRegs + 0x124, static_cast<u32>(2 * 3 - 8) & 0x3FFFF);
    e.wr(kRegs + 0x130, 0x03);
    e.wr(kRegs + 0x120, 9);
    CHECK_EQ(e.at(10, 30), 0x77u);
    CHECK_EQ(e.at(18, 33), 0u);
    // An error of zero steps the minor axis, unless DST_BRES_SIGN: (0, 40)
    // to (4, 42), and the same with the sign bit.
    for (int sign = 0; sign < 2; ++sign) {
        const unsigned y0 = 40 + 10 * static_cast<unsigned>(sign);
        e.wr(kRegs + 0x10C, y0);
        e.wr(kRegs + 0x124, 0);
        e.wr(kRegs + 0x128, 4);
        e.wr(kRegs + 0x12C, static_cast<u32>(-4) & 0x3FFFF);
        e.wr(kRegs + 0x130, 0x23u | (sign ? 1u << 11 : 0));
        e.wr(kRegs + 0x120, 5);
        CHECK_EQ(e.at(1, y0 + 1), sign ? 0u : 0x77u);
        CHECK_EQ(e.at(1, y0), sign ? 0x77u : 0u);
    }
}

// The 8x8 mono pattern at the destination's position, and the colour
// compare leaving matching pixels alone.
TEST(mach64_engine_patterns_and_colour_compare) {
    Engine e;
    e.wr(kRegs + 0x280, 0x000000F0);  // row 0: four on, four off
    e.wr(kRegs + 0x284, 0);
    e.wr(kRegs + 0x288, 1);           // mono pattern
    e.wr(kRegs + 0x2D8, 0x00010100);  // mono from the pattern
    e.wr(kRegs + 0x2C4, 0x44);
    e.rect(8, 16, 16, 2);
    CHECK_EQ(e.at(8, 16), 0x44u);
    CHECK_EQ(e.at(11, 16), 0x44u);
    CHECK_EQ(e.at(12, 16), 0u);
    CHECK_EQ(e.at(16, 16), 0x44u);  // repeats every 8
    CHECK_EQ(e.at(8, 17), 0u);      // row 1 empty
    // Colour compare: pixels equal to 0x44 stay.
    e.wr(kRegs + 0x2D8, 0x00000100);
    e.wr(kRegs + 0x2C4, 0x55);
    e.wr(kRegs + 0x300, 0x44);
    e.wr(kRegs + 0x308, 5);
    e.rect(8, 16, 8, 1);
    CHECK_EQ(e.at(8, 16), 0x44u);
    CHECK_EQ(e.at(12, 16), 0x55u);
}

// Packed 24 bits: the engine draws bytes, each taking its component of the
// colour by its position.
TEST(mach64_engine_packed_24_bit_fills) {
    Engine e;
    e.wr(kRegs + 0x2D0, 0x00000205);
    e.wr(kRegs + 0x2C4, 0x00332211);
    e.rect(3, 0, 6, 1);  // pixels 1 and 2
    CHECK_EQ(e.m.vram()[3], 0x11u);
    CHECK_EQ(e.m.vram()[4], 0x22u);
    CHECK_EQ(e.m.vram()[5], 0x33u);
    CHECK_EQ(e.m.vram()[8], 0x33u);
    CHECK_EQ(e.m.vram()[9], 0u);
}
