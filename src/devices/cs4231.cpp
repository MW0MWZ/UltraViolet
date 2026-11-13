// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
#include "devices/cs4231.h"

#include <algorithm>
#include <cmath>

#include "devices/ebus_dma.h"

namespace uv {

namespace {

constexpr double kCrystal1 = 24.576e6, kCrystal2 = 16.9344e6;
constexpr double kTimerHz = kCrystal1 / 245;  // the timer's time base, about 10 us
constexpr unsigned kCalibrationFrames = 384;  // sample periods an autocalibration takes
constexpr double kBlockSeconds = 0.001;       // frames are moved a block at a time

// The registers' values after a reset (I12 and I25 are fixed): inputs and
// outputs muted, autocalibration on.
constexpr std::array<u8, 32> kResetValues = {
    0x00, 0x00, 0x88, 0x88, 0x88, 0x88, 0x80, 0x80, 0x00, 0x08, 0x00, 0x00, 0x8A, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x88, 0x88, 0x00, 0x00, 0x00, 0x00, 0x00, 0xA0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

s16 mu_law(u8 v) {
    v = static_cast<u8>(~v);
    const int exponent = (v >> 4) & 7, mantissa = v & 0x0F;
    const int magnitude = (((mantissa << 3) + 0x84) << exponent) - 0x84;
    return static_cast<s16>((v & 0x80) ? -magnitude : magnitude);
}

s16 a_law(u8 v) {
    v ^= 0x55;
    const int exponent = (v >> 4) & 7, mantissa = v & 0x0F;
    int magnitude = exponent ? ((mantissa << 4) + 0x108) << (exponent - 1) : (mantissa << 4) + 8;
    return static_cast<s16>((v & 0x80) ? magnitude : -magnitude);
}

// G.711 mu-law and A-law encoding (the inverses of the above).
u8 mu_law_encode(int pcm) {
    const int sign = pcm < 0 ? 0x80 : 0;
    int magnitude = std::min(pcm < 0 ? -pcm : pcm, 32635) + 0x84;
    int exponent = 7;
    for (int mask = 0x4000; !(magnitude & mask) && exponent > 0; --exponent, mask >>= 1) {
    }
    const int mantissa = (magnitude >> (exponent + 3)) & 0x0F;
    return static_cast<u8>(~(sign | (exponent << 4) | mantissa));
}

u8 a_law_encode(int pcm) {
    static constexpr int kSegmentEnd[8] = {0x1F, 0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF};
    int v = pcm >> 3;
    u8 mask = 0xD5;
    if (v < 0) {
        mask = 0x55;
        v = -v - 1;
    }
    int segment = 0;
    while (segment < 8 && v > kSegmentEnd[segment]) ++segment;
    if (segment >= 8) return static_cast<u8>(0x7F ^ mask);
    const int a = (segment << 4) | ((segment < 2 ? v >> 1 : v >> segment) & 0x0F);
    return static_cast<u8>(a ^ mask);
}

// A gain in 1.5 dB steps as a factor.
double db_steps(int steps) { return std::pow(10.0, 1.5 * steps / 20.0); }

// The silence a capture of each format records.
u8 silence(u8 format) {
    switch (format >> 5) {
        case Cs4231::kLinear8: return 0x80;
        case Cs4231::kMuLaw: return 0xFF;
        case Cs4231::kALaw: return 0xD5;
        case Cs4231::kAdpcm: return 0x00;
        default: return 0x00;
    }
}

// Bytes per sample (ADPCM: half a byte, counted separately).
unsigned sample_bytes(u8 format) {
    const unsigned f = format >> 5;
    return (f == Cs4231::kLinear16Le || f == Cs4231::kLinear16Be) ? 2 : 1;
}

constexpr int kAdpcmIndex[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
constexpr int kAdpcmStep[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,
    31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,   494,
    544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,
    9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};

}  // namespace

Cs4231::Cs4231(Scheduler& scheduler, double cpu_hz)
    : sched_(scheduler),
      cpu_hz_(cpu_hz),
      frame_timer_(scheduler, [this] { tick(); }),
      timer_(scheduler, [this] {
          raise(kTi);
          timer_restart();
      }),
      calibrate_(scheduler, [this] { update_engine(); }) {
    reset();
}

void Cs4231::reset() {
    regs_ = kResetValues;
    index_ = kMce;  // the index register comes up with MCE set
    status_ = 0;
    errors_ = 0;
    play_count_ = capture_count_ = 0;
    last_frame_ = {};
    adpcm_[0] = adpcm_[1] = {};
    adpcm_pending_ = -1;
    adpcm_capture_[0] = adpcm_capture_[1] = {};
    adpcm_capture_pending_ = -1;
    pio_.clear();
    capture_pio_.clear();
    frame_timer_.cancel();
    timer_.cancel();
    calibrate_.cancel();
    engine_on_ = false;
    adc_last_ = {};
    update_irq();
}

double Cs4231::sample_rate(u8 format) {
    static constexpr unsigned kDivide[8] = {3072, 1536, 896, 768, 448, 384, 512, 2560};
    return ((format & kXtal2) ? kCrystal2 : kCrystal1) / kDivide[(format >> 1) & 7];
}

bool Cs4231::playing() const {
    return (regs_[kInterface] & kPen) && !(index_ & kMce) && !calibrate_.active();
}

bool Cs4231::capturing() const {
    return (regs_[kInterface] & kCen) && !(index_ & kMce) && !calibrate_.active();
}

void Cs4231::update_irq() {
    const bool on = (regs_[kPinControl] & kIen) && (status_ & kStInt);
    if (on == irq_out_) return;
    irq_out_ = on;
    if (irq_) irq_(on);
}

void Cs4231::raise(u8 flag) {
    regs_[kAltStatus] |= flag;
    status_ |= kStInt;
    update_irq();
}

bool Cs4231::read(u64 offset, unsigned size, u64& value) {
    value = 0;
    for (unsigned i = 0; i < size; ++i) {
        u8 b = 0;
        switch ((offset + i) >> 2 & 3) {
            case kIndex: b = index_; break;
            case kData: b = read_indexed(index_ & index_mask()); break;
            case kStatus: {
                b = (status_ & kStInt) | kStSamplePosition;
                if (errors_) b |= kStSer;
                if ((regs_[kInterface] & (kPen | kPpio)) == (kPen | kPpio) && pio_.size() < 4) b |= kStPrdy;
                if ((regs_[kInterface] & (kCen | kCpio)) == (kCen | kCpio) && !capture_pio_.empty()) b |= kStCrdy;
                break;
            }
            case kPio:
                // Captured data in PIO mode (silence if none is waiting).
                if (capture_pio_.empty()) b = silence(capture_format());
                else {
                    b = capture_pio_.front();
                    capture_pio_.pop_front();
                }
                break;
        }
        value = value << 8 | b;
    }
    return true;
}

bool Cs4231::write(u64 offset, unsigned size, u64 value) {
    for (unsigned i = 0; i < size; ++i) {
        const u8 b = static_cast<u8>(value >> (8 * (size - 1 - i)));
        switch ((offset + i) >> 2 & 3) {
            case kIndex: {
                const bool left_mce = (index_ & kMce) && !(b & kMce);
                index_ = b & 0x7F;  // INIT is read-only
                if (left_mce && (regs_[kInterface] & kAcal)) {
                    // Leaving mode change with autocalibration on: ACI for
                    // the time a calibration takes.
                    const double frames = kCalibrationFrames / sample_rate(regs_[kPlayFormat]);
                    calibrate_.schedule_in(static_cast<u64>(frames * cpu_hz_) + 1);
                }
                update_engine();
                update_irq();
                break;
            }
            case kData: write_indexed(index_ & index_mask(), b); break;
            case kStatus:
                // Any write clears every interrupt.
                regs_[kAltStatus] &= static_cast<u8>(~(kPi | kCi | kTi));
                status_ &= static_cast<u8>(~kStInt);
                update_irq();
                break;
            case kPio:
                if ((regs_[kInterface] & kPpio) && pio_.size() < 64) pio_.push_back(b);
                break;
        }
    }
    return true;
}

u8 Cs4231::read_indexed(unsigned i) {
    switch (i) {
        case kErrorStatus: {
            u8 v = errors_;
            if (calibrate_.active()) v |= kAci;
            if ((playing() && !(regs_[kInterface] & kPpio)) || (capturing() && !(regs_[kInterface] & kCpio))) v |= kDrs;
            errors_ = 0;  // underrun and overrun clear when read
            return v;
        }
        case kModeId: return static_cast<u8>(kModeIdFixed | (regs_[kModeId] & kMode2));
        case kVersion: return kVersionCs4231a;
        case kAltStatus: return regs_[kAltStatus] & (kPi | kCi | kTi);
        default: return regs_[i];
    }
}

void Cs4231::write_indexed(unsigned i, u8 v) {
    const bool mce = index_ & kMce;
    switch (i) {
        case kErrorStatus:
        case kVersion: return;  // read-only
        case kModeId: regs_[i] = v & kMode2; return;
        case kPlayFormat:
        case kCaptureFormat:
            if (mce) regs_[i] = v;  // formats change only in mode change
            return;
        case kInterface: {
            // Only the enables change outside mode change.
            const u8 mask = mce ? 0xFF : (kPen | kCen);
            const u8 was = regs_[i];
            regs_[i] = static_cast<u8>((was & ~mask) | (v & mask));
            if ((regs_[i] & kPen) && !(was & kPen)) {
                play_count_ = play_base();
                adpcm_[0] = adpcm_[1] = {};
                adpcm_pending_ = -1;
            }
            if ((regs_[i] & kCen) && !(was & kCen)) {
                capture_count_ = capture_base();
                adpcm_capture_[0] = adpcm_capture_[1] = {};
                adpcm_capture_pending_ = -1;
                capture_pio_.clear();
            }
            update_engine();
            return;
        }
        case kAltStatus:
            // Writing 0 to an interrupt bit clears it.
            regs_[i] &= static_cast<u8>(v | ~(kPi | kCi | kTi));
            if (!(regs_[i] & (kPi | kCi | kTi))) status_ &= static_cast<u8>(~kStInt);
            update_irq();
            return;
        case kFeature1: {
            const bool timer_was = regs_[i] & kTe;
            regs_[i] = v;
            if ((v & kTe) && !timer_was) timer_restart();
            if (!(v & kTe)) timer_.cancel();
            return;
        }
        case kPinControl:
            regs_[i] = v;
            update_irq();
            return;
        case kLeftAux1: case kRightAux1: case kLeftAux2: case kRightAux2: case kLeftLine: case kRightLine:
        case kLeftInput: case kRightInput: case kLoopback:
            regs_[i] = v;
            update_engine();  // a live input unmuted or muted, or looped back
            return;
        default: regs_[i] = v; return;
    }
}

void Cs4231::set_input_live(Input in, bool live) {
    live_[static_cast<unsigned>(in)] = live;
    update_engine();
}

bool Cs4231::analog_heard() const {
    auto passes = [this](Input in, unsigned left) {
        return live_[static_cast<unsigned>(in)] && (!(regs_[left] & kInputMute) || !(regs_[left + 1] & kInputMute));
    };
    return passes(Input::Line, kLeftLine) || passes(Input::Aux1, kLeftAux1) || passes(Input::Aux2, kLeftAux2);
}

void Cs4231::timer_restart() {
    if (!(regs_[kFeature1] & kTe)) return;
    const unsigned count = std::max(1, regs_[kTimerHigh] << 8 | regs_[kTimerLow]);
    timer_.schedule_in(static_cast<u64>(count / kTimerHz * cpu_hz_) + 1);
}

// Whether the loopback runs with something to loop: it freeruns at the
// sample rate whether or not playback or capture is on.
bool Cs4231::loopback_heard() const {
    if (!(regs_[kLoopback] & kLoopbackEnable)) return false;
    for (unsigned c = 0; c < 2; ++c) {
        const unsigned source = regs_[c ? kRightInput : kLeftInput] >> 6;
        if (source == 3 ? analog_heard() : live_[source]) return true;
    }
    return false;
}

void Cs4231::update_engine() {
    const bool on = playing() || capturing() || analog_heard() || loopback_heard();
    if (on == engine_on_) return;
    engine_on_ = on;
    if (on) {
        last_ = sched_.now();
        frame_debt_ = digital_debt_ = 0;
        frame_timer_.schedule_in(1);
    } else {
        frame_timer_.cancel();
    }
}

// The codec runs two clocks: the sample rate (the DAC, the ADC and the
// loopback, and the counters) and the output mixer's, which is analog on
// the chip. The mixer is taken at the sample rate when no analog input is
// heard, else at 44.1 kHz (or the sample rate, if higher), so an input is
// not cut to half a low sample rate on its way to the output.
void Cs4231::tick() {
    if (!engine_on_) return;
    const u64 now = sched_.now();
    const bool digital = playing() || capturing() || loopback_heard();
    const double rate = sample_rate(regs_[kPlayFormat]);
    const double mixer = analog_heard() ? std::max(kAnalogRate, digital ? rate : 0.0) : rate;
    const double seconds = static_cast<double>(now - last_) / cpu_hz_;
    last_ = now;
    const double due = seconds * mixer + frame_debt_;
    const unsigned frames = static_cast<unsigned>(due);
    frame_debt_ = due - frames;
    unsigned samples = 0;
    if (digital) {
        const double d = seconds * rate + digital_debt_;
        samples = static_cast<unsigned>(d);
        digital_debt_ = d - samples;
    }
    if (frames || samples) mix(frames, mixer, samples);
    // The next block: a millisecond's frames, or fewer samples to reach a
    // counter's end on time when its interrupt is enabled and not already
    // raised.
    if (!digital) {
        const unsigned next = std::max(1u, static_cast<unsigned>(mixer * kBlockSeconds));
        frame_timer_.schedule_in(static_cast<u64>((next - frame_debt_) / mixer * cpu_hz_) + 1);
        return;
    }
    unsigned next = std::max(1u, static_cast<unsigned>(rate * kBlockSeconds));
    const bool watched = regs_[kPinControl] & kIen;
    if (playing() && watched && !(regs_[kAltStatus] & kPi)) next = std::min(next, play_count_ + 1u);
    if (capturing() && watched && !(regs_[kAltStatus] & kCi)) next = std::min(next, capture_count_ + 1u);
    frame_timer_.schedule_in(static_cast<u64>((next - digital_debt_) / rate * cpu_hz_) + 1);
}

// One block through the codec: `frames` of the output mixer at `mixer` Hz
// and, over the same time, `samples` sample periods of the DAC, the ADC
// and the loopback. Each sample period spans a run of the mixer's frames:
// the DAC's sample is held through it, and the ADC takes the mean of its
// input over it. With the mixer's output as the ADC's source, the
// loopback takes the ADC's previous sample.
void Cs4231::mix(unsigned frames, double mixer, unsigned samples) {
    const std::size_t n = std::size_t{frames} * 2;
    // Each input's frames, fetched when first wanted.
    std::array<bool, 4> fetched{};
    auto input = [&](unsigned i) -> const std::vector<s16>& {
        if (!fetched[i]) {
            fetched[i] = true;
            in_[i].assign(n, 0);
            if (live_[i] && inputs_[i] && frames) inputs_[i](in_[i].data(), frames, mixer);
        }
        return in_[i];
    };

    // The analog inputs the mixer passes: LINE, AUX1, AUX2.
    analog_.assign(n, 0.0);
    static constexpr struct {
        Input in;
        unsigned left;
    } kMixed[] = {{Input::Line, kLeftLine}, {Input::Aux1, kLeftAux1}, {Input::Aux2, kLeftAux2}};
    for (const auto& m : kMixed) {
        const unsigned i = static_cast<unsigned>(m.in);
        if (!live_[i]) continue;
        for (unsigned c = 0; c < 2; ++c) {
            const u8 control = regs_[m.left + c];
            if (control & kInputMute) continue;
            const double gain = db_steps(8 - (control & 0x1F));  // 01000: 0 dB
            const std::vector<s16>& x = input(i);
            for (unsigned f = 0; f < frames; ++f) analog_[f * 2 + c] += x[f * 2 + c] * gain;
        }
    }

    // The ADC's source and gain per channel.
    std::array<unsigned, 2> source{};
    std::array<double, 2> in_gain{};
    for (unsigned c = 0; c < 2; ++c) {
        const u8 control = regs_[c ? kRightInput : kLeftInput];
        source[c] = control >> 6;
        in_gain[c] = db_steps(control & 0x0F);
        if (source[c] == static_cast<unsigned>(Input::Mic) && (control & kMicGain)) in_gain[c] *= 10.0;  // +20 dB
        if (source[c] != 3 && samples) input(source[c]);
    }
    // The loopback: the ADC's data, attenuated, summed with the DAC's data
    // (clipped at full scale) before the DAC's attenuation. A mono capture
    // format loops its one channel (the left) into both.
    const u8 loop = regs_[kLoopback];
    const double loop_gain = (loop & kLoopbackEnable) ? db_steps(-(loop >> 2)) : 0.0;
    const bool mono_capture = !(capture_format() & kStereo);
    std::array<double, 2> out_gain{};
    for (unsigned c = 0; c < 2; ++c) {
        const u8 att = regs_[c ? kRightOut : kLeftOut];
        out_gain[c] = (att & 0x80) ? 0.0 : db_steps(-(att & 0x3F));
    }

    const bool dac_on = playing(), capture_on = capturing();
    out_.assign(std::size_t{samples} * 2, 0);
    if (dac_on && samples) play(samples);
    adc_.assign(std::size_t{samples} * 2, 0.0);
    mixed_.assign(n, 0.0);
    std::array<double, 2> held{};  // the DAC's output this sample period
    for (unsigned k = 0, f = 0; k < samples || f < frames; ++k) {
        // The mixer's frames this sample period spans (all of them when
        // nothing digital runs).
        const unsigned end = samples ? static_cast<unsigned>(u64{k + 1} * frames / samples) : frames;
        if (k < samples) {
            // The ADC's samples taken from the inputs over the span.
            for (unsigned c = 0; c < 2; ++c) {
                if (source[c] == 3) continue;
                const std::vector<s16>& x = in_[source[c]];
                if (end > f) {
                    double sum = 0;
                    for (unsigned g = f; g < end; ++g) sum += x[g * 2 + c];
                    adc_last_[c] = sum / (end - f) * in_gain[c];
                } else if (frames) {
                    adc_last_[c] = x[std::min(f, frames - 1) * 2 + c] * in_gain[c];
                }
                adc_[k * 2 + c] = adc_last_[c];
            }
            for (unsigned c = 0; c < 2; ++c) {
                double v = dac_on ? out_[k * 2 + c] : 0.0;
                if (loop_gain > 0) v += adc_last_[mono_capture ? 0 : c] * loop_gain;
                held[c] = std::clamp(v, -32768.0, 32767.0) * out_gain[c];
            }
        }
        std::array<double, 2> sum{};
        for (unsigned g = f; g < end; ++g)
            for (unsigned c = 0; c < 2; ++c) {
                const double m = held[c] + analog_[g * 2 + c];
                mixed_[g * 2 + c] = m;
                sum[c] += m;
            }
        if (k < samples)
            for (unsigned c = 0; c < 2; ++c) {
                if (source[c] != 3) continue;
                if (end > f) adc_last_[c] = sum[c] / (end - f) * in_gain[c];
                adc_[k * 2 + c] = adc_last_[c];
            }
        f = end;
        if (!samples) break;
    }
    if (capture_on && samples) capture(samples);

    if (!sink_ || !frames || (!dac_on && !analog_heard() && !loopback_heard())) return;
    // Nothing is heard with the speaker, line and headphone outputs all off.
    const bool heard = !(regs_[kMonoControl] & kMonoMute) || !(regs_[kPinControl] & kXctl0) ||
                       !(regs_[kPinControl] & kXctl1);
    out_.resize(n);
    for (std::size_t i = 0; i < n; ++i)
        out_[i] = heard ? static_cast<s16>(std::lround(std::clamp(mixed_[i], -32768.0, 32767.0))) : s16{0};
    sink_(out_.data(), frames, static_cast<unsigned>(std::lround(mixer)));
}

// Counts `frames` sample periods off a counter: each time it passes zero,
// the interrupt flag rises and the counter reloads from its base.
void Cs4231::count(u16& counter, u16 base, unsigned frames, u8 flag) {
    while (frames) {
        if (frames <= counter) {
            counter = static_cast<u16>(counter - frames);
            return;
        }
        frames -= counter + 1u;
        raise(flag);
        counter = base;
    }
}

s16 Cs4231::adpcm(int channel, unsigned nibble) {
    Adpcm& a = adpcm_[channel];
    const int step = kAdpcmStep[a.index];
    int diff = step >> 3;
    if (nibble & 4) diff += step;
    if (nibble & 2) diff += step >> 1;
    if (nibble & 1) diff += step >> 2;
    a.predictor = std::clamp(a.predictor + ((nibble & 8) ? -diff : diff), -32768, 32767);
    a.index = std::clamp(a.index + kAdpcmIndex[nibble & 15], 0, 88);
    return static_cast<s16>(a.predictor);
}

void Cs4231::play(unsigned frames) {
    const u8 format = regs_[kPlayFormat];
    const unsigned code = format >> 5, channels = (format & kStereo) ? 2 : 1;
    // The bytes these frames take (ADPCM: four bits a sample, less the
    // half byte already waiting).
    std::size_t want;
    if (code == kAdpcm) {
        const std::size_t nibbles = std::size_t{frames} * channels - (adpcm_pending_ >= 0 ? 1 : 0);
        want = (nibbles + 1) / 2;
    } else {
        want = std::size_t{frames} * channels * sample_bytes(format);
    }
    bytes_.resize(want);
    std::size_t got = 0;
    if (regs_[kInterface] & kPpio) {
        got = std::min(want, pio_.size());
        std::copy_n(pio_.begin(), got, bytes_.begin());
        pio_.erase(pio_.begin(), pio_.begin() + static_cast<std::ptrdiff_t>(got));
    } else if (play_dma_ && !transfers_stopped()) {
        got = play_dma_->from_memory(bytes_.data(), want);
    }

    // Decode what came; frames past it are an underrun.
    out_.resize(std::size_t{frames} * 2);
    unsigned decoded = 0;
    if (code == kAdpcm) {
        std::size_t byte = 0;
        auto next_nibble = [&](unsigned& n) {
            if (adpcm_pending_ >= 0) {
                n = static_cast<unsigned>(adpcm_pending_);
                adpcm_pending_ = -1;
                return true;
            }
            if (byte >= got) return false;
            n = bytes_[byte] & 0x0F;  // the low half first
            adpcm_pending_ = bytes_[byte] >> 4;
            ++byte;
            return true;
        };
        for (; decoded < frames; ++decoded) {
            unsigned l, r;
            if (!next_nibble(l)) break;
            if (channels == 2 && !next_nibble(r)) {
                adpcm_pending_ = static_cast<int>(l);  // keep the left half for the next block
                break;
            }
            const s16 left = adpcm(0, l);
            last_frame_ = {left, channels == 2 ? adpcm(1, r) : left};
            out_[decoded * 2] = last_frame_[0];
            out_[decoded * 2 + 1] = last_frame_[1];
        }
    } else {
        const unsigned width = sample_bytes(format), frame_bytes = width * channels;
        for (; decoded < frames && (decoded + 1u) * frame_bytes <= got; ++decoded) {
            const u8* p = bytes_.data() + std::size_t{decoded} * frame_bytes;
            s16 s[2];
            for (unsigned c = 0; c < channels; ++c, p += width) {
                switch (code) {
                    case kLinear8: s[c] = static_cast<s16>((p[0] - 0x80) << 8); break;
                    case kMuLaw: s[c] = mu_law(p[0]); break;
                    case kALaw: s[c] = a_law(p[0]); break;
                    case kLinear16Be: s[c] = static_cast<s16>(p[0] << 8 | p[1]); break;
                    case kLinear16Le: s[c] = static_cast<s16>(p[1] << 8 | p[0]); break;
                    default: s[c] = static_cast<s16>((p[0] - 0x80) << 8); break;  // reserved formats
                }
            }
            last_frame_ = {s[0], channels == 2 ? s[1] : s[0]};
            out_[decoded * 2] = last_frame_[0];
            out_[decoded * 2 + 1] = last_frame_[1];
        }
    }
    if (decoded < frames) {
        // Underrun: the DAC repeats the last sample, or goes to zero.
        errors_ |= kPur;
        if (regs_[kFeature1] & kDacz) last_frame_ = {};
        for (unsigned f = decoded; f < frames; ++f) {
            out_[f * 2] = last_frame_[0];
            out_[f * 2 + 1] = last_frame_[1];
        }
    }
    count(play_count_, play_base(), frames, kPi);
}

unsigned Cs4231::adpcm_encode(int channel, int sample) {
    Adpcm& a = adpcm_capture_[channel];
    int step = kAdpcmStep[a.index];
    int diff = sample - a.predictor;
    unsigned nibble = 0;
    if (diff < 0) {
        nibble = 8;
        diff = -diff;
    }
    int delta = step >> 3;
    for (unsigned bit = 4; bit; bit >>= 1, step >>= 1) {
        if (diff >= step) {
            nibble |= bit;
            diff -= step;
            delta += step;
        }
    }
    a.predictor = std::clamp(a.predictor + ((nibble & 8) ? -delta : delta), -32768, 32767);
    a.index = std::clamp(a.index + kAdpcmIndex[nibble], 0, 88);
    return nibble;
}

// The ADC's frames (adc_) in the capture format, to the capture channel or
// the PIO data register; what neither takes is an overrun. Mono records
// the left channel.
void Cs4231::capture(unsigned frames) {
    const u8 format = capture_format();
    const unsigned code = format >> 5, channels = (format & kStereo) ? 2 : 1;
    bytes_.clear();
    for (unsigned f = 0; f < frames; ++f)
        for (unsigned c = 0; c < channels; ++c) {
            const int v = static_cast<int>(std::lround(std::clamp(adc_[f * 2 + c], -32768.0, 32767.0)));
            switch (code) {
                case kLinear8: bytes_.push_back(static_cast<u8>((v >> 8) + 0x80)); break;
                case kMuLaw: bytes_.push_back(mu_law_encode(v)); break;
                case kALaw: bytes_.push_back(a_law_encode(v)); break;
                case kLinear16Le:
                    bytes_.push_back(static_cast<u8>(v));
                    bytes_.push_back(static_cast<u8>(v >> 8));
                    break;
                case kLinear16Be:
                    bytes_.push_back(static_cast<u8>(v >> 8));
                    bytes_.push_back(static_cast<u8>(v));
                    break;
                case kAdpcm: {
                    const unsigned nibble = adpcm_encode(static_cast<int>(c), v);  // the low half first
                    if (adpcm_capture_pending_ < 0) adpcm_capture_pending_ = static_cast<int>(nibble);
                    else {
                        bytes_.push_back(static_cast<u8>(adpcm_capture_pending_ | (nibble << 4)));
                        adpcm_capture_pending_ = -1;
                    }
                    break;
                }
                default: bytes_.push_back(silence(format)); break;  // reserved formats
            }
        }
    if (regs_[kInterface] & kCpio) {
        // The codec's FIFO holds 16 samples' worth; more is an overrun.
        for (u8 b : bytes_) {
            if (capture_pio_.size() >= 64) {
                errors_ |= kCor;
                break;
            }
            capture_pio_.push_back(b);
        }
    } else {
        const std::size_t taken =
            (capture_dma_ && !transfers_stopped()) ? capture_dma_->to_memory(bytes_.data(), bytes_.size()) : 0;
        if (taken < bytes_.size()) errors_ |= kCor;  // overrun: the samples are lost
    }
    count(capture_count_, capture_base(), frames, kCi);
}

}  // namespace uv
