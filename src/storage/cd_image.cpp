// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
#include "storage/cd_image.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "core/log.h"

namespace uv {

namespace fs = std::filesystem;

namespace {

constexpr u8 kSync[12] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};

u8 bcd(unsigned v) { return static_cast<u8>(((v / 10) << 4) | (v % 10)); }

// ECMA-130 error detection and correction: a 32-bit EDC, then the P and Q
// Reed-Solomon parity over GF(2^8) (x^8 + x^4 + x^3 + x^2 + 1).
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

// mm:ss:ff as a count of sectors; -1 if malformed.
long parse_msf(const std::string& s) {
    unsigned m = 0, sec = 0, f = 0;
    char c1 = 0, c2 = 0;
    std::istringstream in(s);
    if (!(in >> m >> c1 >> sec >> c2 >> f) || c1 != ':' || c2 != ':' || sec > 59 || f > 74) return -1;
    return static_cast<long>((m * 60 + sec) * 75 + f);
}

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// A file a cue sheet names, beside it: as written, else the one whose name
// matches it but for case (sheets made on hosts that ignore case).
std::string find_beside(const fs::path& dir, const std::string& name) {
    fs::path p = fs::path(name).is_absolute() ? fs::path(name) : dir / name;
    std::error_code ec;
    if (fs::exists(p, ec)) return p.string();
    const std::string want = upper(fs::path(name).filename().string());
    for (const auto& e : fs::directory_iterator(p.parent_path(), ec))
        if (upper(e.path().filename().string()) == want) return e.path().string();
    return p.string();
}

}  // namespace

void cd_sync_header(u8* s, u32 lba, u8 mode) {
    std::memcpy(s, kSync, 12);
    const u32 x = lba + 150;
    s[12] = bcd(x / (75 * 60)), s[13] = bcd((x / 75) % 60), s[14] = bcd(x % 75), s[15] = mode;
}

void cd_mode1_ecc(u8* s) {
    const EccTables& t = ecc_tables();
    u32 edc = 0;
    for (unsigned i = 0; i < 0x810; ++i) edc = (edc >> 8) ^ t.edc[(edc ^ s[i]) & 0xFF];
    s[0x810] = static_cast<u8>(edc), s[0x811] = static_cast<u8>(edc >> 8);
    s[0x812] = static_cast<u8>(edc >> 16), s[0x813] = static_cast<u8>(edc >> 24);
    std::memset(s + 0x814, 0, 8);
    ecc_block(s + 0xC, 86, 24, 2, 86, s + 0x81C);   // P parity
    ecc_block(s + 0xC, 52, 43, 86, 88, s + 0x8C8);  // Q parity
}

void CdImage::close() {
    files_.clear();
    tracks_.clear();
    sessions_.clear();
    path_.clear();
}

void CdImage::fail(std::string why) {
    error_ = std::move(why);
    close();
}

bool CdImage::open(const std::string& path) {
    close();
    error_.clear();
    const std::string ext = upper(fs::path(path).extension().string());
    const bool ok = ext == ".CUE" ? open_cue(path) : open_single(path);
    if (!ok) {
        if (error_.empty()) error_ = "cannot open " + path;
        close();
        return false;
    }
    path_ = path;
    return true;
}

// An .iso, or a raw image of whole 2352-byte sectors beginning with a sync
// pattern.
bool CdImage::open_single(const std::string& path) {
    auto f = std::make_unique<DiskImage>();
    if (!f->open(path, true)) return false;
    Track t;
    u8 head[16] = {};
    if (f->size() >= kRawSector && f->size() % kRawSector == 0 && f->read(0, head, 16) &&
        std::memcmp(head, kSync, 12) == 0) {
        t.size = kRawSector;
        t.type = head[15] == 2 ? TrackType::Mode2 : TrackType::Mode1;
    } else {
        t.size = kDataSector;
    }
    t.count = static_cast<u32>(std::min<u64>((f->size() + t.size - 1) / t.size, 0x7FFFFFFF));
    t.file = 0;
    t.end = t.count;
    files_.push_back(std::move(f));
    tracks_.push_back(t);
    sessions_.push_back({1, 1, 1, t.end});
    return true;
}

bool CdImage::open_cue(const std::string& path) {
    std::ifstream in(path);
    if (!in) return false;
    const fs::path dir = fs::path(path).parent_path();

    // What the sheet says of each track (index times in the file's sectors).
    struct Sheet {
        Track t;
        long index0 = -1, index1 = -1, pregap = 0, postgap = 0;
    };
    std::vector<Sheet> sheet;
    int file = -1;
    bool swap = false;
    u8 session = 1;
    std::string line;
    unsigned line_no = 0;
    auto bad = [&](const std::string& why) {
        fail(fs::path(path).filename().string() + " line " + std::to_string(line_no) + ": " + why);
        return false;
    };
    while (std::getline(in, line)) {
        ++line_no;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream l(line);
        std::string word;
        if (!(l >> word)) continue;
        word = upper(word);
        if (word == "FILE") {
            std::string rest, name, type;
            std::getline(l, rest);
            rest.erase(0, rest.find_first_not_of(" \t"));
            if (!rest.empty() && rest[0] == '"') {
                const auto close_quote = rest.find('"', 1);
                if (close_quote == std::string::npos) return bad("unterminated file name");
                name = rest.substr(1, close_quote - 1);
                std::istringstream(rest.substr(close_quote + 1)) >> type;
            } else {
                std::istringstream(rest) >> name >> type;
            }
            type = upper(type);
            if (type != "BINARY" && type != "MOTOROLA") return bad("file type " + type + " is not supported (BINARY or MOTOROLA)");
            auto f = std::make_unique<DiskImage>();
            const std::string where = find_beside(dir, name);
            if (!f->open(where, true)) return bad("cannot open " + where);
            files_.push_back(std::move(f));
            file = static_cast<int>(files_.size()) - 1;
            swap = type == "MOTOROLA";
        } else if (word == "TRACK") {
            unsigned n = 0;
            std::string mode;
            if (!(l >> n >> mode) || n < 1 || n > 99) return bad("bad TRACK");
            if (file < 0) return bad("TRACK before FILE");
            if (!sheet.empty() && n != sheet.back().t.number + 1u) return bad("tracks out of order");
            if (sheet.empty() && n != 1) return bad("the first track is not track 1");
            Sheet s;
            s.t.number = static_cast<u8>(n);
            s.t.session = session;
            s.t.file = file;
            mode = upper(mode);
            if (mode == "AUDIO") s.t.type = TrackType::Audio, s.t.size = kRawSector, s.t.control = 0, s.t.swap = swap;
            else if (mode == "MODE1/2048") s.t.type = TrackType::Mode1, s.t.size = kDataSector;
            else if (mode == "MODE1/2352") s.t.type = TrackType::Mode1, s.t.size = kRawSector;
            else if (mode == "MODE2/2336") s.t.type = TrackType::Mode2, s.t.size = 2336;
            else if (mode == "MODE2/2352") s.t.type = TrackType::Mode2, s.t.size = kRawSector;
            else return bad("track mode " + mode + " is not supported");
            sheet.push_back(s);
        } else if (word == "INDEX") {
            unsigned n = 0;
            std::string at;
            if (sheet.empty() || !(l >> n >> at)) return bad("bad INDEX");
            const long v = parse_msf(at);
            if (v < 0) return bad("bad time " + at);
            if (n == 0) sheet.back().index0 = v;
            else if (n == 1) sheet.back().index1 = v;
            // (indexes above 1 divide a track for players; nothing here uses them)
        } else if (word == "PREGAP" || word == "POSTGAP") {
            std::string at;
            if (sheet.empty() || !(l >> at) || parse_msf(at) < 0) return bad("bad " + word);
            (word == "PREGAP" ? sheet.back().pregap : sheet.back().postgap) = parse_msf(at);
        } else if (word == "FLAGS") {
            if (sheet.empty()) return bad("FLAGS before TRACK");
            for (std::string flag; l >> flag;) {
                flag = upper(flag);
                if (flag == "DCP") sheet.back().t.control |= 0x2;
                else if (flag == "4CH") sheet.back().t.control |= 0x8;
                else if (flag == "PRE") sheet.back().t.control |= 0x1;
            }
        } else if (word == "REM") {
            std::string what;
            unsigned n = 0;
            if ((l >> what) && upper(what) == "SESSION" && (l >> n)) {
                if (n < 1 || n > 99 || n < session) return bad("bad session number");
                session = static_cast<u8>(n);
            }
        }
        // CATALOG, CDTEXTFILE, ISRC, PERFORMER, SONGWRITER, TITLE: not kept.
    }
    if (sheet.empty()) return bad("no tracks");
    for (const Sheet& s : sheet)
        if (s.index1 < 0 || (s.index0 >= 0 && s.index0 > s.index1))
            return bad("track " + std::to_string(s.t.number) + " has no INDEX 01 (or its INDEX 00 follows it)");

    // Where each track's sectors are in its file: one after another, the
    // file's sectors between two tracks' first indexes the earlier track's.
    for (std::size_t i = 0; i < sheet.size(); ++i) {
        Sheet& s = sheet[i];
        const long first = s.index0 >= 0 ? s.index0 : s.index1;
        if (i > 0 && sheet[i - 1].t.file == s.t.file) {
            const Sheet& p = sheet[i - 1];
            const long pfirst = p.index0 >= 0 ? p.index0 : p.index1;
            if (first < pfirst) return bad("track " + std::to_string(s.t.number) + " begins before the one before it");
            s.t.offset = p.t.offset + static_cast<u64>(first - pfirst) * p.t.size;
        } else {
            s.t.offset = static_cast<u64>(first) * s.t.size;
        }
    }
    for (std::size_t i = 0; i < sheet.size(); ++i) {
        Sheet& s = sheet[i];
        u64 bytes;
        if (i + 1 < sheet.size() && sheet[i + 1].t.file == s.t.file) bytes = sheet[i + 1].t.offset - s.t.offset;
        else {
            const u64 size = files_[static_cast<std::size_t>(s.t.file)]->size();
            bytes = size > s.t.offset ? size - s.t.offset : 0;
        }
        s.t.count = static_cast<u32>(bytes / s.t.size);
    }

    // Their addresses on the disc.
    s64 cursor = 0;
    for (std::size_t i = 0; i < sheet.size(); ++i) {
        Sheet& s = sheet[i];
        Track& t = s.t;
        const long first = s.index0 >= 0 ? s.index0 : s.index1;
        const s64 in_file_pregap = s.index1 - first;
        if (i == 0) {
            t.first = -in_file_pregap;  // index 1 is LBA 0
            t.begin = std::min<s64>(t.first, 0);
        } else {
            const Sheet& p = sheet[i - 1];
            if (t.session != p.t.session) {
                cursor += (p.t.session == sheet[0].t.session ? kFirstLeadOut : kLaterLeadOut) + kLeadIn;
                // A session's first track has its pregap, if the sheet gives none.
                if (s.pregap == 0 && in_file_pregap == 0) s.pregap = kPregap;
            }
            t.begin = cursor;
            cursor += s.pregap;
            t.first = cursor;
        }
        t.start = static_cast<u32>(t.first + in_file_pregap);
        cursor = t.first + t.count + s.postgap;
        t.end = static_cast<u32>(std::max<s64>(cursor, t.start));
        if (t.end > 0x7FFFFFFF) return bad("the disc is too long");
        tracks_.push_back(t);
    }
    for (const Track& t : tracks_) {
        if (sessions_.empty() || sessions_.back().number != t.session)
            sessions_.push_back({t.session, t.number, t.number, t.end});
        sessions_.back().last_track = t.number;
        sessions_.back().leadout = t.end;
    }
    return true;
}

const CdImage::Track* CdImage::track_at(u32 lba) const {
    for (const Track& t : tracks_)
        if (static_cast<s64>(lba) >= t.begin && lba < t.end) return &t;
    return nullptr;
}

bool CdImage::xa() const {
    return std::any_of(tracks_.begin(), tracks_.end(), [](const Track& t) { return t.type == TrackType::Mode2; });
}

u64 CdImage::accesses() const {
    u64 n = 0;
    for (const auto& f : files_) n += f->accesses();
    return n;
}

bool CdImage::read_raw(u32 lba, u8* out) {
    const Track* t = track_at(lba);
    if (!t) return false;
    const s64 i = static_cast<s64>(lba) - t->first;
    const bool held = i >= 0 && i < t->count;
    if (t->type == TrackType::Audio) {
        if (!held) {
            std::memset(out, 0, kRawSector);
            return true;
        }
        DiskImage& f = *files_[static_cast<std::size_t>(t->file)];
        if (!f.read(t->offset + static_cast<u64>(i) * t->size, out, kRawSector)) return false;
        if (t->swap)
            for (unsigned b = 0; b < kRawSector; b += 2) std::swap(out[b], out[b + 1]);
        return true;
    }
    const u8 mode = t->type == TrackType::Mode2 ? 2 : 1;
    if (!held) {
        // A gap not in the file: zero data (mode 2: form 1 subheaders of 0).
        std::memset(out, 0, kRawSector);
        cd_sync_header(out, lba, mode);
        if (mode == 1) cd_mode1_ecc(out);
        return true;
    }
    DiskImage& f = *files_[static_cast<std::size_t>(t->file)];
    const u64 at = t->offset + static_cast<u64>(i) * t->size;
    switch (t->size) {
        case kRawSector: return f.read(at, out, kRawSector);
        case 2336:
            cd_sync_header(out, lba, 2);
            return f.read(at, out + 16, 2336);
        default:
            std::memset(out, 0, kRawSector);
            cd_sync_header(out, lba, 1);
            if (!f.read(at, out + 16, kDataSector)) return false;
            cd_mode1_ecc(out);
            return true;
    }
}

bool CdImage::read_data(u32 lba, u8* out, u32 count) {
    while (count) {
        const Track* t = track_at(lba);
        if (!t || t->type == TrackType::Audio) return false;
        const s64 i = static_cast<s64>(lba) - t->first;
        if (i < 0 || i >= t->count) {
            std::memset(out, 0, kDataSector);  // a gap not in the file
            ++lba, --count, out += kDataSector;
            continue;
        }
        DiskImage& f = *files_[static_cast<std::size_t>(t->file)];
        const u64 at = t->offset + static_cast<u64>(i) * t->size;
        if (t->size == kDataSector) {
            // As many as follow in the file at once.
            const u32 n = static_cast<u32>(std::min<s64>(count, t->count - i));
            if (!f.read(at, out, std::size_t{n} * kDataSector)) return false;
            lba += n, count -= n, out += std::size_t{n} * kDataSector;
            continue;
        }
        // Mode 1 data follows the header; mode 2 form 1's the subheader too.
        const unsigned skip = t->size == 2336 ? 8 : (t->type == TrackType::Mode2 ? 24 : 16);
        if (!f.read(at + skip, out, kDataSector)) return false;
        ++lba, --count, out += kDataSector;
    }
    return true;
}

}  // namespace uv
