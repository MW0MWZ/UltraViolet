// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
// A CD image, as the drive sees the disc: tracks of audio or data in one or
// more sessions, each sector 2352 bytes on the disc. It is opened from
//   - an .iso (or any other file): one mode 1 track of 2048-byte sectors;
//   - a raw image of 2352-byte sectors (a whole number of them, beginning
//     with a sector's sync pattern): one data track, its mode the first
//     sector's;
//   - a CDRWIN cue sheet (.cue) over BINARY (little-endian audio) or
//     MOTOROLA (big-endian audio) files: tracks of AUDIO, MODE1/2048,
//     MODE1/2352, MODE2/2336 or MODE2/2352, INDEX 00 and 01, PREGAP and
//     POSTGAP (sectors not in the file), FLAGS (DCP, 4CH, PRE) and sessions
//     (REM SESSION).
// Addresses are logical block addresses: LBA 0 is track 1's index 1, at
// 00:02:00. A later session begins after the earlier one's lead-out and
// its own lead-in (Orange Book: a first session's lead-out is 6750 sectors,
// a later one's 2250; a lead-in 4500), with the 150-sector pregap of its
// first track.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "core/types.h"
#include "storage/disk_image.h"

namespace uv {

class CdImage {
public:
    static constexpr unsigned kRawSector = 2352, kDataSector = 2048;
    // Sectors of the gaps a later session begins after (see above).
    static constexpr u32 kFirstLeadOut = 6750, kLaterLeadOut = 2250, kLeadIn = 4500, kPregap = 150;

    enum class TrackType { Audio, Mode1, Mode2 };
    struct Track {
        u8 number = 1;
        TrackType type = TrackType::Mode1;
        u8 session = 1;
        u8 control = 0x4;  // the Q sub-channel's control field: 4 data, 0 audio; 2 copy permitted, 8 four-channel, 1 pre-emphasis
        s64 begin = 0;     // the track's first sector (its pregap, index 0)
        u32 start = 0;     // index 1: the track's address in the TOC
        u32 end = 0;       // the first sector after it
        // The sectors held in a file: `count` from `first`, at `offset`,
        // `size` bytes each (2048: user data only, 2336: mode 2 without sync
        // and header, 2352: the whole sector).
        int file = -1;
        s64 first = 0;
        u32 count = 0;
        u64 offset = 0;
        unsigned size = kRawSector;
        bool swap = false;  // audio held big-endian (MOTOROLA)
    };
    struct Session {
        u8 number = 1, first_track = 1, last_track = 1;
        u32 leadout = 0;  // its lead-out's address
    };

    bool open(const std::string& path);
    void close();
    bool is_open() const { return !tracks_.empty(); }
    const std::string& path() const { return path_; }
    // Why the last open failed.
    const std::string& error() const { return error_; }

    const std::vector<Track>& tracks() const { return tracks_; }
    const std::vector<Session>& sessions() const { return sessions_; }
    // The lead-out of the last session: the disc's end.
    u32 leadout() const { return sessions_.empty() ? 0 : sessions_.back().leadout; }
    // The track a sector belongs to (its pregap included); nullptr between
    // sessions or past the end.
    const Track* track_at(u32 lba) const;
    // Whether any track is mode 2 (the disc is CD-ROM XA).
    bool xa() const;

    // A sector as on the disc: 2352 bytes of audio (16-bit little-endian
    // samples, left first), or a data sector with sync and header (and,
    // from 2048-byte images, mode 1's EDC and ECC computed). Gap sectors not
    // held in a file read as silence or as zero data. False: no such
    // sector, or the file could not be read.
    bool read_raw(u32 lba, u8* out);
    // Data sectors' 2048 bytes of user data each: mode 1, or mode 2 form 1.
    // False if one is audio, not on the disc, or could not be read.
    bool read_data(u32 lba, u8* out, u32 count = 1);

    // Reads so far (for an activity light).
    u64 accesses() const;

private:
    bool open_cue(const std::string& path);
    bool open_single(const std::string& path);
    void fail(std::string why);

    std::string path_, error_;
    std::vector<std::unique_ptr<DiskImage>> files_;
    std::vector<Track> tracks_;
    std::vector<Session> sessions_;
};

// ECMA-130 mode 1: the EDC (bytes 0x810-0x813) and the P and Q parity
// (0x81C-0x92F) of a sector whose sync, header and user data are in place.
void cd_mode1_ecc(u8* sector);
// Sync and header (MSF in BCD, mode) for a sector at `lba`.
void cd_sync_header(u8* sector, u32 lba, u8 mode);

}  // namespace uv
