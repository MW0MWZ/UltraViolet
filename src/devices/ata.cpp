// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
#include "devices/ata.h"

#include <algorithm>
#include <cstring>

#include "core/endian.h"
#include "core/log.h"
#include "storage/disk_image.h"

namespace uv {

using namespace ata;

namespace {
constexpr double kCommandUs = 20.0;  // from the command write to the device acting on it
constexpr double kBlockUs = 5.0;     // between PIO blocks
constexpr double kResetUs = 50.0;    // a software reset or diagnostic, as far as the guest sees
constexpr unsigned kMaxMultiple = 16;

void put16(u8* b, unsigned word, u16 v) {
    b[2 * word] = static_cast<u8>(v);
    b[2 * word + 1] = static_cast<u8>(v >> 8);
}
}  // namespace

void ata_put_string(u8* words, unsigned first_word, unsigned word_count, const std::string& s) {
    for (unsigned i = 0; i < word_count * 2; ++i) {
        const char c = i < s.size() ? s[i] : ' ';
        // Character 2n is the upper byte of word n.
        words[2 * first_word + (i ^ 1)] = static_cast<u8>(c);
    }
}

// --- AtaDevice ---

AtaDevice::AtaDevice(Scheduler& scheduler, double cpu_hz)
    : sched_(scheduler), cpu_hz_(cpu_hz), timer_(scheduler, [this] {
          auto fn = std::move(next_);
          next_ = nullptr;
          if (fn) fn();
      }) {}

AtaDevice::~AtaDevice() = default;

void AtaDevice::hard_reset() {
    timer_.cancel();
    next_ = nullptr;
    phase_ = Phase::Idle;
    reset_state();
    status_ = reset_status();
    error_ = 0x01;  // diagnostic code: no error
    irq_ = false;
}

void AtaDevice::reset_state() {
    for (unsigned i = 0; i < 2; ++i) {
        feature_[i] = 0;
        count_[i] = 1;
        lba_lo_[i] = 1;
        lba_mid_[i] = lba_hi_[i] = 0;
    }
    device_ = 0;
}

void AtaDevice::soft_reset_begin() {
    timer_.cancel();
    next_ = nullptr;
    phase_ = Phase::Busy;
    status_ = kBsy;
    irq_ = false;
}

void AtaDevice::soft_reset_end() {
    after(kResetUs, [this] {
        phase_ = Phase::Idle;
        reset_state();
        status_ = reset_status();
        error_ = 0x01;
    });
}

void AtaDevice::write_register(unsigned reg, u8 value) {
    auto shift = [value](u8* r) {
        r[1] = r[0];
        r[0] = value;
    };
    switch (reg) {
        case 1: shift(feature_); break;
        case 2: shift(count_); break;
        case 3: shift(lba_lo_); break;
        case 4: shift(lba_mid_); break;
        case 5: shift(lba_hi_); break;
        case 6: device_ = value; break;
        default: break;
    }
}

u8 AtaDevice::read_register(unsigned reg, bool hob) const {
    const unsigned h = hob ? 1 : 0;
    switch (reg) {
        case 1: return error_;
        case 2: return count_[h];
        case 3: return lba_lo_[h];
        case 4: return lba_mid_[h];
        case 5: return lba_hi_[h];
        case 6: return device_;
        default: return status_;
    }
}

void AtaDevice::command(u8 cmd) {
    // DEVICE RESET reaches a packet device even while it is busy.
    if ((status_ & kBsy) && !(packet_device() && cmd == 0x08)) return;
    irq_ = false;
    error_ = 0;
    after(kCommandUs, [this, cmd] { execute(cmd); });
}

void AtaDevice::after(double us, std::function<void()> fn) {
    timer_.cancel();
    phase_ = Phase::Busy;
    status_ = kBsy;
    next_ = std::move(fn);
    timer_.schedule_in(static_cast<u64>(us * cpu_hz_ / 1e6) + 1);
}

void AtaDevice::raise_irq() {
    irq_ = true;
    if (channel_) channel_->update_irq();
}

void AtaDevice::complete(bool interrupt) {
    phase_ = Phase::Idle;
    status_ = kDrdy | kDsc;
    if (interrupt) raise_irq();
}

void AtaDevice::abort_command(u8 error) {
    timer_.cancel();
    next_ = nullptr;
    phase_ = Phase::Idle;
    error_ = error;
    status_ = kDrdy | kDsc | kErr;
    raise_irq();
}

void AtaDevice::transfer_done(bool dma, bool to_host) {
    // PIO reads end without an interrupt (the last block's was enough);
    // writes and DMA end with one.
    complete(dma || !to_host);
}

void AtaDevice::before_drq(bool, std::size_t) {}

void AtaDevice::start_pio_in(u64 bytes, std::size_t block) {
    remaining_ = bytes;
    block_ = std::max<std::size_t>(block, 2);
    if (bytes == 0) {
        phase_ = Phase::Idle;
        transfer_done(false, true);
        return;
    }
    pio_in_block();
}

void AtaDevice::pio_in_block() {
    const std::size_t n = static_cast<std::size_t>(std::min<u64>(remaining_, block_));
    buf_.resize(n);
    if (!data_in(buf_.data(), n)) return;
    remaining_ -= n;
    buf_pos_ = 0;
    phase_ = Phase::PioIn;
    before_drq(true, n);
    status_ = kDrdy | kDsc | kDrq;
    raise_irq();
}

void AtaDevice::start_pio_out(u64 bytes, std::size_t block, bool interrupt_first) {
    remaining_ = bytes;
    block_ = std::max<std::size_t>(block, 2);
    if (bytes == 0) {
        phase_ = Phase::Idle;
        transfer_done(false, false);
        return;
    }
    pio_out_block(interrupt_first);
}

void AtaDevice::pio_out_block(bool interrupt) {
    const std::size_t n = static_cast<std::size_t>(std::min<u64>(remaining_, block_));
    buf_.assign(n, 0);
    buf_pos_ = 0;
    remaining_ -= n;
    phase_ = Phase::PioOut;
    before_drq(false, n);
    status_ = kDrdy | kDsc | kDrq;
    if (interrupt) raise_irq();
}

void AtaDevice::block_written() {
    if (!data_out(buf_.data(), buf_.size())) return;
    if (remaining_) {
        pio_out_block(true);
    } else {
        phase_ = Phase::Idle;
        transfer_done(false, false);
    }
}

u32 AtaDevice::pio_read(unsigned size) {
    if (phase_ != Phase::PioIn) return 0;
    const unsigned n = size == 4 ? 4 : 2;  // a byte read still takes a whole word
    u32 v = 0;
    for (unsigned i = 0; i < n; ++i)
        if (buf_pos_ < buf_.size()) v |= u32{buf_[buf_pos_++]} << (8 * i);
    if (buf_pos_ >= buf_.size()) {
        if (remaining_) {
            after(kBlockUs, [this] { pio_in_block(); });
        } else {
            phase_ = Phase::Idle;
            transfer_done(false, true);
        }
    }
    return size == 1 ? (v & 0xFF) : v;
}

void AtaDevice::pio_write(unsigned size, u32 value) {
    if (phase_ != Phase::PioOut) return;
    const unsigned n = size == 4 ? 4 : 2;
    for (unsigned i = 0; i < n && buf_pos_ < buf_.size(); ++i) buf_[buf_pos_++] = static_cast<u8>(value >> (8 * i));
    if (buf_pos_ >= buf_.size()) after(kBlockUs, [this] { block_written(); });
}

void AtaDevice::start_dma(bool to_host, u64 bytes) {
    remaining_ = bytes;
    phase_ = to_host ? Phase::DmaIn : Phase::DmaOut;
    status_ = kDrdy | kDsc | kDrq;
    if (channel_) channel_->dma_request();
}

bool AtaDevice::dma_move(u8* data, std::size_t length) {
    const bool ok = phase_ == Phase::DmaIn ? data_in(data, length) : data_out(data, length);
    if (!ok) return false;
    remaining_ -= std::min<u64>(remaining_, length);
    return true;
}

void AtaDevice::dma_end() {
    const bool to_host = phase_ == Phase::DmaIn;
    phase_ = Phase::Idle;
    transfer_done(true, to_host);
}

u64 AtaDevice::lba28() const {
    return (u64{device_ & 0x0Fu} << 24) | (u64{lba_hi_[0]} << 16) | (u64{lba_mid_[0]} << 8) | lba_lo_[0];
}

u64 AtaDevice::lba48() const {
    return (u64{lba_hi_[1]} << 40) | (u64{lba_mid_[1]} << 32) | (u64{lba_lo_[1]} << 24) | (u64{lba_hi_[0]} << 16) |
           (u64{lba_mid_[0]} << 8) | lba_lo_[0];
}

void AtaDevice::set_lba48_regs(u64 lba) {
    lba_lo_[0] = static_cast<u8>(lba);
    lba_mid_[0] = static_cast<u8>(lba >> 8);
    lba_hi_[0] = static_cast<u8>(lba >> 16);
    lba_lo_[1] = static_cast<u8>(lba >> 24);
    lba_mid_[1] = static_cast<u8>(lba >> 32);
    lba_hi_[1] = static_cast<u8>(lba >> 40);
}

// --- AtaChannel ---

AtaChannel::AtaChannel(Scheduler& scheduler, double cpu_hz)
    : sched_(scheduler), cpu_hz_(cpu_hz), bm_timer_(scheduler, [this] { dma_run(); }) {}

AtaChannel::~AtaChannel() = default;

void AtaChannel::attach(unsigned unit, AtaDevice* device) {
    if (dev_[unit & 1]) dev_[unit & 1]->channel_ = nullptr;
    dev_[unit & 1] = device;
    if (device) {
        device->channel_ = this;
        device->unit_ = unit & 1;
        device->hard_reset();
    }
}

void AtaChannel::reset() {
    select_ = 0;
    control_ = 0;
    bm_cmd_ = bm_status_ = 0;
    bm_prd_ = 0;
    bm_timer_.cancel();
    for (AtaDevice* d : dev_)
        if (d) d->hard_reset();
    update_irq();
}

void AtaChannel::update_irq() {
    const AtaDevice* d = selected();
    const bool level = d && d->irq_ && !(control_ & kNien);
    if (level && !line_) bm_status_ |= 0x04;  // the bus master's interrupt bit latches INTRQ
    line_ = level;
    if (irq_sink_) irq_sink_(level);
}

u32 AtaChannel::read_command(unsigned reg, unsigned size) {
    AtaDevice* d = selected();
    if (reg == 0) return d ? d->pio_read(size) : 0;
    const bool hob = control_ & kHob;
    if (d) {
        const u8 v = d->read_register(reg, hob);
        if (reg == 7) {
            d->irq_ = false;  // reading Status acknowledges the interrupt
            update_irq();
        }
        return v;
    }
    // Device 0 answers for an absent device 1: Status reads zero, the other
    // registers as device 0 holds them.
    if (const AtaDevice* other = dev_[select_ ^ 1]) return reg == 7 ? 0 : other->read_register(reg, hob);
    return 0;
}

void AtaChannel::write_command(unsigned reg, unsigned size, u32 value) {
    const u8 v = static_cast<u8>(value);
    if (reg == 0) {
        if (AtaDevice* d = selected()) d->pio_write(size, value);
        return;
    }
    control_ &= static_cast<u8>(~kHob);
    if (reg < 7) {
        for (AtaDevice* d : dev_)
            if (d) d->write_register(reg, v);
        if (reg == 6) {
            select_ = (v & kDev) ? 1 : 0;
            update_irq();
        }
        return;
    }
    if (v == 0x90) {
        execute_diagnostic();
    } else if (AtaDevice* d = selected()) {
        d->command(v);
        update_irq();
    }
}

// EXECUTE DEVICE DIAGNOSTIC goes to both devices whichever is selected;
// device 0 reports for the pair.
void AtaChannel::execute_diagnostic() {
    select_ = 0;
    for (AtaDevice* d : dev_) {
        if (!d) continue;
        d->irq_ = false;
        d->after(kResetUs, [this, d] {
            d->phase_ = AtaDevice::Phase::Idle;
            d->reset_state();
            d->status_ = d->reset_status();
            d->error_ = 0x01;  // device 0 passed, device 1 passed or absent
            if (d->unit_ == 0 || !dev_[0]) d->raise_irq();
        });
    }
    update_irq();
}

u8 AtaChannel::read_control(unsigned reg) {
    const AtaDevice* d = selected();
    if (reg == 2) return d ? d->status_ : 0;  // alternate status: no acknowledge
    // Drive address: the selects and head (active low), write gate inactive.
    const u8 head = d ? (d->device_ & 0x0F) : 0;
    return static_cast<u8>(0x80 | 0x40 | ((~head & 0x0F) << 2) | (select_ ? 0x01 : 0x02));
}

void AtaChannel::write_control(unsigned reg, u8 value) {
    if (reg != 2) return;
    const u8 old = control_;
    control_ = value;
    if ((value & kSrst) && !(old & kSrst)) {
        for (AtaDevice* d : dev_)
            if (d) d->soft_reset_begin();
    } else if (!(value & kSrst) && (old & kSrst)) {
        select_ = 0;
        for (AtaDevice* d : dev_)
            if (d) d->soft_reset_end();
    }
    update_irq();
}

u8 AtaChannel::bm_read(unsigned reg) const {
    switch (reg) {
        case 0: return bm_cmd_;
        case 2: return bm_status_;
        case 4: case 5: case 6: case 7: return static_cast<u8>(bm_prd_ >> (8 * (reg - 4)));
        default: return 0;
    }
}

void AtaChannel::bm_write(unsigned reg, u8 value) {
    switch (reg) {
        case 0: {
            const u8 old = bm_cmd_;
            bm_cmd_ = value & 0x09;
            if ((value & 1) && !(old & 1)) {
                bm_status_ |= 0x01;
                dma_request();
            } else if (!(value & 1) && (old & 1)) {
                bm_status_ &= static_cast<u8>(~0x01);
                bm_timer_.cancel();
            }
            break;
        }
        case 2:
            // Error and interrupt are cleared by writing one; the DMA capable
            // bits are software's.
            bm_status_ = static_cast<u8>((bm_status_ & ~(value & 0x06) & ~0x60) | (value & 0x60));
            break;
        case 4: case 5: case 6: case 7: {
            const unsigned sh = 8 * (reg - 4);
            bm_prd_ = (bm_prd_ & ~(0xFFu << sh)) | (u32{value} << sh);
            bm_prd_ &= ~3u;
            break;
        }
        default: break;
    }
}

void AtaChannel::dma_request() {
    const AtaDevice* d = selected();
    if (!(bm_status_ & 1) || !d || !d->dma_pending() || bm_timer_.active()) return;
    const double cycles = static_cast<double>(d->dma_remaining()) * cpu_hz_ / dma_rate_;
    bm_timer_.schedule_in(static_cast<u64>(cycles) + 1);
}

// Moves the selected device's DMA data through the PRD table: each entry is
// a little-endian 32-bit address and a 16-bit byte count (0 = 64 KiB), bit
// 31 of its second word marking the last.
void AtaChannel::dma_run() {
    AtaDevice* d = selected();
    if (!(bm_status_ & 1) || !d || !d->dma_pending()) return;
    if (((bm_cmd_ & 0x08) != 0) != d->dma_to_host())
        UV_LOG(Debug, "ata", "bus master direction differs from the device's transfer");
    const bool to_host = d->dma_to_host();
    std::vector<u8> tmp;
    u32 prd = bm_prd_;
    bool table_done = false;
    auto bus_error = [this] {
        bm_status_ = static_cast<u8>((bm_status_ | 0x02) & ~0x01);
    };
    while (d->dma_remaining()) {
        u8 e[8];
        if (!dma_read_ || !dma_read_(prd, e, 8)) return bus_error();
        prd += 8;
        const u32 addr = load_le<u32>(e) & ~1u;
        u32 count = load_le<u16>(e + 4) & ~1u;
        if (count == 0) count = 0x10000;
        const bool last = e[7] & 0x80;
        const std::size_t n = static_cast<std::size_t>(std::min<u64>(count, d->dma_remaining()));
        tmp.resize(n);
        if (to_host) {
            if (!d->dma_move(tmp.data(), n)) return;  // the device ended the command
            if (!dma_write_ || !dma_write_(addr, tmp.data(), n)) return bus_error();
        } else {
            if (!dma_read_ || !dma_read_(addr, tmp.data(), n)) return bus_error();
            if (!d->dma_move(tmp.data(), n)) return;
        }
        if (n < count) break;  // the device finished inside this entry
        if (last) {
            table_done = true;
            break;
        }
    }
    // The engine goes idle when it has used the whole table; if the device
    // finished first it stays active (software stops it).
    if (table_done) bm_status_ &= static_cast<u8>(~0x01);
    if (d->dma_remaining() == 0) d->dma_end();
}

// --- AtaDisk ---

AtaDisk::AtaDisk(Scheduler& scheduler, double cpu_hz, DiskImage& image, std::string model)
    : AtaDevice(scheduler, cpu_hz), image_(image), model_(std::move(model)), sectors_(image.size() / 512) {
    const u64 per_cyl = u64{heads_} * spt_;
    cyl_ = static_cast<unsigned>(std::clamp<u64>(sectors_ / per_cyl, 1, 16383));
    hard_reset();
}

void AtaDisk::reset_state() {
    AtaDevice::reset_state();
    // Power-on defaults (reverting to them after a software reset is the
    // default too).
    cur_cyl_ = cyl_;
    cur_heads_ = heads_;
    cur_spt_ = spt_;
    multiple_ = 0;
    pio_mode_ = 0x08;
    dma_mode_ = 0;
    power_ = 0xFF;
}

bool AtaDisk::target(bool ext, u64& lba) {
    if (ext) {
        lba = lba48();
    } else if (device_reg() & kLba) {
        lba = lba28();
    } else {
        const unsigned cyl = lba_mid() | (lba_hi() << 8), head = device_reg() & 0x0F, sec = lba_lo();
        if (sec == 0 || sec > cur_spt_ || head >= cur_heads_ || cyl >= cur_cyl_) {
            abort_command(kIdnf);
            return false;
        }
        lba = (u64{cyl} * cur_heads_ + head) * cur_spt_ + sec - 1;
    }
    return true;
}

bool AtaDisk::address(bool ext, u64& lba, u64& n) {
    if (!target(ext, lba)) return false;
    n = ext ? (count48() ? count48() : 65536) : (count() ? count() : 256);
    if (lba >= sectors_ || n > sectors_ - lba) {
        set_address(std::max(lba, sectors_), ext);
        abort_command(kIdnf);
        return false;
    }
    return true;
}

void AtaDisk::set_address(u64 lba, bool ext) {
    if (ext) {
        set_lba48_regs(lba);
    } else if (device_reg() & kLba) {
        set_lba_regs(static_cast<u8>(lba), static_cast<u8>(lba >> 8), static_cast<u8>(lba >> 16));
        set_device_head(static_cast<u8>(lba >> 24));
    } else {
        const u64 per_cyl = u64{cur_heads_} * cur_spt_;
        const u64 cyl = lba / per_cyl, rem = lba % per_cyl;
        set_lba_regs(static_cast<u8>(rem % cur_spt_ + 1), static_cast<u8>(cyl), static_cast<u8>(cyl >> 8));
        set_device_head(static_cast<u8>(rem / cur_spt_));
    }
}

void AtaDisk::begin_media(u64 lba, u64 n, bool write, bool dma, bool ext, std::size_t block) {
    if (write && image_.read_only()) {
        abort_command(kAbrt);
        return;
    }
    src_ = Source::Media;
    pos_ = lba * 512;
    xfer_lba_ = lba;
    xfer_count_ = n;
    xfer_ext_ = ext;
    power_ = 0xFF;
    if (dma) start_dma(!write, n * 512);
    else if (write) start_pio_out(n * 512, block);
    else start_pio_in(n * 512, block);
}

void AtaDisk::build_identify() {
    u8* id = identify_.data();
    identify_.fill(0);
    put16(id, 0, 0x0040);  // fixed device
    put16(id, 1, static_cast<u16>(cyl_));
    put16(id, 3, static_cast<u16>(heads_));
    put16(id, 6, static_cast<u16>(spt_));
    ata_put_string(id, 10, 10, "UV00000001");
    put16(id, 20, 3);      // dual-ported buffer with read caching
    put16(id, 21, 512);    // buffer size in sectors
    put16(id, 22, 4);      // ECC bytes on READ/WRITE LONG
    ata_put_string(id, 23, 4, "1.0");
    ata_put_string(id, 27, 20, model_);
    put16(id, 47, 0x8000 | kMaxMultiple);
    put16(id, 49, 0x0F00);  // IORDY, IORDY disable, LBA, DMA
    put16(id, 50, 0x4000);
    put16(id, 51, 0x0200);
    put16(id, 52, 0x0200);
    put16(id, 53, 0x0007);  // words 54-58, 64-70 and 88 valid
    put16(id, 54, static_cast<u16>(cur_cyl_));
    put16(id, 55, static_cast<u16>(cur_heads_));
    put16(id, 56, static_cast<u16>(cur_spt_));
    const u32 cur_cap = cur_cyl_ * cur_heads_ * cur_spt_;
    put16(id, 57, static_cast<u16>(cur_cap));
    put16(id, 58, static_cast<u16>(cur_cap >> 16));
    put16(id, 59, static_cast<u16>(multiple_ ? (0x0100 | multiple_) : 0));
    const u32 lba28_sectors = static_cast<u32>(std::min<u64>(sectors_, 0x0FFFFFFF));
    put16(id, 60, static_cast<u16>(lba28_sectors));
    put16(id, 61, static_cast<u16>(lba28_sectors >> 16));
    const bool mw = dma_mode_ >= 0x20 && dma_mode_ <= 0x22, udma = dma_mode_ >= 0x40 && dma_mode_ <= 0x44;
    put16(id, 63, static_cast<u16>(0x0007 | (mw ? (0x100 << (dma_mode_ & 7)) : 0)));
    put16(id, 64, 0x0003);  // PIO modes 3 and 4
    put16(id, 65, 120);
    put16(id, 66, 120);
    put16(id, 67, 120);
    put16(id, 68, 120);
    put16(id, 80, 0x007E);  // ATA-1 to ATA-6
    put16(id, 82, 0x7020);  // NOP, READ/WRITE BUFFER, write cache
    put16(id, 83, 0x7400);  // FLUSH CACHE (EXT), 48-bit addressing
    put16(id, 84, 0x4000);
    put16(id, 85, static_cast<u16>(0x7000 | (write_cache_ ? 0x0020 : 0)));
    put16(id, 86, 0x3400);
    put16(id, 87, 0x4000);
    put16(id, 88, static_cast<u16>(0x001F | (udma ? (0x100 << (dma_mode_ & 7)) : 0)));
    put16(id, 93, unit_ == 0 ? 0x400B : 0x4300);  // hardware reset results
    for (unsigned i = 0; i < 4; ++i) put16(id, 100 + i, static_cast<u16>(sectors_ >> (16 * i)));
    id[510] = 0xA5;
    u8 sum = 0;
    for (unsigned i = 0; i < 511; ++i) sum = static_cast<u8>(sum + id[i]);
    id[511] = static_cast<u8>(-sum);
}

void AtaDisk::set_features() {
    switch (feature()) {
        case 0x03: {  // transfer mode
            const u8 m = count();
            if (m <= 0x01 || (m >= 0x08 && m <= 0x0C)) pio_mode_ = m;
            else if ((m >= 0x20 && m <= 0x22) || (m >= 0x40 && m <= 0x44)) dma_mode_ = m;
            else return abort_command();
            break;
        }
        case 0x02: write_cache_ = true; break;
        case 0x82: write_cache_ = false; break;
        case 0x66: case 0xCC: case 0xAA: case 0x55: break;  // reverting to defaults, read look-ahead
        default: return abort_command();
    }
    complete();
}

void AtaDisk::execute(u8 cmd) {
    u64 lba = 0, n = 0;
    const std::size_t multi = multiple_ * 512;
    switch (cmd) {
        // Reads and writes: PIO, multiple, DMA; 28- and 48-bit.
        case 0x20: case 0x21: if (address(false, lba, n)) begin_media(lba, n, false, false, false, 512); return;
        case 0x24: if (address(true, lba, n)) begin_media(lba, n, false, false, true, 512); return;
        case 0xC4: case 0x29:
            if (!multiple_) return abort_command();
            if (address(cmd == 0x29, lba, n)) begin_media(lba, n, false, false, cmd == 0x29, multi);
            return;
        case 0x30: case 0x31: if (address(false, lba, n)) begin_media(lba, n, true, false, false, 512); return;
        case 0x34: if (address(true, lba, n)) begin_media(lba, n, true, false, true, 512); return;
        case 0xC5: case 0x39:
            if (!multiple_) return abort_command();
            if (address(cmd == 0x39, lba, n)) begin_media(lba, n, true, false, cmd == 0x39, multi);
            return;
        case 0xC8: case 0xC9: if (address(false, lba, n)) begin_media(lba, n, false, true, false, 0); return;
        case 0x25: if (address(true, lba, n)) begin_media(lba, n, false, true, true, 0); return;
        case 0xCA: case 0xCB: if (address(false, lba, n)) begin_media(lba, n, true, true, false, 0); return;
        case 0x35: if (address(true, lba, n)) begin_media(lba, n, true, true, true, 0); return;
        case 0x40: case 0x41: case 0x42:  // READ VERIFY
            if (address(cmd == 0x42, lba, n)) {
                set_address(lba + n - 1, cmd == 0x42);
                power_ = 0xFF;
                complete();
            }
            return;
        case 0x91: {  // INITIALIZE DEVICE PARAMETERS
            const unsigned heads = (device_reg() & 0x0F) + 1, spt = count();
            if (spt == 0) return abort_command();
            cur_heads_ = heads;
            cur_spt_ = spt;
            cur_cyl_ = static_cast<unsigned>(std::min<u64>(sectors_ / (u64{heads} * spt), 65535));
            return complete();
        }
        case 0xC6:  // SET MULTIPLE MODE
            if (count() > kMaxMultiple || (count() & (count() - 1))) return abort_command();
            multiple_ = count();
            return complete();
        case 0xEC:  // IDENTIFY DEVICE
            build_identify();
            src_ = Source::Buffer;
            std::memcpy(staging_.data(), identify_.data(), 512);
            pos_ = 0;
            return start_pio_in(512, 512);
        case 0xE4:  // READ BUFFER
            src_ = Source::Buffer;
            std::memcpy(staging_.data(), buffer_.data(), 512);
            pos_ = 0;
            return start_pio_in(512, 512);
        case 0xE8:  // WRITE BUFFER
            src_ = Source::Buffer;
            pos_ = 0;
            return start_pio_out(512, 512);
        case 0xE7: case 0xEA:  // FLUSH CACHE (EXT)
            image_.flush();
            return complete();
        case 0xEF: return set_features();
        case 0xE0: case 0xE2: case 0xE6: power_ = 0x00; return complete();  // standby (immediate), sleep
        case 0xE1: case 0xE3: power_ = 0x80; return complete();             // idle (immediate)
        case 0xE5: set_count(power_); return complete();                    // CHECK POWER MODE
        case 0xF8: {  // READ NATIVE MAX ADDRESS
            const u64 max = std::min<u64>(sectors_, u64{1} << 28) - 1;
            set_lba_regs(static_cast<u8>(max), static_cast<u8>(max >> 8), static_cast<u8>(max >> 16));
            set_device_head(static_cast<u8>(max >> 24));
            return complete();
        }
        case 0x27: set_lba48_regs(sectors_ - 1); return complete();  // READ NATIVE MAX ADDRESS EXT
        default:
            if ((cmd & 0xF0) == 0x10) return complete();  // RECALIBRATE
            if ((cmd & 0xF0) == 0x70) {                   // SEEK
                if (!target(false, lba)) return;
                if (lba >= sectors_) return abort_command(kIdnf);
                power_ = 0xFF;
                return complete();
            }
            UV_LOG(Debug, "ata", "disk: command %02x not supported", cmd);
            return abort_command();
    }
}

bool AtaDisk::data_in(u8* dst, std::size_t length) {
    if (src_ == Source::Buffer) {
        for (std::size_t i = 0; i < length; ++i) dst[i] = pos_ + i < 512 ? staging_[pos_ + i] : 0;
        pos_ += length;
        return true;
    }
    if (!image_.read(pos_, dst, length)) {
        set_address(pos_ / 512, xfer_ext_);
        abort_command(kUnc);
        return false;
    }
    pos_ += length;
    return true;
}

bool AtaDisk::data_out(const u8* src, std::size_t length) {
    if (src_ == Source::Buffer) {
        for (std::size_t i = 0; i < length && pos_ + i < 512; ++i) buffer_[pos_ + i] = src[i];
        pos_ += length;
        return true;
    }
    if (!image_.write(pos_, src, length)) {
        set_address(pos_ / 512, xfer_ext_);
        abort_command(kAbrt);
        return false;
    }
    pos_ += length;
    return true;
}

void AtaDisk::transfer_done(bool dma, bool to_host) {
    if (src_ == Source::Media) {
        // The registers end on the last sector moved.
        set_address(xfer_lba_ + xfer_count_ - 1, xfer_ext_);
        set_count(0);
    }
    AtaDevice::transfer_done(dma, to_host);
}

}  // namespace uv
