// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
#include "devices/sun_keyboard.h"

#include <algorithm>

namespace uv {

namespace {

// Commands from the host, and the keyboard's codes.
constexpr u8 kReset = 0x01, kBellOn = 0x02, kBellOff = 0x03, kClickOn = 0x0A, kClickOff = 0x0B, kSetLed = 0x0E,
             kLayout = 0x0F;
constexpr u8 kResetResponse = 0xFF, kLayoutResponse = 0xFE, kIdle = 0x7F, kTypeId = 0x04;
// From the reset command to the self-test response.
constexpr double kSelfTestSeconds = 0.005;

LineFormat sun_line() {
    LineFormat f;
    f.baud = 1200;
    return f;
}

}  // namespace

SunKeyboard::SunKeyboard(Scheduler& scheduler, SerialLine& line, double cpu_hz)
    : line_(line), cpu_hz_(cpu_hz), self_test_(scheduler, [this] { self_test_done(); }) {
    line_.device = sun_line();
    line_.transmit = [this](u8 b) { command(b); };
}

void SunKeyboard::reset() {
    self_test_.cancel();
    leds_ = 0;
    bell_ = click_ = led_next_ = false;
}

void SunKeyboard::command(u8 b) {
    if (led_next_) {  // SETLED's parameter, whatever its value
        leds_ = b;
        led_next_ = false;
        return;
    }
    switch (b) {
        case kReset:
            // The self-test: what was waiting to be sent is dropped, LEDs
            // off, click off; then the response.
            line_.rx_clear();
            leds_ = 0;
            bell_ = click_ = false;
            self_test_.schedule_in(static_cast<u64>(cpu_hz_ * kSelfTestSeconds));
            break;
        case kBellOn: bell_ = true; break;
        case kBellOff: bell_ = false; break;
        case kClickOn: click_ = true; break;
        case kClickOff: click_ = false; break;
        case kSetLed: led_next_ = true; break;
        case kLayout:
            send(kLayoutResponse);
            send(layout_);
            break;
        default: break;  // (others are for other keyboards)
    }
}

void SunKeyboard::self_test_done() {
    // Passed: the type, then the keys held down (or all keys up).
    send(kResetResponse);
    send(kTypeId);
    bool any = false;
    for (unsigned k = 1; k < down_.size(); ++k)
        if (down_[k]) send(static_cast<u8>(k)), any = true;
    if (!any) send(kIdle);
}

void SunKeyboard::key(u8 code, bool down) {
    if (code == 0 || code >= kIdle - 1 || down_[code] == down) return;
    down_[code] = down;
    if (self_test_.active()) return;  // (reported with the self-test response)
    if (down) {
        send(code);
        return;
    }
    send(static_cast<u8>(0x80 | code));
    if (std::none_of(down_.begin(), down_.end(), [](bool d) { return d; })) send(kIdle);
}

void SunKeyboard::release_all() {
    for (unsigned k = 1; k < down_.size(); ++k)
        if (down_[k]) key(static_cast<u8>(k), false);
}

u8 SunKeyboard::from_usb(unsigned usage) {
    // Letters (usage 4-29: A-Z) by the rows they sit in on a Type 5.
    static constexpr u8 kLetters[26] = {0x4D, 0x68, 0x66, 0x4F, 0x38, 0x50, 0x51, 0x52, 0x3D, 0x53, 0x54, 0x55, 0x6A,
                                        0x69, 0x3E, 0x3F, 0x36, 0x39, 0x4E, 0x3A, 0x3C, 0x67, 0x37, 0x65, 0x3B, 0x64};
    if (usage >= 0x04 && usage <= 0x1D) return kLetters[usage - 0x04];
    if (usage >= 0x1E && usage <= 0x27) return static_cast<u8>(0x1E + (usage - 0x1E));  // 1-9, 0
    if (usage >= 0x3A && usage <= 0x45) {  // F1-F12
        static constexpr u8 kF[12] = {0x05, 0x06, 0x08, 0x0A, 0x0C, 0x0E, 0x10, 0x11, 0x12, 0x07, 0x09, 0x0B};
        return kF[usage - 0x3A];
    }
    if (usage >= 0x59 && usage <= 0x62) {  // keypad 1-9, 0
        static constexpr u8 kPad[10] = {0x70, 0x71, 0x72, 0x5B, 0x5C, 0x5D, 0x44, 0x45, 0x46, 0x5E};
        return kPad[usage - 0x59];
    }
    switch (usage) {
        case 0x28: return 0x59;  // Return
        case 0x29: return 0x1D;  // Escape
        case 0x2A: return 0x2B;  // Backspace
        case 0x2B: return 0x35;  // Tab
        case 0x2C: return 0x79;  // Space
        case 0x2D: return 0x28;  // - _
        case 0x2E: return 0x29;  // = +
        case 0x2F: return 0x40;  // [ {
        case 0x30: return 0x41;  // ] }
        case 0x31: return 0x58;  // backslash
        case 0x32: return 0x58;  // (ISO) # ~, in the same place
        case 0x33: return 0x56;  // ; :
        case 0x34: return 0x57;  // ' "
        case 0x35: return 0x2A;  // ` ~
        case 0x36: return 0x6B;  // , <
        case 0x37: return 0x6C;  // . >
        case 0x38: return 0x6D;  // / ?
        case 0x39: return 0x77;  // Caps Lock
        case 0x46: return 0x16;  // Print Screen
        case 0x47: return 0x17;  // Scroll Lock
        case 0x48: return 0x15;  // Pause
        case 0x49: return 0x2C;  // Insert
        case 0x4A: return 0x34;  // Home
        case 0x4B: return 0x60;  // Page Up
        case 0x4C: return 0x42;  // Delete
        case 0x4D: return 0x4A;  // End
        case 0x4E: return 0x7B;  // Page Down
        case 0x4F: return 0x1C;  // Right
        case 0x50: return 0x18;  // Left
        case 0x51: return 0x1B;  // Down
        case 0x52: return 0x14;  // Up
        case 0x53: return 0x62;  // Num Lock
        case 0x54: return 0x2E;  // keypad /
        case 0x55: return 0x2F;  // keypad *
        case 0x56: return 0x47;  // keypad -
        case 0x57: return 0x7D;  // keypad +
        case 0x58: return 0x5A;  // keypad Enter
        case 0x63: return 0x32;  // keypad .
        case 0x64: return 0x7C;  // (ISO) the key left of Z
        case 0x65: return 0x43;  // Menu: Compose
        case 0x66: return 0x30;  // Power
        case 0x74: return 0x48;  // Execute: Open
        case 0x75: return 0x76;  // Help
        case 0x77: return 0x31;  // Select: Front
        case 0x78: return 0x01;  // Stop
        case 0x79: return 0x03;  // Again
        case 0x7A: return 0x1A;  // Undo
        case 0x7B: return 0x61;  // Cut
        case 0x7C: return 0x33;  // Copy
        case 0x7D: return 0x49;  // Paste
        case 0x7E: return 0x5F;  // Find
        case 0x7F: return 0x2D;  // Mute
        case 0x80: return 0x04;  // Volume Up
        case 0x81: return 0x02;  // Volume Down
        case 0xE0:
        case 0xE4: return 0x4C;  // Control (the Type 5 has one)
        case 0xE1: return 0x63;  // Shift
        case 0xE5: return 0x6E;
        case 0xE2: return 0x13;  // Alt
        case 0xE6: return 0x0D;  // Alt Graph
        case 0xE3: return 0x78;  // Meta (the GUI keys)
        case 0xE7: return 0x7A;
        default: return 0;
    }
}

SunMouse::SunMouse(SerialLine& line) : line_(line) { line_.device = sun_line(); }

void SunMouse::reset() {
    dx_ = dy_ = 0;
    sent_buttons_ = buttons_;
}

void SunMouse::poll() {
    if (line_.rx_pending()) return;  // (the last packet still going: movement gathers)
    if (dx_ == 0 && dy_ == 0 && buttons_ == sent_buttons_) return;
    // Each movement byte within -112..127 (a byte of 0x80-0x8F mid-packet
    // could be taken for the start of one); what does not fit waits.
    auto take = [](int& v) {
        const int t = std::clamp(v, -112, 127);
        v -= t;
        return static_cast<u8>(static_cast<s8>(t));
    };
    int up = -dy_;
    const u8 x1 = take(dx_), y1 = take(up), x2 = take(dx_), y2 = take(up);
    dy_ = -up;
    // Buttons: 1 left, 2 middle, 4 right here; down as 0 bits, left 4,
    // middle 2, right 1 on the wire.
    const unsigned wire = ((buttons_ & 1) ? 4 : 0) | ((buttons_ & 2) ? 2 : 0) | ((buttons_ & 4) ? 1 : 0);
    line_.receive(static_cast<u8>(0x80 | (~wire & 7)));
    line_.receive(x1);
    line_.receive(y1);
    line_.receive(x2);
    line_.receive(y2);
    sent_buttons_ = buttons_;
}

}  // namespace uv
