// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
#include "devices/serial.h"

#include <algorithm>
#include <cmath>

namespace uv {

void WireDecoder::send(u8 byte, const LineFormat& tx, bool contiguous, const LineFormat& rx,
                       std::vector<WireChar>& out) {
    const double bit = 1.0 / tx.baud;
    if (!contiguous || bit != bit_) {
        if (!bits_.empty()) idle(rx, out);
        bit_ = bit;
    }
    bits_.push_back(0);  // start
    unsigned ones = 0;
    for (unsigned i = 0; i < tx.data_bits; ++i) {
        const u8 b = (byte >> i) & 1;
        ones += b;
        bits_.push_back(b);
    }
    if (tx.parity) bits_.push_back(static_cast<u8>((ones & 1) ^ (tx.odd ? 1 : 0)));
    for (unsigned i = 0; i < tx.stop_bits; ++i) bits_.push_back(1);
    decode(rx, false, out);
}

void WireDecoder::idle(const LineFormat& rx, std::vector<WireChar>& out) {
    decode(rx, true, out);
    bits_.clear();
    cursor_ = 0;
    wait_mark_ = false;
}

int WireDecoder::level(double t) const {
    if (t < 0) return 1;
    const auto i = static_cast<std::size_t>(std::floor(t / bit_ + 1e-9));
    return i < bits_.size() ? bits_[i] : 1;  // (mark after what was sent)
}

void WireDecoder::decode(const LineFormat& rx, bool final, std::vector<WireChar>& out) {
    if (bits_.empty() || rx.baud <= 0) return;
    const double end = bits_.size() * bit_, rbit = 1.0 / rx.baud;
    const unsigned nbits = rx.data_bits + (rx.parity ? 1 : 0);
    for (;;) {
        if (wait_mark_) {  // after a framing error or break: the line back at mark first
            auto k = static_cast<std::size_t>(std::floor(cursor_ / bit_ + 1e-9));
            while (k < bits_.size() && bits_[k] == 0) ++k;
            if (k >= bits_.size() && !final) return;
            if (k * bit_ > cursor_) cursor_ = k * bit_;
            wait_mark_ = false;
        }
        // The next falling edge: a bit boundary from mark to space.
        auto k = static_cast<std::size_t>(std::ceil(cursor_ / bit_ - 1e-9));
        while (k < bits_.size() && !(bits_[k] == 0 && (k == 0 ? 1 : bits_[k - 1]) == 1)) ++k;
        if (k >= bits_.size()) {
            if (final) return;
            cursor_ = std::max(cursor_, (bits_.size() - 1) * bit_);  // (an edge may come with the next bit)
            return;
        }
        const double s = k * bit_;
        const double last = s + (1.5 + nbits) * rbit;  // the stop bit's sample
        if (last >= end && !final) {
            cursor_ = std::max(cursor_, s - 1e-12);
            return;
        }
        if (level(s + 0.5 * rbit)) {  // a glitch, not a start bit
            cursor_ = s + 0.5 * rbit;
            continue;
        }
        WireChar c;
        unsigned ones = 0;
        bool any = false;
        for (unsigned i = 0; i < rx.data_bits; ++i) {
            const int b = level(s + (1.5 + i) * rbit);
            c.byte |= static_cast<u8>(b << i);
            ones += b;
            any |= b != 0;
        }
        if (rx.parity) {
            const int p = level(s + (1.5 + rx.data_bits) * rbit);
            any |= p != 0;
            c.parity_error = ((ones + p) & 1) != (rx.odd ? 1u : 0u);
        }
        const int stop = level(last);
        c.framing_error = stop == 0;
        c.brk = !any && stop == 0;
        if (c.brk) c.byte = 0, c.parity_error = false;
        out.push_back(c);
        cursor_ = last;
        wait_mark_ = stop == 0;
    }
}

}  // namespace uv
