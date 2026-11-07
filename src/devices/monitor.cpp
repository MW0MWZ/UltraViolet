// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
#include "devices/monitor.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>

namespace uv {

namespace {
constexpr u8 kEdidAddress = 0x50;
}

// Data changes while SCL is low; a change of SDA while SCL is high is a
// start (falling) or a stop (rising). A bit is taken as SCL rises, and the
// monitor puts its next bit (or acknowledge) on SDA as SCL falls.
void Monitor::drive(bool scl, bool sda) {
    const bool rising = scl && !scl_, falling = !scl && scl_;
    if (scl && scl_ && sda != sda_) {
        pull_sda_ = false;
        ack_ = false;
        bits_ = 0;
        state_ = sda || edid_.empty() ? State::Idle : State::Address;
        note(sda ? " P" : " S");
    } else if (rising && state_ != State::Idle) {
        if (state_ == State::Read) {
            if (ack_) {
                nak_ = sda;  // the card's acknowledge
                note(nak_ ? "-" : "");
            }
            else ++bits_;
        } else if (!ack_) {
            shift_ = static_cast<u8>(shift_ << 1 | (sda ? 1 : 0));
            ++bits_;
        }
    } else if (falling && state_ != State::Idle) {
        if (state_ == State::Read) {
            if (ack_ && nak_) {
                // The last byte the card wanted: let go till the stop.
                pull_sda_ = false;
                state_ = State::Idle;
            } else if (ack_) {
                send_next();
            } else if (bits_ == 8) {
                pull_sda_ = false;
                ack_ = true;
            } else {
                pull_sda_ = !((shift_ << bits_) & 0x80);
            }
        } else if (ack_) {
            // The monitor's acknowledge done: a read starts sending.
            if (to_read_) {
                state_ = State::Read;
                send_next();
            } else {
                ack_ = false;
                bits_ = 0;
                pull_sda_ = false;
            }
        } else if (bits_ == 8) {
            received(shift_);
        }
    }
    scl_ = scl;
    sda_ = sda;
}

void Monitor::note(const char* text) {
    if (trace_.size() < 16384) trace_ += text;
}

void Monitor::received(u8 byte) {
    char t[8];
    std::snprintf(t, sizeof t, " %02x", byte);
    note(t);
    if (state_ == State::Address) {
        if (byte >> 1 != kEdidAddress) {
            state_ = State::Idle;
            return;
        }
        to_read_ = byte & 1;
        if (!to_read_) state_ = State::Offset;
    } else {
        offset_ = byte;
    }
    pull_sda_ = true;
    ack_ = true;
}

void Monitor::send_next() {
    shift_ = offset_ < edid_.size() ? edid_[offset_] : 0xFF;
    ++offset_;
    char t[8];
    std::snprintf(t, sizeof t, " <%02x", shift_);
    note(t);
    ack_ = false;
    bits_ = 0;
    pull_sda_ = !(shift_ & 0x80);
}

namespace {

// The established timings' bits (bytes 35-36): width, height, rate.
constexpr struct {
    unsigned byte, bit, width, height, rate;
} kEstablished[] = {
    {35, 5, 640, 480, 60},   {35, 2, 640, 480, 75},   {35, 0, 800, 600, 60},   {36, 7, 800, 600, 72},
    {36, 6, 800, 600, 75},   {36, 3, 1024, 768, 60},  {36, 2, 1024, 768, 70},  {36, 1, 1024, 768, 75},
    {36, 0, 1280, 1024, 75},
};

// A standard timing's two bytes, if its aspect ratio has a code (16:10,
// 4:3, 5:4, 16:9) and its rate is 60-123 Hz.
bool standard(const MonitorTiming& t, u8 out[2]) {
    static constexpr unsigned kAspect[4][2] = {{16, 10}, {4, 3}, {5, 4}, {16, 9}};
    if (t.width % 8 || t.width < 256 || t.width > 2288 || t.rate < 60 || t.rate > 123) return false;
    for (unsigned a = 0; a < 4; ++a)
        if (t.width * kAspect[a][1] == t.height * kAspect[a][0]) {
            out[0] = static_cast<u8>(t.width / 8 - 31);
            out[1] = static_cast<u8>(a << 6 | (t.rate - 60));
            return true;
        }
    return false;
}

void detailed(const MonitorTiming& t, u8* d) {
    const unsigned hblank = t.h_front + t.h_sync + t.h_back, vblank = t.v_front + t.v_sync + t.v_back;
    const unsigned clock = (t.pixel_khz + 5) / 10;  // 10 kHz units
    d[0] = clock & 0xFF, d[1] = static_cast<u8>(clock >> 8);
    d[2] = t.width & 0xFF, d[3] = hblank & 0xFF, d[4] = static_cast<u8>((t.width >> 8) << 4 | hblank >> 8);
    d[5] = t.height & 0xFF, d[6] = vblank & 0xFF, d[7] = static_cast<u8>((t.height >> 8) << 4 | vblank >> 8);
    d[8] = t.h_front & 0xFF, d[9] = t.h_sync & 0xFF, d[10] = static_cast<u8>((t.v_front & 15) << 4 | (t.v_sync & 15));
    d[11] = static_cast<u8>((t.h_front >> 8) << 6 | (t.h_sync >> 8) << 4 | (t.v_front >> 4) << 2 | t.v_sync >> 4);
    // The picture's size (mm): the mode's shape, as large as the screen
    // (400 x 300) holds.
    unsigned w = 400, h = 400 * t.height / t.width;
    if (h > 300) h = 300, w = 300 * t.width / t.height;
    d[12] = w & 0xFF, d[13] = h & 0xFF, d[14] = static_cast<u8>((w >> 8) << 4 | h >> 8);
    d[17] = t.positive_sync ? 0x1E : 0x18;  // digital separate sync
}

}  // namespace

const MonitorTiming* find_timing(const std::vector<MonitorTiming>& list, const std::string& size) {
    for (const MonitorTiming& t : list)
        if (std::to_string(t.width) + "x" + std::to_string(t.height) == size) return &t;
    return nullptr;
}

std::vector<u8> monitor_edid(const std::vector<MonitorTiming>& list, const MonitorTiming& preferred) {
    std::vector<MonitorTiming> timings{preferred};
    for (const MonitorTiming& t : list)
        if (t.width != preferred.width || t.height != preferred.height) timings.push_back(t);
    return make_edid(timings, "UltraViolet");
}

std::vector<u8> make_edid(const std::vector<MonitorTiming>& timings, const char* name) {
    std::vector<u8> e(128, 0);
    static constexpr u8 kHeader[8] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
    std::copy(kHeader, kHeader + 8, e.begin());
    e[8] = ('U' - 64) << 2 | ('V' - 64) >> 3;  // the maker "UVM", in five-bit letters
    e[9] = static_cast<u8>((('V' - 64) & 7) << 5 | ('M' - 64));
    e[10] = 1;                                 // product 1, no serial number
    e[16] = 0, e[17] = 2026 - 1990;            // made in 2026
    e[18] = 1, e[19] = 3;                      // EDID 1.3
    e[20] = 0x0E;  // analogue, 0.7 V: separate sync, composite sync and sync on green
    e[21] = 40, e[22] = 30;  // cm
    e[23] = 120;             // gamma 2.2
    e[24] = 0xEE;  // DPMS standby, suspend and off; RGB colour; sRGB; the preferred timing first
    static constexpr u8 kSrgb[10] = {0xEE, 0x91, 0xA3, 0x54, 0x4C, 0x99, 0x26, 0x0F, 0x50, 0x54};
    std::copy(kSrgb, kSrgb + 10, e.begin() + 25);

    // The standard timings: eight in the block's own list, six more in a
    // descriptor of them.
    std::vector<std::array<u8, 2>> standards;
    for (std::size_t i = 0; i < timings.size(); ++i) {
        const MonitorTiming& t = timings[i];
        bool established = false;
        for (const auto& x : kEstablished)
            if (x.width == t.width && x.height == t.height && x.rate == t.rate) {
                e[x.byte] |= static_cast<u8>(1u << x.bit);
                established = true;
            }
        std::array<u8, 2> st{};
        if ((i == 0 || !established) && standard(t, st.data())) standards.push_back(st);
    }
    for (unsigned i = 0; i < 8; ++i) {
        e[38 + 2 * i] = i < standards.size() ? standards[i][0] : 1;
        e[39 + 2 * i] = i < standards.size() ? standards[i][1] : 1;
    }

    // The descriptors: the preferred timing, the name, the range limits,
    // then the standard timings past eight (or nothing).
    u8* d = e.data() + 54;
    if (!timings.empty()) detailed(timings.front(), d);
    d += 18;
    d[3] = 0xFC;
    std::string text = std::string(name).substr(0, 13);
    if (text.size() < 13) text += '\n';
    text.resize(13, ' ');
    std::copy(text.begin(), text.end(), d + 5);
    d += 18;
    if (!timings.empty()) {
        double vmin = 1e9, vmax = 0, hmin = 1e9, hmax = 0;
        unsigned clock = 0;
        for (const MonitorTiming& t : timings) {
            const double line = t.width + t.h_front + t.h_sync + t.h_back;
            const double frame = t.height + t.v_front + t.v_sync + t.v_back;
            const double h = t.pixel_khz / line, v = t.pixel_khz * 1000.0 / line / frame;
            vmin = std::min(vmin, v), vmax = std::max(vmax, v), hmin = std::min(hmin, h), hmax = std::max(hmax, h);
            clock = std::max(clock, t.pixel_khz);
        }
        d[3] = 0xFD;
        d[5] = static_cast<u8>(std::floor(vmin)), d[6] = static_cast<u8>(std::ceil(vmax));
        // (With room for the standard timings' clocks as GTF makes them,
        // a little above the card's.)
        d[7] = static_cast<u8>(std::floor(hmin)), d[8] = static_cast<u8>(std::ceil(hmax * 1.05));
        d[9] = static_cast<u8>(std::ceil(clock * 1.05 / 10000));  // 10 MHz units
        d[10] = 0, d[11] = 0x0A;
        std::fill(d + 12, d + 18, u8{' '});
    }
    d += 18;
    if (standards.size() > 8) {
        d[3] = 0xFA;
        for (unsigned i = 0; i < 6; ++i) {
            const std::size_t k = 8 + i;
            d[5 + 2 * i] = k < standards.size() ? standards[k][0] : 1;
            d[6 + 2 * i] = k < standards.size() ? standards[k][1] : 1;
        }
        d[17] = 0x0A;
    } else {
        d[3] = 0x10;  // a dummy descriptor
    }

    u8 sum = 0;
    for (unsigned i = 0; i < 127; ++i) sum = static_cast<u8>(sum + e[i]);
    e[127] = static_cast<u8>(-sum);
    return e;
}

}  // namespace uv
