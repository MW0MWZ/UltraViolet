// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
#include "devices/sab82532.h"

#include <algorithm>

namespace uv {

namespace {
// STAR
constexpr u8 kStarXfw = 0x40, kStarRfne = 0x20, kStarTec = 0x08, kStarCec = 0x04, kStarCts = 0x02;
// CMDR
constexpr u8 kCmdRmc = 0x80, kCmdRres = 0x40, kCmdRfrd = 0x20, kCmdXf = 0x08, kCmdXres = 0x01;
// ISR0 / ISR1
constexpr u8 kIsr0Tcd = 0x80, kIsr0Time = 0x40, kIsr0Rpf = 0x01;
constexpr u8 kIsr1Alls = 0x20, kIsr1Xpr = 0x01;
// RFC
constexpr u8 kRfcRfdf = 0x10, kRfcTcde = 0x01;
// Register offsets within a channel
constexpr unsigned kStar = 0x20, kCmdr = 0x20, kMode = 0x22, kTimr = 0x23, kXon = 0x24, kXoff = 0x25,
                   kTcr = 0x26, kDafo = 0x27, kRfc = 0x28, kRbcl = 0x2A, kRbch = 0x2B, kCcr0 = 0x2C,
                   kCcr1 = 0x2D, kCcr2 = 0x2E, kCcr3 = 0x2F, kTsax = 0x30, kTsar = 0x31, kXccr = 0x32,
                   kRccr = 0x33, kVstr = 0x34, kBgr = 0x34, kTic = 0x35, kMxn = 0x36, kMxf = 0x37, kGis = 0x38,
                   kIva = 0x38, kIpc = 0x39, kIsr0 = 0x3A, kImr0 = 0x3A, kIsr1 = 0x3B, kImr1 = 0x3B, kPvr = 0x3C,
                   kPis = 0x3D, kPim = 0x3D, kPcr = 0x3E, kCcr4 = 0x3F;
constexpr u8 kVersion = 0x02;  // VSTR: version 3.2, carrier present (CD = 0)
}  // namespace

Sab82532::Channel::Channel(Sab82532& c, SerialLine& l)
    : chip(c), line(l), tx_timer(c.sched_, [this] { tx_tick(); }), rx_timer(c.sched_, [this] { rx_tick(); }) {}

void Sab82532::Channel::reset() {
    rx.clear();
    sending.clear();
    xcount = 0;
    released = 0;
    tx_timer.cancel();
    rx_timer.cancel();
    mode = timr = tcr = dafo = rfc = 0;
    xon = 0x11;
    xoff = 0x13;
    ccr0 = ccr1 = ccr2 = ccr3 = ccr4 = bgr = tic = mxn = mxf = 0;
    tsax = tsar = xccr = rccr = xbcl = xbch = 0;
    isr0 = 0;
    isr1 = kIsr1Xpr | kIsr1Alls;
    imr0 = imr1 = 0xFF;
    tic_busy = false;
}

// Baud rate from the generator (BGR with CCR2 bits 7:6 as its bits 9:8):
// N in bits 5:0 and M in bits 9:6 divide the clock by 16 x (N + 1) x 2^M.
u64 Sab82532::Channel::char_cycles() const {
    const unsigned v = bgr | ((ccr2 & 0xC0u) << 2);
    const unsigned n = v & 0x3F, m = (v >> 6) & 0xF;
    double baud = kClockHz / 16.0 / ((n + 1) * static_cast<double>(1u << m));
    baud = std::clamp(baud, 50.0, 2000000.0);
    const unsigned bits = 1 + (8 - (dafo & 3)) + ((dafo & 4) ? 1 : 0) + ((dafo & 0x20) ? 2 : 1);
    return static_cast<u64>(chip.cpu_hz_ * bits / baud) + 1;
}

unsigned Sab82532::Channel::threshold() const {
    static constexpr unsigned kThreshold[4] = {1, 4, 16, 32};
    return kThreshold[(rfc >> 2) & 3];
}

void Sab82532::Channel::start_tx() {
    if (!tx_timer.active() && !sending.empty()) tx_timer.schedule_in(char_cycles());
}

void Sab82532::Channel::tx_tick() {
    if (sending.empty()) return;
    const u8 b = sending.front();
    sending.pop_front();
    if (line.transmit) line.transmit(b);
    if (!sending.empty()) {
        tx_timer.schedule_in(char_cycles());
        return;
    }
    tic_busy = false;
    isr1 |= kIsr1Xpr | kIsr1Alls;
    chip.update_irq();
}

void Sab82532::Channel::rx_tick() {
    const unsigned per_char = (rfc & kRfcRfdf) ? 2 : 1;
    if (line.rx_pending()) {
        if (rx.size() + per_char <= 32) {
            const u8 b = line.rx_take();
            rx.push_back(b);
            if (per_char == 2) rx.push_back(0);  // status: no errors
            if ((rfc & kRfcTcde) && b == tcr) {
                release_for_read(false);
            } else if (rx.size() >= threshold() && released == 0) {
                released = threshold();
                isr0 |= kIsr0Rpf;
            }
            chip.update_irq();
        }
        rx_timer.schedule_in(char_cycles());
        return;
    }
    // The line has gone quiet: data below the threshold is offered after
    // the timeout.
    if (!rx.empty() && released == 0) release_for_read(true);
}

void Sab82532::Channel::release_for_read(bool timeout) {
    released = static_cast<unsigned>(rx.size());
    isr0 |= timeout ? kIsr0Time : kIsr0Tcd;
    chip.update_irq();
}

void Sab82532::Channel::command(u8 cmd) {
    if (cmd & kCmdXres) {
        sending.clear();
        tx_timer.cancel();
        xcount = 0;
        isr1 |= kIsr1Xpr | kIsr1Alls;
    }
    if (cmd & kCmdRres) {
        rx.clear();
        released = 0;
    }
    if (cmd & kCmdRmc) {
        // Receive message complete: the block offered by the last RPF, TCD or
        // TIME has been taken; whatever of it was not read is discarded.
        const unsigned n = std::min<unsigned>(released, static_cast<unsigned>(rx.size()));
        rx.erase(rx.begin(), rx.begin() + n);
        released = 0;
        if (rx.size() >= threshold()) {
            released = threshold();
            isr0 |= kIsr0Rpf;
        }
        if (!rx_timer.active() && (line.rx_pending() || !rx.empty())) rx_timer.schedule_in(char_cycles());
    }
    if (cmd & kCmdRfrd) {
        if (!rx.empty()) release_for_read(false);
    }
    if (cmd & kCmdXf) {
        for (unsigned i = 0; i < xcount; ++i) sending.push_back(xfifo[i]);
        xcount = 0;
        isr1 &= static_cast<u8>(~(kIsr1Xpr | kIsr1Alls));
        start_tx();
        if (sending.empty()) isr1 |= kIsr1Xpr | kIsr1Alls;
    }
    chip.update_irq();
}

bool Sab82532::Channel::read(unsigned reg, u8& v) {
    if (reg < 0x20) {
        // The RFIFO reads in order at any address of its window.
        if (rx.empty()) {
            v = 0;
        } else {
            v = rx.front();
            rx.pop_front();
            if (released) --released;
        }
        return true;
    }
    switch (reg) {
        case kStar: {
            v = kStarCts;
            if (xcount < 32 && sending.empty()) v |= kStarXfw;
            if (!rx.empty()) v |= kStarRfne;
            if (tic_busy) v |= kStarTec;
            (void)kStarCec;  // commands complete at once
            return true;
        }
        case kMode: v = mode; return true;
        case kTimr: v = timr; return true;
        case kXon: v = xon; return true;
        case kXoff: v = xoff; return true;
        case kTcr: v = tcr; return true;
        case kDafo: v = dafo; return true;
        case kRfc: v = rfc; return true;
        case kRbcl: v = static_cast<u8>(released ? released : rx.size()); return true;
        case kRbch: v = 0; return true;
        case kCcr0: v = ccr0; return true;
        case kCcr1: v = ccr1; return true;
        case kCcr2: v = ccr2; return true;
        case kCcr3: v = ccr3; return true;
        case kVstr: v = kVersion; return true;
        case kIsr0:
            v = isr0;
            isr0 = 0;
            chip.update_irq();
            return true;
        case kIsr1:
            v = isr1;
            isr1 = 0;  // the event bits clear on read
            chip.update_irq();
            return true;
        case kCcr4: v = ccr4; return true;
        default: v = 0; return true;
    }
}

void Sab82532::Channel::write(unsigned reg, u8 v) {
    if (reg < 0x20) {
        xfifo[reg] = v;
        xcount = std::max(xcount, reg + 1);
        return;
    }
    switch (reg) {
        case kCmdr: command(v); break;
        case kMode: mode = v; break;
        case kTimr: timr = v; break;
        case kXon: xon = v; break;
        case kXoff: xoff = v; break;
        case kTcr: tcr = v; break;
        case kDafo: dafo = v; break;
        case kRfc: rfc = v; break;
        case kRbcl: xbcl = v; break;  // XBCL on the write side
        case kRbch: xbch = v; break;
        case kCcr0: ccr0 = v; break;
        case kCcr1: ccr1 = v; break;
        case kCcr2: ccr2 = v; break;
        case kCcr3: ccr3 = v; break;
        case kTsax: tsax = v; break;
        case kTsar: tsar = v; break;
        case kXccr: xccr = v; break;
        case kRccr: rccr = v; break;
        case kBgr: bgr = v; break;
        case kTic:
            tic = v;
            tic_busy = true;
            sending.push_front(v);
            start_tx();
            break;
        case kMxn: mxn = v; break;
        case kMxf: mxf = v; break;
        case kImr0:
            imr0 = v;
            chip.update_irq();
            break;
        case kImr1:
            imr1 = v;
            chip.update_irq();
            break;
        case kCcr4: ccr4 = v; break;
        default: break;
    }
}

Sab82532::Sab82532(Scheduler& scheduler, SerialLine& a, SerialLine& b, double cpu_hz)
    : sched_(scheduler), cpu_hz_(cpu_hz) {
    ch_[0] = std::make_unique<Channel>(*this, a);
    ch_[1] = std::make_unique<Channel>(*this, b);
    reset();
}

void Sab82532::reset() {
    for (auto& c : ch_) c->reset();
    ipc_ = iva_ = pvr_ = pcr_ = pis_ = 0;
    pim_ = 0xFF;
    update_irq();
}

void Sab82532::poll_lines() {
    for (auto& c : ch_)
        if (!c->rx_timer.active() && c->line.rx_pending()) c->rx_timer.schedule_in(c->char_cycles());
}

void Sab82532::update_irq() {
    bool pending = false;
    for (auto& c : ch_)
        if ((c->isr0 & ~c->imr0) || (c->isr1 & ~c->imr1)) pending = true;
    if (irq_) irq_(pending);
}

bool Sab82532::read(u64 offset, unsigned, u64& value) {
    const unsigned reg = static_cast<unsigned>(offset & 0x3F);
    Channel& c = *ch_[(offset >> 6) & 1];
    switch (reg) {
        case kGis: {
            u8 g = 0;
            if (ch_[0]->isr0 & ~ch_[0]->imr0) g |= 0x04;
            if (ch_[0]->isr1 & ~ch_[0]->imr1) g |= 0x08;
            if (ch_[1]->isr0 & ~ch_[1]->imr0) g |= 0x01;
            if (ch_[1]->isr1 & ~ch_[1]->imr1) g |= 0x02;
            if (pis_ & ~pim_) g |= 0x80;
            value = g;
            return true;
        }
        case kIpc: value = ipc_; return true;
        case kPvr: value = pvr_; return true;
        case kPis:
            value = pis_;
            pis_ = 0;
            return true;
        case kPcr: value = pcr_; return true;
        default: {
            u8 v;
            c.read(reg, v);
            value = v;
            return true;
        }
    }
}

bool Sab82532::write(u64 offset, unsigned, u64 value) {
    const unsigned reg = static_cast<unsigned>(offset & 0x3F);
    const u8 v = static_cast<u8>(value);
    Channel& c = *ch_[(offset >> 6) & 1];
    switch (reg) {
        case kIva: iva_ = v; return true;
        case kIpc: ipc_ = v; return true;
        case kPvr: pvr_ = v; return true;
        case kPim: pim_ = v; return true;
        case kPcr: pcr_ = v; return true;
        default: c.write(reg, v); return true;
    }
}

}  // namespace uv
