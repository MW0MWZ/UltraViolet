// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
// The Creator3D: its PROM's byte lanes, the strapping, the DAC's indirect
// registers, the FBC's identity and fields, drawing (rectangles, vertical
// scroll, font rows; triangles, lines and dots from vertices), picking, the
// pixel tests (compares, X clip, depth bounds), depth cueing, buffer C, the
// frame buffer views, the picture through the
// window lookups in each window mode, the window transfer and the
// vertical retrace interrupt.
#include <cmath>
#include <initializer_list>
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
    // ID true colour from buffer A, all six window ID bits addressing them.
    void mode_1152x900() {
        dac(0x3153, 0x3F);
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

// A block fill counts its width and height from the block its start is in,
// a block being as the frame buffer's organisation (FBCFG0) sets it: 16 x 4
// pixels in the high resolutions, 8 x 4 in the standard ones, as Xsun's
// block fills take them.
TEST(ffb_block_fill_counts_from_the_organisations_block) {
    for (const bool high : {true, false}) {
        Rig r;
        r.fbc(0x10270, high ? 0xCA30003B : 0xC83C002B);
        r.fbc(0x200, 0x869BAF);
        r.fbc(0x254, 0x200875AA);
        r.fbc(0x258, 0x00838383);
        r.fbc(0x290, 0xFFFFFF);
        r.fbc(0x294, 0xFF);
        r.fbc(0x300, 0x09);                       // fast fill
        r.fbc(0x208, 0x00336699);
        r.fbc(0x60, 3), r.fbc(0x68, 0), r.fbc(0x6C, 0), r.fbc(0x70, 4), r.fbc(0x74, 0x40);  // the colour block
        // 40 x 6 from (24, 10): x 8 past a 16-pixel block, 0 past an 8-pixel one.
        r.fbc(0x60, 2), r.fbc(0x68, 10), r.fbc(0x6C, 24), r.fbc(0x70, 6 + (10 & 3));
        r.fbc(0x64, 40 + (24 & (high ? 15 : 7)));
        CHECK_EQ(r.pixel(24, 10), 0x00336699u);
        CHECK_EQ(r.pixel(63, 15), 0x00336699u);
        CHECK_EQ(r.pixel(64, 10), 0u);
        CHECK_EQ(r.pixel(24, 16), 0u);
    }
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

TEST(ffb_high_resolutions_count_four_pixels_horizontally) {
    // 1600 x 1200 at 75 Hz as the FCode sets it: 8/2:1 pixels, a 202.5 MHz
    // clock (13.5 x 60 / 4), 2160 x 1250 a frame (VESA's).
    Rig r;
    r.dac(0x0000, 0x4000 | 4 << 7 | 60);
    r.dac(0x1000, 3);
    r.wr(kDac, 0x6001);
    for (u32 v : {48u, 1248u, 2u, 1249u, 491u, 123u, 523u, 47u, 539u, 519u, 119u, 0u, 0u, 0u}) r.wr(kDac + 4, v);
    r.dac(0x6000, 3);
    Display::Frame frame;
    CHECK(r.f.render(frame));
    CHECK_EQ(frame.width, 1600u);
    CHECK_EQ(frame.height, 1200u);
    const double line = 100e6 * 2160 / 202.5e6;
    r.now = static_cast<u64>(line * (1250 + 10.5));
    CHECK_EQ(r.dac(0x600F), 10u);
    // The same timing 4/2:1 is half as wide.
    r.dac(0x1000, 2);
    CHECK(r.f.render(frame));
    CHECK_EQ(frame.width, 800u);
}

TEST(ffb_interlaced_modes_count_half_lines) {
    // 640 x 480 NTSC as the FCode sets it: interlaced, a 12.27 MHz clock
    // (13.5 x 80 / 11 / 8), 390 pixels a half-line, 525 half-lines a field.
    Rig r;
    r.dac(0x0000, 0x5DD0);
    r.dac(0x1000, 2);
    r.wr(kDac, 0x6001);
    for (u32 v : {37u, 517u, 5u, 524u, 165u, 59u, 184u, 28u, 194u, 180u, 55u, 13u, 11u, 518u}) r.wr(kDac + 4, v);
    r.dac(0x6000, 0x63);
    Display::Frame frame;
    CHECK(r.f.render(frame));
    CHECK_EQ(frame.width, 640u);
    CHECK_EQ(frame.height, 480u);
    // Fields at NTSC's rate: the vertical count back to 0 after 525 half-lines.
    const double half_line = 100e6 * 390 / (13.5e6 * 80 / 11 / 8);
    r.now = static_cast<u64>(half_line * (525 + 3.5));
    CHECK_EQ(r.dac(0x600F), 3u);
}

namespace {

// The card's side of an I2C read of the monitor's EDID, through the DAC's
// monitor port (SCL bit 0, SDA bit 1).
struct Ddc {
    Rig& r;
    u32 lines = 3;
    void set(u32 v) { lines = v, r.dac(0x8001, v); }
    void scl(bool on) { set(on ? lines | 1 : lines & ~1u); }
    void sda(bool on) { set(on ? lines | 2 : lines & ~2u); }
    bool sda_in() { return r.dac(0x8002) & 2; }
    void start() { sda(true), scl(true), sda(false), scl(false); }
    void stop() { sda(false), scl(true), sda(true); }
    bool out(u8 b) {
        for (int i = 7; i >= 0; --i) sda(b >> i & 1), scl(true), scl(false);
        sda(true), scl(true);
        const bool ack = !sda_in();
        scl(false);
        return ack;
    }
    u8 in(bool last) {
        u8 b = 0;
        sda(true);
        for (int i = 0; i < 8; ++i) {
            scl(true);
            b = static_cast<u8>(b << 1 | (sda_in() ? 1 : 0));
            scl(false);
        }
        sda(last), scl(true), scl(false);
        return b;
    }
};

}  // namespace

TEST(ffb_monitor_answers_ddc_with_its_edid) {
    Rig r;
    Ddc d{r};
    d.start();
    CHECK(!d.out(0xA0));  // no monitor on DDC: nothing acknowledges
    d.stop();
    const MonitorTiming* t = find_timing(Ffb::resolutions(), "1280x1024");
    CHECK(t != nullptr);
    r.f.monitor().set_edid(monitor_edid(Ffb::resolutions(), *t));
    d.start();
    CHECK(d.out(0xA0));
    CHECK(d.out(0x00));
    d.stop();
    d.start();
    CHECK(d.out(0xA1));
    std::vector<u8> e;
    for (int i = 0; i < 128; ++i) e.push_back(d.in(i == 127));
    d.stop();
    CHECK(e == r.f.monitor().edid());
    u8 sum = 0;
    for (u8 b : e) sum = static_cast<u8>(sum + b);
    CHECK_EQ(sum, 0u);
    CHECK_EQ(e[0], 0u);
    CHECK_EQ(e[1], 0xFFu);
    // The preferred timing first: 1280 x 1024 at 108 MHz; its standard
    // timing first too (1280 / 8 - 31, 5:4, 60 Hz).
    CHECK_EQ(e[54] | e[55] << 8, 10800u);
    CHECK_EQ(e[56] | (e[58] >> 4) << 8, 1280u);
    CHECK_EQ(e[38], 1280u / 8 - 31);
    CHECK_EQ(e[39], 2u << 6 | 0);
}

TEST(ffb_monitor_sense) {
    Rig r;
    CHECK_EQ((r.rd(0x200010) >> 24) & 7, 7u);  // nothing sensed
    r.f.monitor().set_sense(5);
    CHECK_EQ((r.rd(0x200010) >> 24) & 7, 5u);
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


namespace {

// The picture's pixel at (x, 0) for each X value given to buffer A's
// pixel there, its colour red 0x30, green 0x20, blue 0x10.
std::vector<u32> shown(Rig& r, std::initializer_list<u32> xs) {
    unsigned x = 0;
    for (u32 v : xs) r.wr(kDfb32 + 4 * x++, v << 24 | 0x102030);
    Display::Frame frame;
    CHECK(r.f.render(frame));
    return {frame.pixels.begin(), frame.pixels.begin() + x};
}

}  // namespace

TEST(ffb_separate_4_splits_the_x_planes_into_overlay_and_window_id) {
    Rig r;
    r.mode_1152x900();
    // Separate_4 with the overlay; two primary and two overlay window IDs;
    // overlay 15 transparent.
    r.dac(0x1001, 0x18);
    r.dac(0x3153, 0x303);
    r.dac(0x3151, 0x0F);
    r.dac(0x3152, 0x0F);
    r.dac(0x3240, 0x4000);         // primary 0: true colour from A
    r.dac(0x3241, 0x1000 | 0x100); // primary 1: palette 0 from red
    r.dac(0x3120, 0x400 | 0x100 | 0x10 | 2);  // overlay 0: transparent, palette 1 section 2
    r.dac(0x3121, 0x800);                     // overlay 1: opaque, no lookup
    r.dac(0x2000 + 0x30, 0x0000AA);
    r.dac(0x2000 + 256 + 2 * 16 + 3, 0x00FF00);
    const auto p = shown(r, {0x0F, 0x03, 0x1F, 0x45, 0x4F});
    CHECK_EQ(p[0], 0x302010u);  // window 0, overlay transparent: true colour
    CHECK_EQ(p[1], 0x00FF00u);  // overlay 3 from palette 1's section 2
    CHECK_EQ(p[2], 0xAA0000u);  // window 1 (X[5:4]): palette 0 from red
    CHECK_EQ(p[3], 0x050505u);  // overlay window 1 (X[7:6]) opaque: grey 5
    CHECK_EQ(p[4], 0x0F0F0Fu);  // opaque whatever the key
    // Without the overlay enable, the window ID alone.
    r.dac(0x1001, 0x10);
    const auto q = shown(r, {0x03, 0x13});
    CHECK_EQ(q[0], 0x302010u);
    CHECK_EQ(q[1], 0xAA0000u);
    // The primary mask: none of the window ID's bits leaves entry 0.
    r.dac(0x3153, 0x300);
    CHECK_EQ(shown(r, {0x13})[0], 0x302010u);
}

TEST(ffb_combined_mode_shows_the_overlay_through_its_entry) {
    Rig r;
    r.mode_1152x900();
    // Combined with the overlay, 32 window IDs at X 0xE0-0xFF, as a
    // single-buffered card is set up.
    r.dac(0x1001, 0x08);
    r.dac(0x3153, 0x1F);
    r.dac(0x3151, 0xE0);
    r.dac(0x3152, 0xFF);
    r.dac(0x3120, 0x500);  // overlay 0: transparent, palette 0
    CHECK_EQ(r.dac(0x3120), 0x500u);
    r.dac(0x3245, 0x1000 | 0x100 | 0x10);  // window ID 5: palette 1 from red
    r.dac(0x2000 + 0x10, 0x112233);
    r.dac(0x2000 + 256 + 0x30, 0x0000CC);
    const auto p = shown(r, {0xFF, 0x10, 0xE5});
    CHECK_EQ(p[0], 0x302010u);  // window ID 31: true colour
    CHECK_EQ(p[1], 0x332211u);  // an overlay pixel through palette 0
    CHECK_EQ(p[2], 0xCC0000u);  // window ID 5
    // The overlay entry's type none, or the overlay disabled: window IDs only.
    r.dac(0x3120, 0x100);
    CHECK_EQ(shown(r, {0x10})[0], 0x302010u);
    r.dac(0x3120, 0x900);  // opaque: even the key
    r.dac(0x2000 + 0xFF, 0x445566);
    CHECK_EQ(shown(r, {0xFF})[0], 0x665544u);
    r.dac(0x1001, 0x00);
    CHECK_EQ(shown(r, {0x10, 0xE5})[1], 0xCC0000u);
}

TEST(ffb_window_address_mask_zero_uses_entry_0) {
    Rig r;
    r.mode_1152x900();
    r.dac(0x1001, 0x00);
    r.dac(0x3153, 0x00);
    r.dac(0x3261, 0x100);  // window ID 0x21: palette 0 from X
    CHECK_EQ(shown(r, {0x21})[0], 0x302010u);
    r.dac(0x3153, 0x3F);
    CHECK_EQ(shown(r, {0x21})[0], 0u);
}

TEST(ffb_window_transfer_moves_both_tables_at_the_blank) {
    Rig r;
    r.mode_1152x900();
    r.dac(0x1001, 0x08);
    r.dac(0x3153, 0x1F);
    r.dac(0x3151, 0xFF);
    r.dac(0x3152, 0xFF);
    // Shadow entries: not shown, nor read back from the active tables.
    r.dac(0x3100, 0x900);  // overlay 0 opaque, palette 0
    r.dac(0x3201, 0x100);  // window ID 1: palette 0 from X
    CHECK_EQ(r.dac(0x3100), 0x900u);
    CHECK_EQ(r.dac(0x3120), 0u);
    CHECK_EQ(r.dac(0x3241), 0x4000u);
    r.dac(0x2001, 0x0000EE);
    CHECK_EQ(shown(r, {0x01})[0], 0x302010u);
    // The transfer: busy till the blank; the command bit reads 0, the
    // others as written.
    r.dac(0x3150, 0x12);
    CHECK_EQ(r.dac(0x3150), 0x11u);
    r.next_event();
    CHECK_EQ(r.dac(0x3150), 0x10u);
    CHECK_EQ(r.dac(0x3120), 0x900u);
    CHECK_EQ(r.dac(0x3241), 0x100u);
    CHECK_EQ(shown(r, {0x01})[0], 0xEE0000u);  // the overlay, opaque
    // With the timing generator off a transfer is made at once.
    r.dac(0x6000, 0);
    r.dac(0x3101, 0x400);
    r.dac(0x3150, 2);
    CHECK_EQ(r.dac(0x3150), 0u);
    CHECK_EQ(r.dac(0x3121), 0x400u);
    CHECK_EQ(r.sched.next_event(), Scheduler::kNever);
}

TEST(ffb_interlaced_window_transfer_at_the_next_frame_or_field) {
    // NTSC, interlaced: fields 525 half-lines; the blank at VBAP (517).
    Rig r;
    r.dac(0x0000, 0x5DD0);
    r.dac(0x1000, 2);
    r.wr(kDac, 0x6001);
    for (u32 v : {37u, 517u, 5u, 524u, 165u, 59u, 184u, 28u, 194u, 180u, 55u, 13u, 11u, 518u}) r.wr(kDac + 4, v);
    r.dac(0x6000, 0x63);
    // In the frame's first field: TE waits for the second's blank.
    r.dac(0x3150, 6);
    r.next_event();
    CHECK_EQ(r.dac(0x3150) & 1, 1u);
    r.next_event();
    CHECK_EQ(r.dac(0x3150), 4u);
    // Without TE, the next field's.
    r.now += 1000;
    r.dac(0x3150, 2);
    r.next_event();
    CHECK_EQ(r.dac(0x3150), 0u);
}

TEST(ffb_retrace_interrupt_follows_the_timing_generator) {
    Rig r;
    unsigned pulses = 0;
    r.f.set_interrupt_sink([&] { ++pulses; });
    r.mode_1152x900();
    // Pending is set only by the card; the enables as written.
    r.fbc(0x10900, 0xE);
    CHECK_EQ(r.rd(kFbc + 0x10900), 0xAu);
    // The timing generator off: no blank, no interrupt.
    r.dac(0x6000, 1);
    CHECK_EQ(r.sched.next_event(), Scheduler::kNever);
    // On again: the next blank raises it, once.
    r.dac(0x6000, 3);
    r.next_event();
    CHECK_EQ(pulses, 1u);
    CHECK_EQ(r.rd(kFbc + 0x10900), 0xEu);
    // Acknowledged with 5: pending cleared, the retrace's enable too.
    r.fbc(0x10900, 5);
    CHECK_EQ(r.rd(kFbc + 0x10900), 0u);
    CHECK_EQ(r.sched.next_event(), Scheduler::kNever);
}

// --- The raster engine: picking, the vertex interface, the pixel tests ---

namespace {

constexpr u32 kZOne = 0x40000000;  // 1.0 for a vertex's colour, alpha and z

// A pixel coordinate as the vertex registers take it (16.16).
u32 fixed(double v) { return static_cast<u32>(static_cast<s32>(std::lround(v * 65536))); }

// A vertex by its x register (0x24 strip, 0x34 restart, 0x44 fan), y in the
// register before it.
void vertex(Rig& r, u32 xreg, double x, double y) {
    r.fbc(xreg - 4, fixed(y));
    r.fbc(xreg, fixed(x));
}

// Buffer A written, every colour group and X on, raster operation new;
// colour, X and Y constant, Z from the vertices, no clip, pattern, blend
// or depth cue.
void plain_3d(Rig& r) {
    r.fbc(0x254, 0x200875AA);
    r.fbc(0x258, 0x00838383);
    r.fbc(0x290, 0xFFFFFF);
    r.fbc(0x294, 0xFF);
    r.fbc(0x200, 0x869ABF);
}

// Buffer C's word at (x, y), through the smart 32-bit view with C read.
u32 c_word(Rig& r, unsigned x, unsigned y) {
    r.fbc(0x254, 0xC000);
    const u32 v = static_cast<u32>(r.rd(kSfb32 + (u64{y} * Ffb::kLine + x) * 4));
    r.fbc(0x254, 0x4000);
    return v;
}

void dot(Rig& r, int x, int y) { r.fbc(0x300, 0x00), r.fbc(0x70, static_cast<u32>(y)), r.fbc(0x74, static_cast<u32>(x)); }

}  // namespace

TEST(ffb_pick_hits_when_a_pixel_passes_with_picking_enabled) {
    Rig r;
    plain_3d(r);
    r.fbc(0x208, 0x00112233);
    auto pick = [&] { return static_cast<u32>(r.rd(kFbc + 0x900) >> 16) & 0xF; };
    CHECK_EQ(pick(), 0xAu);  // disabled, no hit
    dot(r, 1, 1);
    CHECK_EQ(pick(), 0xAu);  // disabled: drawing leaves the status
    r.fbc(0x900, 0xC0000);
    CHECK_EQ(pick(), 0xEu);
    // A pixel the depth test turns away is no hit.
    r.fbc(0x25C, 0x00840000);  // buffer C magnitude: never
    dot(r, 2, 1);
    CHECK_EQ(pick(), 0xEu);
    CHECK_EQ(r.pixel(2, 1), 0u);
    // One that passes is, whether or not it is written (colour groups off).
    r.fbc(0x25C, 0x00800000);
    r.fbc(0x254, 0x00000055);
    dot(r, 3, 1);
    CHECK_EQ(pick(), 0xFu);
    CHECK_EQ(r.pixel(3, 1), 0u);
    CHECK_EQ(r.rd(kFbc + 0x900) & 0x03000000, 0u);  // never busy
    // Sticky until written: 10 no hit; 00 leaves both fields.
    r.fbc(0x900, 0);
    CHECK_EQ(pick(), 0xFu);
    r.fbc(0x900, 0x20000);
    CHECK_EQ(pick(), 0xEu);
    // 11 sets hit (a saved status restored); disabling keeps the status.
    r.fbc(0x900, 0x30000);
    r.fbc(0x900, 0x80000);
    CHECK_EQ(pick(), 0xBu);
    r.fbc(0x900, 0xA0000);
    dot(r, 4, 1);
    CHECK_EQ(pick(), 0xAu);
}

TEST(ffb_triangle_covers_the_samples_inside_by_the_top_left_rule) {
    for (const bool reversed : {false, true}) {
        Rig r;
        plain_3d(r);
        r.fbc(0x208, 0x00445566);
        r.fbc(0x300, 0x06);
        vertex(r, 0x34, 10, 10);
        vertex(r, 0x24, reversed ? 10 : 20, reversed ? 20 : 10);
        CHECK_EQ(r.pixel(10, 10), 0u);  // nothing before the third
        vertex(r, 0x24, reversed ? 20 : 10, reversed ? 10 : 20);
        CHECK_EQ(r.pixel(10, 10), 0x00445566u);
        CHECK_EQ(r.pixel(19, 10), 0x00445566u);
        CHECK_EQ(r.pixel(20, 10), 0u);  // right
        CHECK_EQ(r.pixel(14, 15), 0x00445566u);
        CHECK_EQ(r.pixel(15, 15), 0u);  // on the right edge
        CHECK_EQ(r.pixel(10, 19), 0x00445566u);
        CHECK_EQ(r.pixel(10, 20), 0u);  // bottom
        CHECK_EQ(r.pixel(9, 12), 0u);
    }
    // Two triangles sharing an edge (a strip making a square), drawn with
    // xor: each pixel drawn once.
    Rig r;
    plain_3d(r);
    r.fbc(0x258, 0x00868686);
    r.fbc(0x208, 0x000000FF);
    r.fbc(0x300, 0x06);
    vertex(r, 0x34, 30, 0), vertex(r, 0x24, 40, 0), vertex(r, 0x24, 30, 10), vertex(r, 0x24, 40, 10);
    unsigned drawn = 0;
    for (unsigned y = 0; y < 12; ++y)
        for (unsigned x = 28; x < 42; ++x) drawn += r.pixel(x, y) == 0xFFu;
    CHECK_EQ(drawn, 100u);
    CHECK_EQ(r.pixel(35, 5), 0xFFu);
    // A triangle of no area draws nothing.
    vertex(r, 0x34, 50, 0), vertex(r, 0x24, 60, 0), vertex(r, 0x24, 70, 0);
    CHECK_EQ(r.pixel(55, 0), 0u);
}

TEST(ffb_triangle_strips_and_fans) {
    // The square (0,0) (10,0) (10,10) (0,10): as a fan the second triangle
    // is (v0, v2, v3), as a strip (v1, v2, v3).
    for (const bool fan : {true, false}) {
        Rig r;
        plain_3d(r);
        r.fbc(0x208, 0x00000011);
        r.fbc(0x300, 0x06);
        const u32 next = fan ? 0x44 : 0x24;
        vertex(r, 0x34, 0, 0), vertex(r, next, 10, 0), vertex(r, next, 10, 10);
        CHECK_EQ(r.pixel(8, 2), 0x11u);
        // The colour (flat: the foreground) as it is when the vertex completes.
        r.fbc(0x208, 0x00000022);
        vertex(r, next, 0, 10);
        CHECK_EQ(r.pixel(1, 8), fan ? 0x22u : 0u);
        CHECK_EQ(r.pixel(8, 2), fan ? 0x11u : 0x22u);
        CHECK_EQ(r.pixel(8, 9), fan ? 0x22u : 0x22u);
        // A restart draws nothing and starts again.
        r.fbc(0x208, 0x00000033);
        vertex(r, 0x34, 100, 100), vertex(r, next, 110, 100);
        CHECK_EQ(r.pixel(101, 100), 0u);
        vertex(r, next, 100, 110);
        CHECK_EQ(r.pixel(101, 101), 0x33u);
    }
}

TEST(ffb_triangle_interpolates_colour_and_depth) {
    Rig r;
    plain_3d(r);
    r.fbc(0x200, 0x0000AA);  // colour and Z from the vertices
    r.fbc(0x254, 0xA0000800);  // A and C written, Z on
    r.fbc(0x29C, 0x0FFFFFFF);
    r.fbc(0x300, 0x06);
    r.fbc(0x10, kZOne), r.fbc(0x14, 0), r.fbc(0x18, 0), r.fbc(0x1C, kZOne / 2);
    vertex(r, 0x34, 10, 10);
    r.fbc(0x10, 0), r.fbc(0x1C, 0);
    vertex(r, 0x24, 20, 10);
    vertex(r, 0x24, 10, 20);
    CHECK_EQ(r.pixel(10, 10), 0x000000FFu);  // red 1.0: 255
    CHECK_EQ(r.pixel(15, 10), 0x00000080u);  // half way
    CHECK_EQ(c_word(r, 10, 10), 0x08000000u);  // z 0.5: bits 29:2
    CHECK_EQ(c_word(r, 15, 10), 0x04000000u);
}

TEST(ffb_ddline_draws_from_the_first_pixel_to_before_the_last) {
    Rig r;
    plain_3d(r);
    r.fbc(0x208, 0x000000FF);
    r.fbc(0x300, 0x04);
    vertex(r, 0x34, 10, 10);
    CHECK_EQ(r.pixel(10, 10), 0u);  // a restart draws nothing
    vertex(r, 0x24, 20, 10);
    CHECK_EQ(r.pixel(10, 10), 0xFFu);
    CHECK_EQ(r.pixel(19, 10), 0xFFu);
    CHECK_EQ(r.pixel(20, 10), 0u);
    // A strip on from there, steep: y-major, the joint drawn once (xor).
    r.fbc(0x258, 0x00868686);
    vertex(r, 0x24, 22, 20);
    CHECK_EQ(r.pixel(20, 10), 0xFFu);
    CHECK_EQ(r.pixel(21, 15), 0xFFu);  // x 21.0 at y 15
    CHECK_EQ(r.pixel(22, 19), 0xFFu);
    CHECK_EQ(r.pixel(22, 20), 0u);
}

TEST(ffb_ddline_pattern_runs_on_from_line_to_line) {
    Rig r;
    plain_3d(r);
    r.fbc(0x208, 0x000000FF);
    r.fbc(0x300, 0x04);
    r.fbc(0x30C, 0x00040003);  // pattern 0011, length 4
    vertex(r, 0x34, 0, 50), vertex(r, 0x24, 3, 50);
    CHECK_EQ(r.pixel(0, 50), 0xFFu);
    CHECK_EQ(r.pixel(1, 50), 0xFFu);
    CHECK_EQ(r.pixel(2, 50), 0u);
    CHECK_EQ((r.rd(kFbc + 0x30C) >> 24) & 0xF, 3u);
    vertex(r, 0x24, 6, 50);
    CHECK_EQ(r.pixel(3, 50), 0u);
    CHECK_EQ(r.pixel(4, 50), 0xFFu);
    CHECK_EQ(r.pixel(5, 50), 0xFFu);
    // Only writing lpat starts it again.
    r.fbc(0x30C, 0x00040003);
    vertex(r, 0x34, 0, 60), vertex(r, 0x24, 2, 60);
    CHECK_EQ(r.pixel(1, 60), 0xFFu);
}

TEST(ffb_antialiased_lines_and_dots_weight_the_alpha) {
    Rig r;
    plain_3d(r);
    r.fbc(0x200, 0x8B);  // X from the vertex alpha
    r.fbc(0x208, 0x00FFFFFF);
    r.fbc(0x0C, kZOne);  // alpha 1.0
    r.fbc(0x300, 0x05);
    vertex(r, 0x34, 0, 10.25), vertex(r, 0x24, 4, 10.25);
    CHECK_EQ(r.pixel(1, 10) >> 24, 191u);  // 0.75 of 255
    CHECK_EQ(r.pixel(1, 11) >> 24, 64u);   // 0.25
    CHECK_EQ(r.pixel(1, 12), 0u);
    CHECK_EQ(r.pixel(4, 10), 0u);
    r.fbc(0x300, 0x01);
    vertex(r, 0x24, 5.5, 20);
    CHECK_EQ(r.pixel(5, 20) >> 24, 128u);
    CHECK_EQ(r.pixel(6, 20) >> 24, 128u);
    CHECK_EQ(r.pixel(5, 21), 0u);
    // DOT from a vertex: the pixel holding it.
    r.fbc(0x300, 0x00);
    vertex(r, 0x24, 3.75, 30.5);
    CHECK_EQ(r.pixel(3, 30), 0xFFFFFFFFu);
}

TEST(ffb_compares_test_buffer_c_and_the_colour_buffers) {
    Rig r;
    plain_3d(r);
    r.fbc(0x254, 0xA0000800);  // A and C, Z on
    r.fbc(0x29C, 0x0FFFFFFF);
    r.fbc(0x200, 0xC0);        // Z constant
    r.fbc(0x26C, 0x0FFFFFFF);
    auto at = [&](u32 z, u32 c) { r.fbc(0x214, z), r.fbc(0x208, c), dot(r, 5, 5); };
    r.fbc(0x25C, 0x00800000);  // always
    at(0x8000000, 0x11);
    r.fbc(0x25C, 0x00870000);  // the depth test: less
    at(0xC000000, 0x22);
    CHECK_EQ(r.pixel(5, 5), 0x11u);
    CHECK_EQ(c_word(r, 5, 5), 0x08000000u);
    at(0x4000000, 0x33);
    CHECK_EQ(r.pixel(5, 5), 0x33u);
    CHECK_EQ(c_word(r, 5, 5), 0x04000000u);
    // The match test on the stencil planes: equal to the constant Y.
    r.fbc(0x25C, 0x82800000);
    r.fbc(0x264, 0xF0000000);
    r.fbc(0x210, 3);
    r.fbc(0x200, 0x30);
    at(0x1000, 0x44);
    CHECK_EQ(r.pixel(5, 5), 0x33u);  // stencil 0, not 3
    r.fbc(0x210, 0);
    at(0x1000, 0x55);
    CHECK_EQ(r.pixel(5, 5), 0x55u);
    // The AB match test on the X planes against the current window ID.
    r.fbc(0x25C, 0x80808200);
    r.fbc(0x260, 0xFF000000);
    r.fbc(0x254, 0x20000000);
    r.wr(kDfb32 + (u64{5} * Ffb::kLine + 7) * 4, 0x05000000);
    r.fbc(0x200, 0x4);  // X from the window ID
    r.fbc(0x204, 6);
    r.fbc(0x208, 0x77);
    dot(r, 7, 5);
    CHECK_EQ(r.pixel(7, 5), 0x05000000u);
    r.fbc(0x204, 5);
    dot(r, 7, 5);
    CHECK_EQ(r.pixel(7, 5), 0x05000077u);
    // Bit 6 passes it for buffer A whatever.
    r.fbc(0x25C, 0xC300);  // A always, not equal
    r.fbc(0x208, 0x88);
    dot(r, 7, 5);
    CHECK_EQ(r.pixel(7, 5), 0x05000088u);
    // The AB magnitude test on red: greater.
    r.fbc(0x25C, 0x8081);
    r.fbc(0x268, 0xFF);
    r.fbc(0x208, 0x80);
    dot(r, 7, 5);
    CHECK_EQ(r.pixel(7, 5), 0x05000088u);
    r.fbc(0x208, 0x90);
    dot(r, 7, 5);
    CHECK_EQ(r.pixel(7, 5), 0x05000090u);
}

TEST(ffb_x_clip_tests_the_x_source_value) {
    Rig r;
    plain_3d(r);
    r.fbc(0x218, 0x70080);  // less than 0x80
    r.fbc(0x208, 0x40000011);
    dot(r, 1, 1);
    r.fbc(0x208, 0xC0000022);
    dot(r, 2, 1);
    CHECK_EQ(r.pixel(1, 1), 0x40000011u);
    CHECK_EQ(r.pixel(2, 1), 0u);
    r.fbc(0x218, 0x40000);  // never
    dot(r, 1, 1);
    r.fbc(0x218, 0x10080);  // greater
    dot(r, 2, 1);
    CHECK_EQ(r.pixel(1, 1), 0x40000011u);
    CHECK_EQ(r.pixel(2, 1), 0xC0000022u);
}

TEST(ffb_view_clip_3d_bounds_the_depth) {
    Rig r;
    plain_3d(r);
    r.fbc(0x220, 0), r.fbc(0x224, 100 << 16 | 100);
    r.fbc(0x228, 0x100), r.fbc(0x22C, 0x200);
    r.fbc(0x200, 0x30C0);  // view clip 3D, Z constant
    r.fbc(0x208, 0x11);
    for (u32 z : {0xFFu, 0x100u, 0x200u, 0x201u}) {
        r.fbc(0x214, z);
        dot(r, static_cast<int>(z & 0xF), 1);
    }
    CHECK_EQ(r.pixel(0xF, 1), 0u);
    CHECK_EQ(r.pixel(0, 1), 0x11u);   // 0x100 and 0x200: inside, inclusive
    CHECK_EQ(r.pixel(1, 1), 0u);      // 0x201
    // In 2D the depth is not bounded.
    r.fbc(0x200, 0x2000);
    r.fbc(0x214, 0x201);
    dot(r, 1, 1);
    CHECK_EQ(r.pixel(1, 1), 0x11u);
}

TEST(ffb_depth_cue_fades_toward_the_blend_colours) {
    Rig r;
    plain_3d(r);
    r.fbc(0x200, 0x300C0);  // depth cue on, Z constant
    // Front 0 at scale 1.0, back 0x0FFFFFFF at 0: slope 1.0 (M 256, E 0);
    // toward 100 in each channel (BLENDC1 -100, BLENDC2 +100).
    r.fbc(0x238, 0), r.fbc(0x23C, 0x0FFFFFFF);
    r.fbc(0x230, 256), r.fbc(0x234, 0);
    r.fbc(0x21C, 256u << 22);
    const u32 neg = 0x200 - 100;
    r.fbc(0x248, neg | neg << 9 | neg << 18);
    r.fbc(0x24C, 100 | 100 << 9 | 100 << 18);
    r.fbc(0x208, 0x00C800C8);
    auto at = [&](u32 z, int x) { r.fbc(0x214, z), dot(r, x, 0); };
    at(0, 0), at(0x8000000, 1), at(0x0FFFFFFF, 2);
    CHECK_EQ(r.pixel(0, 0), 0x00C800C8u);  // at the front: as it is
    CHECK_EQ(r.pixel(1, 0), 0x00963296u);  // half: (c + 100) / 2
    CHECK_EQ(r.pixel(2, 0), 0x00646464u);  // at the back: the colour
    // A further breakpoint (FFB2+): from 0x8000000 at scale 64, flat.
    r.fbc(0x370, 0x8000000), r.fbc(0x360, 64), r.fbc(0x350, 0);
    at(0xC000000, 3);
    CHECK_EQ(r.pixel(3, 0) & 0xFF, 125u);  // (200 - 100) * 64 / 256 + 100
    // Off, the blend colours do nothing.
    r.fbc(0x200, 0x20000);
    at(0xC000000, 4);
    CHECK_EQ(r.pixel(4, 0), 0x00C800C8u);
}

TEST(ffb_buffer_c_written_through_its_own_enables) {
    Rig r;
    plain_3d(r);
    r.fbc(0x200, 0xF0);         // Y and Z constant
    r.fbc(0x214, 0x01234567);
    r.fbc(0x210, 5);
    r.fbc(0x29C, 0x0FFFFFFF);
    r.fbc(0x298, 0xF);
    r.fbc(0x208, 0x00ABCDEF);
    r.fbc(0x254, 0x80000600);   // C alone; Z off, Y on (the colour groups still on)
    dot(r, 1, 1);
    CHECK_EQ(c_word(r, 1, 1), 0x50000000u);
    CHECK_EQ(r.pixel(1, 1), 0u);
    r.fbc(0x254, 0x00000900);   // Z on, Y off
    dot(r, 2, 1);
    CHECK_EQ(c_word(r, 2, 1), 0x01234567u);
    // On through ZPMASK.
    r.fbc(0x254, 0x00000C00);
    r.fbc(0x29C, 0x0000FFFF);
    r.fbc(0x214, 0x0FFFFFFF);
    dot(r, 2, 1);
    CHECK_EQ(c_word(r, 2, 1), 0x0123FFFFu);
    // The colour and X groups off do not stop it; the raster operation's
    // Y/Z field applies (xor).
    r.fbc(0x254, 0x00000855);
    r.fbc(0x258, 0x00860000);
    r.fbc(0x214, 0x0000000F);
    dot(r, 2, 1);
    CHECK_EQ(c_word(r, 2, 1), 0x0123FFF0u);
    // Written through the smart 32-bit view: the value's Z (Z source VAR).
    r.fbc(0x258, 0x00830000);
    r.fbc(0x200, 0x80);
    r.wr(kSfb32 + (u64{1} * Ffb::kLine + 3) * 4, 0xF7654321);
    CHECK_EQ(c_word(r, 3, 1), 0x07654321u);
}

TEST(ffb_rop_fields_change_only_with_their_edit_bit) {
    Rig r;
    // Reset: new in every group.
    CHECK_EQ(r.rd(kFbc + 0x258), 0x00838383u);
    r.fbc(0x254, 0x200875AA);
    r.fbc(0x290, 0xFFFFFF);
    r.fbc(0x294, 0xFF);
    r.fbc(0x200, 0x869BAF);
    r.fbc(0x208, 0x12345678);
    dot(r, 0, 0);
    CHECK_EQ(r.pixel(0, 0), 0x12345678u);
    // A field written with its edit bit clear is left; with it set, taken
    // (0x80: clear).
    r.fbc(0x258, 0x00000086);
    CHECK_EQ(r.rd(kFbc + 0x258), 0x00838386u);
    r.fbc(0x258, 0x00008000);
    CHECK_EQ(r.rd(kFbc + 0x258), 0x00838086u);
    r.fbc(0x208, 0xFF0000FF);
    dot(r, 0, 0);
    CHECK_EQ(r.pixel(0, 0), 0x00345687u);
}

TEST(ffb_setup_state_saved_and_restored) {
    Rig r;
    plain_3d(r);
    r.fbc(0x208, 0x00000044);
    r.fbc(0x300, 0x06);
    vertex(r, 0x34, 0, 0), vertex(r, 0x24, 10, 0);
    std::vector<u32> saved;
    for (u32 a = 0x100; a < 0x180; a += 4) saved.push_back(static_cast<u32>(r.rd(kFbc + a)));
    vertex(r, 0x34, 500, 500);
    for (u32 i = 0; i < saved.size(); ++i) r.fbc(0x100 + 4 * i, saved[i]);
    vertex(r, 0x24, 0, 10);
    CHECK_EQ(r.pixel(2, 2), 0x44u);
}

TEST(ffb_sfb64_writes_colour_and_buffer_c_as_one_pixel) {
    Rig r;
    plain_3d(r);
    r.fbc(0x200, 0x8A);  // colour and Z from the value
    r.fbc(0x254, 0xA0000800);
    r.fbc(0x29C, 0x0FFFFFFF);
    const u64 kSfb64 = 0x6000000;
    r.wr(kSfb64 + (u64{2} * Ffb::kLine + 4) * 8, 0x00AABBCC00000123ull, 8);
    CHECK_EQ(r.pixel(4, 2), 0x00AABBCCu);
    CHECK_EQ(c_word(r, 4, 2), 0x123u);
    CHECK_EQ(r.rd(kSfb64 + (u64{2} * Ffb::kLine + 4) * 8, 8), 0x00AABBCC00000123ull);
    // Compared on the Z it carries: the depth test (less) turns this away.
    r.fbc(0x25C, 0x00870000);
    r.fbc(0x26C, 0x0FFFFFFF);
    r.wr(kSfb64 + (u64{2} * Ffb::kLine + 4) * 8, 0x0011111100000200ull, 8);
    CHECK_EQ(r.pixel(4, 2), 0x00AABBCCu);
}

TEST(ffb_video_views_convert_4_2_2) {
    Rig r;
    plain_3d(r);
    // Y0 235 (white), Cb 128, Y1 16 (black), Cr 128 at pixels 0 and 1 of line 1.
    r.wr(kDfb32 + (u64{1} * Ffb::kLine) * 4, 0x7F000000);
    r.wr(0x9000000 + 1 * 4096, 0xEB801080);
    CHECK_EQ(r.pixel(0, 1), 0x7FFFFFFFu);  // the X planes kept
    CHECK_EQ(r.pixel(1, 1), 0u);
    CHECK_EQ(r.rd(0x9000000 + 1 * 4096), 0u);
    // Line doubled: line 1 to lines 2 and 3 of buffer B.
    r.wr(0xB800000 + 1 * 4096 + 4, 0xEB80EB80);
    CHECK_EQ(r.rd(kDfb24B + (u64{2} * Ffb::kLine + 2) * 4) & 0xFFFFFF, 0xFFFFFFu);
    CHECK_EQ(r.rd(kDfb24B + (u64{3} * Ffb::kLine + 3) * 4) & 0xFFFFFF, 0xFFFFFFu);
    // Through the pixel processor (colour from the value): xor, twice.
    r.fbc(0x200, 0x2);
    r.fbc(0x258, 0x00838386);
    r.wr(0xD000000 + 6 * 4096, 0xEB80EB80);
    CHECK_EQ(r.pixel(1, 6), 0xFFFFFFu);
    r.wr(0xD000000 + 6 * 4096, 0xEB80EB80);
    CHECK_EQ(r.pixel(1, 6), 0u);
}

TEST(ffb_z_views_reach_the_top_of_the_depth) {
    Rig r;
    plain_3d(r);
    r.fbc(0x200, 0xF0);
    r.fbc(0x210, 0xA), r.fbc(0x214, 0x0FFFFFFF);
    r.fbc(0x254, 0x80000A00);
    r.fbc(0x298, 0xF), r.fbc(0x29C, 0x0FFFFFFF);
    dot(r, 3, 0);
    r.wr(0xC000000 + 3 * 2, 0xABCD, 2);
    CHECK_EQ(c_word(r, 3, 0), 0xAABCD000u);  // Y kept, the bits below cleared
    CHECK_EQ(r.rd(0xC000000 + 3 * 2, 2), 0xABCDu);
    r.wr(0xC800000 + 3, 0x12, 1);
    CHECK_EQ(c_word(r, 3, 0), 0xA1200000u);
    CHECK_EQ(r.rd(0xC800000 + 3, 1), 0x12u);
}
