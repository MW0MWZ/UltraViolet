// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
// An ATAPI CD-ROM drive (SFF-8020i / MMC-1 era) reading a CD image
// (storage/cd_image.h: an .iso, a raw image or a cue sheet with audio and
// data tracks in one or more sessions): the PACKET protocol with PIO byte
// counts or DMA, and the commands such a drive answers: TEST UNIT READY,
// REQUEST SENSE (with the audio status when there is no error), INQUIRY,
// MODE SENSE/SELECT (6 and 10; the CD audio control page's port volumes and
// channels can be changed), START STOP UNIT (eject and load), PREVENT ALLOW
// MEDIUM REMOVAL, READ CAPACITY, READ (10 and 12), SEEK, REZERO UNIT, READ
// TOC (formats 0-2), READ SUB-CHANNEL, READ HEADER, READ CD (and MSF; audio,
// mode 1 and mode 2 formless, form 1 and form 2; C2 error flags, all zero;
// the Q sub-channel, formatted or raw), PLAY AUDIO (10, 12, MSF and
// track/index), PAUSE/RESUME, STOP PLAY/SCAN, MECHANISM STATUS and SET CD
// SPEED. AUDIO SCAN, optional in SFF-8020i, is not offered.
//
// Reads follow SFF-8020i's rules: a read of user data stops where the
// sectors stop being user data (audio, a pregap, a post-gap of mode 0), a
// READ CD where audio and data meet or where a sector is not the type asked
// for, with the sectors before it transferred and the first one not in the
// sense data. A play must be all audio from start to end. The commands
// SFF-8020i lists (reads, seeks, READ HEADER, SET CD SPEED, START STOP
// UNIT, a reset) stop a play.
//
// Audio is played as a real drive plays it, to its analog output: the
// cable to the system board (on an Ultra 5/10, the CS4231's AUX1 input).
// analog() gives that output as the codec samples it.
//
// What an image does not hold is not made up: a cue sheet has no ATIP, so
// the raw TOC gives the points of a pressed disc (A0-A2 and the tracks; no
// B0 or C0), and the sub-channels R-W (not offered) and the Q's ISRC and
// catalogue number are not recorded. The drive has no mechanical timing:
// a seek or the start of a play takes no time.
#pragma once

#include <array>
#include <functional>
#include <string>
#include <vector>

#include "devices/ata.h"

namespace uv {

class CdImage;

class AtapiCdrom final : public AtaDevice {
public:
    static constexpr unsigned kBlock = 2048;
    static constexpr unsigned kAudioRate = 44100;  // CD audio: 588 stereo frames a sector, 75 sectors a second
    // READ SUB-CHANNEL's audio status
    static constexpr u8 kPlaying = 0x11, kPaused = 0x12, kPlayDone = 0x13, kPlayError = 0x14, kNoAudioStatus = 0x15;

    AtapiCdrom(Scheduler& scheduler, double cpu_hz, std::string vendor = "UVIOLET", std::string product = "ATAPI CD-ROM",
               std::string revision = "1.0");
    const char* name() const override { return "atapi-cdrom"; }
    bool packet_device() const override { return true; }
    void describe_more(std::string& out) const override;

    // Puts a disc in the drive and closes the tray (nullptr: empty). The
    // guest sees a medium change.
    void insert(CdImage* image);
    bool has_media() const { return media_; }
    bool locked() const { return prevent_; }
    // The guest ejected the disc (START STOP UNIT).
    std::function<void()> on_eject;

    // The disc's addressable blocks (to the last session's lead-out).
    u32 blocks() const;

    // The analog output: `frames` stereo frames at `rate` up to the present
    // guest time, through the audio control page's volumes (silence when
    // not playing).
    void analog(s16* frames, std::size_t count, double rate);
    // Audio play started or stopped (the analog output has sound, or not).
    std::function<void(bool)> on_audio;
    u8 audio_status() const { return audio_status_; }

protected:
    void execute(u8 command) override;
    bool data_in(u8* dst, std::size_t length) override;
    bool data_out(const u8* src, std::size_t length) override;
    void transfer_done(bool dma, bool to_host) override;
    void before_drq(bool to_host, std::size_t length) override;
    void reset_state() override;
    void hardware_reset() override;
    u8 reset_status() const override { return 0; }

private:
    enum class Source { Reply, Media, Raw, Parameters, Packet, Identify };
    // What a sector is, as READ CD's expected sector type names it.
    enum class Kind { Audio, Mode1, Formless, Form1, Form2, Mode0 };

    void packet();
    void good();
    void check_condition(u8 key, u8 asc, u8 ascq = 0);
    // The same, naming the block it is about in the sense data's information field.
    void check_condition_at(u32 lba, u8 key, u8 asc);
    // Whether a sector holds user data a READ can take.
    bool user_data(u32 lba) const;
    // The sub-channels of a sector: Q (as on the disc, BCD with its CRC) and P.
    void subchannel_q(u32 lba, u8* q, bool& p) const;
    bool need_media();
    void reply(std::vector<u8> data, std::size_t allocation);
    void data_phase(bool to_host, u64 bytes);
    void read_blocks(u64 lba, u64 count);
    void read_cd(u64 lba, u64 count, u8 type, u8 flags, u8 subchannel);
    bool sector_kind(u32 lba, u8 type, Kind& kind);
    unsigned field_bytes(Kind kind, u8 flags, u8 subchannel) const;
    bool next_sector();
    void mode_sense(bool ten);
    void mode_select();
    void read_toc();
    void read_subchannel();
    void build_identify();
    void set_features();
    void signature();
    std::vector<u8> mode_page(u8 page, unsigned control) const;

    // Audio play
    void play(u32 from, u32 to);
    void stop_play(u8 status);
    u64 play_position() const;  // in samples (588 a sector) from LBA 0
    u32 audio_lba() const;      // the sector playing, or the last read

    std::string vendor_, product_, revision_;
    CdImage* image_ = nullptr;
    bool media_ = false, tray_open_ = false, prevent_ = false;
    u8 sense_key_ = 0, asc_ = 0, ascq_ = 0;
    u32 info_ = 0;
    bool info_valid_ = false;
    // A read that stops short: the sense it ends with, once the sectors
    // before `stop_lba_` are transferred (0: none).
    u8 stop_asc_ = 0;
    u32 stop_lba_ = 0;
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

    // READ and READ CD: sectors in turn, the fields wanted from each
    u32 sector_lba_ = 0;
    u8 raw_type_ = 0, raw_flags_ = 0, raw_sub_ = 0;
    std::array<u8, 2352> raw_{};
    std::vector<u8> sector_out_;
    std::size_t sector_off_ = 0;

    // Audio play: the samples from `play_from_` (at guest time `play_t0_`)
    // up to the sector `play_end_`; when paused, the position reached.
    Timer play_timer_;
    u8 audio_status_ = kNoAudioStatus;
    u64 play_from_ = 0, play_t0_ = 0;
    u32 play_end_ = 0;
    // The CD audio control page: each output port's channels and volume.
    std::array<u8, 2> port_channels_{0x01, 0x02}, port_volume_{0xFF, 0xFF};
    // Two sectors of audio, kept for analog()
    std::array<std::array<u8, 2352>, 2> audio_cache_{};
    std::array<u32, 2> audio_cached_{~0u, ~0u};

    std::array<u8, 512> identify_{};
};

}  // namespace uv
