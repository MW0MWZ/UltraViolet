// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
#include <algorithm>
#include <vector>

#include "core/endian.h"
#include "core/scheduler.h"
#include "devices/cs4231.h"
#include "devices/ebus_dma.h"
#include "test.h"

using namespace uv;

namespace {

constexpr double kHz = 48e6;  // the clock guest time is counted in

// A codec on two DMA channels over a small memory, with a clock the test moves.
struct Rig {
    Scheduler sched;
    u64 now = 0;
    std::vector<u8> mem = std::vector<u8>(4096);
    EbusDma play{"play"}, rec{"rec"};
    Cs4231 codec{sched, kHz};
    std::vector<s16> heard;
    unsigned rate = 0;
    bool play_irq = false, codec_irq = false;

    Rig() {
        sched.set_clock([this] { return now; });
        for (EbusDma* d : {&play, &rec})
            d->set_dma_port([this](u32 a, u8* p, std::size_t n) { std::copy_n(mem.begin() + a, n, p); return true; },
                            [this](u32 a, const u8* p, std::size_t n) { std::copy_n(p, n, mem.begin() + a); return true; });
        play.set_irq_sink([this](bool on) { play_irq = on; });
        codec.set_dma(&play, &rec);
        codec.set_irq_sink([this](bool on) {
            codec_irq = on;
            play.set_device_irq(on);
            rec.set_device_irq(on);
        });
        codec.set_audio_sink([this](const s16* f, std::size_t n, unsigned r) {
            heard.insert(heard.end(), f, f + n * 2);
            rate = r;
        });
    }
    void out(unsigned reg, u8 v) { codec.write(reg * 4, 1, v); }
    u8 in(unsigned reg) {
        u64 v = 0;
        codec.read(reg * 4, 1, v);
        return static_cast<u8>(v);
    }
    void set(unsigned index, u8 v, bool mce = false) {
        out(Cs4231::kIndex, static_cast<u8>(index | (mce ? Cs4231::kMce : 0)));
        out(Cs4231::kData, v);
    }
    u8 get(unsigned index) {
        out(Cs4231::kIndex, static_cast<u8>(index));
        return in(Cs4231::kData);
    }
    void dma(EbusDma& d, unsigned reg, u32 v) { d.write(reg, 4, byteswap(v)); }
    u32 dma(EbusDma& d, unsigned reg) {
        u64 v = 0;
        d.read(reg, 4, v);
        return byteswap(static_cast<u32>(v));
    }
    // Guest time passes as the machine passes it: up to each event in turn.
    void run_for(double seconds) {
        const u64 end = now + static_cast<u64>(seconds * kHz);
        while (sched.next_event() <= end) {
            now = std::max(now, sched.next_event());
            sched.run_due(now);
        }
        now = end;
    }
    // Mode 2, a format, autocalibration off, outputs unmuted at full level.
    void setup(u8 format) {
        set(Cs4231::kModeId, Cs4231::kMode2, true);
        set(Cs4231::kInterface, 0, true);
        set(Cs4231::kPlayFormat, format, true);
        set(Cs4231::kCaptureFormat, format, true);
        out(Cs4231::kIndex, 0);  // leave mode change
        set(Cs4231::kLeftOut, 0);
        set(Cs4231::kRightOut, 0);
    }
};

}  // namespace

TEST(cs4231_identifies_itself_in_both_modes) {
    Rig r;
    CHECK_EQ(r.in(Cs4231::kIndex), Cs4231::kMce);  // MCE after a reset
    CHECK_EQ(r.get(Cs4231::kModeId), 0x8Au);
    CHECK_EQ(r.get(Cs4231::kInterface), Cs4231::kAcal);
    // In mode 1 only the first 16 registers answer: I25 is I9.
    CHECK_EQ(r.get(Cs4231::kVersion), r.get(Cs4231::kInterface));
    r.set(Cs4231::kModeId, 0xFF);
    CHECK_EQ(r.get(Cs4231::kModeId), 0xCAu);
    CHECK_EQ(r.get(Cs4231::kVersion), Cs4231::kVersionCs4231a);
    r.set(Cs4231::kVersion, 0);  // read-only
    CHECK_EQ(r.get(Cs4231::kVersion), Cs4231::kVersionCs4231a);
}

TEST(cs4231_formats_change_only_in_mode_change) {
    Rig r;
    r.set(Cs4231::kModeId, Cs4231::kMode2);
    r.set(Cs4231::kPlayFormat, 0x5C, true);
    CHECK_EQ(r.get(Cs4231::kPlayFormat), 0x5Cu);
    r.out(Cs4231::kIndex, 0);
    r.set(Cs4231::kPlayFormat, 0x00);
    CHECK_EQ(r.get(Cs4231::kPlayFormat), 0x5Cu);
    // Outside mode change only the enables of I9 move.
    r.set(Cs4231::kInterface, 0xFF);
    CHECK_EQ(r.get(Cs4231::kInterface), Cs4231::kAcal | Cs4231::kPen | Cs4231::kCen);
    CHECK(Cs4231::sample_rate(0x0C) == 48000.0);   // crystal 1, divide by 512
    CHECK(Cs4231::sample_rate(0x0B) == 44100.0);   // crystal 2, divide by 384
    CHECK(Cs4231::sample_rate(0x00) == 8000.0);
}

TEST(cs4231_plays_dma_buffers_in_time_and_chained) {
    Rig r;
    // 16-bit big-endian stereo at 8 kHz.
    r.setup(static_cast<u8>(Cs4231::kLinear16Be << 5 | Cs4231::kStereo));
    // Two buffers of 8 frames: frame n is (n, -n) in the first, (100 + n, 0) in the second.
    for (unsigned n = 0; n < 8; ++n) {
        store_be<u16>(&r.mem[0x100 + n * 4], static_cast<u16>(n));
        store_be<u16>(&r.mem[0x102 + n * 4], static_cast<u16>(-static_cast<int>(n)));
        store_be<u16>(&r.mem[0x200 + n * 4], static_cast<u16>(100 + n));
        store_be<u16>(&r.mem[0x202 + n * 4], 0);
    }
    r.dma(r.play, 8, 32);
    r.dma(r.play, 4, 0x100);
    r.dma(r.play, 0, EbusDma::kIntEn | EbusDma::kEnDma | EbusDma::kEnCnt | EbusDma::kEnNext);
    r.dma(r.play, 8, 32);
    r.dma(r.play, 4, 0x200);
    r.set(Cs4231::kInterface, Cs4231::kPen);
    r.run_for(0.9e-3);  // frames move a millisecond at a time
    CHECK(r.heard.empty());
    r.run_for(0.2e-3);
    CHECK_EQ(r.heard.size(), 16u);
    CHECK_EQ(r.rate, 8000u);
    CHECK(r.heard[2 * 3] == 3 && r.heard[2 * 3 + 1] == -3);
    CHECK(r.dma(r.play, 0) & EbusDma::kTc);  // the first buffer's end interrupts
    CHECK(r.play_irq);
    r.dma(r.play, 0, r.dma(r.play, 0));
    CHECK(!r.play_irq);
    r.run_for(1e-3);
    CHECK_EQ(r.heard.size(), 32u);
    CHECK(r.heard[2 * 8] == 100 && r.heard[2 * 15] == 107);  // the next buffer follows without a gap
    CHECK_EQ(r.get(Cs4231::kErrorStatus) & Cs4231::kPur, 0u);
    // The data runs out: an underrun repeats the last frame.
    r.run_for(1e-3);
    CHECK(r.heard[r.heard.size() - 2] == 107);
    CHECK(r.in(Cs4231::kStatus) & Cs4231::kStSer);
    CHECK(r.get(Cs4231::kErrorStatus) & Cs4231::kPur);
    CHECK_EQ(r.get(Cs4231::kErrorStatus) & Cs4231::kPur, 0u);  // cleared by the read
    // With DAC zero, an underrun goes to silence.
    r.set(Cs4231::kFeature1, Cs4231::kDacz);
    r.run_for(1e-3);
    CHECK(r.heard.back() == 0 && r.heard[r.heard.size() - 2] == 0);
}

TEST(cs4231_decodes_every_format) {
    struct Case {
        u8 format;
        std::vector<u8> bytes;
        std::vector<s16> left;
    };
    const Case cases[] = {
        {Cs4231::kLinear8 << 5, {0x80, 0xFF, 0x00}, {0, 0x7F00, -0x8000}},
        {Cs4231::kMuLaw << 5, {0xFF, 0x7F, 0x00, 0x80}, {0, 0, -32124, 32124}},
        {Cs4231::kALaw << 5, {0xD5, 0x55, 0xAA, 0x2A}, {8, -8, 32256, -32256}},
        {Cs4231::kLinear16Le << 5, {0x34, 0x12, 0x00, 0x80}, {0x1234, -0x8000}},
        // IMA ADPCM, the low half of a byte first: 7 then 0 from the first step.
        {Cs4231::kAdpcm << 5, {0x07}, {11, 13}},
    };
    for (const Case& c : cases) {
        Rig r;
        r.setup(c.format);
        std::copy(c.bytes.begin(), c.bytes.end(), r.mem.begin());
        r.dma(r.play, 8, static_cast<u32>(c.bytes.size()));
        r.dma(r.play, 4, 0);
        r.dma(r.play, 0, EbusDma::kEnDma | EbusDma::kEnCnt);
        r.set(Cs4231::kInterface, Cs4231::kPen);
        r.run_for(1.1e-3);
        CHECK(r.heard.size() >= c.left.size() * 2);
        for (unsigned i = 0; i < c.left.size() && i * 2 < r.heard.size(); ++i) {
            CHECK_EQ(static_cast<u16>(r.heard[i * 2]), static_cast<u16>(c.left[i]));
            CHECK_EQ(r.heard[i * 2], r.heard[i * 2 + 1]);  // mono to both sides
        }
    }
}

TEST(cs4231_attenuates_and_mutes_the_output) {
    Rig r;
    r.setup(Cs4231::kLinear16Le << 5);
    for (unsigned n = 0; n < 16; ++n) store_be<u16>(&r.mem[n * 2], 0x0040);  // 0x4000 little-endian
    r.dma(r.play, 8, 32);
    r.dma(r.play, 4, 0);
    r.dma(r.play, 0, EbusDma::kEnDma | EbusDma::kEnCnt);
    r.set(Cs4231::kLeftOut, 4);    // 6 dB down
    r.set(Cs4231::kRightOut, 0x80);  // muted
    r.set(Cs4231::kInterface, Cs4231::kPen);
    r.run_for(1.1e-3);
    CHECK(r.heard.size() >= 2);
    CHECK(r.heard[0] > 0x1FC0 && r.heard[0] < 0x2040);
    CHECK(r.heard[1] == 0);
}

TEST(cs4231_counters_timer_and_calibration_interrupt) {
    Rig r;
    r.setup(Cs4231::kLinear8 << 5);
    r.set(Cs4231::kPinControl, Cs4231::kIen);
    // Playback counter: base 9 interrupts every 10 sample periods.
    r.set(Cs4231::kPlayCountHigh, 0);
    r.set(Cs4231::kPlayCountLow, 9);
    r.set(Cs4231::kInterface, Cs4231::kPen);
    r.run_for(9.0 / 8000);
    CHECK(!r.codec_irq);
    r.run_for(1.5 / 8000);
    CHECK(r.codec_irq);
    CHECK(r.in(Cs4231::kStatus) & Cs4231::kStInt);
    CHECK(r.get(Cs4231::kAltStatus) & Cs4231::kPi);
    CHECK(r.dma(r.play, 0) & EbusDma::kIntPend);  // seen through the DMA channel
    r.set(Cs4231::kAltStatus, static_cast<u8>(~Cs4231::kPi));  // writing 0 clears it
    CHECK(!r.codec_irq);
    r.set(Cs4231::kInterface, 0);

    // Capture with no DMA: an overrun.
    r.set(Cs4231::kInterface, Cs4231::kCen);
    r.run_for(1.1e-3);
    CHECK(r.get(Cs4231::kErrorStatus) & Cs4231::kCor);
    // Capture through DMA records silence.
    std::fill(r.mem.begin(), r.mem.begin() + 8, 0x12);
    r.dma(r.rec, 8, 4);
    r.dma(r.rec, 4, 0);
    r.dma(r.rec, 0, EbusDma::kWrite | EbusDma::kEnDma | EbusDma::kEnCnt);
    r.run_for(1e-3);
    CHECK(r.mem[0] == 0x80 && r.mem[3] == 0x80 && r.mem[4] == 0x12);
    r.set(Cs4231::kInterface, 0);
    r.out(Cs4231::kStatus, 0);  // any write clears every interrupt
    CHECK(!r.codec_irq);

    // The timer: 100 ticks of about 10 us.
    r.set(Cs4231::kTimerHigh, 0);
    r.set(Cs4231::kTimerLow, 100);
    r.set(Cs4231::kFeature1, Cs4231::kTe);
    r.run_for(990e-6);
    CHECK(!r.codec_irq);
    r.run_for(10e-6);
    CHECK(r.codec_irq);
    CHECK(r.get(Cs4231::kAltStatus) & Cs4231::kTi);
    r.set(Cs4231::kFeature1, 0);
    r.out(Cs4231::kStatus, 0);

    // Autocalibration on leaving mode change: 384 sample periods of ACI.
    r.set(Cs4231::kInterface, Cs4231::kAcal, true);
    r.out(Cs4231::kIndex, 0);
    CHECK(r.get(Cs4231::kErrorStatus) & Cs4231::kAci);
    r.run_for(380.0 / 8000);
    CHECK(r.get(Cs4231::kErrorStatus) & Cs4231::kAci);
    r.run_for(5.0 / 8000);
    CHECK_EQ(r.get(Cs4231::kErrorStatus) & Cs4231::kAci, 0u);
}
