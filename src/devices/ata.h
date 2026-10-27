// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
// ATA/ATAPI: a channel with up to two devices (0 and 1) behind the command
// block registers (data, error/features, sector count, LBA low/mid/high,
// device, status/command) and the control block (alternate status and
// device control, drive address); and the SFF-8038i bus-master DMA engine
// that serves it (command, status and PRD table pointer). Register values
// are as on the ATA cable: the data register's 16-bit word holds the first
// byte of the stream in its low half.
//
// AtaDevice holds what every device keeps: the shadow task file (with the
// previous contents of each register for 48-bit commands), the command
// protocol states, and its PIO and DMA data phases. Subclasses supply
// commands and the bytes of each data phase.
#pragma once

#include <array>
#include <functional>
#include <string>
#include <vector>

#include "core/scheduler.h"
#include "core/types.h"

namespace uv {

class DiskImage;
class AtaChannel;

namespace ata {
// Status
constexpr u8 kBsy = 0x80, kDrdy = 0x40, kDf = 0x20, kDsc = 0x10, kDrq = 0x08, kErr = 0x01;
// Error
constexpr u8 kAmnf = 0x01, kAbrt = 0x04, kIdnf = 0x10, kUnc = 0x40;
// Device control
constexpr u8 kNien = 0x02, kSrst = 0x04, kHob = 0x80;
// Device register
constexpr u8 kDev = 0x10, kLba = 0x40;
}  // namespace ata

class AtaDevice {
public:
    AtaDevice(Scheduler& scheduler, double cpu_hz);
    virtual ~AtaDevice();
    AtaDevice(const AtaDevice&) = delete;
    AtaDevice& operator=(const AtaDevice&) = delete;

    virtual const char* name() const = 0;
    virtual bool packet_device() const { return false; }

    // Power-on or hardware reset: the device is ready at once with its
    // signature in the task file.
    void hard_reset();

protected:
    friend class AtaChannel;

    // A command written to this device (selected, not busy). Runs after the
    // command delay with BSY set; must end in a data phase, complete() or
    // abort_command().
    virtual void execute(u8 command) = 0;
    // Bytes of the current data phase, in stream order. Return false after
    // calling abort_command() (or fail()) when the data cannot be moved.
    virtual bool data_in(u8* dst, std::size_t length) = 0;
    virtual bool data_out(const u8* src, std::size_t length) = 0;
    // The data phase has moved its last byte. Default (ATA): ready, with an
    // interrupt after a DMA transfer or a PIO write.
    virtual void transfer_done(bool dma, bool to_host);
    // A PIO block of `length` bytes is about to be offered (packet devices
    // put the byte count and interrupt reason in the task file).
    virtual void before_drq(bool to_host, std::size_t length);
    // The task file after a reset (signature) and the state to return to.
    virtual void reset_state();
    // Status after a reset: ATA devices are ready; packet devices clear DRDY.
    virtual u8 reset_status() const { return ata::kDrdy | ata::kDsc; }

    // Protocol helpers for execute() and transfers.
    void complete(bool interrupt = true);
    void abort_command(u8 error = ata::kAbrt);
    void start_pio_in(u64 bytes, std::size_t block);
    void start_pio_out(u64 bytes, std::size_t block, bool interrupt_first = false);
    void start_dma(bool to_host, u64 bytes);
    // Runs `fn` after `us` microseconds with BSY set.
    void after(double us, std::function<void()> fn);
    void raise_irq();

    // Task file access for commands.
    u8 feature() const { return feature_[0]; }
    u8 feature_prev() const { return feature_[1]; }
    u8 count() const { return count_[0]; }
    u16 count48() const { return static_cast<u16>(count_[0] | (count_[1] << 8)); }
    u8 device_reg() const { return device_; }
    u8 lba_lo() const { return lba_lo_[0]; }
    u8 lba_mid() const { return lba_mid_[0]; }
    u8 lba_hi() const { return lba_hi_[0]; }
    u64 lba28() const;
    u64 lba48() const;
    void set_count(u8 v) { count_[0] = v; }
    void set_lba_regs(u8 lo, u8 mid, u8 hi) { lba_lo_[0] = lo, lba_mid_[0] = mid, lba_hi_[0] = hi; }
    void set_lba48_regs(u64 lba);
    void set_device_head(u8 head) { device_ = static_cast<u8>((device_ & 0xF0) | (head & 0x0F)); }
    void set_status(u8 v) { status_ = v; }
    void set_error(u8 v) { error_ = v; }
    u8 status() const { return status_; }

    Scheduler& sched_;
    double cpu_hz_;
    AtaChannel* channel_ = nullptr;
    unsigned unit_ = 0;

private:
    enum class Phase { Idle, Busy, PioIn, PioOut, DmaIn, DmaOut };

    // From the channel.
    void write_register(unsigned reg, u8 value);
    u8 read_register(unsigned reg, bool hob) const;
    void command(u8 cmd);
    void soft_reset_begin();
    void soft_reset_end();
    u32 pio_read(unsigned size);
    void pio_write(unsigned size, u32 value);
    bool dma_pending() const { return phase_ == Phase::DmaIn || phase_ == Phase::DmaOut; }
    bool dma_to_host() const { return phase_ == Phase::DmaIn; }
    u64 dma_remaining() const { return remaining_; }
    bool dma_move(u8* data, std::size_t length);  // false: the device ended the command
    void dma_end();

    void pio_in_block();
    void pio_out_block(bool interrupt);
    void block_written();

    Timer timer_;
    std::function<void()> next_;
    Phase phase_ = Phase::Idle;
    std::vector<u8> buf_;
    std::size_t buf_pos_ = 0;
    std::size_t block_ = 512;
    u64 remaining_ = 0;  // bytes of the data phase not yet in buf_ (or moved by DMA)

    u8 feature_[2]{}, count_[2]{}, lba_lo_[2]{}, lba_mid_[2]{}, lba_hi_[2]{};
    u8 device_ = 0, status_ = 0, error_ = 0;
    bool irq_ = false;
};

// One ATA channel: two device positions, the register decode, INTRQ, and
// the bus-master engine for it.
class AtaChannel {
public:
    AtaChannel(Scheduler& scheduler, double cpu_hz);
    ~AtaChannel();

    // Connects a device at position 0 or 1 (nullptr: empty).
    void attach(unsigned unit, AtaDevice* device);
    AtaDevice* device(unsigned unit) const { return dev_[unit & 1]; }
    void reset();

    // Command block (reg 0-7) and control block (reg 2: alternate status /
    // device control; reg 3: drive address).
    u32 read_command(unsigned reg, unsigned size);
    void write_command(unsigned reg, unsigned size, u32 value);
    u8 read_control(unsigned reg);
    void write_control(unsigned reg, u8 value);

    // Bus-master registers (0: command, 2: status, 4-7: PRD table
    // address); bytes 1 and 3 belong to the controller.
    u8 bm_read(unsigned reg) const;
    void bm_write(unsigned reg, u8 value);

    // INTRQ of the selected device (gated by nIEN), and the controller's
    // memory access for DMA.
    bool interrupt() const { return line_; }
    void set_irq_sink(std::function<void(bool)> sink) { irq_sink_ = std::move(sink); }
    void set_dma(std::function<bool(u32, u8*, std::size_t)> read, std::function<bool(u32, const u8*, std::size_t)> write) {
        dma_read_ = std::move(read);
        dma_write_ = std::move(write);
    }
    // Bus-master transfer rate (bytes per second), for timing.
    void set_dma_rate(double bytes_per_second) { dma_rate_ = bytes_per_second; }

private:
    friend class AtaDevice;

    AtaDevice* selected() const { return dev_[select_]; }
    void update_irq();
    void dma_request();
    void dma_run();
    void execute_diagnostic();

    Scheduler& sched_;
    double cpu_hz_;
    std::array<AtaDevice*, 2> dev_{};
    unsigned select_ = 0;
    u8 control_ = 0;
    bool line_ = false;
    std::function<void(bool)> irq_sink_;

    u8 bm_cmd_ = 0, bm_status_ = 0;
    u32 bm_prd_ = 0;
    Timer bm_timer_;
    double dma_rate_ = 16.6e6;
    std::function<bool(u32, u8*, std::size_t)> dma_read_;
    std::function<bool(u32, const u8*, std::size_t)> dma_write_;
};

// An ATA hard disk on a raw image file (512-byte sectors): CHS and 28/48-bit
// LBA addressing, PIO (single and multiple), multiword and Ultra DMA, the
// sector buffer, power modes and the identification data.
class AtaDisk final : public AtaDevice {
public:
    AtaDisk(Scheduler& scheduler, double cpu_hz, DiskImage& image, std::string model = "UltraViolet ATA disk");
    const char* name() const override { return "ata-disk"; }

    u64 sectors() const { return sectors_; }
    // Default translation: up to 16383 cylinders, 16 heads, 63 sectors.
    unsigned cylinders() const { return cyl_; }

protected:
    void execute(u8 command) override;
    bool data_in(u8* dst, std::size_t length) override;
    bool data_out(const u8* src, std::size_t length) override;
    void transfer_done(bool dma, bool to_host) override;
    void reset_state() override;

private:
    enum class Source { Media, Buffer };
    // The command's first sector and count from the task file; false (with
    // the command aborted) if out of range.
    bool address(bool ext, u64& lba, u64& n);
    // The first sector alone (CHS checked against the current geometry).
    bool target(bool ext, u64& lba);
    void set_address(u64 lba, bool ext);
    void build_identify();
    void begin_media(u64 lba, u64 n, bool write, bool dma, bool ext, std::size_t block);
    void set_features();

    DiskImage& image_;
    std::string model_;
    u64 sectors_;
    unsigned cyl_ = 0, heads_ = 16, spt_ = 63;           // default translation
    unsigned cur_cyl_ = 0, cur_heads_ = 16, cur_spt_ = 63;  // INITIALIZE DEVICE PARAMETERS
    unsigned multiple_ = 0;                                // sectors per DRQ block for R/W MULTIPLE
    u8 pio_mode_ = 0x08, dma_mode_ = 0;                   // SET FEATURES transfer mode (count value)
    u8 power_ = 0xFF;                                      // CHECK POWER MODE: active
    bool write_cache_ = true;

    Source src_ = Source::Media;
    u64 pos_ = 0;           // byte position in the image or buffer
    u64 xfer_lba_ = 0, xfer_count_ = 0;
    bool xfer_ext_ = false;
    std::array<u8, 512> buffer_{};    // the sector buffer (READ/WRITE BUFFER)
    std::array<u8, 512> identify_{};
    std::array<u8, 512> staging_{};   // what a buffer data-in phase returns
};

// ATA strings are stored two characters per word, first character in the
// word's upper half, padded with spaces.
void ata_put_string(u8* words, unsigned first_word, unsigned word_count, const std::string& s);

}  // namespace uv
