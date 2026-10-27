// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
// An ATAPI CD-ROM drive (SFF-8020i / MMC-1 era) reading a 2048-byte-sector
// image (an .iso): the PACKET protocol with PIO byte counts or DMA, and
// the commands such a drive answers: TEST UNIT READY, REQUEST SENSE,
// INQUIRY, MODE SENSE/SELECT (6 and 10), START STOP UNIT (eject and load),
// PREVENT ALLOW MEDIUM REMOVAL, READ CAPACITY, READ (10 and 12), SEEK,
// READ TOC (formats 0-2), READ SUB-CHANNEL, READ HEADER, READ CD (MSF),
// MECHANISM STATUS and SET CD SPEED. The disc is a single mode 1 data
// track; READ CD builds raw sectors (sync, header, EDC/ECC) from it.
#pragma once

#include <array>
#include <functional>
#include <string>
#include <vector>

#include "devices/ata.h"

namespace uv {

class AtapiCdrom final : public AtaDevice {
public:
    static constexpr unsigned kBlock = 2048;

    AtapiCdrom(Scheduler& scheduler, double cpu_hz, std::string vendor = "UVIOLET", std::string product = "ATAPI CD-ROM",
               std::string revision = "1.0");
    const char* name() const override { return "atapi-cdrom"; }
    bool packet_device() const override { return true; }

    // Puts a disc in the drive and closes the tray (nullptr: empty). The
    // guest sees a medium change.
    void insert(DiskImage* image);
    bool has_media() const { return media_; }
    bool locked() const { return prevent_; }
    // The guest ejected the disc (START STOP UNIT).
    std::function<void()> on_eject;

    u32 blocks() const;

protected:
    void execute(u8 command) override;
    bool data_in(u8* dst, std::size_t length) override;
    bool data_out(const u8* src, std::size_t length) override;
    void transfer_done(bool dma, bool to_host) override;
    void before_drq(bool to_host, std::size_t length) override;
    void reset_state() override;
    u8 reset_status() const override { return 0; }

private:
    enum class Source { Reply, Media, Raw, Discard, Packet, Identify };

    void packet();
    void good();
    void check_condition(u8 key, u8 asc, u8 ascq = 0);
    bool need_media();
    void reply(std::vector<u8> data, std::size_t allocation);
    void data_phase(bool to_host, u64 bytes);
    void read_blocks(u64 lba, u64 count);
    void read_cd(u64 lba, u64 count, u8 type, u8 flags, u8 subchannel);
    void mode_sense(bool ten);
    void read_toc();
    void read_subchannel();
    bool build_raw_sector(u32 lba);
    void build_identify();
    void set_features();
    void signature();
    std::vector<u8> mode_page(u8 page, unsigned control) const;

    std::string vendor_, product_, revision_;
    DiskImage* image_ = nullptr;
    bool media_ = false, tray_open_ = false, prevent_ = false;
    u8 sense_key_ = 0, asc_ = 0, ascq_ = 0;
    u8 unit_attention_ = 0x29;  // ASC of a pending unit attention (0: none); power-on reset
    u8 pio_mode_ = 0x08, dma_mode_ = 0, power_ = 0xFF;

    // The current packet command
    std::array<u8, 12> cdb_{};
    bool packet_cmd_ = false;  // the data phase belongs to a packet command
    bool dma_ = false;
    std::size_t byte_limit_ = 0xFFFE;
    Source src_ = Source::Reply;
    std::vector<u8> reply_;
    u64 pos_ = 0;
    u32 cur_lba_ = 0;  // last block read (for READ SUB-CHANNEL)

    // READ CD: the fields wanted from each raw sector
    u32 raw_lba_ = 0;
    u8 raw_flags_ = 0;
    std::array<u8, 2352> raw_{};
    std::size_t raw_off_ = 0, raw_len_ = 0;
    std::vector<u8> raw_out_;

    std::array<u8, 512> identify_{};
};

}  // namespace uv
