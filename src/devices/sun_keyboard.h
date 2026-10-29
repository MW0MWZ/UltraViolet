// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
// Sun Type 5 keyboard and Sun (Mouse Systems) mouse, at the far end of the
// SuperIO's keyboard and mouse UARTs. Both run at 1200 baud, 8 data bits,
// no parity, one stop bit.
//
// The keyboard answers the host's commands (reset with its self-test
// response, layout, LEDs, bell and click) and sends a key number for each
// key pressed and 0x80 | the number when it is released, then 0x7F when no
// key is left down. The mouse sends five-byte packets: buttons (0x80 | the
// buttons up: left 4, middle 2, right 1), then two samples of movement, x
// to the right and y up.
#pragma once

#include <array>

#include "core/scheduler.h"
#include "devices/serial.h"

namespace uv {

class SunKeyboard {
public:
    static constexpr u8 kLayoutUs = 0x21, kLayoutUk = 0x2E;  // Type 5 layouts (the DIP switch value)

    SunKeyboard(Scheduler& scheduler, SerialLine& line, double cpu_hz);
    void reset();

    // A key number (1-0x7D) going down or up. Repeats of a key already down
    // are ignored (the host repeats keys itself).
    void key(u8 code, bool down);
    // Every key up (as when the window loses the keyboard).
    void release_all();

    // The Type 5 key in the place of a USB keyboard's key (HID usage page 7,
    // as SDL's scancodes number them); 0 if there is none.
    static u8 from_usb(unsigned usage);

    void set_layout(u8 layout) { layout_ = layout & 0x3F; }
    u8 leds() const { return leds_; }  // num 1, compose 2, scroll 4, caps 8, kana 0x10
    bool bell() const { return bell_; }
    bool click() const { return click_; }

private:
    void command(u8 byte);
    void self_test_done();
    void send(u8 byte) { line_.receive(byte); }

    SerialLine& line_;
    double cpu_hz_;
    Timer self_test_;
    u8 layout_ = kLayoutUs, leds_ = 0;
    bool bell_ = false, click_ = false, led_next_ = false;
    std::array<bool, 128> down_{};
};

class SunMouse {
public:
    explicit SunMouse(SerialLine& line);
    void reset();

    // Movement (x right, y down the screen, in mouse counts) and buttons
    // (left 1, middle 2, right 4), gathered until the next packet.
    void move(int dx, int dy) { dx_ += dx, dy_ += dy; }
    void set_buttons(unsigned buttons) { buttons_ = buttons & 7; }
    // Sends a packet if there is news and the line has finished the last.
    void poll();

private:
    SerialLine& line_;
    int dx_ = 0, dy_ = 0;
    unsigned buttons_ = 0, sent_buttons_ = 0;
};

}  // namespace uv
