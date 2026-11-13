// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
// Crystal CS4231A audio codec, as on the Ultra 5/10's EBus (SUNW,CS4231):
// four byte registers a word apart (index, indexed data, status, PIO
// data) in front of 32 indexed registers (16 outside mode 2). Playback and
// capture move through two EBus DMA channels; the codec's one interrupt
// line reaches the system through both channels (INT_PEND).
//
// Playback takes sample frames from the playback channel at the rate the
// crystal and divider select, in any of the codec's formats (8-bit
// unsigned, mu-law, A-law, 16-bit little- or big-endian, IMA ADPCM), mono
// or stereo. The DAC's output (after its attenuation and mute, the ADC's
// digital loopback added before them) is mixed with the analog inputs
// LINE, AUX1 and AUX2 (each with its gain and mute) and handed to the host
// as 16-bit stereo. Capture takes the input the ADC selects (line, AUX1,
// microphone, or the mixer's output), with its gain (and the microphone's
// +20 dB), and writes it in the capture format through the capture channel
// or the PIO data register. The analog inputs are what set_input() gives
// them (on an Ultra 5/10: the CD-ROM drive on AUX1, the line-in and
// microphone jacks). The output mixer, analog on the chip, runs at 44.1 kHz
// (or the sample rate, if higher) while an analog input is heard, else at
// the sample rate. The base counters, the timer, underrun and overrun, and
// autocalibration follow the datasheet.
#pragma once

#include <array>
#include <deque>
#include <functional>
#include <vector>

#include "core/scheduler.h"
#include "mem/bus.h"

namespace uv {

class EbusDma;

class Cs4231 final : public Device {
public:
    static constexpr u32 kSize = 0x100;
    // Direct registers (a word apart on the EBus)
    static constexpr unsigned kIndex = 0, kData = 1, kStatus = 2, kPio = 3;
    // Index register bits
    static constexpr u8 kInit = 0x80, kMce = 0x40, kTrd = 0x20;
    // Status register bits
    static constexpr u8 kStInt = 0x01, kStPrdy = 0x02, kStSer = 0x10, kStCrdy = 0x20;
    // PL/R, PU/L, CL/R, CU/L: the next PIO byte is a left sample's upper
    // (or only) byte, playback and capture, as a real Ultra 10 reads at ok.
    static constexpr u8 kStSamplePosition = 0xCC;
    // Indexed registers
    static constexpr unsigned kLeftInput = 0, kRightInput = 1, kLeftAux1 = 2, kRightAux1 = 3, kLeftAux2 = 4,
                              kRightAux2 = 5, kLoopback = 13, kLeftLine = 18, kRightLine = 19;
    static constexpr unsigned kLeftOut = 6, kRightOut = 7, kPlayFormat = 8, kInterface = 9, kPinControl = 10,
                              kErrorStatus = 11, kModeId = 12, kPlayCountHigh = 14, kPlayCountLow = 15,
                              kFeature1 = 16, kFeature2 = 17, kTimerLow = 20, kTimerHigh = 21, kAltStatus = 24,
                              kVersion = 25, kMonoControl = 26, kCaptureFormat = 28, kCaptureCountHigh = 30,
                              kCaptureCountLow = 31;
    // Interface configuration (I9)
    static constexpr u8 kPen = 0x01, kCen = 0x02, kAcal = 0x08, kPpio = 0x40, kCpio = 0x80;
    // Pin control (I10)
    static constexpr u8 kIen = 0x02, kXctl0 = 0x40, kXctl1 = 0x80;
    // Error status (I11)
    static constexpr u8 kDrs = 0x10, kAci = 0x20, kPur = 0x40, kCor = 0x80;
    // Alternate feature status (I24)
    static constexpr u8 kPi = 0x10, kCi = 0x20, kTi = 0x40;
    // Feature enable I (I16): DAC zero on underrun, timer enable
    static constexpr u8 kDacz = 0x01, kTe = 0x40;
    // Mode and ID (I12): mode 2, and the CS4231's ID; version (I25): CS4231A
    static constexpr u8 kMode2 = 0x40, kModeIdFixed = 0x8A, kVersionCs4231a = 0xA0;
    static constexpr u8 kMonoMute = 0x40;  // I26: the mono output (the speaker) muted
    // Format register fields: format (bits 7:5), stereo, crystal 2
    enum Format : u8 { kLinear8 = 0, kMuLaw = 1, kLinear16Le = 2, kALaw = 3, kAdpcm = 5, kLinear16Be = 6 };
    static constexpr u8 kStereo = 0x10, kXtal2 = 0x01;
    // Input control (I0/I1): the ADC's source (bits 7:6), the microphone's
    // +20 dB, the gain (1.5 dB steps); analog input control (I2-I5,
    // I18/I19): mute, gain (+12 dB down to -34.5 dB in 1.5 dB steps);
    // loopback (I13): enable, attenuation (bits 7:2, 1.5 dB steps).
    static constexpr u8 kMicGain = 0x20, kInputMute = 0x80, kLoopbackEnable = 0x01;
    static constexpr double kAnalogRate = 44100;  // the mixer's rate while an analog input is heard

    // The analog inputs, as the ADC's source select numbers the first three.
    enum class Input : unsigned { Line = 0, Aux1 = 1, Mic = 2, Aux2 = 3 };
    // What an input hears: `count` stereo frames at `rate`, up to the
    // present guest time.
    using InputSource = std::function<void(s16* frames, std::size_t count, double rate)>;

    Cs4231(Scheduler& scheduler, double cpu_hz);

    const char* name() const override { return "cs4231"; }
    bool read(u64 offset, unsigned size, u64& value) override;
    bool write(u64 offset, unsigned size, u64 value) override;
    void reset();

    void set_dma(EbusDma* playback, EbusDma* capture) {
        play_dma_ = playback;
        capture_dma_ = capture;
    }
    // The interrupt line (to both DMA channels' device interrupt).
    void set_irq_sink(std::function<void(bool)> sink) { irq_ = std::move(sink); }
    // Where played samples go: 16-bit stereo frames (left, right) at a rate.
    using AudioSink = std::function<void(const s16* frames, std::size_t count, unsigned rate)>;
    void set_audio_sink(AudioSink sink) { sink_ = std::move(sink); }

    // An analog input's source, and whether it has sound now (a live input
    // the mixer passes keeps the codec's mixer running with nothing digital).
    void set_input(Input in, InputSource source) { inputs_[static_cast<unsigned>(in)] = std::move(source); }
    void set_input_live(Input in, bool live);

    u8 reg(unsigned index) const { return regs_[index & 31]; }
    // The sample rate (Hz) a format register selects.
    static double sample_rate(u8 format);

private:
    u8 read_indexed(unsigned i);
    void write_indexed(unsigned i, u8 v);
    unsigned index_mask() const { return (regs_[kModeId] & kMode2) ? 31 : 15; }
    u8 capture_format() const { return (regs_[kModeId] & kMode2) ? regs_[kCaptureFormat] : regs_[kPlayFormat]; }
    u16 play_base() const { return static_cast<u16>(regs_[kPlayCountHigh] << 8 | regs_[kPlayCountLow]); }
    u16 capture_base() const {
        return (regs_[kModeId] & kMode2) ? static_cast<u16>(regs_[kCaptureCountHigh] << 8 | regs_[kCaptureCountLow])
                                         : play_base();
    }
    bool playing() const;
    bool capturing() const;
    bool transfers_stopped() const { return (index_ & kTrd) && (status_ & kStInt); }
    void update_irq();
    void raise(u8 flag);
    void update_engine();  // starts or stops the frame clock as the registers say
    void tick();
    bool analog_heard() const;  // a live input the output mixer passes
    bool loopback_heard() const;  // the loopback on with an input it hears
    void mix(unsigned frames, double mixer, unsigned samples);
    void play(unsigned frames);
    void capture(unsigned frames);
    void count(u16& counter, u16 base, unsigned frames, u8 flag);
    void timer_restart();
    s16 adpcm(int channel, unsigned nibble);
    unsigned adpcm_encode(int channel, int sample);

    Scheduler& sched_;
    double cpu_hz_;
    Timer frame_timer_;  // moves sample frames, a block at a time
    Timer timer_;        // the codec's timer (I20/I21)
    Timer calibrate_;    // autocalibration (ACI) ends
    std::array<u8, 32> regs_{};
    u8 index_ = 0, status_ = 0;
    u8 errors_ = 0;  // I11's latched bits: PUR, COR
    u16 play_count_ = 0, capture_count_ = 0;
    bool engine_on_ = false;
    u64 last_ = 0;           // the guest time frames were last moved to
    double frame_debt_ = 0;  // part of a mixer frame due but not yet moved
    double digital_debt_ = 0;  // part of a sample period due
    std::array<s16, 2> last_frame_{};
    // IMA ADPCM state per channel, and a byte's second sample waiting
    struct Adpcm {
        int predictor = 0, index = 0;
    } adpcm_[2], adpcm_capture_[2];
    int adpcm_pending_ = -1;
    int adpcm_capture_pending_ = -1;  // a captured nibble waiting for its byte's other half
    std::vector<u8> pio_;          // playback bytes written in PIO mode
    std::deque<u8> capture_pio_;   // captured bytes for the PIO data register
    std::vector<u8> bytes_;        // scratch
    std::vector<s16> out_;         // the DAC's samples (stereo), then the output
    std::vector<double> adc_;      // what the ADC samples (stereo)
    std::vector<double> analog_, mixed_;  // the analog inputs' sum, and the mixer's output
    std::array<double, 2> adc_last_{};    // the ADC's last sample
    std::array<std::vector<s16>, 4> in_;  // each input's frames this block
    std::array<InputSource, 4> inputs_;
    std::array<bool, 4> live_{};
    EbusDma* play_dma_ = nullptr;
    EbusDma* capture_dma_ = nullptr;
    std::function<void(bool)> irq_;
    AudioSink sink_;
    bool irq_out_ = false;
};

}  // namespace uv
