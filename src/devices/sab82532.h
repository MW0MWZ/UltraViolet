// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
// Siemens SAB 82532 ESCC2: the two "se" serial ports (ttya, ttyb) in
// asynchronous mode. Each channel has 32-byte receive and transmit FIFOs
// with the frame commands (XF, RMC, RFRD, resets), the transmit immediate
// character, receive thresholds and the timeout, and interrupt status and
// masks; the global interrupt status, port and vector registers are shared.
// Register map as used by the Linux sunsab driver.
#pragma once

#include <array>
#include <deque>
#include <functional>
#include <memory>

#include "core/scheduler.h"
#include "devices/serial.h"
#include "mem/bus.h"

namespace uv {

class Sab82532 final : public Device {
public:
    static constexpr double kClockHz = 29491200.0;

    Sab82532(Scheduler& scheduler, SerialLine& a, SerialLine& b, double cpu_hz);

    const char* name() const override { return "sab82532"; }
    bool read(u64 offset, unsigned size, u64& value) override;
    u64 stable_until(u64 offset, unsigned size) const override;
    bool write(u64 offset, unsigned size, u64 value) override;

    void reset();
    void set_irq_sink(std::function<void(bool)> sink) { irq_ = std::move(sink); }
    // Fetches bytes the host has delivered to either line.
    void poll_lines();

private:
    struct Channel {
        Channel(Sab82532& chip, SerialLine& line);
        Sab82532& chip;
        SerialLine& line;
        Timer tx_timer, rx_timer;
        std::deque<u8> rx;          // RFIFO bytes (data, or data and status pairs)
        std::array<u8, 32> xfifo{};
        unsigned xcount = 0;        // bytes written to the XFIFO since the last XF
        std::deque<u8> sending;     // bytes being shifted out
        unsigned released = 0;      // bytes of the block offered by the last RPF/TCD/TIME not yet read
        u8 mode = 0, timr = 0, xon = 0x11, xoff = 0x13, tcr = 0, dafo = 0, rfc = 0;
        u8 ccr0 = 0, ccr1 = 0, ccr2 = 0, ccr3 = 0, ccr4 = 0, bgr = 0, tic = 0, mxn = 0, mxf = 0;
        u8 tsax = 0, tsar = 0, xccr = 0, rccr = 0, xbcl = 0, xbch = 0;
        u8 isr0 = 0, isr1 = 0, imr0 = 0xFF, imr1 = 0xFF;
        bool tic_busy = false;
        void reset();
        u64 char_cycles() const;
        unsigned threshold() const;
        void start_tx();
        void tx_tick();
        void rx_tick();
        void release_for_read(bool timeout);
        bool read(unsigned reg, u8& v);
        void write(unsigned reg, u8 v);
        void command(u8 cmd);
    };

    void update_irq();

    Scheduler& sched_;
    double cpu_hz_;
    std::unique_ptr<Channel> ch_[2];
    std::function<void(bool)> irq_;
    // An interrupt status register's readable bits: all of them with IPC:VIS
    // set, else only the unmasked ones.
    u8 visible(u8 status, u8 mask) const;
    u8 ipc_ = 0, iva_ = 0, pvr_ = 0, pim_ = 0xFF, pcr_ = 0, pis_ = 0;
};

}  // namespace uv
