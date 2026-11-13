// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
// Intel 82077AA-compatible floppy disk controller (the SuperIO's, "fdthree"
// on the Ultra 5/10), in PC-AT mode: digital output (drive selects,
// motors, reset), tape drive, main status and data rate select,
// the FIFO data port, digital input (the disk change line) and
// configuration control; command, execution and result phases; and the
// reset interrupt with its four sense-interrupt results. The command set,
// status bits and result values follow the PC87332's datasheet (the
// Ultra 5/10's SuperIO).
//
// Its drive, a 3.5" high-density drive as the Ultra 5/10 has, is on the
// drive 0 select and motor lines (DR0, MTR0). As in a PC, the lines come
// from the digital output register: a drive select is active only with
// its motor bit set, and the SuperIO's logical drive exchange (FCR bit 4)
// swaps drives 0 and 1. A command's drive bits pick only the present
// cylinder register and what ST0 and ST3 report: the drive the DOR selects
// answers whatever a command names (Sun's own OpenBoot sends its commands
// to drive 1 with drive 0 selected). With no drive selected, a seek finds
// no track 0 and a transfer ends at once, as an empty controller's do. It holds a disk image: 1.44 MB (80
// cylinders, 2 heads, 18 sectors of 512 bytes, read at 500 kbit/s) or
// 720 KB (9 sectors, at 250 kbit/s). It turns at 300 rpm while its motor
// is on: each sector comes under the head at its place in the revolution
// and its bytes at the data rate, and the head steps at the SPECIFY step
// rate. The disk change line is set when a disk goes in or out and cleared
// by a step with a disk in. With no disk (or the motor off) a command
// that needs the disk waits for an index pulse that never comes, until
// the host writes to the FIFO. Ejecting is by hand (the drive's button).
//
// Data moves through the floppy's EBus DMA channel, whose terminal count
// ends a transfer, or, in non-DMA mode, through the FIFO a byte at a time
// (16 bytes deep): a byte not taken (or given) in time is an overrun
// (underrun). The image holds only what a normally formatted disk does:
// every sector's data mark is a normal one (a write of deleted data writes
// normal data), and a format writes the sectors whose IDs fit the image's
// geometry.
//
// Beside it on the EBus (0x720000) is the RIO's floppy auxiliary register,
// a 32-bit register of two bits (Solaris's fdreg.h): bit 1 the density
// select to the drive (written; Solaris and the PROM write 1, high
// density, and Solaris 0 for a 720 KB disk), bit 0 the drive's density
// sense, read from the disk in the drive. Nothing known reads bit 0; its
// polarity is taken as the older Sun auxio register's density bit (1 high
// density, as a 1.44 MB disk), with no disk 0. The drive reads a disk at
// the controller's data rate whatever the density select says. It is the
// RIO's own register, taking a 32-bit access whole. Solaris writes it
// little-endian and the PROM big-endian (2 at offset 0 and at offset 3 of
// the word), so it is taken as one byte on every byte lane.
#pragma once

#include <array>
#include <deque>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

#include "core/scheduler.h"
#include "mem/bus.h"
#include "storage/disk_image.h"

namespace uv {

class EbusDma;

class Fdc82077 final : public Device {
public:
    static constexpr unsigned kSector = 512;

    Fdc82077(Scheduler& scheduler, double cpu_hz);

    const char* name() const override { return "fdc82077"; }
    bool read(u64 offset, unsigned size, u64& value) override;
    bool write(u64 offset, unsigned size, u64 value) override;

    void reset();
    void set_irq_sink(std::function<void(bool)> sink) { irq_ = std::move(sink); }
    // The floppy's EBus DMA channel (its terminal count is the controller's TC).
    void set_dma(EbusDma* dma) { dma_ = dma; }
    // The SuperIO's logical drive exchange (FCR bit 4), as it is now.
    void set_exchange(std::function<bool()> exchange) { exchange_ = std::move(exchange); }

    // A disk image into the drive (false, with the reason: not a size of
    // disk the drive takes, or not opened); out of it.
    bool insert(const std::string& path, std::string& error);
    void eject();
    bool has_disk() const { return disk_ != nullptr; }
    std::string disk_path() const { return disk_ ? disk_->path() : std::string(); }
    // The controller's state, for a diagnostic report.
    std::string describe() const;
    // Reads and writes of the disk so far (for an activity light).
    u64 accesses() const { return disk_ ? disk_->accesses() : 0; }

    // The floppy auxiliary register (EBus 0x720000).
    class Aux final : public Device {
    public:
        explicit Aux(const Fdc82077& fdc) : fdc_(fdc) {}
        const char* name() const override { return "floppy-aux"; }
        bool read(u64 offset, unsigned size, u64& value) override;
        bool write(u64 offset, unsigned size, u64 value) override;
        void reset() { select_ = 0; }
        u8 select() const { return select_; }
    private:
        const Fdc82077& fdc_;
        u8 select_ = 0;   // bit 1, the density select
    };
    Aux& aux() { return aux_; }

private:
    enum class Op { None, Read, ReadDeleted, ReadTrack, Write, WriteDeleted, Verify, Scan, Format, ReadId, Hang };

    void controller_reset();
    void set_irq(bool on);
    void command_byte(u8 b);
    void execute();
    void result(std::initializer_list<u8> bytes);
    unsigned command_length(u8 cmd) const;

    // The drive and the disk
    u64 byte_cycles() const;         // a byte's time at the data rate
    u64 revolution() const;          // a revolution's time
    u64 slot_time(unsigned r) const; // the next time sector r's data begins under the head
    bool selected() const;           // the DOR selects the drive, its motor on
    bool spinning() const;           // a disk in and the drive selected
    bool readable() const;           // spinning, at the disk's density and data rate
    u64 offset(unsigned cyl, unsigned head, unsigned r) const;
    void seek(unsigned drive, unsigned head, unsigned target, bool recalibrate);

    // The execution phase of the commands that read or write the disk
    void begin(Op op);
    void next_sector();
    void sector_data();       // a sector's data has passed (DMA) or begins (non-DMA)
    void sector_done();       // a sector finished: on to the next, or the end
    void finish(u8 st0_ic, u8 st1, u8 st2);
    void fail_after(u64 cycles, u8 st1, u8 st2);
    bool sector_matches(unsigned r) const;
    void pio_update();        // the non-DMA FIFO's state at the present time
    bool pio_active() const { return op_ != Op::None && op_ != Op::Hang && !dma_mode_ && pio_on_; }

    Scheduler& sched_;
    double cpu_hz_;
    Timer timer_;      // reset and seek interrupts
    Timer xfer_;       // the execution phase's next event
    Timer fail_;       // a command's end in error, after its search
    u8 fail_st1_ = 0, fail_st2_ = 0;
    std::function<void(bool)> irq_;
    bool irq_line_ = false;
    EbusDma* dma_ = nullptr;
    std::function<bool()> exchange_;

    u8 dor_ = 0, tdr_ = 0, dsr_ = 0x02, ccr_ = 0x02;
    u8 rate_ = 0x02;             // the data rate last set (CCR or DSR): 0 500k, 1 300k, 2 250k, 3 1M
    std::vector<u8> cmd_;
    std::deque<u8> result_;
    bool busy_ = false;          // a command is in progress (execution phase)
    unsigned pending_sense_ = 0; // drives still to report after a reset or seek
    u8 sense_st0_[4]{};
    u8 pcn_[4]{};                // present cylinder per drive
    u8 config_ = 0x20, pretrk_ = 0, specify_[2]{0, 0};
    bool lock_ = false, perpendicular_ = false;
    u8 st0_seek_ = 0;            // ST0 to report for the last seek/recalibrate

    // The drive: its disk, the head's cylinder, the disk change line
    std::unique_ptr<DiskImage> disk_;
    unsigned spt_ = 18;          // sectors a track
    u8 media_rate_ = 0;          // the data rate the disk is read at
    unsigned cyl_ = 0;
    bool changed_ = true;

    // The command in its execution phase
    Op op_ = Op::None;
    bool mt_ = false, mfm_ = false, sk_ = false, dma_mode_ = false, tc_ = false;
    unsigned head_ = 0, drive_ = 0;
    u8 c_ = 0, h_ = 0, r_ = 0, n_ = 0, eot_ = 0, dtl_ = 0;
    u8 st2_ = 0;                 // ST2 bits gathered (CM, scan results)
    unsigned left_ = 0;          // verify's sectors to go (EC), format's IDs to take
    unsigned scan_kind_ = 0;     // 0 equal, 1 low or equal, 2 high or equal
    bool scan_hit_ = false;
    std::array<u8, kSector> buf_{}, host_{};
    // Non-DMA: the sector's bytes from data_t0_, one each byte time; how
    // many the host has taken (read) or given (write).
    bool pio_on_ = false;
    u64 data_t0_ = 0;
    unsigned host_n_ = 0;
    std::vector<u8> format_ids_;
    Aux aux_{*this};
};

}  // namespace uv
