// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
#include "devices/m48t59.h"

#include <algorithm>
#include <cstdio>

namespace uv {

namespace {

constexpr unsigned kFlags = 0x1FF0, kAlarmSec = 0x1FF2, kInterrupts = 0x1FF6, kWatchdog = 0x1FF7,
                   kControl = 0x1FF8, kSeconds = 0x1FF9, kMinutes = 0x1FFA, kHours = 0x1FFB, kDay = 0x1FFC,
                   kDate = 0x1FFD, kMonth = 0x1FFE, kYear = 0x1FFF;
constexpr u8 kCtlWrite = 0x80, kCtlRead = 0x40, kSecStop = 0x80;
constexpr unsigned kIdprom = 0x1FD8;

u8 to_bcd(int v) { return static_cast<u8>(((v / 10) << 4) | (v % 10)); }
int from_bcd(u8 v) { return (v >> 4) * 10 + (v & 15); }

// UTC broken-down time <-> seconds, without the host's time zone.
std::time_t utc_to_time(std::tm tm) {
    const int y = tm.tm_year + 1900, m = tm.tm_mon + 1;
    const int a = (14 - m) / 12, yy = y + 4800 - a, mm = m + 12 * a - 3;
    const long long jdn = tm.tm_mday + (153 * mm + 2) / 5 + 365LL * yy + yy / 4 - yy / 100 + yy / 400 - 32045;
    return static_cast<std::time_t>((jdn - 2440588) * 86400 + tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec);
}

std::tm time_to_utc(std::time_t t) {
    std::tm tm{};
    long long days = t / 86400, secs = t % 86400;
    if (secs < 0) secs += 86400, --days;
    tm.tm_hour = static_cast<int>(secs / 3600);
    tm.tm_min = static_cast<int>(secs / 60 % 60);
    tm.tm_sec = static_cast<int>(secs % 60);
    tm.tm_wday = static_cast<int>((days + 4) % 7 + 7) % 7;  // 1970-01-01 was a Thursday
    long long z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
    long long doe = z - era * 146097, yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long y = yoe + era * 400, doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    tm.tm_mday = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    tm.tm_mon = static_cast<int>(mp < 10 ? mp + 2 : mp - 10);
    tm.tm_year = static_cast<int>(y + (tm.tm_mon <= 1 ? 1 : 0)) - 1900;
    return tm;
}

}  // namespace

M48t59::M48t59(std::function<std::time_t()> host_clock) : host_clock_(std::move(host_clock)) {}

std::time_t M48t59::now() const {
    if (stopped_) return stopped_at_;
    return (host_clock_ ? host_clock_() : std::time(nullptr)) + offset_;
}

void M48t59::latch_clock() {
    const std::time_t t = now();
    if (t != converted_) {
        const std::tm tm = time_to_utc(t);
        converted_regs_ = {to_bcd(tm.tm_sec), to_bcd(tm.tm_min), to_bcd(tm.tm_hour),
                           static_cast<u8>(tm.tm_wday == 0 ? 7 : tm.tm_wday), to_bcd(tm.tm_mday), to_bcd(tm.tm_mon + 1),
                           to_bcd(((tm.tm_year + 1900 - kYearBase) % 100 + 100) % 100)};
        converted_ = t;
    }
    const auto& r = converted_regs_;
    mem_[kSeconds] = static_cast<u8>(r[0] | (stopped_ ? kSecStop : 0));
    mem_[kMinutes] = r[1];
    mem_[kHours] = r[2];
    mem_[kDay] = static_cast<u8>((mem_[kDay] & 0x40) | r[3]);
    mem_[kDate] = r[4];
    mem_[kMonth] = r[5];
    mem_[kYear] = r[6];
}

u8 M48t59::read_byte(unsigned addr) {
    if (addr >= kSeconds && !(mem_[kControl] & (kCtlRead | kCtlWrite))) latch_clock();
    const u8 v = mem_[addr];
    if (addr == kFlags) mem_[kFlags] &= 0x3F;  // the watchdog and alarm flags clear on read
    return v;
}

void M48t59::write_byte(unsigned addr, u8 v) {
    if (addr < kFlags) {
        if (mem_[addr] != v) dirty_ = true;
        mem_[addr] = v;
        return;
    }
    switch (addr) {
        case kControl: {
            const bool was_writing = mem_[kControl] & kCtlWrite;
            if (!was_writing && (v & kCtlWrite)) latch_clock();
            if (!(mem_[kControl] & kCtlRead) && (v & kCtlRead)) latch_clock();
            mem_[kControl] = v;
            if (was_writing && !(v & kCtlWrite)) {
                // Leaving write mode loads the counters from the registers.
                std::tm tm{};
                tm.tm_sec = from_bcd(mem_[kSeconds] & 0x7F);
                tm.tm_min = from_bcd(mem_[kMinutes] & 0x7F);
                tm.tm_hour = from_bcd(mem_[kHours] & 0x3F);
                tm.tm_mday = from_bcd(mem_[kDate] & 0x3F);
                tm.tm_mon = from_bcd(mem_[kMonth] & 0x1F) - 1;
                int year = kYearBase + from_bcd(mem_[kYear]);
                while (year < 1970) year += 100;
                tm.tm_year = year - 1900;
                const std::time_t host = host_clock_ ? host_clock_() : std::time(nullptr);
                offset_ = static_cast<long long>(utc_to_time(tm)) - host;
                const bool stop = mem_[kSeconds] & kSecStop;
                if (stop && !stopped_) stopped_at_ = utc_to_time(tm);
                stopped_ = stop;
                dirty_ = true;
            }
            break;
        }
        case kSeconds:
            if (mem_[kControl] & kCtlWrite) {
                mem_[addr] = v;
            } else if ((v & kSecStop) != (stopped_ ? kSecStop : 0)) {
                // The stop bit can be changed on its own.
                if (v & kSecStop) {
                    stopped_at_ = now();
                    stopped_ = true;
                } else {
                    const std::time_t host = host_clock_ ? host_clock_() : std::time(nullptr);
                    offset_ = static_cast<long long>(stopped_at_) - host;
                    stopped_ = false;
                }
            }
            break;
        case kDay:
            // FT (frequency test) is writable at any time; the day only in write mode.
            mem_[addr] = (mem_[kControl] & kCtlWrite) ? v : static_cast<u8>((mem_[addr] & 0x07) | (v & 0x40));
            break;
        case kMinutes:
        case kHours:
        case kDate:
        case kMonth:
        case kYear:
            if (mem_[kControl] & kCtlWrite) mem_[addr] = v;
            break;
        case kFlags: break;  // read-only
        default:             // alarm, interrupt enables, watchdog
            mem_[addr] = v;
            (void)kAlarmSec;
            (void)kInterrupts;
            (void)kWatchdog;
            break;
    }
}

// The NVRAM and the control registers read without side effects; the
// flags too while none of the bits a read clears is set; the clock
// registers, which a read latches from the running clock, until the next
// second (unless the clock is stopped or held for reading or writing).
u64 M48t59::stable_until(u64 offset, unsigned size) const {
    u64 t = kForever;
    for (unsigned i = 0; i < size; ++i) {
        const unsigned addr = static_cast<unsigned>((offset + i) & (kSize - 1));
        if (addr == kFlags && (mem_[kFlags] & 0xC0)) return 0;
        if (addr >= kSeconds && !(mem_[kControl] & (kCtlRead | kCtlWrite)) && !stopped_)
            t = std::min(t, next_second_ ? next_second_() : 0);
    }
    return t;
}

bool M48t59::read(u64 offset, unsigned size, u64& value) {
    value = 0;
    for (unsigned i = 0; i < size; ++i) value = (value << 8) | read_byte(static_cast<unsigned>((offset + i) & (kSize - 1)));
    return true;
}

bool M48t59::write(u64 offset, unsigned size, u64 value) {
    for (unsigned i = 0; i < size; ++i)
        write_byte(static_cast<unsigned>((offset + i) & (kSize - 1)), static_cast<u8>(value >> (8 * (size - 1 - i))));
    return true;
}

void M48t59::write_idprom(u8 machine_type, const u8 mac[6], u32 serial) {
    u8* p = mem_.data() + kIdprom;
    p[0] = 0x01;  // format
    p[1] = machine_type;
    for (int i = 0; i < 6; ++i) p[2 + i] = mac[i];
    p[8] = p[9] = p[10] = p[11] = 0;  // manufacturing date
    p[12] = static_cast<u8>(serial >> 16);
    p[13] = static_cast<u8>(serial >> 8);
    p[14] = static_cast<u8>(serial);
    u8 sum = 0;
    for (int i = 0; i < 15; ++i) sum ^= p[i];
    p[15] = sum;
    dirty_ = true;
}

bool M48t59::load(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    const std::size_t n = std::fread(mem_.data(), 1, kSize, f);
    std::fclose(f);
    dirty_ = false;
    return n == kSize;
}

bool M48t59::save(const std::string& path) const {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(mem_.data(), 1, kSize, f) == kSize;
    return std::fclose(f) == 0 && ok;
}

}  // namespace uv
