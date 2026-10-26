// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
// Intel 82077AA-compatible floppy disk controller (the SuperIO's, "fdthree"
// on the Ultra 5/10): status registers A and B, digital output (drive
// selects, motors, reset), tape drive, main status and data rate select,
// the FIFO data port, digital input and configuration control; command,
// execution and result phases; and the reset interrupt with its four
// sense-interrupt results. No drives are connected yet: commands needing
// one end with an equipment check, as an empty controller does.
#pragma once

#include <deque>
#include <functional>
#include <initializer_list>
#include <vector>

#include "core/scheduler.h"
#include "mem/bus.h"

namespace uv {

class Fdc82077 final : public Device {
public:
    explicit Fdc82077(Scheduler& scheduler);

    const char* name() const override { return "fdc82077"; }
    bool read(u64 offset, unsigned size, u64& value) override;
    bool write(u64 offset, unsigned size, u64 value) override;

    void reset();
    void set_irq_sink(std::function<void(bool)> sink) { irq_ = std::move(sink); }

private:
    void controller_reset();
    void set_irq(bool on);
    void command_byte(u8 b);
    void execute();
    void result(std::initializer_list<u8> bytes);
    unsigned command_length(u8 cmd) const;

    Timer timer_;
    std::function<void(bool)> irq_;
    bool irq_line_ = false;

    u8 dor_ = 0, tdr_ = 0, dsr_ = 0x02, ccr_ = 0x02;
    std::vector<u8> cmd_;
    std::deque<u8> result_;
    bool busy_ = false;          // a command is in progress (execution phase)
    unsigned pending_sense_ = 0; // drives still to report after a reset or seek
    u8 sense_st0_[4]{};
    u8 pcn_[4]{};                // present cylinder per drive
    u8 config_ = 0x20, pretrk_ = 0, specify_[2]{0, 0};
    bool lock_ = false, perpendicular_ = false;
    u8 st0_seek_ = 0;            // ST0 to report for the last seek/recalibrate
};

}  // namespace uv
