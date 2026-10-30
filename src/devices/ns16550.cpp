// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
#include "devices/ns16550.h"

#include <algorithm>

namespace uv {

namespace {
constexpr u8 kLsrDr = 0x01, kLsrOe = 0x02, kLsrPe = 0x04, kLsrFe = 0x08, kLsrBi = 0x10, kLsrThre = 0x20,
             kLsrTemt = 0x40, kLsrFifoError = 0x80;
constexpr u8 kIerRda = 0x01, kIerThre = 0x02, kIerRls = 0x04, kIerMs = 0x08;
constexpr u8 kMcrLoop = 0x10;
constexpr unsigned kFifoDepth = 16;
}  // namespace

Ns16550::Ns16550(Scheduler& scheduler, SerialLine& line, double clock_hz, double cpu_hz)
    : line_(line), clock_hz_(clock_hz), cpu_hz_(cpu_hz),
      tx_timer_(scheduler, [this] { tx_done(); }), rx_timer_(scheduler, [this] { rx_tick(); }) {
    reset();
}

void Ns16550::reset() {
    rx_fifo_.clear();
    tx_fifo_.clear();
    tx_timer_.cancel();
    rx_timer_.cancel();
    ier_ = lcr_ = mcr_ = scr_ = fcr_ = 0;
    lsr_ = kLsrThre | kLsrTemt;
    msr_ = msr_inputs_;
    divisor_ = 12;
    rx_wire_ = {}, tx_wire_ = {};
    rx_streaming_ = tx_streaming_ = false;
    thre_pending_ = false;
    rx_timeout_ = false;
    update_irq();
}

LineFormat Ns16550::format() const {
    LineFormat f;
    f.baud = clock_hz_ / (16.0 * std::max<u16>(divisor_, 1));
    f.data_bits = 5 + (lcr_ & 3);
    f.parity = lcr_ & 8;
    f.odd = !(lcr_ & 0x10);
    f.stop_bits = (lcr_ & 4) ? 2 : 1;  // (1.5 with 5 data bits: taken as 2)
    return f;
}

u64 Ns16550::char_cycles() const { return static_cast<u64>(cpu_hz_ * format().frame_seconds()) + 1; }

void Ns16550::set_modem_inputs(bool cts, bool dsr, bool ri, bool dcd) {
    const u8 in = static_cast<u8>((cts ? 0x10 : 0) | (dsr ? 0x20 : 0) | (ri ? 0x40 : 0) | (dcd ? 0x80 : 0));
    if (mcr_ & kMcrLoop) {
        msr_inputs_ = in;
        return;
    }
    const u8 changed = (msr_ ^ in) & 0xF0;
    u8 delta = static_cast<u8>(((changed & 0x10) ? 1 : 0) | ((changed & 0x20) ? 2 : 0) | ((changed & 0x80) ? 8 : 0));
    if ((msr_ & 0x40) && !(in & 0x40)) delta |= 4;  // trailing edge of RI
    msr_ = static_cast<u8>(in | (msr_ & 0x0F) | delta);
    msr_inputs_ = in;
    update_irq();
}

// Received data interrupts (as IIR shows it) at the FIFO's trigger level,
// or with less after the timeout; without the FIFO, at every character.
bool Ns16550::rx_interrupt() const {
    static constexpr unsigned kTrigger[4] = {1, 4, 8, 14};
    if (!(ier_ & kIerRda) || !(lsr_ & kLsrDr)) return false;
    return rx_timeout_ || !(fcr_ & 1) || rx_fifo_.size() >= kTrigger[fcr_ >> 6];
}

void Ns16550::update_irq() {
    bool pending = false;
    if ((ier_ & kIerRls) && (lsr_ & 0x1E)) pending = true;
    if (rx_interrupt()) pending = true;
    if ((ier_ & kIerThre) && thre_pending_) pending = true;
    if ((ier_ & kIerMs) && (msr_ & 0x0F)) pending = true;
    if (irq_) irq_(pending && (mcr_ & 0x08) /* OUT2 gates the interrupt */);
}

void Ns16550::tx_done() {
    if (tx_fifo_.empty()) return;
    const u8 b = tx_fifo_.front();
    tx_fifo_.pop_front();
    if (mcr_ & kMcrLoop) {
        rx_push(b, 0);
    } else if (line_.device.baud > 0) {
        // To the device as it samples the UART's characters; it takes those
        // that arrive whole.
        std::vector<WireChar> got;
        tx_wire_.send(b, format(), tx_streaming_, line_.device, got);
        if (tx_fifo_.empty()) tx_wire_.idle(line_.device, got);
        for (const WireChar& c : got)
            if (!c.framing_error && !c.parity_error && line_.transmit) line_.transmit(c.byte);
    } else if (line_.transmit) {
        line_.transmit(b);
    }
    tx_streaming_ = !tx_fifo_.empty();
    if (tx_fifo_.empty()) {
        lsr_ |= kLsrThre | kLsrTemt;
        thre_pending_ = true;
    } else {
        tx_timer_.schedule_in(char_cycles());
    }
    update_irq();
}

// A character into the receive FIFO (or lost to an overrun). Its errors
// show in LSR once it is at the top.
void Ns16550::rx_push(u8 byte, u8 errors) {
    if (rx_fifo_.size() < ((fcr_ & 1) ? kFifoDepth : 1)) {
        rx_fifo_.push_back({byte, errors});
        if (rx_fifo_.size() == 1) rx_top_errors();
    } else {
        lsr_ |= kLsrOe;
    }
    lsr_ |= kLsrDr;
    rx_timeout_ = false;
}

void Ns16550::rx_top_errors() {
    if (!rx_fifo_.empty()) lsr_ |= rx_fifo_.front().errors;
}

u64 Ns16550::device_cycles() const {
    return line_.device.baud > 0 ? static_cast<u64>(cpu_hz_ * line_.device.frame_seconds()) + 1 : char_cycles();
}

void Ns16550::poll_line() {
    if (!rx_timer_.active() && line_.rx_pending() && !(mcr_ & kMcrLoop)) rx_timer_.schedule_in(device_cycles());
}

void Ns16550::rx_tick() {
    if (line_.rx_break() && !(mcr_ & kMcrLoop)) {
        // A break: one zero character, with BI (and the framing error a
        // zero stop bit is).
        line_.rx_take();
        rx_wire_ = {};
        rx_streaming_ = false;
        rx_push(0, kLsrBi | kLsrFe);
        rx_timer_.schedule_in(device_cycles());
    } else if (line_.rx_pending() && !(mcr_ & kMcrLoop)) {
        const u8 b = line_.rx_take();
        if (line_.device.baud > 0) {
            // The device's character as the UART samples it at its own rate:
            // none, one or several characters, with their errors.
            std::vector<WireChar> got;
            rx_wire_.send(b, line_.device, rx_streaming_, format(), got);
            for (const WireChar& c : got)
                rx_push(c.byte, static_cast<u8>((c.parity_error ? kLsrPe : 0) | (c.framing_error ? kLsrFe : 0) |
                                                (c.brk ? kLsrBi : 0)));
            rx_streaming_ = true;
        } else {
            rx_push(b, 0);
        }
        rx_timer_.schedule_in(device_cycles());
    } else if (rx_streaming_) {
        // The device's line idles: what the UART had started finishes.
        std::vector<WireChar> got;
        rx_wire_.idle(format(), got);
        for (const WireChar& c : got)
            rx_push(c.byte, static_cast<u8>((c.parity_error ? kLsrPe : 0) | (c.framing_error ? kLsrFe : 0) |
                                            (c.brk ? kLsrBi : 0)));
        rx_streaming_ = false;
        if (!rx_fifo_.empty() && (fcr_ & 1)) rx_timer_.schedule_in(4 * char_cycles());
    } else if (!rx_fifo_.empty() && (fcr_ & 1)) {
        // No more characters: the FIFO timeout raises the interrupt for
        // data below the trigger level (after four character times).
        rx_timeout_ = true;
    }
    update_irq();
}

// Every register but the receive buffer and the interrupt identification
// reads without side effects, while no error or modem status bit is
// waiting to be cleared by the read; their values change only by writes,
// scheduled events (transmit and receive timing) and input.
u64 Ns16550::stable_until(u64 offset, unsigned) const {
    const bool dlab = lcr_ & 0x80;
    switch (offset & 7) {
        case 0: return dlab ? kForever : 0;
        case 2: return 0;
        case 5: return (lsr_ & 0x1E) ? 0 : kForever;
        case 6: return (mcr_ & kMcrLoop) || !(msr_ & 0x0F) ? kForever : 0;
        default: return kForever;
    }
}

bool Ns16550::read(u64 offset, unsigned, u64& value) {
    const bool dlab = lcr_ & 0x80;
    switch (offset & 7) {
        case 0:
            if (dlab) {
                value = divisor_ & 0xFF;
                break;
            }
            if (rx_fifo_.empty()) {
                value = 0;
            } else {
                value = rx_fifo_.front().byte;
                rx_fifo_.pop_front();
                rx_top_errors();
            }
            if (rx_fifo_.empty()) {
                lsr_ &= static_cast<u8>(~kLsrDr);
                rx_timeout_ = false;
            }
            poll_line();
            update_irq();
            break;
        case 1: value = dlab ? (divisor_ >> 8) : ier_; break;
        case 2: {
            u8 iir = 0x01;
            if ((ier_ & kIerRls) && (lsr_ & 0x1E)) iir = 0x06;
            else if (rx_interrupt()) iir = rx_timeout_ ? 0x0C : 0x04;
            if (iir == 0x01 && (ier_ & kIerThre) && thre_pending_) {
                iir = 0x02;
                thre_pending_ = false;  // reading IIR acknowledges it
            } else if (iir == 0x01 && (ier_ & kIerMs) && (msr_ & 0x0F)) {
                iir = 0x00;
            }
            value = iir | ((fcr_ & 1) ? 0xC0 : 0);
            update_irq();
            break;
        }
        case 3: value = lcr_; break;
        case 4: value = mcr_; break;
        case 5: {
            value = lsr_;
            // FIFO mode: some character in the FIFO has an error.
            if (fcr_ & 1)
                for (const RxChar& c : rx_fifo_)
                    if (c.errors) value |= kLsrFifoError;
            lsr_ &= static_cast<u8>(~0x1E);  // the error bits clear on read
            update_irq();
            break;
        }
        case 6:
            if (mcr_ & kMcrLoop) {
                value = static_cast<u8>(((mcr_ & 2) ? 0x10 : 0) | ((mcr_ & 1) ? 0x20 : 0) | ((mcr_ & 4) ? 0x40 : 0) |
                                        ((mcr_ & 8) ? 0x80 : 0));
            } else {
                value = msr_;
                msr_ &= 0xF0;
                update_irq();
            }
            break;
        default: value = scr_; break;
    }
    return true;
}

bool Ns16550::write(u64 offset, unsigned, u64 value) {
    const u8 v = static_cast<u8>(value);
    const bool dlab = lcr_ & 0x80;
    switch (offset & 7) {
        case 0:
            if (dlab) {
                divisor_ = static_cast<u16>((divisor_ & 0xFF00) | v);
                break;
            }
            if (tx_fifo_.size() < ((fcr_ & 1) ? kFifoDepth : 1)) tx_fifo_.push_back(v);
            lsr_ &= static_cast<u8>(~(kLsrThre | kLsrTemt));
            thre_pending_ = false;
            if (!tx_timer_.active()) tx_timer_.schedule_in(char_cycles());
            update_irq();
            break;
        case 1:
            if (dlab) {
                divisor_ = static_cast<u16>((divisor_ & 0x00FF) | (v << 8));
            } else {
                // Enabling the THRE interrupt with the THR empty raises it at once.
                if (!(ier_ & kIerThre) && (v & kIerThre) && (lsr_ & kLsrThre)) thre_pending_ = true;
                ier_ = v & 0x0F;
                update_irq();
            }
            break;
        case 2:
            if ((v & 1) != (fcr_ & 1)) {
                rx_fifo_.clear();
                tx_fifo_.clear();
            }
            if (v & 2) {
                rx_fifo_.clear();
                lsr_ &= static_cast<u8>(~kLsrDr);
                rx_timeout_ = false;
            }
            if (v & 4) {
                tx_fifo_.clear();
                tx_timer_.cancel();
                lsr_ |= kLsrThre | kLsrTemt;
            }
            fcr_ = v & 0xC1;
            update_irq();
            break;
        case 3: lcr_ = v; break;
        case 4:
            mcr_ = v & 0x1F;
            update_irq();
            break;
        case 5: break;  // LSR is read-only
        case 6: break;
        default: scr_ = v; break;
    }
    return true;
}

}  // namespace uv
