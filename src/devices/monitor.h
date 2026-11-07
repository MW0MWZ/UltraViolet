// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
// The monitor on a display card's connector, as the card sees it: its
// sense lines, and its DDC lines (VESA DDC2B: an I2C bus with the
// monitor's EDID in a 24C02-style memory at address 0x50).
//
// The I2C lines are open drain: each reads low when either end pulls it
// low. The card drives SCL and SDA through a register and reads them back
// through another; the monitor never holds SCL low (no clock stretching)
// and pulls SDA low to acknowledge and to send a 0.
#pragma once

#include <string>
#include <vector>

#include "core/types.h"

namespace uv {

// A video timing as a monitor's EDID gives it: the mode's name (width,
// height, refresh rate), the pixel clock, and the blanking either side of
// each sync.
struct MonitorTiming {
    unsigned width, height, rate;
    unsigned pixel_khz;
    unsigned h_front, h_sync, h_back;
    unsigned v_front, v_sync, v_back;
    bool positive_sync;  // else negative
};

// An EDID 1.3 block for a monitor that takes these timings, the first its
// preferred: that one as its detailed timing, and each (the first again
// among them) as an established timing or standard timing where VESA's
// codes can say it; a name, and range limits that cover them all.
std::vector<u8> make_edid(const std::vector<MonitorTiming>& timings, const char* name);
// A size's timing in a card's list ("1280x1024"); null if it has none.
const MonitorTiming* find_timing(const std::vector<MonitorTiming>& list, const std::string& size);
// UltraViolet's monitor, which takes every timing in a card's list: its
// EDID, preferring `preferred`.
std::vector<u8> monitor_edid(const std::vector<MonitorTiming>& list, const MonitorTiming& preferred);

class Monitor {
public:
    // The sense code: the monitor's three sense lines (0-7); 7, nothing
    // sensed, by default.
    void set_sense(u32 code) { sense_ = code & 7; }
    u32 sense() const { return sense_; }

    // The EDID the monitor answers with (128 bytes a block); empty: no DDC
    // (nothing answers on the lines).
    void set_edid(std::vector<u8> edid) { edid_ = std::move(edid); }
    const std::vector<u8>& edid() const { return edid_; }

    // The lines as the card drives them (true: released); then what each
    // reads.
    void drive(bool scl, bool sda);
    bool scl() const { return scl_; }
    bool sda() const { return sda_ && !pull_sda_; }

    // The transfers seen, for debugging: "S" a start, "P" a stop, the
    // bytes received ("a0"), and each byte sent, with "-" when the card
    // did not acknowledge it.
    const std::string& trace() const { return trace_; }

private:
    void received(u8 byte);  // a byte clocked in (address or offset)
    void send_next();        // put the next byte's first bit on SDA
    void note(const char* text);

    enum class State { Idle, Address, Offset, Read };
    u32 sense_ = 7;
    std::vector<u8> edid_;
    State state_ = State::Idle;
    bool scl_ = true, sda_ = true;  // the card's
    bool pull_sda_ = false;          // the monitor's
    bool ack_ = false;               // in an acknowledge bit
    bool nak_ = false;               // the card did not acknowledge a byte read
    bool to_read_ = false;           // addressed to read
    unsigned bits_ = 0;              // bits of the byte so far
    u8 shift_ = 0;
    u8 offset_ = 0;
    std::string trace_;
};

}  // namespace uv
