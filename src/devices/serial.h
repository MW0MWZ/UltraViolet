// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
// The far end of an emulated serial line: what the guest transmits goes to
// `transmit`; the host side queues received bytes with receive() (on the
// emulation thread) and the UART collects them when it has room.
#pragma once

#include <deque>
#include <functional>

#include "core/types.h"

namespace uv {

class SerialLine {
public:
    std::function<void(u8)> transmit;  // guest -> host

    void receive(u8 byte) { rx_.push_back(byte); }  // host -> guest
    bool rx_pending() const { return !rx_.empty(); }
    u8 rx_take() {
        const u8 b = rx_.front();
        rx_.pop_front();
        return b;
    }

private:
    std::deque<u8> rx_;
};

}  // namespace uv
