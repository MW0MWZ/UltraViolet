// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
// National Semiconductor 16550A UART (the "su" ports of the SuperIO, for
// the Sun keyboard and mouse): divisor latch, 16-byte FIFOs, line and
// modem status, and its prioritised interrupt identification.
#pragma once

#include <deque>
#include <functional>

#include "core/scheduler.h"
#include "devices/serial.h"
#include "mem/bus.h"

namespace uv {

class Ns16550 final : public Device {
public:
    // `clock_hz`: the UART's input clock (divided by 16 x divisor for the baud
    // rate). `cpu_hz`: the scheduler's clock, to time characters.
    Ns16550(Scheduler& scheduler, SerialLine& line, double clock_hz, double cpu_hz);

    const char* name() const override { return "ns16550"; }
    bool read(u64 offset, unsigned size, u64& value) override;
    bool write(u64 offset, unsigned size, u64 value) override;

    void reset();
    void set_irq_sink(std::function<void(bool)> sink) { irq_ = std::move(sink); }
    // Fetches bytes waiting on the line into the receive FIFO (called when
    // the host side has delivered some).
    void poll_line();
    // Modem status inputs (CTS, DSR, RI, DCD).
    void set_modem_inputs(bool cts, bool dsr, bool ri, bool dcd);

private:
    void update_irq();
    u64 char_cycles() const;
    void tx_done();
    void rx_tick();

    SerialLine& line_;
    double clock_hz_, cpu_hz_;
    Timer tx_timer_, rx_timer_;
    std::function<void(bool)> irq_;

    std::deque<u8> rx_fifo_, tx_fifo_;
    u8 ier_ = 0, lcr_ = 0, mcr_ = 0, lsr_ = 0x60, msr_ = 0, scr_ = 0, fcr_ = 0;
    u16 divisor_ = 12;
    bool thre_pending_ = false;  // THR empty interrupt not yet acknowledged
    bool rx_timeout_ = false;
    u8 msr_inputs_ = 0;          // CTS DSR RI DCD in MSR bits 4..7
};

}  // namespace uv
