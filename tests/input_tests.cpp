// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
// Serial wires with a rate at each end, and the Sun keyboard and mouse on
// the SuperIO's UARTs.
#include <vector>

#include "devices/ns16550.h"
#include "devices/sun_keyboard.h"
#include "test.h"

using namespace uv;

namespace {

LineFormat at(double baud) {
    LineFormat f;
    f.baud = baud;
    return f;
}

std::vector<WireChar> over_wire(const std::vector<u8>& bytes, double tx_baud, double rx_baud) {
    WireDecoder w;
    std::vector<WireChar> got;
    bool first = true;
    for (u8 b : bytes) {
        w.send(b, at(tx_baud), !first, at(rx_baud), got);
        first = false;
    }
    w.idle(at(rx_baud), got);
    return got;
}

// A SuperIO UART (1.8432 MHz clock) at 100 MHz of CPU time, with a Sun
// keyboard or mouse at the end of its line.
struct Port {
    u64 now = 0;
    Scheduler sched;
    SerialLine line;
    Ns16550 uart{sched, line, 1843200.0, 100e6};
    Port() {
        sched.set_clock([this] { return now; });
        set_rate(0x60);  // 1200 baud
    }
    void set_rate(u16 divisor, u8 lcr = 3) {
        uart.write(3, 1, 0x80);
        uart.write(0, 1, divisor & 0xFF);
        uart.write(1, 1, divisor >> 8);
        uart.write(3, 1, lcr);
        uart.write(2, 1, 0x07);  // FIFOs on, cleared
    }
    unsigned rd(unsigned r) {
        u64 v = 0;
        uart.read(r, 1, v);
        return static_cast<unsigned>(v);
    }
    void run(double seconds) {
        const u64 end = now + static_cast<u64>(seconds * 100e6);
        while (now < end) {
            now = std::min(end, now + 100000);  // 1 ms
            sched.run_due(now);
            uart.poll_line();
        }
    }
    // What has arrived, and the errors seen (LSR bits 1-4 as each byte came off).
    std::vector<u8> take(std::vector<u8>* errors = nullptr) {
        std::vector<u8> got;
        for (;;) {
            const unsigned lsr = rd(5);
            if (!(lsr & 1)) break;
            if (errors) errors->push_back(static_cast<u8>(lsr & 0x1E));
            got.push_back(static_cast<u8>(rd(0)));
        }
        return got;
    }
};

}  // namespace

// Characters at the same rate cross whole; a slower device's characters
// read at eight times its rate fall apart as a real receiver's would: a
// zero byte is a break, 0x87 reads as 0x80 with a good stop bit; a faster
// one's short pulses are not taken for start bits.
TEST(serial_wire_samples_characters_at_the_receivers_rate) {
    const std::vector<WireChar> same = over_wire({0x41, 0x00, 0xFF, 0x87}, 1200, 1200);
    CHECK_EQ(same.size(), 4u);
    CHECK_EQ(same[0].byte, 0x41u);
    CHECK_EQ(same[1].byte, 0x00u);
    CHECK(!same[1].brk && !same[1].framing_error);
    CHECK_EQ(same[3].byte, 0x87u);
    const std::vector<WireChar> zero = over_wire({0x00}, 1200, 9600);
    CHECK(!zero.empty());
    CHECK(zero[0].brk);
    CHECK(zero[0].framing_error);
    const std::vector<WireChar> header = over_wire({0x87}, 1200, 9600);
    CHECK(!header.empty());
    CHECK_EQ(header[0].byte, 0x80u);
    CHECK(!header[0].framing_error);
    CHECK(!over_wire({0x41}, 1200, 1236).at(0).framing_error);  // 3 % fast still reads
    CHECK_EQ(over_wire({0x41}, 1200, 1236).at(0).byte, 0x41u);
    // A faster sender read slowly: a start bit shorter than half the
    // receiver's bit is a glitch, not a character.
    CHECK(over_wire({0xFF, 0xFF}, 9600, 1200).empty());
}

// Reset: the self-test response (type 4, all keys up), click and LEDs off;
// SETLED's parameter is not a command even when it is 0x01; the layout.
TEST(sun_keyboard_answers_reset_layout_and_leds) {
    Port p;
    SunKeyboard kbd(p.sched, p.line, 100e6);
    p.uart.write(0, 1, 0x0A);  // click on
    p.uart.write(0, 1, 0x01);  // reset
    p.run(0.1);
    CHECK(!kbd.click());
    const std::vector<u8> reply = p.take();
    CHECK_EQ(reply.size(), 3u);
    CHECK_EQ(reply.at(0), 0xFFu);
    CHECK_EQ(reply.at(1), 0x04u);
    CHECK_EQ(reply.at(2), 0x7Fu);
    p.uart.write(0, 1, 0x0E);  // SETLED 0x01 (num lock): no reset
    p.uart.write(0, 1, 0x01);
    p.run(0.1);
    CHECK_EQ(kbd.leds(), 0x01u);
    CHECK(p.take().empty());
    kbd.set_layout(SunKeyboard::kLayoutUk);
    p.uart.write(0, 1, 0x0F);
    p.uart.write(0, 1, 0x02);  // bell on
    p.run(0.1);
    CHECK(kbd.bell());
    const std::vector<u8> layout = p.take();
    CHECK_EQ(layout.size(), 2u);
    CHECK_EQ(layout.at(0), 0xFEu);
    CHECK_EQ(layout.at(1), 0x2Eu);
    // The self-test response comes 1200 baud at a time (3 bytes: 25 ms).
    p.uart.write(0, 1, 0x01);
    p.run(0.02);
    CHECK(p.take().size() < 3);
}

// Keys: make, then break; all keys up only once the last is released;
// repeats of a key already down are dropped; keys held through a reset
// replace "all keys up" in its response (Stop-N at power-on and the like).
TEST(sun_keyboard_sends_keys_and_all_keys_up) {
    Port p;
    SunKeyboard kbd(p.sched, p.line, 100e6);
    kbd.key(0x01, true);   // Stop
    kbd.key(0x4D, true);   // A
    kbd.key(0x4D, true);   // (host repeat)
    kbd.key(0x4D, false);
    kbd.key(0x01, false);
    p.run(0.2);
    const std::vector<u8> keys = p.take();
    CHECK_EQ(keys.size(), 5u);
    CHECK_EQ(keys.at(0), 0x01u);
    CHECK_EQ(keys.at(1), 0x4Du);
    CHECK_EQ(keys.at(2), 0xCDu);
    CHECK_EQ(keys.at(3), 0x81u);
    CHECK_EQ(keys.at(4), 0x7Fu);
    kbd.key(0x01, true);
    kbd.key(0x6A, true);  // N
    p.run(0.1);
    p.take();
    p.uart.write(0, 1, 0x01);
    p.run(0.1);
    const std::vector<u8> reply = p.take();
    CHECK_EQ(reply.size(), 4u);
    CHECK_EQ(reply.at(2), 0x01u);
    CHECK_EQ(reply.at(3), 0x6Au);
    kbd.release_all();
    p.run(0.1);
    const std::vector<u8> up = p.take();
    CHECK_EQ(up.size(), 3u);
    CHECK_EQ(up.at(2), 0x7Fu);
}

// The mouse: 0x80 | buttons up (left 4, middle 2, right 1), x right, y up,
// each byte within -112..127 with the rest in the next packet; nothing when
// nothing changes. At 9600 baud a receiver sees its zero bytes as breaks
// (how Solaris finds its rate).
TEST(sun_mouse_sends_five_byte_packets) {
    Port p;
    SunMouse mouse(p.line);
    mouse.move(10, 20);  // right, and down the screen
    mouse.set_buttons(1);  // left
    mouse.poll();
    p.run(0.1);
    const std::vector<u8> pkt = p.take();
    CHECK_EQ(pkt.size(), 5u);
    CHECK_EQ(pkt.at(0), 0x83u);
    CHECK_EQ(pkt.at(1), 10u);
    CHECK_EQ(pkt.at(2), static_cast<u8>(-20));
    CHECK_EQ(pkt.at(3), 0u);
    CHECK_EQ(pkt.at(4), 0u);
    mouse.poll();
    p.run(0.1);
    CHECK(p.take().empty());
    mouse.move(300, -300);
    mouse.poll();
    p.run(0.1);
    const std::vector<u8> big = p.take();
    CHECK_EQ(big.size(), 5u);
    CHECK_EQ(big.at(1), 127u);
    CHECK_EQ(big.at(2), 127u);
    CHECK_EQ(big.at(3), 127u);
    CHECK_EQ(big.at(4), 127u);
    mouse.poll();
    p.run(0.1);
    const std::vector<u8> rest = p.take();
    CHECK_EQ(rest.size(), 5u);
    CHECK_EQ(rest.at(1), 46u);
    CHECK_EQ(rest.at(2), 46u);
    mouse.move(-130, 0);  // left: no byte below -112 (0x90)
    mouse.poll();
    p.run(0.1);
    const std::vector<u8> left = p.take();
    CHECK_EQ(left.size(), 5u);
    CHECK_EQ(left.at(1), 0x90u);
    CHECK_EQ(left.at(3), static_cast<u8>(-18));
    p.set_rate(0x0C);  // 9600
    mouse.move(0, 0);
    mouse.set_buttons(0);
    mouse.poll();
    p.run(0.1);
    std::vector<u8> errors;
    p.take(&errors);
    bool brk = false;
    for (u8 e : errors) brk |= (e & 0x10) != 0;
    CHECK(brk);
}

// The keyboard does not take commands sent at another rate.
TEST(sun_keyboard_ignores_commands_at_the_wrong_rate) {
    Port p;
    SunKeyboard kbd(p.sched, p.line, 100e6);
    p.set_rate(0x0C);  // 9600
    p.uart.write(0, 1, 0x01);
    p.run(0.1);
    p.set_rate(0x60);
    CHECK(p.take().empty());
}

// Host keys (USB usage numbers, as SDL's scancodes) to the Type 5's keys by
// position: letters, digits, Return, the modifiers (both Controls are the
// one Control), the arrows, the keypad and the Sun keys; none for keys the
// Type 5 does not have.
TEST(sun_keyboard_maps_usb_keys_by_position) {
    CHECK_EQ(SunKeyboard::from_usb(0x04), 0x4Du);  // A
    CHECK_EQ(SunKeyboard::from_usb(0x1D), 0x64u);  // Z
    CHECK_EQ(SunKeyboard::from_usb(0x14), 0x36u);  // Q
    CHECK_EQ(SunKeyboard::from_usb(0x1E), 0x1Eu);  // 1
    CHECK_EQ(SunKeyboard::from_usb(0x27), 0x27u);  // 0
    CHECK_EQ(SunKeyboard::from_usb(0x28), 0x59u);  // Return
    CHECK_EQ(SunKeyboard::from_usb(0xE0), 0x4Cu);  // left Control
    CHECK_EQ(SunKeyboard::from_usb(0xE4), 0x4Cu);  // right Control
    CHECK_EQ(SunKeyboard::from_usb(0xE3), 0x78u);  // left GUI: Meta
    CHECK_EQ(SunKeyboard::from_usb(0x52), 0x14u);  // Up
    CHECK_EQ(SunKeyboard::from_usb(0x3A), 0x05u);  // F1
    CHECK_EQ(SunKeyboard::from_usb(0x45), 0x0Bu);  // F12
    CHECK_EQ(SunKeyboard::from_usb(0x62), 0x5Eu);  // keypad 0
    CHECK_EQ(SunKeyboard::from_usb(0x78), 0x01u);  // Stop
    CHECK_EQ(SunKeyboard::from_usb(0x67), 0u);     // keypad = (no such key on a Type 5)
    CHECK_EQ(SunKeyboard::from_usb(0x68), 0u);     // F13
}
