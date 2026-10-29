// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
// The far end of an emulated serial line: what the guest transmits goes to
// `transmit`; the host side queues received bytes with receive() (on the
// emulation thread) and the UART collects them when it has room.
//
// A line to a terminal carries bytes whatever the UART's rate. A line to a
// device with a rate of its own (the Sun keyboard and mouse) says so in
// `device`: the UART then receives the device's bit stream as it samples it
// at its own rate and format (a mismatch gives framing errors and breaks,
// which drivers that find the rate rely on), and the device receives the
// UART's the same way.
#pragma once

#include <deque>
#include <functional>
#include <vector>

#include "core/types.h"

namespace uv {

// Asynchronous character format: start bit, data bits (LSB first), an
// optional parity bit, stop bits. A rate of 0: no rate (bytes pass as they
// are).
struct LineFormat {
    double baud = 0;
    unsigned data_bits = 8;
    bool parity = false, odd = false;
    unsigned stop_bits = 1;

    unsigned frame_bits() const { return 1 + data_bits + (parity ? 1 : 0) + stop_bits; }
    double frame_seconds() const { return frame_bits() / baud; }
};

// A character as a receiver took it off the wire.
struct WireChar {
    u8 byte = 0;
    bool framing_error = false, parity_error = false, brk = false;
};

// One direction of a wire: characters sent at one format, received at
// another. A receiver finds a start bit by its falling edge, checks it at
// half a bit, samples each data bit (and parity) at the middle of its bit
// time and the first stop bit likewise; a stop bit sampled low is a framing
// error, and with every bit low a break, after which it waits for the line
// to return to mark. The line idles at mark.
class WireDecoder {
public:
    // A character sent at `tx`: straight after the previous one
    // (`contiguous`) or after the line has idled. Characters the receiver
    // (at `rx`) has finished with go to `out`.
    void send(u8 byte, const LineFormat& tx, bool contiguous, const LineFormat& rx, std::vector<WireChar>& out);
    // The line idles: the receiver finishes what it has started.
    void idle(const LineFormat& rx, std::vector<WireChar>& out);

private:
    void decode(const LineFormat& rx, bool final, std::vector<WireChar>& out);
    int level(double t) const;

    std::vector<u8> bits_;  // the levels sent since the line last idled, a bit time each
    double bit_ = 0;        // the transmitter's bit time
    double cursor_ = 0;     // where the receiver has got to (from the first bit)
    bool wait_mark_ = false;
};

class SerialLine {
public:
    std::function<void(u8)> transmit;  // guest -> host
    LineFormat device;                 // the far end's format (baud 0: a terminal)

    void receive(u8 byte) { rx_.push_back(byte); }  // host -> guest
    bool rx_pending() const { return !rx_.empty(); }
    u8 rx_take() {
        const u8 b = rx_.front();
        rx_.pop_front();
        return b;
    }
    void rx_clear() { rx_.clear(); }

private:
    std::deque<u8> rx_;
};

}  // namespace uv
