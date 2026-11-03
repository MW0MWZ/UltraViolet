// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
// The Creator3D: its PROM's byte lanes, the strapping, the DAC's indirect
// registers, the FBC's identity and fields, drawing (rectangles, vertical
// scroll, font rows), the frame buffer views, and the picture through the
// window lookups.
#include <cmath>
#include <utility>
#include <vector>

#include "devices/ffb.h"
#include "test.h"

using namespace uv;

namespace {

constexpr u64 kDac = 0x400000, kFbc = 0x600000, kDfb32 = 0x3000000, kDfb24 = 0x2000000, kDfb8R = 0x1000000,
              kSfb32 = 0x5000000, kDfb24B = 0xA000000;

struct Rig {
    u64 now = 0;
    Scheduler sched;
    Ffb f{sched, 100e6, {0xF3, 0x03, 0xC7, 0x71}};
    Rig() { sched.set_clock([this] { return now; }); }
    u64 rd(u64 a, unsigned size = 4) {
        u64 v = 0;
        f.read(a, size, v);
        return v;
    }
    void wr(u64 a, u64 v, unsigned size = 4) { f.write(a, size, v); }
    void dac(u32 reg, u32 v) { wr(kDac, reg), wr(kDac + 4, v); }
    u32 dac(u32 reg) { return wr(kDac, reg), static_cast<u32>(rd(kDac + 4)); }
    void fbc(u32 reg, u32 v) { wr(kFbc + reg, v); }
    // On to the next event (the vertical blank's start), run.
    void next_event() {
        now = sched.next_event();
        sched.run_due(now);
    }
    u32 pixel(unsigned x, unsigned y) { return static_cast<u32>(rd(kDfb32 + (u64{y} * Ffb::kLine + x) * 4)); }
    // 1152 x 900 at 66 Hz, as the FCode sets it: the pixel clock, the timing
    // registers from VBNP up, the timing generator and video on, every window
    // ID true colour from buffer A.
    void mode_1152x900() {
        dac(0x0000, 0x4000 | 1 << 11 | 5 << 7 | 0x46);
        wr(kDac, 0x6001);
        for (u32 v : {0x22u, 0x3A6u, 3u, 0x3A8u, 0x2BBu, 0xA7u, 0x2E7u, 0x3Fu, 0x2FBu, 0x2E3u, 0xA3u, 0u, 0u, 0u}) wr(kDac + 4, v);
        wr(kDac, 0x3200);
        for (unsigned i = 0; i < 64; ++i) wr(kDac + 4, 0x4000);
        dac(0x3150, 2);
        dac(0x6000, 3);
    }
};

}  // namespace

TEST(ffb_prom_strapping_and_identity) {
    Rig r;
    // A byte of the PROM at each fourth address, the other lanes fixed.
    CHECK_EQ(r.rd(0, 4), 0xF300012Cu);
    CHECK_EQ(r.rd(4, 1), 0x03u);
    CHECK_EQ(r.rd(0x10, 1), 0xFFu);  // (past the image)
    CHECK_EQ(r.rd(0x200000, 1), 0x33u);
    CHECK_EQ((r.rd(0x200010) >> 24) & 7, 7u);
    CHECK_EQ(r.rd(kFbc + 0x800), 0x3241906Du);
    CHECK_EQ(r.rd(kFbc + 0x900) & 0x03000000, 0u);  // never busy
    CHECK_EQ(r.dac(0x8000), 0xA236E1ADu);
    CHECK_EQ((r.dac(0x1001) >> 8) & 0xF, 1u);
}

TEST(ffb_dac_pointer_increments) {
    Rig r;
    r.wr(kDac, 0x2000);
    for (u32 i = 0; i < 4; ++i) r.wr(kDac + 4, i * 0x010101);
    CHECK_EQ(r.rd(kDac), 0x2004u);
    CHECK_EQ(r.dac(0x2003), 0x030303u);
    // The monitor port: no DDC monitor, so the lines read as driven.
    r.dac(0x8001, 1);
    CHECK_EQ(r.dac(0x8002), 1u);
}

TEST(ffb_fields_change_only_where_written) {
    Rig r;
    r.fbc(0x254, 0x200875AA);
    r.fbc(0x254, 0x00000001);  // the red group off, the rest as they were
    CHECK_EQ(r.rd(kFbc + 0x254), 0x200875A9u);
    r.fbc(0x258, 0x00838383);
    r.fbc(0x258, 0x0000008A);  // RGB to NOLD; X kept
    CHECK_EQ(r.rd(kFbc + 0x258), 0x0083838Au);
}

TEST(ffb_rectangle_vscroll_and_font) {
    Rig r;
    r.fbc(0x200, 0x869BAF);      // constant colour and X, no clip or pattern
    r.fbc(0x254, 0x200875AA);    // buffer A, every colour group and X on
    r.fbc(0x258, 0x00838383);    // new
    r.fbc(0x290, 0xFFFFFF);
    r.fbc(0x294, 0xFF);
    r.fbc(0x208, 0x00336699);
    r.fbc(0x300, 0x08);
    r.fbc(0x60, 10), r.fbc(0x64, 20), r.fbc(0x70, 2), r.fbc(0x74, 3);  // y, x, h, w
    CHECK_EQ(r.pixel(20, 10), 0x00336699u);
    CHECK_EQ(r.pixel(22, 11), 0x00336699u);
    CHECK_EQ(r.pixel(23, 11), 0u);
    CHECK_EQ(r.pixel(20, 12), 0u);
    // Invert the colour planes: the X plane kept.
    r.wr(kDfb32 + (u64{5} * Ffb::kLine + 5) * 4, 0x11000000);
    r.fbc(0x258, 0x0085858A);
    r.fbc(0x60, 5), r.fbc(0x64, 5), r.fbc(0x70, 1), r.fbc(0x74, 1);
    CHECK_EQ(r.pixel(5, 5), 0x11FFFFFFu);
    // Vertical scroll: the block at (20, 10) to (40, 9), overlapping rows.
    r.fbc(0x300, 0x0B);
    r.fbc(0x60, 10), r.fbc(0x64, 20), r.fbc(0x68, 9), r.fbc(0x6C, 40), r.fbc(0x70, 2), r.fbc(0x74, 3);
    CHECK_EQ(r.pixel(40, 9), 0x00336699u);
    CHECK_EQ(r.pixel(42, 10), 0x00336699u);
    // A font row: set bits in the foreground, clear in the background,
    // then down a line.
    r.fbc(0x258, 0x00838383);
    r.fbc(0x200, 0x200);  // an opaque background
    r.fbc(0x20C, 0x00000011);
    r.fbc(0x318, 4);
    r.fbc(0x31C, 0x10000);
    r.fbc(0x314, 100 << 16 | 50);
    r.fbc(0x320, 0xA0000000);
    CHECK_EQ(r.pixel(50, 100), 0x00336699u);
    CHECK_EQ(r.pixel(51, 100), 0x00000011u);
    CHECK_EQ(r.pixel(53, 100), 0x00000011u);
    CHECK_EQ(r.rd(kFbc + 0x314), (101u << 16) | 50u);
}

TEST(ffb_views) {
    Rig r;
    r.fbc(0x254, 0x200875AA);
    r.wr(kDfb32 + 8, 0x01020304);
    r.wr(kDfb24 + 8, 0x00AABBCC);  // the X plane kept
    CHECK_EQ(r.pixel(2, 0), 0x01AABBCCu);
    CHECK_EQ(r.rd(kDfb8R + 2, 1), 0xCCu);
    r.wr(kDfb8R + 0x400000 * 3 + 2, 0x7F, 1);  // X
    CHECK_EQ(r.pixel(2, 0), 0x7FAABBCCu);
    // A smart write through the raster operation and the plane masks: the
    // groups enabled through their masks (11).
    r.fbc(0x254, 0x000000FF);
    r.fbc(0x258, 0x00868686);  // xor
    r.fbc(0x290, 0x0000FF);
    r.fbc(0x294, 0);
    r.wr(kSfb32 + 8, 0xFFFFFFFF);
    CHECK_EQ(r.pixel(2, 0), 0x7FAABB33u);
    // On (10), a group is written whole whatever its mask, as Xsun writes
    // its 24-bit images with a plane mask of 0.
    r.fbc(0x254, 0x0000006A);  // red, green, blue on, X off
    r.fbc(0x290, 0);
    r.wr(kSfb32 + 8, 0xFFFFFFFF);
    CHECK_EQ(r.pixel(2, 0), 0x7F5544CCu);
}

TEST(ffb_polygon) {
    Rig r;
    r.fbc(0x200, 0x869BAF);
    r.fbc(0x254, 0x200875AA);
    r.fbc(0x258, 0x00838383);
    r.fbc(0x290, 0xFFFFFF);
    r.fbc(0x294, 0xFF);
    r.fbc(0x208, 0x00445566);
    // A triangle (10,10) (20,10) (10,20) as Xsun gives it: the first
    // vertices through by/bx, one through bh/bw, the last through ebyi/ebxi.
    r.fbc(0x300, 0x07);
    r.fbc(0x60, 10), r.fbc(0x64, 10);
    r.fbc(0x70, 10), r.fbc(0x74, 20);
    CHECK_EQ(r.pixel(10, 10), 0u);  // nothing until the last
    r.fbc(0x50, 20), r.fbc(0x54, 10);
    CHECK_EQ(r.pixel(10, 10), 0x00445566u);
    CHECK_EQ(r.pixel(19, 10), 0x00445566u);
    CHECK_EQ(r.pixel(20, 10), 0u);  // the right edge not drawn
    CHECK_EQ(r.pixel(14, 15), 0x00445566u);
    CHECK_EQ(r.pixel(15, 15), 0u);
    CHECK_EQ(r.pixel(10, 19), 0x00445566u);
    CHECK_EQ(r.pixel(10, 20), 0u);  // nor the bottom
    // by/bx and bh/bw start no other operation meanwhile.
    CHECK_EQ(r.pixel(20, 20), 0u);
}

TEST(ffb_window_mask_writes_the_x_planes_through_the_window_id_plane_mask) {
    Rig r;
    // Buffer B's X planes 0xC5 at x 0 and 1 (overlay selection 3, window ID 5).
    r.fbc(0x254, 0x4008B5AA);  // write B
    r.fbc(0x200, 0x869BAF);
    r.fbc(0x258, 0x00838383);
    r.fbc(0x290, 0xFFFFFF);
    r.fbc(0x294, 0xFF);
    r.fbc(0x300, 0x08);
    r.fbc(0x208, 0xC5000000);
    r.fbc(0x60, 0), r.fbc(0x64, 0), r.fbc(0x70, 1), r.fbc(0x74, 2);
    // As Xsun paints a window: the window ID (1) through the low six planes,
    // whatever was there; then the overlay selection (0) through the top two.
    r.fbc(0x254, 0x403CB5D5);
    r.fbc(0x204, 0);
    for (auto [c, m] : {std::pair{0x01000000u, 0x3Fu}, std::pair{0u, 0xC0u}}) {
        r.fbc(0x208, c);
        r.fbc(0x35C, m);
        r.fbc(0x60, 0), r.fbc(0x64, 0), r.fbc(0x70, 1), r.fbc(0x74, 1);
    }
    CHECK_EQ(r.rd(kDfb24B + 0) >> 24, 0x01u);  // painted
    CHECK_EQ(r.rd(kDfb24B + 4) >> 24, 0xC5u);  // outside the box: kept
    r.fbc(0x254, 0x20007000);
    r.mode_1152x900();
    r.dac(0x1001, 0x12C);  // Separate_8: window IDs from B's X planes
    r.dac(0x3201, 0x4000 | 0x8000);  // window ID 1 true colour from B
    r.dac(0x3150, 2);
    r.next_event();
    r.wr(kDfb24B + 0, 0x00112233);
    Display::Frame frame;
    CHECK(r.f.render(frame));
    CHECK_EQ(frame.pixels[0], 0x332211u);  // window ID 1: B
}

TEST(ffb_dumb_views_are_buffer_a) {
    Rig r;
    // A rectangle drawn into B alone: the dumb views read A whatever the
    // read buffer, DFB24B reads B.
    r.fbc(0x200, 0x869BAF);
    r.fbc(0x254, 0x4008B5AA);  // write B, read B
    r.fbc(0x258, 0x00838383);
    r.fbc(0x290, 0xFFFFFF);
    r.fbc(0x294, 0xFF);
    r.fbc(0x208, 0x12345678);
    r.fbc(0x300, 0x08);
    r.fbc(0x60, 0), r.fbc(0x64, 3), r.fbc(0x70, 1), r.fbc(0x74, 1);
    CHECK_EQ(r.pixel(3, 0), 0u);
    CHECK_EQ(r.rd(kDfb24B + 12) & 0xFFFFFF, 0x345678u);
    // A dumb write lands in A whatever the write buffer.
    r.wr(kDfb32 + 12, 0x01020304);
    r.fbc(0x254, 0x20007000);  // write A, read A
    CHECK_EQ(r.pixel(3, 0), 0x01020304u);
    CHECK_EQ(r.rd(kDfb24B + 12) & 0xFFFFFF, 0x345678u);
}

TEST(ffb_picture_through_the_window_lookups) {
    Rig r;
    Display::Frame frame;
    CHECK(!r.f.render(frame));  // no timing yet
    r.mode_1152x900();
    r.fbc(0x254, 0x200875AA);
    r.wr(kDfb32 + 4, 0x00102030);  // red 0x30, green 0x20, blue 0x10
    CHECK(r.f.render(frame));
    CHECK_EQ(frame.width, 1152u);
    CHECK_EQ(frame.height, 900u);
    CHECK_EQ(frame.pixels[1], 0x302010u);
    // Window ID 1 as pseudocolour from the red channel through palette 1.
    r.dac(0x3201, 0x1000 | 0x100 | 0x10);
    r.dac(0x3150, 2);
    r.next_event();
    r.dac(0x2000 + 256 + 0x30, 0x0000FF);
    r.wr(kDfb32 + 4, 0x01000030);
    CHECK(r.f.render(frame));
    CHECK_EQ(frame.pixels[1], 0xFF0000u);
    // The vertical counter runs at 1528 x 937 pixel clocks a frame.
    const double line = 100e6 * 1528 / 94.5e6;
    r.now = static_cast<u64>(line * (937 + 10.5));
    CHECK_EQ(r.dac(0x600F), 10u);
}

TEST(ffb_window_transfer_waits_for_the_vertical_blank) {
    Rig r;
    Display::Frame frame;
    r.mode_1152x900();
    r.wr(kDfb32 + 4, 0x01000030);
    r.dac(0x2000 + 256 + 0x30, 0x0000FF);
    r.dac(0x3201, 0x1000 | 0x100 | 0x10);  // window ID 1: palette 1 from red
    r.dac(0x3150, 6);                      // at the next frame
    CHECK_EQ(r.dac(0x3150) & 1, 1u);       // busy
    CHECK(r.f.render(frame));
    CHECK_EQ(frame.pixels[1], 0x300000u);  // still true colour
    // At the start of the vertical blank (VBAP, line 0x3A6).
    const double line = 100e6 * 1528 / 94.5e6;
    CHECK_EQ(r.sched.next_event(), static_cast<u64>(std::ceil(line * 0x3A6)));
    r.next_event();
    CHECK_EQ(r.dac(0x3150), 4u);
    CHECK(r.f.render(frame));
    CHECK_EQ(frame.pixels[1], 0xFF0000u);
    CHECK_EQ(r.sched.next_event(), Scheduler::kNever);
}

TEST(ffb_vertical_retrace_interrupt) {
    Rig r;
    unsigned pulses = 0;
    r.f.set_interrupt_sink([&] { ++pulses; });
    r.mode_1152x900();
    r.fbc(0x10900, 8);
    CHECK_EQ(pulses, 0u);
    r.next_event();
    CHECK_EQ(pulses, 1u);
    CHECK_EQ(r.rd(kFbc + 0x10900), 0xCu);
    r.next_event();  // still flagged: no second pulse
    CHECK_EQ(pulses, 1u);
    // Acknowledged as Solaris's driver does: the flag and the enable cleared.
    r.fbc(0x10900, 5);
    CHECK_EQ(r.rd(kFbc + 0x10900), 0u);
    CHECK_EQ(r.sched.next_event(), Scheduler::kNever);
    r.fbc(0x10900, 8);
    r.next_event();
    CHECK_EQ(pulses, 2u);
}

TEST(ffb_smart_view_blends_the_foreground_by_the_alpha_written) {
    Rig r;
    // As NetBSD draws its antialiased font: colour from the foreground, the
    // X source the value written, alpha blending on (source alpha,
    // destination 1 - alpha, 0xFF as 1.0), only each pixel's alpha written.
    r.fbc(0x254, 0x202C757F);  // write A, X planes off
    r.fbc(0x258, 0x00838383);
    r.fbc(0x290, 0xFFFFFFFF);
    r.wr(kDfb32 + 0, 0x00000000);
    r.wr(kDfb32 + 4, 0x00000000);
    r.wr(kDfb32 + 8, 0x00204060);
    r.fbc(0x244, 0x10 | 0x8 | 0x3);
    r.fbc(0x208, 0x00FFFFFF);
    r.fbc(0x200, 0x86DAAB);
    r.wr(kSfb32 + 0, 0xFF000000);
    r.wr(kSfb32 + 4, 0x80000000);
    r.wr(kSfb32 + 8, 0x00000000);
    CHECK_EQ(r.pixel(0, 0), 0x00FFFFFFu);  // all foreground
    CHECK_EQ(r.pixel(1, 0), 0x00808080u);  // half
    CHECK_EQ(r.pixel(2, 0), 0x00204060u);  // the old pixel kept
    // Without blending, a constant colour source writes the foreground.
    r.fbc(0x200, 0x869AAB);
    r.wr(kSfb32 + 12, 0x12345678);
    CHECK_EQ(r.pixel(3, 0) & 0xFFFFFF, 0xFFFFFFu);
}

TEST(ffb_auxiliary_clip_adds_to_or_takes_from_the_view_clip) {
    Rig r;
    // As Xsun draws a window's text: the view clip one row, the rest of the
    // window in an added rectangle; dots at y 0 (the view clip), 5 (the
    // rectangle) and 20 (neither).
    r.fbc(0x254, 0x200875AA);
    r.fbc(0x258, 0x00838383);
    r.fbc(0x208, 0x00112233);
    r.fbc(0x220, 0 << 16 | 0), r.fbc(0x224, 0 << 16 | 100);   // view clip: row 0
    r.fbc(0x2A0, 1 << 16 | 0), r.fbc(0x2A4, 10 << 16 | 100);  // rectangle: rows 1-10
    r.fbc(0x2A8, 1 << 16 | 1), r.fbc(0x2AC, 0);               // (empty)
    r.fbc(0x2B0, 1 << 16 | 1), r.fbc(0x2B4, 0);
    r.fbc(0x2B8, 1 << 16 | 1), r.fbc(0x2BC, 0);
    r.fbc(0x300, 0x00);  // dots
    auto dots = [&](u32 ppc) {
        r.fbc(0x200, ppc);
        for (int y : {0, 5, 20}) r.fbc(0x70, static_cast<u32>(y)), r.fbc(0x74, 10);
    };
    dots(0x8EABFF);  // view clip 2D, auxiliary clip added
    CHECK_EQ(r.pixel(10, 0), 0x00112233u);
    CHECK_EQ(r.pixel(10, 5), 0x00112233u);
    CHECK_EQ(r.pixel(10, 20), 0u);
    r.fbc(0x208, 0x00445566);
    dots(0x8AABFF);  // the rectangle taken out
    CHECK_EQ(r.pixel(10, 0), 0x00445566u);
    CHECK_EQ(r.pixel(10, 5), 0x00112233u);
}

