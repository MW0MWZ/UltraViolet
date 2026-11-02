// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
// The Creator3D: its PROM's byte lanes, the strapping, the DAC's indirect
// registers, the FBC's identity and fields, drawing (rectangles, vertical
// scroll, font rows), the frame buffer views, and the picture through the
// window lookups.
#include <vector>

#include "devices/ffb.h"
#include "test.h"

using namespace uv;

namespace {

constexpr u64 kDac = 0x400000, kFbc = 0x600000, kDfb32 = 0x3000000, kDfb24 = 0x2000000, kDfb8R = 0x1000000,
              kSfb32 = 0x5000000;

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
    // A smart write through the raster operation and plane mask.
    r.fbc(0x258, 0x00868686);  // xor
    r.fbc(0x290, 0x0000FF);
    r.fbc(0x294, 0);
    r.wr(kSfb32 + 8, 0xFFFFFFFF);
    CHECK_EQ(r.pixel(2, 0), 0x7FAABB33u);
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
    r.dac(0x2000 + 256 + 0x30, 0x0000FF);
    r.wr(kDfb32 + 4, 0x01000030);
    CHECK(r.f.render(frame));
    CHECK_EQ(frame.pixels[1], 0xFF0000u);
    // The vertical counter runs at 1528 x 937 pixel clocks a frame.
    const double line = 100e6 * 1528 / 94.5e6;
    r.now = static_cast<u64>(line * 10.5);
    CHECK_EQ(r.dac(0x600F), 10u);
}
