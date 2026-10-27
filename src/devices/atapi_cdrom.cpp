// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
#include "devices/atapi_cdrom.h"

#include <algorithm>
#include <cstring>

#include "core/log.h"
#include "storage/disk_image.h"

namespace uv {

using namespace ata;

namespace {

constexpr double kPacketUs = 50.0;  // from the packet to the drive acting on it

// Sense keys
constexpr u8 kNotReady = 0x02, kMediumError = 0x03, kIllegalRequest = 0x05, kUnitAttention = 0x06;

u16 be16(const u8* p) { return static_cast<u16>((p[0] << 8) | p[1]); }
u32 be32(const u8* p) { return (u32{p[0]} << 24) | (u32{p[1]} << 16) | (u32{p[2]} << 8) | p[3]; }
void put_be16(u8* p, u32 v) { p[0] = static_cast<u8>(v >> 8), p[1] = static_cast<u8>(v); }
void put_be32(u8* p, u32 v) { put_be16(p, v >> 16), put_be16(p + 2, v); }

void put16(u8* b, unsigned word, u16 v) {
    b[2 * word] = static_cast<u8>(v);
    b[2 * word + 1] = static_cast<u8>(v >> 8);
}

// A block address as minutes/seconds/frames (75 frames a second, with the
// two-second pregap before LBA 0).
struct Msf {
    u8 m, s, f;
};
Msf to_msf(u32 lba) {
    const u32 x = lba + 150;
    return {static_cast<u8>(x / (75 * 60)), static_cast<u8>((x / 75) % 60), static_cast<u8>(x % 75)};
}
u32 from_msf(const u8* p) { return (u32{p[0]} * 60 + p[1]) * 75 + p[2]; }
u8 bcd(u8 v) { return static_cast<u8>(((v / 10) << 4) | (v % 10)); }

// A TOC address field: MSF (0, M, S, F) or LBA.
void put_addr(u8* p, u32 lba, bool msf) {
    if (msf) {
        const Msf a = to_msf(lba);
        p[0] = 0, p[1] = a.m, p[2] = a.s, p[3] = a.f;
    } else {
        put_be32(p, lba);
    }
}

// ECMA-130 error detection and correction for mode 1 sectors: a 32-bit EDC
// over sync, header and data, then the P and Q Reed-Solomon parity over
// GF(2^8) (x^8 + x^4 + x^3 + x^2 + 1).
struct EccTables {
    u8 f[256], b[256];
    u32 edc[256];
    EccTables() {
        for (unsigned i = 0; i < 256; ++i) {
            const unsigned j = (i << 1) ^ ((i & 0x80) ? 0x11D : 0);
            f[i] = static_cast<u8>(j);
            b[i ^ j] = static_cast<u8>(i);
            u32 e = i;
            for (int k = 0; k < 8; ++k) e = (e >> 1) ^ ((e & 1) ? 0xD8018001u : 0);
            edc[i] = e;
        }
    }
};
const EccTables& ecc_tables() {
    static const EccTables t;
    return t;
}

void ecc_block(const u8* src, unsigned major_count, unsigned minor_count, unsigned major_mult, unsigned minor_inc,
               u8* dest) {
    const EccTables& t = ecc_tables();
    const unsigned size = major_count * minor_count;
    for (unsigned major = 0; major < major_count; ++major) {
        unsigned index = (major >> 1) * major_mult + (major & 1);
        u8 a = 0, b = 0;
        for (unsigned minor = 0; minor < minor_count; ++minor) {
            const u8 v = src[index];
            index += minor_inc;
            if (index >= size) index -= size;
            a ^= v;
            b ^= v;
            a = t.f[a];
        }
        a = t.b[t.f[a] ^ b];
        dest[major] = a;
        dest[major + major_count] = static_cast<u8>(a ^ b);
    }
}

}  // namespace

AtapiCdrom::AtapiCdrom(Scheduler& scheduler, double cpu_hz, std::string vendor, std::string product, std::string revision)
    : AtaDevice(scheduler, cpu_hz), vendor_(std::move(vendor)), product_(std::move(product)),
      revision_(std::move(revision)) {
    hard_reset();
}

u32 AtapiCdrom::blocks() const {
    if (!image_) return 0;
    return static_cast<u32>(std::min<u64>((image_->size() + kBlock - 1) / kBlock, 0xFFFFFFFFu));
}

void AtapiCdrom::insert(DiskImage* image) {
    image_ = image && image->is_open() ? image : nullptr;
    media_ = image_ != nullptr;
    tray_open_ = false;
    if (media_) unit_attention_ = 0x28;  // not ready to ready: medium may have changed
}

void AtapiCdrom::signature() {
    set_count(1);
    set_lba_regs(1, 0x14, 0xEB);
}

void AtapiCdrom::reset_state() {
    AtaDevice::reset_state();
    signature();
    packet_cmd_ = false;
}

void AtapiCdrom::before_drq(bool to_host, std::size_t length) {
    // Interrupt reason (CoD, IO) in the count register; the byte count of
    // this block in LBA mid/high.
    if (src_ == Source::Packet) {
        set_count(0x01);
        return;
    }
    if (!packet_cmd_) return;
    set_count(to_host ? 0x02 : 0x00);
    set_lba_regs(lba_lo(), static_cast<u8>(length), static_cast<u8>(length >> 8));
}

void AtapiCdrom::good() {
    packet_cmd_ = false;
    set_count(0x03);  // CoD and IO: status
    complete();
}

void AtapiCdrom::check_condition(u8 key, u8 asc, u8 ascq) {
    sense_key_ = key;
    asc_ = asc;
    ascq_ = ascq;
    packet_cmd_ = false;
    set_count(0x03);
    // Error: the sense key, and ABRT for a command the drive refused.
    const bool refused = key == kIllegalRequest && asc == 0x20;
    abort_command(static_cast<u8>((key << 4) | (refused ? kAbrt : 0)));
}

bool AtapiCdrom::need_media() {
    if (media_) return true;
    check_condition(kNotReady, 0x3A, tray_open_ ? 0x02 : 0x01);
    return false;
}

void AtapiCdrom::data_phase(bool to_host, u64 bytes) {
    if (bytes == 0) return good();
    if (dma_) start_dma(to_host, bytes);
    else if (to_host) start_pio_in(bytes, byte_limit_);
    else start_pio_out(bytes, byte_limit_, true);
}

void AtapiCdrom::reply(std::vector<u8> data, std::size_t allocation) {
    if (data.size() > allocation) data.resize(allocation);
    reply_ = std::move(data);
    src_ = Source::Reply;
    pos_ = 0;
    data_phase(true, reply_.size());
}

void AtapiCdrom::build_identify() {
    u8* id = identify_.data();
    identify_.fill(0);
    put16(id, 0, 0x85C0);  // ATAPI, CD-ROM, removable, 50 us DRQ, 12-byte packets
    ata_put_string(id, 10, 10, "UV00000002");
    ata_put_string(id, 23, 4, revision_);
    ata_put_string(id, 27, 20, vendor_ + " " + product_);
    put16(id, 49, 0x0F00);  // IORDY, IORDY disable, LBA, DMA
    put16(id, 51, 0x0200);
    put16(id, 53, 0x0006);  // words 64-70 and 88 valid
    const bool mw = dma_mode_ >= 0x20 && dma_mode_ <= 0x22, udma = dma_mode_ >= 0x40 && dma_mode_ <= 0x42;
    put16(id, 63, static_cast<u16>(0x0007 | (mw ? (0x100 << (dma_mode_ & 7)) : 0)));
    put16(id, 64, 0x0003);
    put16(id, 65, 120);
    put16(id, 66, 120);
    put16(id, 67, 120);
    put16(id, 68, 120);
    put16(id, 80, 0x001E);  // ATA/ATAPI-1 to 4
    put16(id, 82, 0x4210);  // NOP, PACKET, DEVICE RESET
    put16(id, 83, 0x4000);
    put16(id, 84, 0x4000);
    put16(id, 85, 0x4210);
    put16(id, 87, 0x4000);
    put16(id, 88, static_cast<u16>(0x0007 | (udma ? (0x100 << (dma_mode_ & 7)) : 0)));
    put16(id, 93, unit_ == 0 ? 0x400B : 0x4300);
    id[510] = 0xA5;
    u8 sum = 0;
    for (unsigned i = 0; i < 511; ++i) sum = static_cast<u8>(sum + id[i]);
    id[511] = static_cast<u8>(-sum);
}

void AtapiCdrom::set_features() {
    switch (feature()) {
        case 0x03: {
            const u8 m = count();
            if (m <= 0x01 || (m >= 0x08 && m <= 0x0C)) pio_mode_ = m;
            else if ((m >= 0x20 && m <= 0x22) || (m >= 0x40 && m <= 0x42)) dma_mode_ = m;
            else return abort_command();
            break;
        }
        case 0x02: case 0x82: case 0x66: case 0xCC: case 0xAA: case 0x55: break;
        default: return abort_command();
    }
    complete();
}

void AtapiCdrom::execute(u8 cmd) {
    packet_cmd_ = false;
    switch (cmd) {
        case 0xA0:  // PACKET
            if (feature() & 0x02) return abort_command();  // no overlapped commands
            dma_ = feature() & 0x01;
            byte_limit_ = static_cast<std::size_t>(lba_mid() | (lba_hi() << 8)) & ~std::size_t{1};
            if (byte_limit_ == 0) byte_limit_ = 0xFFFE;
            src_ = Source::Packet;
            return start_pio_out(12, 12, false);
        case 0xA1:  // IDENTIFY PACKET DEVICE
            build_identify();
            src_ = Source::Identify;
            pos_ = 0;
            return start_pio_in(512, 512);
        case 0x08:  // DEVICE RESET
            reset_state();
            complete(false);
            set_status(reset_status());
            set_error(0x01);
            return;
        case 0xEF: return set_features();
        case 0xE0: case 0xE6: power_ = 0x00; return complete();  // standby immediate, sleep
        case 0xE1: power_ = 0x80; return complete();             // idle immediate
        case 0xE5: set_count(power_); return complete();         // CHECK POWER MODE
        default:
            // Other ATA commands are refused with the packet signature in the
            // task file, so a driver looking for a disk finds this drive.
            signature();
            return abort_command();
    }
}

void AtapiCdrom::transfer_done(bool dma, bool to_host) {
    if (src_ == Source::Packet) {
        packet_cmd_ = true;
        after(kPacketUs, [this] { packet(); });
        return;
    }
    if (packet_cmd_) {
        if (src_ == Source::Media) cur_lba_ = static_cast<u32>(pos_ / kBlock) - 1;
        return good();
    }
    AtaDevice::transfer_done(dma, to_host);
}

bool AtapiCdrom::data_in(u8* dst, std::size_t length) {
    switch (src_) {
        case Source::Identify:
            for (std::size_t i = 0; i < length; ++i) dst[i] = pos_ + i < 512 ? identify_[pos_ + i] : 0;
            pos_ += length;
            return true;
        case Source::Reply:
            for (std::size_t i = 0; i < length; ++i) dst[i] = pos_ + i < reply_.size() ? reply_[pos_ + i] : 0;
            pos_ += length;
            return true;
        case Source::Media:
            if (!image_ || !image_->read(pos_, dst, length)) {
                check_condition(kMediumError, 0x11);  // unrecovered read error
                return false;
            }
            pos_ += length;
            return true;
        case Source::Raw:
            while (length) {
                if (raw_off_ == raw_len_) {
                    if (!build_raw_sector(raw_lba_++)) return false;
                }
                const std::size_t n = std::min(length, raw_len_ - raw_off_);
                std::memcpy(dst, raw_out_.data() + raw_off_, n);
                raw_off_ += n;
                dst += n;
                length -= n;
            }
            return true;
        default:
            std::memset(dst, 0, length);
            return true;
    }
}

bool AtapiCdrom::data_out(const u8* src, std::size_t length) {
    if (src_ == Source::Packet) {
        std::memcpy(cdb_.data(), src, std::min(length, cdb_.size()));
        return true;
    }
    return true;  // parameter data (MODE SELECT) is taken and not used
}

// One sector as READ CD returns it: the fields selected in byte 9 of the
// command (sync, header, user data, EDC/ECC) of the mode 1 raw sector.
bool AtapiCdrom::build_raw_sector(u32 lba) {
    u8* s = raw_.data();
    std::memset(s, 0, raw_.size());
    s[0] = 0x00;
    std::memset(s + 1, 0xFF, 10);
    s[11] = 0x00;
    const Msf a = to_msf(lba);
    s[12] = bcd(a.m), s[13] = bcd(a.s), s[14] = bcd(a.f), s[15] = 0x01;
    if (!image_ || !image_->read(u64{lba} * kBlock, s + 16, kBlock)) {
        check_condition(kMediumError, 0x11);
        return false;
    }
    if (raw_flags_ & 0x08) {
        const EccTables& t = ecc_tables();
        u32 edc = 0;
        for (unsigned i = 0; i < 0x810; ++i) edc = (edc >> 8) ^ t.edc[(edc ^ s[i]) & 0xFF];
        s[0x810] = static_cast<u8>(edc), s[0x811] = static_cast<u8>(edc >> 8);
        s[0x812] = static_cast<u8>(edc >> 16), s[0x813] = static_cast<u8>(edc >> 24);
        ecc_block(s + 0xC, 86, 24, 2, 86, s + 0x81C);   // P parity
        ecc_block(s + 0xC, 52, 43, 86, 88, s + 0x8C8);  // Q parity
    }
    raw_out_.clear();
    const unsigned headers = (raw_flags_ >> 5) & 3;
    if (raw_flags_ & 0x80) raw_out_.insert(raw_out_.end(), s, s + 12);
    if (headers == 1 || headers == 3) raw_out_.insert(raw_out_.end(), s + 12, s + 16);  // mode 1 has no subheader
    if (raw_flags_ & 0x10) raw_out_.insert(raw_out_.end(), s + 16, s + 16 + kBlock);
    if (raw_flags_ & 0x08) raw_out_.insert(raw_out_.end(), s + 0x810, s + 2352);
    raw_off_ = 0;
    raw_len_ = raw_out_.size();
    return true;
}

void AtapiCdrom::read_blocks(u64 lba, u64 count) {
    if (!need_media()) return;
    if (lba + count > blocks()) return check_condition(kIllegalRequest, 0x21);  // LBA out of range
    if (count == 0) return good();
    src_ = Source::Media;
    pos_ = lba * kBlock;
    power_ = 0xFF;
    data_phase(true, count * kBlock);
}

void AtapiCdrom::read_cd(u64 lba, u64 count, u8 type, u8 flags, u8 subchannel) {
    if (!need_media()) return;
    // A single mode 1 data track: CD-DA and mode 2 sector types do not match it.
    if (type != 0 && type != 2) return check_condition(kIllegalRequest, 0x64);
    // Neither C2 error pointers nor sub-channel data are offered (capabilities page).
    if ((flags & 0x06) || subchannel) return check_condition(kIllegalRequest, 0x24);
    if (lba + count > blocks()) return check_condition(kIllegalRequest, 0x21);
    const unsigned headers = (flags >> 5) & 3;
    const unsigned per = ((flags & 0x80) ? 12 : 0) + ((headers == 1 || headers == 3) ? 4 : 0) +
                         ((flags & 0x10) ? kBlock : 0) + ((flags & 0x08) ? 288 : 0);
    if (count == 0 || per == 0) return good();
    src_ = Source::Raw;
    raw_flags_ = flags;
    raw_lba_ = static_cast<u32>(lba);
    raw_off_ = raw_len_ = 0;
    cur_lba_ = static_cast<u32>(lba + count - 1);
    power_ = 0xFF;
    data_phase(true, count * per);
}

std::vector<u8> AtapiCdrom::mode_page(u8 page, unsigned control) const {
    const bool changeable = control == 1;
    std::vector<u8> p;
    switch (page) {
        case 0x01:  // read error recovery
            p = {0x01, 0x06, 0x00, 0x05, 0, 0, 0, 0};
            break;
        case 0x0D:  // CD device parameters: inactivity timer, 60 S per M, 75 F per S
            p = {0x0D, 0x06, 0x00, 0x00, 0x00, 0x3C, 0x00, 0x4B};
            break;
        case 0x0E:  // CD audio control: immediate, ports 0 and 1 on channels 0 and 1 at full volume
            p = {0x0E, 0x0E, 0x04, 0, 0, 0, 0, 0, 0x01, 0xFF, 0x02, 0xFF, 0, 0, 0, 0};
            break;
        case 0x2A: {  // capabilities and mechanical status
            p.assign(20, 0);
            p[0] = 0x2A;
            p[1] = 0x12;
            p[4] = 0x71;  // audio play, mode 2 form 1 and 2, multisession
            p[5] = 0x03;  // CD-DA commands, accurate stream
            p[6] = static_cast<u8>(0x29 | (prevent_ ? 0x02 : 0));  // tray, eject, lock (and its state)
            p[7] = 0x03;  // separate volume and mute per channel
            put_be16(&p[8], 5632);   // maximum speed (32x), KB/s
            put_be16(&p[10], 256);   // volume levels
            put_be16(&p[12], 128);   // buffer, KB
            put_be16(&p[14], 5632);  // current speed
            break;
        }
        default: return {};
    }
    // Nothing here can be changed.
    if (changeable) std::fill(p.begin() + 2, p.end(), 0);
    return p;
}

void AtapiCdrom::mode_sense(bool ten) {
    const u8 page = cdb_[2] & 0x3F;
    const unsigned control = cdb_[2] >> 6;
    if (control == 3) return check_condition(kIllegalRequest, 0x39);  // saved values not supported
    std::vector<u8> pages;
    if (page == 0x3F) {
        for (u8 p : {0x01, 0x0D, 0x0E, 0x2A}) {
            auto v = mode_page(p, control);
            pages.insert(pages.end(), v.begin(), v.end());
        }
    } else {
        pages = mode_page(page, control);
        if (pages.empty()) return check_condition(kIllegalRequest, 0x24);
    }
    const u8 medium = media_ ? 0x01 : (tray_open_ ? 0x71 : 0x70);
    std::vector<u8> d;
    if (ten) {
        d.assign(8, 0);
        put_be16(&d[0], static_cast<u32>(6 + pages.size()));
        d[2] = medium;
    } else {
        d.assign(4, 0);
        d[0] = static_cast<u8>(3 + pages.size());
        d[1] = medium;
    }
    d.insert(d.end(), pages.begin(), pages.end());
    reply(std::move(d), ten ? be16(&cdb_[7]) : cdb_[4]);
}

void AtapiCdrom::read_toc() {
    if (!need_media()) return;
    const bool msf = cdb_[1] & 0x02;
    unsigned format = cdb_[2] & 0x0F;
    if (format == 0) format = cdb_[9] >> 6;  // where SFF-8020 drives took it
    const u8 track = cdb_[6];
    const u32 leadout = blocks();
    std::vector<u8> d(4, 0);
    d[2] = 1, d[3] = 1;  // first and last track (or session)
    switch (format) {
        case 0:
            if (track > 1 && track != 0xAA) return check_condition(kIllegalRequest, 0x24);
            if (track <= 1) {
                const u8 e[8] = {0, 0x14, 1, 0, 0, 0, 0, 0};
                d.insert(d.end(), e, e + 8);
                put_addr(&d[d.size() - 4], 0, msf);
            }
            {
                const u8 e[8] = {0, 0x14, 0xAA, 0, 0, 0, 0, 0};
                d.insert(d.end(), e, e + 8);
                put_addr(&d[d.size() - 4], leadout, msf);
            }
            break;
        case 1: {  // session information: the first track of the last session
            const u8 e[8] = {0, 0x14, 1, 0, 0, 0, 0, 0};
            d.insert(d.end(), e, e + 8);
            put_addr(&d[d.size() - 4], 0, msf);
            break;
        }
        case 2: {  // the raw TOC: points A0, A1, A2 and track 1
            const Msf lo = to_msf(leadout), t1 = to_msf(0);
            const u8 q[4][11] = {
                {1, 0x14, 0, 0xA0, 0, 0, 0, 0, 1, 0x00, 0},  // first track, CD-ROM disc
                {1, 0x14, 0, 0xA1, 0, 0, 0, 0, 1, 0, 0},     // last track
                {1, 0x14, 0, 0xA2, 0, 0, 0, 0, lo.m, lo.s, lo.f},
                {1, 0x14, 0, 0x01, 0, 0, 0, 0, t1.m, t1.s, t1.f},
            };
            for (const auto& e : q) d.insert(d.end(), e, e + 11);
            break;
        }
        default: return check_condition(kIllegalRequest, 0x24);
    }
    put_be16(&d[0], static_cast<u32>(d.size() - 2));
    reply(std::move(d), be16(&cdb_[7]));
}

void AtapiCdrom::read_subchannel() {
    if (!need_media()) return;
    const bool msf = cdb_[1] & 0x02, subq = cdb_[2] & 0x40;
    const u8 format = cdb_[3];
    std::vector<u8> d(4, 0);
    d[1] = 0x15;  // no current audio status
    if (subq) {
        switch (format) {
            case 1: {  // current position
                u8 e[12] = {0x01, 0x14, 1, 1};
                put_addr(e + 4, cur_lba_, msf);
                put_addr(e + 8, cur_lba_, msf);
                d.insert(d.end(), e, e + 12);
                break;
            }
            case 2: case 3: {  // media catalogue number, ISRC: not recorded
                u8 e[20] = {format, static_cast<u8>(format == 3 ? 0x14 : 0), static_cast<u8>(format == 3 ? cdb_[6] : 0)};
                d.insert(d.end(), e, e + 20);
                break;
            }
            default: return check_condition(kIllegalRequest, 0x24);
        }
    }
    put_be16(&d[2], static_cast<u32>(d.size() - 4));
    reply(std::move(d), be16(&cdb_[7]));
}

void AtapiCdrom::packet() {
    const u8 op = cdb_[0];
    // A pending unit attention ends the next command, except those that
    // report status without touching the medium.
    if (unit_attention_ && op != 0x12 && op != 0x03) {
        const u8 asc = unit_attention_;
        unit_attention_ = 0;
        return check_condition(kUnitAttention, asc);
    }
    switch (op) {
        case 0x00:  // TEST UNIT READY
            if (need_media()) good();
            return;
        case 0x03: {  // REQUEST SENSE
            std::vector<u8> d(18, 0);
            d[0] = 0x70;
            d[7] = 10;
            if (sense_key_ == 0 && unit_attention_) {
                d[2] = kUnitAttention;
                d[12] = unit_attention_;
                unit_attention_ = 0;
            } else {
                d[2] = sense_key_;
                d[12] = asc_;
                d[13] = ascq_;
            }
            sense_key_ = asc_ = ascq_ = 0;
            return reply(std::move(d), cdb_[4]);
        }
        case 0x12: {  // INQUIRY
            if (cdb_[1] & 0x01) return check_condition(kIllegalRequest, 0x24);  // no vital product data
            std::vector<u8> d(36, ' ');
            d[0] = 0x05;  // CD-ROM
            d[1] = 0x80;  // removable
            d[2] = 0x00;
            d[3] = 0x21;  // ATAPI version 2, response data format 1
            d[4] = 31;
            d[5] = d[6] = d[7] = 0;
            for (std::size_t i = 0; i < 8 && i < vendor_.size(); ++i) d[8 + i] = static_cast<u8>(vendor_[i]);
            for (std::size_t i = 0; i < 16 && i < product_.size(); ++i) d[16 + i] = static_cast<u8>(product_[i]);
            for (std::size_t i = 0; i < 4 && i < revision_.size(); ++i) d[32 + i] = static_cast<u8>(revision_[i]);
            return reply(std::move(d), cdb_[4]);
        }
        case 0x1A: return mode_sense(false);
        case 0x5A: return mode_sense(true);
        case 0x15: case 0x55: {  // MODE SELECT: nothing is changeable, the data is taken
            src_ = Source::Discard;
            return data_phase(false, op == 0x15 ? cdb_[4] : be16(&cdb_[7]));
        }
        case 0x1B: {  // START STOP UNIT
            const bool loej = cdb_[4] & 0x02, start = cdb_[4] & 0x01;
            if (loej && !start) {
                if (prevent_) return check_condition(kIllegalRequest, 0x53, 0x02);  // removal prevented
                media_ = false;
                tray_open_ = true;
                if (on_eject) on_eject();
            } else if (loej && start) {
                tray_open_ = false;
                if (image_ && !media_) {
                    media_ = true;
                    unit_attention_ = 0x28;
                }
            }
            return good();
        }
        case 0x1E:  // PREVENT ALLOW MEDIUM REMOVAL
            prevent_ = cdb_[4] & 0x01;
            return good();
        case 0x25: {  // READ CAPACITY
            if (!need_media()) return;
            std::vector<u8> d(8, 0);
            put_be32(&d[0], blocks() - 1);
            put_be32(&d[4], kBlock);
            return reply(std::move(d), 8);
        }
        case 0x28: return read_blocks(be32(&cdb_[2]), be16(&cdb_[7]));  // READ (10)
        case 0xA8: return read_blocks(be32(&cdb_[2]), be32(&cdb_[6]));  // READ (12)
        case 0x2B:  // SEEK (10)
            if (!need_media()) return;
            if (be32(&cdb_[2]) >= blocks()) return check_condition(kIllegalRequest, 0x21);
            cur_lba_ = be32(&cdb_[2]);
            return good();
        case 0x43: return read_toc();
        case 0x42: return read_subchannel();
        case 0x44: {  // READ HEADER
            if (!need_media()) return;
            const u32 lba = be32(&cdb_[2]);
            if (lba >= blocks()) return check_condition(kIllegalRequest, 0x21);
            std::vector<u8> d(8, 0);
            d[0] = 0x01;  // mode 1
            put_addr(&d[4], lba, cdb_[1] & 0x02);
            return reply(std::move(d), be16(&cdb_[7]));
        }
        case 0xBE:  // READ CD
            return read_cd(be32(&cdb_[2]), (u32{cdb_[6]} << 16) | (u32{cdb_[7]} << 8) | cdb_[8], (cdb_[1] >> 2) & 7,
                           cdb_[9], cdb_[10] & 7);
        case 0xB9: {  // READ CD MSF
            const u32 start = from_msf(&cdb_[3]), end = from_msf(&cdb_[6]);
            if (start < 150 || end < start) return check_condition(kIllegalRequest, 0x24);
            return read_cd(start - 150, end - start, (cdb_[1] >> 2) & 7, cdb_[9], cdb_[10] & 7);
        }
        case 0xBD: {  // MECHANISM STATUS
            std::vector<u8> d(8, 0);
            d[1] = static_cast<u8>(tray_open_ ? 0x10 : 0);  // idle, door open or closed
            d[2] = static_cast<u8>(cur_lba_ >> 16), d[3] = static_cast<u8>(cur_lba_ >> 8), d[4] = static_cast<u8>(cur_lba_);
            return reply(std::move(d), be16(&cdb_[8]));
        }
        case 0xBB: return good();  // SET CD SPEED
        case 0x4E: return good();  // STOP PLAY/SCAN: nothing is playing
        case 0x4B: return check_condition(kIllegalRequest, 0x2C);  // PAUSE/RESUME: command sequence error
        case 0x45: case 0x47: case 0xA5:  // PLAY AUDIO: the track holds data
            if (!need_media()) return;
            return check_condition(kIllegalRequest, 0x64);
        default:
            UV_LOG(Debug, "atapi", "packet command %02x not supported", op);
            return check_condition(kIllegalRequest, 0x20);  // invalid command operation code
    }
}

}  // namespace uv
