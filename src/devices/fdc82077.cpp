// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
#include "devices/fdc82077.h"

namespace uv {

namespace {
// Main status register
constexpr u8 kMsrRqm = 0x80, kMsrDio = 0x40, kMsrCb = 0x10;
// Digital output register
constexpr u8 kDorReset = 0x04;  // 0: held in reset
// ST0
constexpr u8 kSt0Abnormal = 0x40, kSt0Invalid = 0x80, kSt0SeekEnd = 0x20, kSt0EquipCheck = 0x10;
constexpr u64 kSeekCycles = 50000;  // time a recalibrate or seek takes to report
}  // namespace

Fdc82077::Fdc82077(Scheduler& scheduler) : timer_(scheduler, [this] {
    busy_ = false;
    set_irq(true);
}) {
    reset();
}

void Fdc82077::reset() {
    dor_ = 0;
    tdr_ = 0;
    dsr_ = 0x02;
    ccr_ = 0x02;
    config_ = 0x20;
    pretrk_ = 0;
    lock_ = false;
    perpendicular_ = false;
    for (u8& p : pcn_) p = 0;
    controller_reset();
}

// A software or hardware reset: the controller interrupts once it is ready,
// and then reports a status for each of its four drive positions.
void Fdc82077::controller_reset() {
    cmd_.clear();
    result_.clear();
    timer_.cancel();
    busy_ = false;
    set_irq(false);
    if (!lock_) config_ = 0x20;
    for (unsigned d = 0; d < 4; ++d) sense_st0_[d] = static_cast<u8>(0xC0 | d);  // ready line changed
    pending_sense_ = 4;
    timer_.schedule_in(1000);
}

void Fdc82077::set_irq(bool on) {
    irq_line_ = on;
    // The interrupt reaches the system only with DMA/interrupt enable (DOR bit 3).
    if (irq_) irq_(on && (dor_ & 0x08));
}

unsigned Fdc82077::command_length(u8 cmd) const {
    switch (cmd & 0x1F) {
        case 0x02: return 9;  // read track
        case 0x03: return 3;  // specify
        case 0x04: return 2;  // sense drive status
        case 0x05: return 9;  // write data
        case 0x06: return 9;  // read data
        case 0x07: return 2;  // recalibrate
        case 0x08: return 1;  // sense interrupt status
        case 0x09: return 9;  // write deleted data
        case 0x0A: return 2;  // read ID
        case 0x0C: return 9;  // read deleted data
        case 0x0D: return 6;  // format track
        case 0x0E: return 1;  // dump registers
        case 0x0F: return 3;  // seek
        case 0x10: return 1;  // version
        case 0x11: return 9;  // scan equal
        case 0x12: return 2;  // perpendicular mode
        case 0x13: return 4;  // configure
        case 0x14: return 1;  // lock
        case 0x16: return 2;  // verify
        case 0x19: return 9;  // scan low or equal
        case 0x1D: return 9;  // scan high or equal
        default: return 1;    // invalid: answered with ST0 = 0x80
    }
}

void Fdc82077::result(std::initializer_list<u8> bytes) {
    for (u8 b : bytes) result_.push_back(b);
}

void Fdc82077::command_byte(u8 b) {
    cmd_.push_back(b);
    if (cmd_.size() < command_length(cmd_[0])) return;
    execute();
    cmd_.clear();
}

void Fdc82077::execute() {
    const u8 c = cmd_[0];
    const unsigned drive = cmd_.size() > 1 ? (cmd_[1] & 3) : 0;
    switch (c & 0x1F) {
        case 0x03:  // specify
            specify_[0] = cmd_[1];
            specify_[1] = cmd_[2];
            break;
        case 0x04:  // sense drive status: ST3 (no drive: not ready, not track 0)
            result({static_cast<u8>(drive | (cmd_[1] & 4))});
            break;
        case 0x07:  // recalibrate: no drive answers track 0
        case 0x0F:  // seek
            if ((c & 0x1F) == 0x0F) pcn_[drive] = cmd_[2];
            st0_seek_ = static_cast<u8>(kSt0Abnormal | kSt0SeekEnd | kSt0EquipCheck | drive);
            sense_st0_[drive] = st0_seek_;
            pending_sense_ = 0;
            busy_ = true;
            timer_.schedule_in(kSeekCycles);
            break;
        case 0x08:  // sense interrupt status
            set_irq(false);
            if (pending_sense_) {
                const unsigned d = 4 - pending_sense_;
                --pending_sense_;
                result({sense_st0_[d], pcn_[d]});
            } else if (st0_seek_) {
                result({st0_seek_, pcn_[st0_seek_ & 3]});
                st0_seek_ = 0;
            } else {
                result({kSt0Invalid});
            }
            break;
        case 0x0A:  // read ID: no media
        case 0x02:
        case 0x05:
        case 0x06:
        case 0x09:
        case 0x0C:
        case 0x0D:
        case 0x11:
        case 0x16:
        case 0x19:
        case 0x1D: {
            // Without a drive the command ends at once, abnormally, with
            // the controller not ready (ST0) and missing address mark (ST1).
            const u8 head = static_cast<u8>((cmd_[1] >> 2) & 1);
            const u8 st0 = static_cast<u8>(kSt0Abnormal | 0x08 /* not ready */ | (head << 2) | drive);
            const u8 cyl = cmd_.size() > 2 ? cmd_[2] : 0, hd = cmd_.size() > 3 ? cmd_[3] : 0;
            const u8 sec = cmd_.size() > 4 ? cmd_[4] : 0, n = cmd_.size() > 5 ? cmd_[5] : 0;
            result({st0, 0x01, 0x00, cyl, hd, sec, n});
            busy_ = true;
            timer_.schedule_in(1000);
            break;
        }
        case 0x0E:  // dump registers
            result({pcn_[0], pcn_[1], pcn_[2], pcn_[3], specify_[0], specify_[1], 0,
                    static_cast<u8>((lock_ ? 0x80 : 0) | (perpendicular_ ? 0x02 : 0)), config_, pretrk_});
            break;
        case 0x10: result({0x90}); break;  // version: enhanced controller
        case 0x12: perpendicular_ = cmd_[1] & 3; break;
        case 0x13:  // configure
            config_ = cmd_[2];
            pretrk_ = cmd_[3];
            break;
        case 0x14:  // lock / unlock
            lock_ = c & 0x80;
            result({static_cast<u8>(lock_ ? 0x10 : 0x00)});
            break;
        default: result({kSt0Invalid}); break;
    }
}

bool Fdc82077::read(u64 offset, unsigned, u64& value) {
    switch (offset & 7) {
        case 0: value = irq_line_ ? 0x80 : 0; break;  // SRA: interrupt pending
        case 1: value = 0xC0 | (dor_ & 0x01 ? 0x20 : 0) | ((dor_ >> 4) & 0x03); break;  // SRB
        case 2: value = dor_; break;
        case 3: value = tdr_; break;
        case 4: {  // main status register
            u8 msr = 0;
            if (!(dor_ & kDorReset)) {
                msr = 0;
            } else if (!result_.empty()) {
                msr = kMsrRqm | kMsrDio | kMsrCb;
            } else if (busy_) {
                msr = kMsrCb;
            } else {
                msr = kMsrRqm | (cmd_.empty() ? 0 : kMsrCb);
            }
            value = msr;
            break;
        }
        case 5:  // FIFO
            if (result_.empty()) {
                value = 0;
            } else {
                value = result_.front();
                result_.pop_front();
            }
            break;
        case 7: value = 0x7F & ~0x80; break;  // DIR: no disk change
        default: value = 0xFF; break;
    }
    return true;
}

bool Fdc82077::write(u64 offset, unsigned, u64 value) {
    const u8 v = static_cast<u8>(value);
    switch (offset & 7) {
        case 2: {
            const bool was_reset = !(dor_ & kDorReset);
            dor_ = v;
            if (!(v & kDorReset)) {
                cmd_.clear();
                result_.clear();
                set_irq(false);
            } else if (was_reset) {
                controller_reset();
            }
            set_irq(irq_line_);
            break;
        }
        case 3: tdr_ = v; break;
        case 4:  // DSR: bit 7 is a software reset
            dsr_ = v & 0x7F;
            if (v & 0x80) controller_reset();
            break;
        case 5:
            if ((dor_ & kDorReset) && result_.empty() && !busy_) command_byte(v);
            break;
        case 7: ccr_ = v & 3; break;
        default: break;
    }
    return true;
}

}  // namespace uv
