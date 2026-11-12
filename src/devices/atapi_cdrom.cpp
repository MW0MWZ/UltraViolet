// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
#include "devices/atapi_cdrom.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/log.h"
#include "core/text.h"
#include "storage/cd_image.h"

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

// A TOC address field: MSF (0, M, S, F) or LBA.
void put_addr(u8* p, u32 lba, bool msf) {
    if (msf) {
        const Msf a = to_msf(lba);
        p[0] = 0, p[1] = a.m, p[2] = a.s, p[3] = a.f;
    } else {
        put_be32(p, lba);
    }
}

// A track-relative address as MSF (no 150-sector offset), or a signed LBA.
void put_relative(u8* p, s64 rel, bool msf) {
    if (msf) {
        const u32 x = static_cast<u32>(rel < 0 ? -rel : rel);
        p[0] = 0, p[1] = static_cast<u8>(x / (75 * 60)), p[2] = static_cast<u8>((x / 75) % 60), p[3] = static_cast<u8>(x % 75);
    } else {
        put_be32(p, static_cast<u32>(static_cast<s32>(rel)));
    }
}

constexpr u32 kSamplesPerSector = 588;

}  // namespace


AtapiCdrom::AtapiCdrom(Scheduler& scheduler, double cpu_hz, std::string vendor, std::string product, std::string revision)
    : AtaDevice(scheduler, cpu_hz), vendor_(std::move(vendor)), product_(std::move(product)),
      revision_(std::move(revision)), play_timer_(scheduler, [this] { stop_play(kPlayDone); }) {
    hard_reset();
}

u32 AtapiCdrom::blocks() const { return image_ ? image_->leadout() : 0; }

void AtapiCdrom::insert(CdImage* image) {
    stop_play(kNoAudioStatus);
    image_ = image && image->is_open() ? image : nullptr;
    media_ = image_ != nullptr;
    tray_open_ = false;
    audio_cached_ = {~0u, ~0u};
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
    stop_play(kNoAudioStatus);
}

// A power-on or hardware reset ends any prevention of medium removal
// (PREVENT ALLOW MEDIUM REMOVAL: it lasts until allowed again or a hard
// reset), and the next command reports the reset as a unit attention.
void AtapiCdrom::hardware_reset() {
    prevent_ = false;
    unit_attention_ = 0x29;
    port_channels_ = {0x01, 0x02};
    port_volume_ = {0xFF, 0xFF};
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
        if (src_ == Source::Media || src_ == Source::Raw) cur_lba_ = sector_lba_ - 1;
        if (src_ == Source::Parameters) mode_select();
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
        case Source::Raw:
            while (length) {
                if (sector_off_ == sector_out_.size()) {
                    if (src_ == Source::Media && length >= kBlock) {
                        // Whole sectors straight from the image.
                        const u32 n = static_cast<u32>(length / kBlock);
                        if (!image_ || !image_->read_data(sector_lba_, dst, n)) {
                            check_condition(kMediumError, 0x11);  // unrecovered read error
                            return false;
                        }
                        sector_lba_ += n;
                        dst += std::size_t{n} * kBlock;
                        length -= std::size_t{n} * kBlock;
                        continue;
                    }
                    if (!next_sector()) return false;
                }
                const std::size_t n = std::min(length, sector_out_.size() - sector_off_);
                std::memcpy(dst, sector_out_.data() + sector_off_, n);
                sector_off_ += n;
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
    if (src_ == Source::Parameters) reply_.insert(reply_.end(), src, src + length);
    return true;
}

// Whether `count` sectors from `lba` are all on the disc (none between
// sessions) and, for `data_only`, none of them audio; if not, the command
// ends with the reason.
bool AtapiCdrom::readable(u32 lba, u64 count, bool data_only) {
    const u64 end = u64{lba} + count;
    for (u64 l = lba; l < end;) {
        const CdImage::Track* t = image_->track_at(static_cast<u32>(l));
        if (!t) {
            check_condition(kIllegalRequest, 0x21);  // logical block address out of range
            return false;
        }
        if (data_only && t->type == CdImage::TrackType::Audio) {
            check_condition(kIllegalRequest, 0x64);  // illegal mode for this track
            return false;
        }
        l = t->end;
    }
    return true;
}

// What the sector at `lba` is, and whether it is what READ CD's expected
// sector type asks for (0: any).
bool AtapiCdrom::sector_kind(u32 lba, u8 type, Kind& kind) {
    const CdImage::Track* t = image_->track_at(lba);
    if (!t) return false;
    switch (t->type) {
        case CdImage::TrackType::Audio: kind = Kind::Audio; break;
        case CdImage::TrackType::Mode1: kind = Kind::Mode1; break;
        case CdImage::TrackType::Mode2:
            if (type == 3) kind = Kind::Formless;
            else {
                if (!image_->read_raw(lba, raw_.data())) return false;
                kind = (raw_[18] & 0x20) ? Kind::Form2 : Kind::Form1;  // the subheader's submode: form 2
            }
            break;
    }
    static constexpr Kind kWanted[6] = {Kind::Audio, Kind::Audio, Kind::Mode1, Kind::Formless, Kind::Form1, Kind::Form2};
    return type == 0 || (type <= 5 && kind == kWanted[type]);
}

// The bytes READ CD returns of a sector of a kind: the fields selected in
// byte 9 (sync, header codes, user data, EDC/ECC). An audio sector is its
// 2352 bytes of samples, if user data is asked for.
unsigned AtapiCdrom::field_bytes(Kind kind, u8 flags) const {
    if (kind == Kind::Audio) return (flags & 0x10) ? 2352 : 0;
    const unsigned headers = (flags >> 5) & 3;
    const bool xa = kind == Kind::Form1 || kind == Kind::Form2;
    static constexpr unsigned kUser[5] = {0, 2048, 2336, 2048, 2324}, kEdc[5] = {0, 288, 0, 280, 4};
    const unsigned k = static_cast<unsigned>(kind);
    return ((flags & 0x80) ? 12 : 0) + ((headers & 1) ? 4 : 0) + ((headers & 2) && xa ? 8 : 0) +
           ((flags & 0x10) ? kUser[k] : 0) + ((flags & 0x08) ? kEdc[k] : 0);
}

// The next sector of a READ (its user data) or READ CD (the fields asked for).
bool AtapiCdrom::next_sector() {
    sector_out_.clear();
    sector_off_ = 0;
    const u32 lba = sector_lba_++;
    if (src_ == Source::Media) {
        sector_out_.resize(kBlock);
        if (image_ && image_->read_data(lba, sector_out_.data())) return true;
        check_condition(kMediumError, 0x11);
        return false;
    }
    Kind kind;
    if (!image_ || !sector_kind(lba, raw_type_, kind) || !image_->read_raw(lba, raw_.data())) {
        check_condition(kMediumError, 0x11);
        return false;
    }
    const u8* s = raw_.data();
    auto add = [&](unsigned from, unsigned to) { sector_out_.insert(sector_out_.end(), s + from, s + to); };
    const u8 f = raw_flags_;
    const unsigned headers = (f >> 5) & 3;
    switch (kind) {
        case Kind::Audio:
            if (f & 0x10) add(0, 2352);
            break;
        case Kind::Mode1:
        case Kind::Formless:
            if (f & 0x80) add(0, 12);
            if (headers & 1) add(12, 16);
            if (f & 0x10) add(16, kind == Kind::Mode1 ? 2064 : 2352);
            if ((f & 0x08) && kind == Kind::Mode1) add(2064, 2352);
            break;
        case Kind::Form1:
        case Kind::Form2: {
            const unsigned user_end = kind == Kind::Form1 ? 2072 : 2348;
            if (f & 0x80) add(0, 12);
            if (headers & 1) add(12, 16);
            if (headers & 2) add(16, 24);
            if (f & 0x10) add(24, user_end);
            if (f & 0x08) add(user_end, 2352);
            break;
        }
    }
    return true;
}

void AtapiCdrom::read_blocks(u64 lba, u64 count) {
    if (!need_media()) return;
    if (lba + count > blocks()) return check_condition(kIllegalRequest, 0x21);  // LBA out of range
    if (count == 0) return good();
    if (!readable(static_cast<u32>(lba), count, true)) return;
    stop_play(kNoAudioStatus);  // a read ends audio play
    src_ = Source::Media;
    sector_lba_ = static_cast<u32>(lba);
    sector_out_.clear();
    sector_off_ = 0;
    power_ = 0xFF;
    data_phase(true, count * kBlock);
}

void AtapiCdrom::read_cd(u64 lba, u64 count, u8 type, u8 flags, u8 subchannel) {
    if (!need_media()) return;
    if (type > 5) return check_condition(kIllegalRequest, 0x24);
    // Neither C2 error pointers nor sub-channel data are offered (capabilities page).
    if ((flags & 0x06) || subchannel) return check_condition(kIllegalRequest, 0x24);
    if (lba + count > blocks()) return check_condition(kIllegalRequest, 0x21);
    if (count == 0) return good();
    if (!readable(static_cast<u32>(lba), count, false)) return;
    u64 total = 0;
    for (u64 l = lba; l < lba + count; ++l) {
        Kind kind;
        if (!sector_kind(static_cast<u32>(l), type, kind)) return check_condition(kIllegalRequest, 0x64);
        total += field_bytes(kind, flags);
    }
    if (total == 0) return good();
    stop_play(kNoAudioStatus);
    src_ = Source::Raw;
    raw_type_ = type;
    raw_flags_ = flags;
    sector_lba_ = static_cast<u32>(lba);
    sector_out_.clear();
    sector_off_ = 0;
    power_ = 0xFF;
    data_phase(true, total);
}

std::vector<u8> AtapiCdrom::mode_page(u8 page, unsigned control) const {
    const bool changeable = control == 1, defaults = control == 2;
    std::vector<u8> p;
    switch (page) {
        case 0x01:  // read error recovery
            p = {0x01, 0x06, 0x00, 0x05, 0, 0, 0, 0};
            break;
        case 0x0D:  // CD device parameters: inactivity timer, 60 S per M, 75 F per S
            p = {0x0D, 0x06, 0x00, 0x00, 0x00, 0x3C, 0x00, 0x4B};
            break;
        case 0x0E:  // CD audio control: immediate; output ports 0 and 1's channels and volumes
            if (changeable) return {0x0E, 0x0E, 0, 0, 0, 0, 0, 0, 0x0F, 0xFF, 0x0F, 0xFF, 0, 0, 0, 0};
            p = {0x0E, 0x0E, 0x04, 0, 0, 0, 0, 0, 0x01, 0xFF, 0x02, 0xFF, 0, 0, 0, 0};
            if (!defaults) p[8] = port_channels_[0], p[9] = port_volume_[0], p[10] = port_channels_[1], p[11] = port_volume_[1];
            return p;
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
    // Nothing else here can be changed.
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

// MODE SELECT's parameter list (now in reply_): the CD audio control page's
// channels and volumes are taken; nothing else can be changed.
void AtapiCdrom::mode_select() {
    const std::vector<u8>& d = reply_;
    const bool ten = cdb_[0] == 0x55;
    const std::size_t header = ten ? 8 : 4;
    if (d.size() < header) return;
    std::size_t p = header + (ten ? be16(&d[6]) : d[3]);  // past the block descriptors
    while (p + 2 <= d.size()) {
        const u8 code = d[p] & 0x3F, len = d[p + 1];
        if (code == 0x0E && len >= 10 && p + 12 <= d.size()) {
            port_channels_[0] = d[p + 8] & 0x0F;
            port_volume_[0] = d[p + 9];
            port_channels_[1] = d[p + 10] & 0x0F;
            port_volume_[1] = d[p + 11];
        }
        p += 2u + len;
    }
}

void AtapiCdrom::read_toc() {
    if (!need_media()) return;
    const bool msf = cdb_[1] & 0x02;
    unsigned format = cdb_[2] & 0x0F;
    if (format == 0) format = cdb_[9] >> 6;  // where SFF-8020 drives took it
    const u8 track = cdb_[6];
    const auto& tracks = image_->tracks();
    const auto& sessions = image_->sessions();
    const CdImage::Track& last = tracks.back();
    std::vector<u8> d(4, 0);
    auto entry = [&](u8 control, u8 number, u32 lba) {
        const u8 e[8] = {0, static_cast<u8>(0x10 | control), number, 0, 0, 0, 0, 0};
        d.insert(d.end(), e, e + 8);
        put_addr(&d[d.size() - 4], lba, msf);
    };
    switch (format) {
        case 0:  // the tracks from `track` on, then the lead-out
            d[2] = tracks.front().number, d[3] = last.number;
            if (track > last.number && track != 0xAA) return check_condition(kIllegalRequest, 0x24);
            for (const CdImage::Track& t : tracks)
                if (t.number >= track && track != 0xAA) entry(t.control, t.number, t.start);
            entry(last.control, 0xAA, image_->leadout());
            break;
        case 1: {  // session information: the first track of the last session
            const CdImage::Session& s = sessions.back();
            d[2] = sessions.front().number, d[3] = s.number;
            const CdImage::Track& t = tracks[s.first_track - tracks.front().number];
            entry(t.control, t.number, t.start);
            break;
        }
        case 2: {  // the raw TOC from session `track` on: points A0, A1, A2 and the tracks of each
            d[2] = sessions.front().number, d[3] = sessions.back().number;
            if (track > sessions.back().number) return check_condition(kIllegalRequest, 0x24);
            const u8 disc_type = image_->xa() ? 0x20 : 0x00;  // CD-ROM XA, else CD-DA or CD-ROM
            for (const CdImage::Session& s : sessions) {
                if (s.number < track) continue;
                const CdImage::Track& first = tracks[s.first_track - tracks.front().number];
                const CdImage::Track& end = tracks[s.last_track - tracks.front().number];
                auto point = [&](u8 control, u8 p, u8 pm, u8 ps, u8 pf) {
                    const u8 e[11] = {s.number, static_cast<u8>(0x10 | control), 0, p, 0, 0, 0, 0, pm, ps, pf};
                    d.insert(d.end(), e, e + 11);
                };
                point(first.control, 0xA0, first.number, disc_type, 0);
                point(end.control, 0xA1, end.number, 0, 0);
                const Msf lo = to_msf(s.leadout);
                point(end.control, 0xA2, lo.m, lo.s, lo.f);
                for (u8 n = s.first_track; n <= s.last_track; ++n) {
                    const CdImage::Track& t = tracks[n - tracks.front().number];
                    const Msf a = to_msf(t.start);
                    point(t.control, n, a.m, a.s, a.f);
                }
            }
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
    d[1] = audio_status_;
    // A play's end (or failure) is reported once.
    if (audio_status_ == kPlayDone || audio_status_ == kPlayError) audio_status_ = kNoAudioStatus;
    if (subq) {
        switch (format) {
            case 1: {  // current position: absolute, and within its track
                const u32 lba = audio_lba();
                const CdImage::Track* t = image_->track_at(lba);
                const u8 control = t ? t->control : 0x4;
                u8 e[12] = {0x01, static_cast<u8>(0x10 | control), static_cast<u8>(t ? t->number : 1),
                            static_cast<u8>(t && lba < t->start ? 0 : 1)};
                put_addr(e + 4, lba, msf);
                put_relative(e + 8, t ? static_cast<s64>(lba) - t->start : static_cast<s64>(lba), msf);
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

// --- Audio play ---------------------------------------------------------

u64 AtapiCdrom::play_position() const {
    if (audio_status_ != kPlaying) return play_from_;
    const double played = static_cast<double>(sched_.now() - play_t0_) * kAudioRate / cpu_hz_;
    return std::min<u64>(play_from_ + static_cast<u64>(played), u64{play_end_} * kSamplesPerSector);
}

u32 AtapiCdrom::audio_lba() const {
    if (audio_status_ != kPlaying && audio_status_ != kPaused) return cur_lba_;
    const u32 lba = static_cast<u32>(play_position() / kSamplesPerSector);
    return play_end_ ? std::min(lba, play_end_ - 1) : lba;
}

// Plays from sector `from` up to (not including) `to`, as far as the audio
// tracks go on from it: a data track or the gap between sessions ends it.
void AtapiCdrom::play(u32 from, u32 to) {
    for (u32 l = from; l < to;) {
        const CdImage::Track* t = image_->track_at(l);
        if (!t || t->type != CdImage::TrackType::Audio) {
            to = l;
            break;
        }
        l = t->end;
    }
    const bool was = audio_status_ == kPlaying;
    play_from_ = u64{from} * kSamplesPerSector;
    play_end_ = to;
    play_t0_ = sched_.now();
    audio_status_ = kPlaying;
    cur_lba_ = from;
    const double samples = static_cast<double>(u64{to} * kSamplesPerSector - play_from_);
    play_timer_.schedule_in(static_cast<u64>(samples / kAudioRate * cpu_hz_) + 1);
    if (!was && on_audio) on_audio(true);
}

void AtapiCdrom::stop_play(u8 status) {
    if (audio_status_ != kPlaying && audio_status_ != kPaused) return;
    const bool was = audio_status_ == kPlaying;
    cur_lba_ = audio_lba();
    play_from_ = play_position();
    play_timer_.cancel();
    audio_status_ = status;
    if (was && on_audio) on_audio(false);
}

void AtapiCdrom::analog(s16* out, std::size_t count, double rate) {
    std::fill(out, out + count * 2, s16{0});
    if (audio_status_ != kPlaying || !image_ || count == 0) return;
    const double now = static_cast<double>(sched_.now()), t0 = static_cast<double>(play_t0_);
    const double cycles_per_frame = cpu_hz_ / rate, samples_per_cycle = kAudioRate / cpu_hz_;
    const double end = static_cast<double>(u64{play_end_} * kSamplesPerSector);
    // A sample of the disc (0 left, 1 right), through the two-sector cache.
    auto sample = [this](u64 index, unsigned channel) -> int {
        const u32 lba = static_cast<u32>(index / kSamplesPerSector);
        const unsigned slot = lba & 1;
        if (audio_cached_[slot] != lba) {
            if (!image_->read_raw(lba, audio_cache_[slot].data())) audio_cache_[slot].fill(0);
            audio_cached_[slot] = lba;
        }
        const u8* p = audio_cache_[slot].data() + (index % kSamplesPerSector) * 4 + channel * 2;
        return static_cast<s16>(p[0] | (p[1] << 8));
    };
    for (std::size_t k = 0; k < count; ++k) {
        const double t = now - static_cast<double>(count - k) * cycles_per_frame;
        if (t < t0) continue;
        const double pos = static_cast<double>(play_from_) + (t - t0) * samples_per_cycle;
        if (pos + 1 >= end) break;
        const u64 i = static_cast<u64>(pos);
        const double frac = pos - static_cast<double>(i);
        double ch[2];
        for (unsigned c = 0; c < 2; ++c) ch[c] = sample(i, c) * (1 - frac) + sample(i + 1, c) * frac;
        // Each output port: the channels it is given (both: their mean), at its volume.
        for (unsigned port = 0; port < 2; ++port) {
            const u8 sel = port_channels_[port] & 3;
            const double v = sel == 3 ? (ch[0] + ch[1]) / 2 : sel == 1 ? ch[0] : sel == 2 ? ch[1] : 0.0;
            out[k * 2 + port] = static_cast<s16>(std::lround(std::clamp(v * port_volume_[port] / 255.0, -32768.0, 32767.0)));
        }
    }
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
    // PLAY AUDIO's checks: the start on the disc and in an audio track.
    auto start_play = [this](u64 from, u64 to) {
        if (!need_media()) return;
        if (to <= from) return good();  // nothing to play: not an error
        if (from >= blocks()) return check_condition(kIllegalRequest, 0x21);
        const CdImage::Track* t = image_->track_at(static_cast<u32>(from));
        if (!t) return check_condition(kIllegalRequest, 0x21);
        if (t->type != CdImage::TrackType::Audio) return check_condition(kIllegalRequest, 0x64);
        play(static_cast<u32>(from), static_cast<u32>(std::min<u64>(to, blocks())));
        good();
    };
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
        case 0x15: case 0x55:  // MODE SELECT: the parameter list, then mode_select()
            src_ = Source::Parameters;
            reply_.clear();
            return data_phase(false, op == 0x15 ? cdb_[4] : be16(&cdb_[7]));
        case 0x1B: {  // START STOP UNIT
            const bool loej = cdb_[4] & 0x02, start = cdb_[4] & 0x01;
            if (loej && !start) {
                if (prevent_) return check_condition(kIllegalRequest, 0x53, 0x02);  // removal prevented
                stop_play(kNoAudioStatus);
                media_ = false;
                tray_open_ = true;
                if (on_eject) on_eject();
            } else if (loej && start) {
                tray_open_ = false;
                if (image_ && !media_) {
                    media_ = true;
                    unit_attention_ = 0x28;
                }
            } else if (!start) {
                stop_play(kNoAudioStatus);  // the disc stops spinning
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
            stop_play(kNoAudioStatus);
            cur_lba_ = be32(&cdb_[2]);
            return good();
        case 0x43: return read_toc();
        case 0x42: return read_subchannel();
        case 0x44: {  // READ HEADER
            if (!need_media()) return;
            const u32 lba = be32(&cdb_[2]);
            const CdImage::Track* t = lba < blocks() ? image_->track_at(lba) : nullptr;
            if (!t) return check_condition(kIllegalRequest, 0x21);
            std::vector<u8> d(8, 0);
            d[0] = t->type == CdImage::TrackType::Audio ? 0 : t->type == CdImage::TrackType::Mode2 ? 2 : 1;
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
            const u32 lba = audio_lba();
            d[2] = static_cast<u8>(lba >> 16), d[3] = static_cast<u8>(lba >> 8), d[4] = static_cast<u8>(lba);
            return reply(std::move(d), be16(&cdb_[8]));
        }
        case 0xBB: return good();  // SET CD SPEED
        case 0x45: {  // PLAY AUDIO (10): from an LBA (all ones: where the head is) for a count
            const u32 lba = be32(&cdb_[2]);
            const u64 from = lba == 0xFFFFFFFFu ? audio_lba() : lba;
            return start_play(from, from + be16(&cdb_[7]));
        }
        case 0xA5: {  // PLAY AUDIO (12)
            const u32 lba = be32(&cdb_[2]);
            const u64 from = lba == 0xFFFFFFFFu ? audio_lba() : lba;
            return start_play(from, from + be32(&cdb_[6]));
        }
        case 0x47: {  // PLAY AUDIO MSF: from (all ones: where the head is) to an end
            const bool here = cdb_[3] == 0xFF && cdb_[4] == 0xFF && cdb_[5] == 0xFF;
            const u32 start = from_msf(&cdb_[3]), end = from_msf(&cdb_[6]);
            if (!here && (start < 150 || end < start)) return check_condition(kIllegalRequest, 0x24);
            if (end < 150) return check_condition(kIllegalRequest, 0x24);
            return start_play(here ? audio_lba() : start - 150, end - 150);
        }
        case 0x48: {  // PLAY AUDIO TRACK/INDEX: index 0 is a track's pregap, 1 the rest of it
            if (!need_media()) return;
            const auto& tracks = image_->tracks();
            const u8 first = tracks.front().number, last = tracks.back().number;
            const u8 st = cdb_[4], si = cdb_[5];
            const u8 et = std::min(cdb_[7], last), ei = cdb_[8];
            if (st < first || st > last || et < st) return check_condition(kIllegalRequest, 0x24);
            const CdImage::Track& s = tracks[st - first];
            const CdImage::Track& e = tracks[et - first];
            const u64 from = si == 0 ? static_cast<u64>(std::max<s64>(s.begin, 0)) : s.start;
            const u64 to = ei == 0 ? e.start : e.end;
            return start_play(from, to);
        }
        case 0x4B: {  // PAUSE/RESUME
            if (audio_status_ != kPlaying && audio_status_ != kPaused)
                return check_condition(kIllegalRequest, 0x2C);  // command sequence error
            if (cdb_[8] & 0x01) {
                if (audio_status_ == kPaused) {
                    const u64 left = u64{play_end_} * kSamplesPerSector - play_from_;
                    play_t0_ = sched_.now();
                    audio_status_ = kPlaying;
                    play_timer_.schedule_in(static_cast<u64>(static_cast<double>(left) / kAudioRate * cpu_hz_) + 1);
                    if (on_audio) on_audio(true);
                }
            } else if (audio_status_ == kPlaying) {
                play_from_ = play_position();
                play_timer_.cancel();
                audio_status_ = kPaused;
                if (on_audio) on_audio(false);
            }
            return good();
        }
        case 0x4E:  // STOP PLAY/SCAN
            stop_play(kNoAudioStatus);
            return good();
        default:
            UV_LOG(Debug, "atapi", "packet command %02x not supported", op);
            return check_condition(kIllegalRequest, 0x20);  // invalid command operation code
    }
}

void AtapiCdrom::describe_more(std::string& out) const {
    appendf(out, "    packet %02x (lba %u), sense %x/%02x/%02x, %s%s, last block read %u, audio status %02x\n", cdb_[0],
            static_cast<unsigned>(cdb_[2] << 24 | cdb_[3] << 16 | cdb_[4] << 8 | cdb_[5]), sense_key_, asc_, ascq_,
            media_ ? "medium in" : "no medium", tray_open_ ? ", tray open" : "", cur_lba_, audio_status_);
}

}  // namespace uv
