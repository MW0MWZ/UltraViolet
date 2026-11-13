// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
#include "devices/fdc82077.h"

#include <algorithm>

#include "core/text.h"

#include "devices/ebus_dma.h"

namespace uv {

namespace {

// What the SuperIO's undriven data lines read, and the tape drive
// register's media sense with no media information (a real Ultra 10)
constexpr u8 kFloating = 0x76, kTdrMediaSense = 0x70;
// Main status register
constexpr u8 kMsrRqm = 0x80, kMsrDio = 0x40, kMsrNdm = 0x20, kMsrCb = 0x10;
// Digital output register
constexpr u8 kDorReset = 0x04;  // 0: held in reset
constexpr u8 kDorMotor0 = 0x10;
// ST0
constexpr u8 kSt0Abnormal = 0x40, kSt0Invalid = 0x80, kSt0SeekEnd = 0x20, kSt0EquipCheck = 0x10;
// ST1: end of track, overrun, no data, not writable, missing address mark
constexpr u8 kSt1Et = 0x80, kSt1Or = 0x10, kSt1Nd = 0x04, kSt1Nw = 0x02, kSt1Ma = 0x01;
// ST2: control mark, wrong cylinder, scan equal hit, scan not satisfied
constexpr u8 kSt2Cm = 0x40, kSt2Wc = 0x10, kSt2Seh = 0x08, kSt2Sns = 0x04;
// ST3: write protected, track 0; bits 5 and 3 always set
constexpr u8 kSt3Wp = 0x40, kSt3T0 = 0x10, kSt3Fixed = 0x28;
constexpr u64 kSeekCycles = 50000;  // time a recalibrate or seek with no drive takes to report
constexpr unsigned kFifo = 16;      // the FIFO's depth
constexpr unsigned kIdBytes = 60;   // from a sector's place to its data: ID field, gap 2, sync, mark
constexpr unsigned kHeads = 2, kCylinders = 80, kLastCylinder = 83;  // the drive's head stops at 83
constexpr double kBitsPerSecond[4] = {500000, 300000, 250000, 1000000};
}  // namespace

Fdc82077::Fdc82077(Scheduler& scheduler, double cpu_hz)
    : sched_(scheduler), cpu_hz_(cpu_hz),
      timer_(scheduler, [this] {
          busy_ = false;
          set_irq(true);
      }),
      xfer_(scheduler, [this] {
          if (pio_active()) pio_update();
          else if (op_ == Op::ReadId) finish(0, 0, 0);
          else if (op_ == Op::Format) sector_done();
          else if (op_ != Op::None && op_ != Op::Hang) sector_data();
      }),
      fail_(scheduler, [this] { finish(kSt0Abnormal, fail_st1_, fail_st2_); }) {
    reset();
}

void Fdc82077::reset() {
    dor_ = 0;
    tdr_ = 0;
    dsr_ = 0x02;
    ccr_ = 0x02;
    rate_ = 0x02;
    config_ = 0x20;
    pretrk_ = 0;
    lock_ = false;
    perpendicular_ = false;
    for (u8& p : pcn_) p = 0;
    aux_.reset();
    controller_reset();
}

// A software or hardware reset: the controller interrupts once it is ready,
// and then reports a status for each of its four drive positions.
void Fdc82077::controller_reset() {
    cmd_.clear();
    result_.clear();
    timer_.cancel();
    xfer_.cancel();
    fail_.cancel();
    op_ = Op::None;
    pio_on_ = false;
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

// --- The drive ------------------------------------------------------------

bool Fdc82077::insert(const std::string& path, std::string& error) {
    auto d = std::make_unique<DiskImage>();
    if (!d->open(path, false)) {
        error = "cannot open " + path;
        return false;
    }
    unsigned spt;
    u8 rate;
    switch (d->size()) {
        case 1474560: spt = 18, rate = 0; break;  // 1.44 MB, high density
        case 737280: spt = 9, rate = 2; break;    // 720 KB, double density
        default:
            error = path + " is not a 1.44 MB or 720 KB floppy disk image";
            return false;
    }
    eject();
    disk_ = std::move(d);
    spt_ = spt;
    media_rate_ = rate;
    changed_ = true;
    return true;
}

void Fdc82077::eject() {
    if (disk_) disk_->flush();
    disk_.reset();
    changed_ = true;
    // A command waiting on the disk now waits for an index pulse that never comes.
    if (op_ != Op::None && op_ != Op::Hang) {
        xfer_.cancel();
        pio_on_ = false;
        op_ = Op::Hang;
    }
}

// (in whole cycles, so that when a byte is due and whether it has come agree)
u64 Fdc82077::byte_cycles() const {
    return std::max<u64>(1, static_cast<u64>(8.0 / kBitsPerSecond[rate_ & 3] * cpu_hz_ + 0.5));
}
u64 Fdc82077::revolution() const { return static_cast<u64>(0.2 * cpu_hz_); }  // 300 rpm
bool Fdc82077::selected() const {
    const unsigned sel = dor_ & 3;
    if (!(dor_ & (kDorMotor0 << sel))) return false;  // a drive select is gated by its motor bit
    const bool swap = exchange_ && exchange_() && sel < 2;
    return (swap ? sel ^ 1 : sel) == 0;
}
bool Fdc82077::spinning() const { return disk_ && selected(); }
bool Fdc82077::readable() const { return spinning() && mfm_ && rate_ == media_rate_; }

// The next time sector r's data begins under the head: the sectors evenly
// around the track from the index, which passes at each multiple of a
// revolution.
u64 Fdc82077::slot_time(unsigned r) const {
    const u64 rev = revolution(), now = sched_.now();
    u64 t = now - now % rev + rev * (r - 1) / spt_ + kIdBytes * byte_cycles();
    while (t < now) t += rev;
    return t;
}

u64 Fdc82077::offset(unsigned cyl, unsigned head, unsigned r) const {
    return ((u64{cyl} * kHeads + head) * spt_ + (r - 1)) * kSector;
}

// A seek or recalibrate: the head stepped at the SPECIFY step rate (16 - SRT
// milliseconds at 500 kbit/s, in proportion at the others); a recalibrate
// gives up after 80 steps short of track 0.
void Fdc82077::seek(unsigned drive, unsigned head, unsigned target, bool recalibrate) {
    pending_sense_ = 0;
    busy_ = true;
    if (!selected()) {
        // No drive answers: no track 0.
        if (!recalibrate) pcn_[drive] = static_cast<u8>(target);
        st0_seek_ = static_cast<u8>(kSt0Abnormal | kSt0SeekEnd | kSt0EquipCheck | drive);
        sense_st0_[drive] = st0_seek_;
        timer_.schedule_in(kSeekCycles);
        return;
    }
    unsigned steps;
    bool reached = true;
    if (recalibrate) {
        steps = std::min(cyl_, kCylinders);
        reached = steps == cyl_;
        cyl_ -= steps;
        pcn_[drive] = 0;
    } else {
        const unsigned to = std::min(target, kLastCylinder);
        steps = to > cyl_ ? to - cyl_ : cyl_ - to;
        cyl_ = to;
        pcn_[drive] = static_cast<u8>(target);
    }
    if (steps && disk_) changed_ = false;
    st0_seek_ = static_cast<u8>(kSt0SeekEnd | (reached ? 0 : kSt0Abnormal | kSt0EquipCheck) | (head << 2) | drive);
    sense_st0_[drive] = st0_seek_;
    const double step_ms = (16 - (specify_[0] >> 4)) * 500000.0 / kBitsPerSecond[rate_ & 3];
    timer_.schedule_in(std::max<u64>(1000, static_cast<u64>(steps * step_ms / 1000 * cpu_hz_)));
}

// --- Commands -------------------------------------------------------------

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
        case 0x16: return 9;  // verify
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
    const unsigned head = cmd_.size() > 1 ? (cmd_[1] >> 2) & 1 : 0;
    switch (c & 0x1F) {
        case 0x03:  // specify
            specify_[0] = cmd_[1];
            specify_[1] = cmd_[2];
            break;
        case 0x04: {  // sense drive status: ST3
            u8 st3 = static_cast<u8>(kSt3Fixed | (head << 2) | drive);
            if (selected()) {
                if (!disk_ || disk_->read_only()) st3 |= kSt3Wp;  // (no disk: the sensor sees no tab)
                if (cyl_ == 0) st3 |= kSt3T0;
            }
            result({st3});
            break;
        }
        case 0x07: seek(drive, 0, 0, true); break;           // recalibrate
        case 0x0F: seek(drive, head, cmd_[2], false); break;  // seek
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
        case 0x02: case 0x05: case 0x06: case 0x09: case 0x0A: case 0x0C: case 0x0D:
        case 0x11: case 0x16: case 0x19: case 0x1D: {
            if (selected()) {
                static constexpr struct {
                    u8 code;
                    Op op;
                } kOps[] = {{0x02, Op::ReadTrack}, {0x05, Op::Write},  {0x06, Op::Read},   {0x09, Op::WriteDeleted},
                            {0x0A, Op::ReadId},    {0x0C, Op::ReadDeleted}, {0x0D, Op::Format}, {0x11, Op::Scan},
                            {0x16, Op::Verify},    {0x19, Op::Scan},   {0x1D, Op::Scan}};
                for (const auto& o : kOps)
                    if (o.code == (c & 0x1F)) begin(o.op);
                break;
            }
            // Without a drive the command ends at once, abnormally, with
            // the controller not ready (ST0) and missing address mark (ST1).
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

// --- The execution phase --------------------------------------------------

void Fdc82077::begin(Op op) {
    const u8 c = cmd_[0];
    op_ = op;
    mt_ = c & 0x80;
    mfm_ = c & 0x40;
    sk_ = c & 0x20;
    head_ = (cmd_[1] >> 2) & 1;
    drive_ = cmd_[1] & 3;
    dma_mode_ = !(specify_[1] & 1);
    busy_ = true;
    st2_ = 0;
    tc_ = scan_hit_ = false;
    pio_on_ = false;
    host_n_ = 0;
    if (op == Op::Format) {
        n_ = cmd_[2];
        left_ = cmd_[3];
        c_ = static_cast<u8>(cyl_), h_ = static_cast<u8>(head_), r_ = 0;
        dtl_ = cmd_[5];  // the filler byte
        format_ids_.clear();
    } else if (op != Op::ReadId) {
        c_ = cmd_[2], h_ = cmd_[3], r_ = cmd_[4], n_ = cmd_[5], eot_ = cmd_[6], dtl_ = cmd_[8];
        left_ = (op == Op::Verify && (cmd_[1] & 0x80)) ? (cmd_[8] ? cmd_[8] : 256u) : 0;
        scan_kind_ = (c & 0x1F) == 0x11 ? 0 : (c & 0x1F) == 0x19 ? 1 : 2;
        if (op == Op::ReadTrack) r_ = 1;
        // An implied seek (CONFIGURE's EIS) to the command's cylinder first.
        if ((config_ & 0x40) && op != Op::ReadTrack) cyl_ = std::min<unsigned>(c_, kLastCylinder), pcn_[drive_] = c_;
    }
    // No disk turning: no index pulse ever comes, and the command waits.
    if (!spinning()) {
        op_ = Op::Hang;
        return;
    }
    const bool writes = op == Op::Write || op == Op::WriteDeleted || op == Op::Format;
    if (writes && disk_->read_only()) return fail_after(1000, kSt1Nw, 0);
    // At the wrong density or data rate no address mark is found in two revolutions.
    if (!readable() && op != Op::Format) return fail_after(2 * revolution(), kSt1Ma, 0);
    if (op == Op::ReadId) {
        // The next ID to come under the head.
        const u64 rev = revolution(), now = sched_.now();
        const unsigned k = static_cast<unsigned>(((now % rev) * spt_ + rev - 1) / rev) % spt_;
        r_ = static_cast<u8>(k + 1);
        c_ = static_cast<u8>(cyl_), h_ = static_cast<u8>(head_), n_ = 2;
        xfer_.schedule_at(slot_time(r_));
        return;
    }
    if (op == Op::Format) {
        // From the index, one revolution; the IDs come from the host as the sectors pass.
        if (dma_mode_) {
            format_ids_.resize(4u * left_);
            const std::size_t got = dma_ ? dma_->from_memory(format_ids_.data(), format_ids_.size()) : 0;
            if (got < format_ids_.size()) return fail_after(revolution() / spt_, kSt1Or, 0);
        } else {
            pio_on_ = true;
            data_t0_ = sched_.now();
        }
        const u64 now = sched_.now(), rev = revolution();
        xfer_.schedule_at(now - now % rev + 2 * rev);
        return;
    }
    next_sector();
}

bool Fdc82077::sector_matches(unsigned r) const {
    return c_ == cyl_ && h_ == head_ && n_ == 2 && r >= 1 && r <= spt_;
}

void Fdc82077::next_sector() {
    if (!sector_matches(r_)) {
        // Not found in two revolutions: no data, and a wrong cylinder if the
        // track's IDs name another one.
        return fail_after(2 * revolution(), kSt1Nd, c_ != cyl_ ? kSt2Wc : 0);
    }
    const u64 t = slot_time(r_);
    const bool to_host = op_ == Op::Read || op_ == Op::ReadDeleted || op_ == Op::ReadTrack;
    if (dma_mode_ || op_ == Op::Verify || (op_ == Op::ReadDeleted && sk_)) {
        xfer_.schedule_at(t + kSector * byte_cycles());
    } else if (to_host) {
        xfer_.schedule_at(t);  // the bytes come from then on
    } else {
        // Write or scan without DMA: the host fills the FIFO ahead of the sector.
        pio_on_ = true;
        data_t0_ = t;
        host_n_ = 0;
        pio_update();
    }
}

// A sector's data: with DMA, it has passed under the head and moves in one
// go (the channel's terminal count reached ends the command after it);
// without, a read's bytes begin to come.
void Fdc82077::sector_data() {
    const u64 at = offset(cyl_, head_, r_);
    switch (op_) {
        case Op::Read: case Op::ReadDeleted: case Op::ReadTrack: case Op::Verify: case Op::Scan:
            if (!disk_->read(at, buf_.data(), kSector)) return finish(kSt0Abnormal, kSt1Nd, st2_);
            break;
        default: break;
    }
    if (op_ == Op::ReadDeleted) st2_ |= kSt2Cm;  // every sector's mark is a normal one
    if (op_ == Op::Verify || (op_ == Op::ReadDeleted && sk_)) return sector_done();
    const bool to_host = op_ == Op::Read || op_ == Op::ReadDeleted || op_ == Op::ReadTrack;
    if (!dma_mode_) {
        if (to_host) {
            pio_on_ = true;
            data_t0_ = sched_.now();
            host_n_ = 0;
            pio_update();
        }
        return;
    }
    const std::size_t room = dma_ ? dma_->available(to_host) : 0;
    if (room == 0) return finish(kSt0Abnormal, kSt1Or, st2_);  // overrun (underrun): no DMA
    const std::size_t moved = to_host ? dma_->to_memory(buf_.data(), kSector) : dma_->from_memory(host_.data(), kSector);
    if (room <= kSector) tc_ = true;
    else if (moved < kSector) return finish(kSt0Abnormal, kSt1Or, st2_);
    if (!to_host) std::fill(host_.begin() + static_cast<std::ptrdiff_t>(moved), host_.end(), u8{0});
    sector_done();
}

// The non-DMA FIFO at the present time. A read's byte k arrives at
// data_t0_ + k byte times; one arriving with 16 untaken is an overrun. A
// write's byte k is wanted then; not given by then, an underrun. The
// controller asks for bytes (RQM, and the interrupt) while it has some
// to give or room for more.
void Fdc82077::pio_update() {
    if (!pio_active()) return;
    const u64 now = sched_.now();
    const u64 bc = byte_cycles();
    const bool to_host = op_ == Op::Read || op_ == Op::ReadDeleted || op_ == Op::ReadTrack;
    auto at = [&](unsigned k) { return data_t0_ + k * bc; };
    const unsigned come = now < data_t0_ ? 0 : static_cast<unsigned>(std::min<u64>(kSector, (now - data_t0_) / bc + 1));
    if (op_ == Op::Format) {
        const unsigned want = 4 * left_;
        set_irq(host_n_ < want);
        return;  // (the revolution's end takes what came)
    }
    if (to_host) {
        if (host_n_ >= kSector) {
            pio_on_ = false;
            return sector_done();
        }
        if (come > host_n_ + kFifo) return finish(kSt0Abnormal, kSt1Or, st2_);
        set_irq(come > host_n_);
        xfer_.schedule_at(come > host_n_ ? at(host_n_ + kFifo) : at(host_n_));
        return;
    }
    // A write (or scan) takes the host's bytes.
    if (host_n_ < kSector && now >= at(host_n_)) return finish(kSt0Abnormal, kSt1Or, st2_);
    if (host_n_ >= kSector) {
        if (now >= at(kSector)) {
            pio_on_ = false;
            return sector_done();
        }
        set_irq(false);
        xfer_.schedule_at(at(kSector));
        return;
    }
    const unsigned taken = now < data_t0_ ? 0 : come;
    set_irq(host_n_ < taken + kFifo);
    xfer_.schedule_at(at(host_n_));
}

void Fdc82077::sector_done() {
    if (op_ == Op::Format) {
        // The revolution is over: the sectors of the IDs the host gave
        // written with the filler byte.
        const bool all = dma_mode_ || host_n_ >= 4 * left_;
        if (!format_ids_.empty()) {
            std::array<u8, kSector> fill;
            fill.fill(dtl_);
            for (std::size_t i = 0; i + 3 < format_ids_.size(); i += 4) {
                const u8 r = format_ids_[i + 2], n = format_ids_[i + 3];
                if (readable() && n == 2 && r >= 1 && r <= spt_) disk_->write(offset(cyl_, head_, r), fill.data(), kSector);
                c_ = format_ids_[i], h_ = format_ids_[i + 1], r_ = r, n_ = n;
            }
        }
        pio_on_ = false;
        return all ? finish(0, 0, 0) : finish(kSt0Abnormal, kSt1Or, 0);
    }
    if (op_ == Op::Write || op_ == Op::WriteDeleted) {
        if (!disk_->write(offset(cyl_, head_, r_), host_.data(), kSector)) return finish(kSt0Abnormal, kSt1Nd, st2_);
    }
    if (op_ == Op::Scan) {
        // Disk data against the host's, a byte at a time: FF on either side matches.
        int order = 0;
        for (unsigned i = 0; i < kSector && order == 0; ++i) {
            const u8 d = buf_[i], h = host_[i];
            if (d == 0xFF || h == 0xFF || d == h) continue;
            order = d < h ? -1 : 1;
        }
        const bool met = scan_kind_ == 0 ? order == 0 : scan_kind_ == 1 ? order <= 0 : order >= 0;
        if (met) return finish(0, 0, static_cast<u8>(st2_ | (order == 0 ? kSt2Seh : 0)));
    }
    if (tc_) return finish(0, 0, st2_);
    if (op_ == Op::ReadDeleted && !sk_) return finish(kSt0Abnormal, 0, st2_);  // no further sectors read
    if (op_ == Op::Verify && left_ && --left_ == 0) return finish(0, 0, st2_);
    const unsigned step = op_ == Op::Scan ? std::max<unsigned>(1, dtl_) : 1;
    if (r_ + step > eot_) {
        if (mt_ && head_ == 0) {
            head_ = 1, h_ = 1, r_ = 1;
            return next_sector();
        }
        if (op_ == Op::Verify) return left_ ? finish(kSt0Abnormal, kSt1Nd, st2_) : finish(0, 0, st2_);
        if (op_ == Op::Scan) return finish(kSt0Abnormal, kSt1Et, static_cast<u8>(st2_ | kSt2Sns));
        return finish(kSt0Abnormal, kSt1Et, st2_);  // the end of the track without TC
    }
    r_ = static_cast<u8>(r_ + step);
    next_sector();
}

// The result phase. After a transfer that ended normally or at the end of
// the track, the address is the next sector's (the datasheet's table 4-10:
// past EOT, the next head with MT, else the next cylinder); after an error,
// the sector it happened at.
void Fdc82077::finish(u8 st0_ic, u8 st1, u8 st2) {
    u8 c = c_, h = h_, r = r_;
    const bool transferred = (st0_ic == 0 || st1 == kSt1Et) && op_ != Op::ReadId && op_ != Op::Format;
    if (transferred) {
        if (r_ < eot_) {
            r = static_cast<u8>(r_ + 1);
        } else if (!mt_) {
            c = static_cast<u8>(c_ + 1), r = 1;
        } else if (head_ == 0) {
            h = 1, r = 1;
        } else {
            c = static_cast<u8>(c_ + 1), h = 0, r = 1;
        }
    }
    xfer_.cancel();
    fail_.cancel();
    pio_on_ = false;
    result({static_cast<u8>(st0_ic | (head_ << 2) | drive_), st1, st2, c, h, r, n_});
    op_ = Op::None;
    busy_ = false;
    set_irq(true);
}

void Fdc82077::fail_after(u64 cycles, u8 st1, u8 st2) {
    xfer_.cancel();
    pio_on_ = false;
    op_ = Op::Hang;  // (searching: the FIFO does not end it)
    busy_ = true;
    fail_st1_ = st1;
    fail_st2_ = st2;
    fail_.schedule_in(cycles);
}

std::string Fdc82077::describe() const {
    std::string out;
    appendf(out, "    fdc: dor %02x op %d busy %d dma %d pio %d (%u of the sector from %llu) cmd %zu result %zu irq %d\n",
            dor_, static_cast<int>(op_), busy_, dma_mode_, pio_on_, host_n_, static_cast<unsigned long long>(data_t0_),
            cmd_.size(), result_.size(), irq_line_);
    appendf(out, "    fdc: aux %02x (density select %s)\n", aux_.select(), aux_.select() ? "high" : "medium");
    appendf(out, "    fdc: C/H/R/N %u/%u/%u/%u EOT %u head %u cyl %u mt %d disk %s now %llu\n", c_, h_, r_, n_, eot_,
            head_, cyl_, mt_, disk_ ? disk_->path().c_str() : "(none)", static_cast<unsigned long long>(sched_.now()));
    return out;
}

// --- Registers ------------------------------------------------------------

// The floppy auxiliary register, taken as one byte on every byte lane of
// its word: Solaris writes it little-endian (the byte at offset 0) and the
// PROM big-endian (offset 3), each 2 for high density.
bool Fdc82077::Aux::read(u64, unsigned size, u64& value) {
    const u8 reg = select_ | (fdc_.disk_ && fdc_.media_rate_ == 0 ? 1 : 0);
    value = 0;
    for (unsigned i = 0; i < size; ++i) value = (value << 8) | reg;
    return true;
}

bool Fdc82077::Aux::write(u64, unsigned size, u64 value) {
    u8 lanes = 0;
    for (unsigned i = 0; i < size; ++i) lanes |= static_cast<u8>(value >> (8 * i));
    select_ = lanes & 2;
    return true;
}

bool Fdc82077::read(u64 offset, unsigned, u64& value) {
    switch (offset & 7) {
        // In PC-AT mode, as the Ultra 5/10's PC87332 runs, status registers
        // A and B are not there and the data lines float (0x76 on
        // a real Ultra 10); nor are bits 6:0 of the digital input register, whose
        // bit 7 is the disk change line. The tape drive register gives
        // the media sense (no media information: 0x70 on a real Ultra 10) above
        // the tape drive select.
        case 0:
        case 1: value = kFloating; break;
        case 2: value = dor_; break;
        case 3: value = kTdrMediaSense | (tdr_ & 0x03); break;
        case 4: {  // main status register
            u8 msr = 0;
            if (!(dor_ & kDorReset)) {
                msr = 0;
            } else if (!result_.empty()) {
                msr = kMsrRqm | kMsrDio | kMsrCb;
            } else if (op_ != Op::None) {
                // The execution phase: without DMA, the FIFO's bytes asked for.
                pio_update();
                msr = kMsrCb | (dma_mode_ ? 0 : kMsrNdm);
                if (pio_active()) {
                    const bool to_host = op_ == Op::Read || op_ == Op::ReadDeleted || op_ == Op::ReadTrack;
                    if (irq_line_) msr |= kMsrRqm | (to_host ? kMsrDio : 0);
                }
            } else if (busy_) {
                msr = kMsrCb;
            } else {
                msr = kMsrRqm | (cmd_.empty() ? 0 : kMsrCb);
            }
            value = msr;
            break;
        }
        case 5:  // FIFO
            if (pio_active()) {
                pio_update();
                const bool to_host = op_ == Op::Read || op_ == Op::ReadDeleted || op_ == Op::ReadTrack;
                value = 0;
                if (pio_active() && to_host && irq_line_) {
                    value = buf_[host_n_++];
                    pio_update();
                }
            } else if (result_.empty()) {
                value = 0;
            } else {
                value = result_.front();
                result_.pop_front();
                if (op_ == Op::None && !pending_sense_ && !st0_seek_) set_irq(false);
            }
            break;
        case 7:  // DIR: the selected drive's disk change line
            value = kFloating & 0x7F;
            if (changed_ && selected()) value |= 0x80;
            break;
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
                xfer_.cancel();
                fail_.cancel();
                op_ = Op::None;
                pio_on_ = false;
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
            rate_ = dsr_ & 3;
            if (v & 0x80) controller_reset();
            break;
        case 5:
            if (op_ == Op::Hang && !fail_.active()) {
                // The host takes the controller out of its wait for the disk.
                finish(kSt0Abnormal, 0, 0);
            } else if (pio_active()) {
                pio_update();
                if (pio_active()) {
                    const bool to_host = op_ == Op::Read || op_ == Op::ReadDeleted || op_ == Op::ReadTrack;
                    if (op_ == Op::Format) {
                        if (host_n_ < 4 * left_) format_ids_.push_back(v), ++host_n_;
                    } else if (!to_host && irq_line_) {
                        host_[host_n_++] = v;
                        pio_update();
                    }
                }
            } else if ((dor_ & kDorReset) && result_.empty() && !busy_) {
                command_byte(v);
            }
            break;
        case 7:
            ccr_ = v & 3;
            rate_ = ccr_;
            break;
        default: break;
    }
    return true;
}

}  // namespace uv
