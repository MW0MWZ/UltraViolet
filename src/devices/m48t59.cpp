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
constexpr u8 kFlagWdf = 0x80, kFlagAf = 0x40;   // flags: watchdog and alarm (BL, bit 4, never set: a good battery)
constexpr u8 kAfe = 0x80, kAbe = 0x20;         // interrupts: alarm flag enable, alarm in battery back-up
constexpr u8 kWds = 0x80, kFt = 0x40;          // watchdog steering; frequency test (in the day register)
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

M48t59::~M48t59() = default;

void M48t59::attach(Scheduler& scheduler, double cpu_hz) {
    cpu_hz_ = cpu_hz;
    alarm_timer_ = std::make_unique<Timer>(scheduler, [this] { alarm(); });
    watchdog_timer_ = std::make_unique<Timer>(scheduler, [this] { watchdog(); });
    schedule_alarm();
}

void M48t59::set_irq(bool on) {
    if (on == irq_) return;
    irq_ = on;
    if (on_irq) on_irq(on);
}

// The next second the alarm registers match, as their repeat bits (RPT4-1,
// bit 7 of the date, hours, minutes and seconds registers) select: every
// second (1111, and any combination not listed), once a minute (1110: the
// seconds match), an hour (1100), a day (1000) or a month (0000: the date
// too).
void M48t59::schedule_alarm() {
    if (!alarm_timer_) return;
    alarm_timer_->cancel();
    if (stopped_ || !next_second_) return;
    const unsigned rpt = (mem_[0x1FF5] >> 7) << 3 | (mem_[0x1FF4] >> 7) << 2 | (mem_[0x1FF3] >> 7) << 1 | (mem_[0x1FF2] >> 7);
    const int sec = from_bcd(mem_[0x1FF2] & 0x7F), min = from_bcd(mem_[0x1FF3] & 0x7F),
              hour = from_bcd(mem_[0x1FF4] & 0x3F), date = from_bcd(mem_[0x1FF5] & 0x3F);
    const std::time_t t = now();
    std::time_t at = 0;
    auto next_mod = [&](long long period, long long offset) {
        long long n = t + 1, r = ((n - offset) % period + period) % period;
        return static_cast<std::time_t>(r ? n + period - r : n);
    };
    switch (rpt) {
        case 0xE: at = sec < 60 ? next_mod(60, sec) : 0; break;
        case 0xC: at = sec < 60 && min < 60 ? next_mod(3600, min * 60 + sec) : 0; break;
        case 0x8: at = sec < 60 && min < 60 && hour < 24 ? next_mod(86400, hour * 3600 + min * 60 + sec) : 0; break;
        case 0x0:
            if (sec < 60 && min < 60 && hour < 24 && date >= 1 && date <= 31) {
                const std::time_t midnight = t - ((t % 86400) + 86400) % 86400;
                for (int d = 0; d < 62 && !at; ++d) {
                    const std::time_t c = midnight + d * 86400LL + hour * 3600 + min * 60 + sec;
                    if (c > t && time_to_utc(c).tm_mday == date) at = c;
                }
            }
            break;
        default: at = t + 1; break;
    }
    if (!at) return;  // no time matches
    alarm_timer_->schedule_at(next_second_() + static_cast<u64>((at - t - 1) * cpu_hz_));
}

void M48t59::alarm() {
    mem_[kFlags] |= kFlagAf;
    if (mem_[kInterrupts] & kAfe) set_irq(true);
    schedule_alarm();
}

// Writing the watchdog register starts its count again; zero stops it.
void M48t59::restart_watchdog() {
    if (!watchdog_timer_) return;
    watchdog_timer_->cancel();
    static constexpr double kResolution[4] = {1.0 / 16, 1.0 / 4, 1.0, 4.0};
    const u8 w = mem_[kWatchdog];
    const unsigned multiplier = (w >> 2) & 31;
    if (multiplier) watchdog_timer_->schedule_in(static_cast<u64>(multiplier * kResolution[w & 3] * cpu_hz_) + 1);
}

void M48t59::watchdog() {
    mem_[kFlags] |= kFlagWdf;
    if (mem_[kWatchdog] & kWds) {
        mem_[kWatchdog] = 0;
        mem_[kDay] &= static_cast<u8>(~kFt);
        mem_[kInterrupts] &= static_cast<u8>(~(kAfe | kAbe));
        if (on_watchdog_reset) on_watchdog_reset();
    } else {
        set_irq(true);
    }
}

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
    if (addr == kFlags) {
        // The watchdog and alarm flags, and the IRQ output, clear on a read
        // (which still sees them set).
        mem_[kFlags] &= static_cast<u8>(~(kFlagWdf | kFlagAf));
        set_irq(false);
    }
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
                schedule_alarm();
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
                schedule_alarm();
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
        case kWatchdog:
            mem_[addr] = v;
            restart_watchdog();
            break;
        case kInterrupts:
            mem_[addr] = v;
            if (!(v & kAfe) && !(mem_[kFlags] & kFlagWdf)) set_irq(false);
            break;
        case kAlarmSec:
        case kAlarmSec + 1:
        case kAlarmSec + 2:
        case kAlarmSec + 3:  // the alarm
            mem_[addr] = v;
            schedule_alarm();
            break;
        case kFlags + 1:  // unused
            mem_[addr] = v;
            break;
        default: break;
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
