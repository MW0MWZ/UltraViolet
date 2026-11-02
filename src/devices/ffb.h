// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
// Sun Creator3D (FFB2+, SUNW,ffb, 501-4788: double-buffered), the Ultra
// 10's UPA graphics card, at UPA port 0x1E.
//
// Its UPA space, as the card's FCode maps it and the open drivers name it:
//   0x0000000  FCode PROM, a byte every 4 addresses
//   0x0200000  expansion: the board strapping (byte 0), monitor sense
//   0x0400000  the RAMDAC (BT498, "Pacifica 2"), registers reached
//              indirectly: configuration and palette, and the cursor
//   0x0600000  the FBC: the raster processor's registers; its own
//              registers (frame buffer configuration, interrupts) at +0x10000
//   0x1000000  the dumb frame buffer, a byte a pixel of R, G, B or X
//   0x2000000  the dumb frame buffer, 32 bits a pixel, the X plane kept
//   0x3000000  the dumb frame buffer, 32 bits a pixel (X, B, G, R)
//   0x4000000  the smart frame buffer, a byte a pixel of R, G, B or X
//   0x5000000  the smart frame buffer, 32 bits a pixel
// The frame buffer is 2048 pixels a line in every view. Pixels are 32 bits:
// the X plane (window ID and overlay) in the top byte, then blue, green
// and red. There are two colour buffers, A and B; the FBC's frame buffer
// control picks which drawing writes and reads. The dumb views reach the
// memory as it is; the smart ones (and drawing) go through the raster
// operation, the plane masks and the plane groups enabled.
//
// The DAC makes the picture: each pixel's window ID (its X plane) selects a
// window lookup entry, which says whether it is true colour or an index
// into one of the four palettes, and from which buffer. Its timing
// generator sets the mode; the vertical counter follows guest time at the
// pixel clock it is set to.
//
// Sources: the card's FCode (ffb2p.fth 2.9), which programs it at power-on
// and draws OpenBoot's console; the X and Linux drivers' register
// definitions; a real card's properties. No programming manual
// for the FBC or the DAC is public.
#pragma once

#include <algorithm>
#include <array>
#include <map>
#include <string>
#include <vector>

#include "core/scheduler.h"
#include "devices/display.h"
#include "mem/bus.h"

namespace uv {

class Ffb final : public Device, public Display {
public:
    static constexpr u64 kSpace = u64{1} << 28;  // the UPA space decoded (256 MiB)
    static constexpr unsigned kLine = 2048;     // pixels a frame buffer line
    static constexpr unsigned kLines = 2048;    // lines the 32-bit views reach

    // `fcode`: the card's FCode PROM image (as read, a byte each).
    Ffb(Scheduler& scheduler, double cpu_hz, std::vector<u8> fcode);
    const char* name() const override { return "ffb"; }
    void reset();

    bool read(u64 offset, unsigned size, u64& value) override;
    bool write(u64 offset, unsigned size, u64 value) override;

    // Display
    using Display::render;
    bool render(Frame& frame) const override;
    bool cursor_position(int& x, int& y) const override;
    u64 display_version() const override { return version_; }

    // A report of the DAC's state and the FBC's main registers (debugging).
    void describe(std::string& out) const;

private:
    // The DAC (dac.cpp's part of ffb.cpp): indirect registers.
    u32 dac_read(u32 address);
    void dac_write(u32 address, u32 value);
    u32 cursor_read(u32 address) const;
    void cursor_write(u32 address, u32 value);

    // The FBC's registers (words at `offset`, 0-0x11FFF).
    u32 fbc_read(u32 offset);
    void fbc_write(u32 offset, u32 value);

    // The frame buffer views (offset from 0x1000000).
    bool fb_read(u64 offset, unsigned size, u64& value);
    bool fb_write(u64 offset, unsigned size, u64 value);

    // Drawing (ffb_engine.cpp): the pixel processor, applied to a pixel
    // the raster processor or a smart view writes; and the operations.
    void draw_pixel(unsigned x, unsigned y, u32 colour, bool background = false);
    void smart_write(unsigned x, unsigned y, u32 value, u32 lanes);
    u32 read_pixel(unsigned x, unsigned y) const;
    void start_drawop(u32 reg);  // the operation's last register (bw or bx) written
    void line(int x0, int y0, int x1, int y1, bool last);
    void fast_fill(u32 reg);
    void rectangle(int x, int y, int w, int h);
    void vscroll(int sx, int sy, int dx, int dy, int w, int h);
    void font_row(u32 bits);
    bool clipped(int x, int y) const;
    void engine_unmodelled(unsigned what, const char* text, u32 detail = 0);

    // Video timing from the DAC's registers; the scan line at `now`.
    struct Timing {
        bool on = false;
        unsigned width = 0, height = 0;  // the active picture
        unsigned total = 0;              // lines a frame
        double line_cycles = 0;
    };
    Timing timing() const;
    unsigned current_line() const;

    // Rows [y0, y1) of frame buffer memory written; a change to what
    // every pixel is shown through (the DAC).
    void changed(unsigned y0, unsigned y1) {
        ++version_;
        for (unsigned y = y0; y < std::min(y1, kLines); ++y) row_version_[y] = version_;
    }
    void changed_all() { dac_version_ = ++version_; }

    Scheduler& sched_;
    double cpu_hz_;
    std::vector<u8> fcode_;

    // Frame buffer memory: buffers A and B (colour and X planes) and C
    // (Y and Z), each kLine x kLines pixels.
    std::array<std::vector<u32>, 3> buf_;
    // The buffers the frame buffer control writes (bit 0 A, 1 B, 2 C) and
    // the one it reads (0 A, 1 B, 2 C).
    unsigned write_buffers() const { return regs_[0x254 / 4] >> 29; }
    unsigned read_buffer() const {
        const unsigned rb = (regs_[0x254 / 4] >> 14) & 3;
        return rb ? rb - 1 : 0;
    }

    // FBC registers: the raster processor's (0-0xFFF) and its own
    // (0x10000-0x11FFF), as words.
    std::vector<u32> regs_, kregs_;
    u32 ucsr_ = 0;
    u32 engine_warned_ = 0;

    // DAC: configuration pointer and registers; the cursor's.
    u32 dac_ptr_ = 0, cur_ptr_ = 0;
    std::array<u32, 4 * 256> clut_{};
    std::array<u32, 64> pwlut_shadow_{}, pwlut_{};
    std::array<u32, 4> owlut_shadow_{}, owlut_{};
    u32 ppll_ = 0, gpll_ = 0, pfctrl_ = 0, uctrl_ = 0, wtctrl_ = 0, tmctrl_ = 0, tcolorkey_ = 0, wamask_ = 0;
    u32 sanal_ = 0, dacctrl_ = 0, tgen_ = 0, mpdata_ = 3;
    std::array<u32, 14> tregs_{};  // 0x6001-0x600E
    std::array<u32, 0x100> cursor_bits_{};
    u32 cursor_ctrl_ = 3, cursor_pos_ = 0;
    std::array<u32, 3> cursor_colour_{};

    u64 version_ = 1;
    std::vector<u64> row_version_;  // the version each row was last written at

    struct FrameState final : Frame::State {
        bool made = false, shown = false;
        u64 seen = 0;
        unsigned width = 0, height = 0;
        std::vector<u32> base;  // without the cursor
        int cx = 0, cy = 0;
        bool cursor = false;
    };
    std::map<std::pair<u32, u32>, unsigned> debug_ops_;  // (fbc, ppc) of drawing, for describe()
    u64 cursor_version_ = 0;
    u64 dac_version_ = 0;  // changes to what the picture is made through
};

}  // namespace uv
