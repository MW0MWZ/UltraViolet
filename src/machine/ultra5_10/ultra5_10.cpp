// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
#include "machine/ultra5_10/ultra5_10.h"

#include <algorithm>
#include <random>

#include "core/int128.h"
#include "core/log.h"
#include "core/text.h"

namespace uv {

namespace {

// The processor module: VER's mask is the part's revision, 0x13 on a
// Sabre (SME1040) module and 0x91 on a Sapphire-Red (SME1430) one, as a
// real Ultra 10 reads them. Sun's Sapphire-Red modules are the 400 and
// 440 MHz ones and the 360 MHz with 256 KiB of E-cache; the 270, 300 and
// 360 MHz (2 MiB) are Sabres; 333 MHz came as either (taken as a Sabre).
usparc::UltraSparc::Options cpu_options(const Ultra5_10::Options& o) {
    usparc::UltraSparc::Options c;
    c.clock_hz = o.cpu_hz;
    c.ecache_bytes = o.ecache_bytes;
    const double mhz = o.cpu_hz / 1e6;
    const bool sapphire_red = mhz >= 380 || (mhz >= 350 && o.ecache_bytes <= (256u << 10));
    const u64 mask = sapphire_red ? 0x91 : 0x13;
    c.version = (c.version & ~(u64{0xFF} << 24)) | (mask << 24);
    return c;
}

// EBus device space (BAR1) offsets, as the Ultra 5/10 OpenBoot device tree
// places them.
constexpr u32 kEbusNvram = 0x000000, kEbusAudio = 0x200000, kEbusSuperIo = 0x300000, kEbusFloppy = 0x3023F0,
              kEbusParallel = 0x3043BC, kEbusMouse = 0x3062F8, kEbusKeyboard = 0x3083F8, kEbusSerial = 0x400000,
              kEbusSmartCard = 0x500000, kEbusPll = 0x504000, kEbusEcppDma = 0x700000,
              kEbusAudioPlayDma = 0x702000, kEbusAudioRecDma = 0x704000, kEbusFloppyDma = 0x706000,
              kEbusFloppyAux = 0x720000, kEbusAudioAux = 0x722000, kEbusPower = 0x724000;
// The auxio node's registers, a word each: the system LED (bit 0 of the
// first byte, all of it that takes writes), then four whose bits the BSDs
// name (PCI slot modes, clock frequency, SCSI oscillator, temperature
// sensor) and that nothing here acts on; their first bytes as a real Ultra 10
// reads them, the rest 0.
constexpr u32 kEbusAuxio[] = {0x726000, 0x728000, 0x72A000, 0x72C000, 0x72F000};
constexpr u8 kAuxioValue[] = {0x00, 0xFF, 0x07, 0x01, 0x06};

// UPA port 0x1E's space: the UPA slot.
constexpr u64 kUpaSlot = 0x1FC00000000;

constexpr double kPciHz = 66.0e6;      // the APB's primary bus clock, which its tick register counts
constexpr double kSuperIoUartHz = 1843200.0;
constexpr u32 kSuperIoWindow = 0x10000;  // its chip selects' ISA addresses (0x2000 floppy ... 0x8000 keyboard)
constexpr u8 kSuperIoSid = 0x90;       // SuperIO identification register value, as a real Ultra 5/10 reads
constexpr u8 kMachineType = 0x80;      // IDPROM machine type: sun4u
constexpr unsigned kNvramIdprom = 0x1FD8;  // the IDPROM in the NVRAM (the configuration below it)
constexpr unsigned kNvramConfigEnd = 0x1CD8;  // the end of what OpenBoot's checksum covers
constexpr char kBoardDate[] = "19991101";     // a new board's date of manufacture (a 1999 board, as a real Ultra 10's)
constexpr u32 kBoardDateBcd = 0x11011999;     // the same, as the IDPROM holds it (MMDDYYYY)

}  // namespace

Ultra5_10::Ultra5_10(const Options& options)
    : opt_(options),
      pacer_(options.timing, options.cpu_hz, options.host_clock),
      ram_(static_cast<std::size_t>(options.ram_mb) << 20),
      cpu_(bus_, cpu_options(options)),
      sabre_(bus_, sched_, [this](u64 inr) { return cpu_.deliver_interrupt_vector(inr, 0, 0); }),
      simba_b_(false, [this] { return pci_ticks(); }),
      simba_a_(true, [this] { return pci_ticks(); }),
      flash_(options.prom, 0x01, 0xD5, 0x10000),
      start_time_(options.start_time ? options.start_time : std::time(nullptr)),
      nvram_([this] { return start_time_ + static_cast<std::time_t>(guest_seconds()); }),
      superio_(kSuperIoSid),
      su_kbd_(sched_, kbd_line_, kSuperIoUartHz, options.cpu_hz),
      su_mouse_(sched_, mouse_line_, kSuperIoUartHz, options.cpu_hz),
      se_(sched_, ttya_, ttyb_, options.cpu_hz),
      fdc_(sched_),
      ide_(sched_, options.cpu_hz),
      hme_(sched_, options.cpu_hz),
      mach64_(sched_, options.cpu_hz),
      cdrom_(sched_, options.cpu_hz),
      audio_(sched_, options.cpu_hz) {
    sched_.set_clock([this] { return cpu_.cycles(); });
    // A device given work by the CPU may want to act before the slice the
    // CPU is running ends: the CPU stops after that instruction, and the
    // next slice runs up to the device's event.
    sched_.set_on_earlier([this] { cpu_.stop_run(); });
    pci_ticks_fx_ = static_cast<u64>(kPciHz / opt_.cpu_hz * 18446744073709551616.0);  // (the PCI clock is the slower)
    hz_ = static_cast<u64>(opt_.cpu_hz + 0.5);
    per_second_fx_ = ~u64{0} / hz_;
    nvram_.set_next_second([this] { return (guest_seconds() + 1) * hz_; });
    // The alarm and the watchdog in guest time. (The IRQ output goes to the
    // power supply, to switch a system in standby on; the reset output is
    // not connected.)
    nvram_.attach(sched_, opt_.cpu_hz);
    // Delay loops the CPU runs at once cost at least the real CPU's time
    // (see Pacer::delay_cycles_per_instruction).
    cpu_.set_delay_cycles_per_step(pacer_.delay_cycles_per_instruction());

    bus_.add_host_region(ram_.get(), ram_.size());
    // The two DIMM groups' pairs (bytes), then their sides: group 0's bottom
    // and top at banks 0 and 2, group 1's at 1 and 3.
    u64 pair[2] = {};
    const u64 total = ram_.size(), mib = u64{1} << 20;
    if (total >= 1024 * mib) {
        pair[0] = pair[1] = 512 * mib;
    } else if (total == 512 * mib) {
        pair[0] = pair[1] = 256 * mib;
    } else {
        for (u64& p : pair) {
            u64 size = std::min<u64>(total - (pair[0] + pair[1]), 256 * mib);
            while (size & (size - 1)) size &= size - 1;
            p = size;
        }
    }
    u64 placed = 0;
    for (unsigned g = 0; g < 2; ++g) {
        const u64 side = std::min(pair[g], kBankWindow);
        for (unsigned top = 0; top < 2 && placed + side <= total && side && pair[g] > top * side; ++top) {
            banks_[g + 2 * top] = {ram_.get() + placed, side, side == 32 * mib ? 10u : 11u};  // (16 MiB DIMMs: 10-bit)
            placed += side;
        }
    }
    if (placed < ram_.size())
        UV_LOG(Warn, "ultra5_10", "%llu MiB of memory left unused (banks are powers of two)",
               static_cast<unsigned long long>((ram_.size() - placed) >> 20));
    map_memory(false);  // Mem_Control0 at power-on: 10-bit columns
    sabre_.on_mem_control0 = [this](u64 v) {
        // The column mode as written with refresh on. (Software turns refresh
        // off, with the rest 0, to change the register, then writes the new
        // value, running from the I-cache in between; with no caches here
        // those instructions come from memory, decoded as before.)
        if ((v & Sabre::kMc0RefEnable) && ((v & Sabre::kMc0Col11) != 0) != col11_) map_memory(!col11_);
    };
    sabre_.map();

    // PCI topology: the APB's function 0 is bus B, function 1 bus A.
    sabre_.root().attach(1, 0, &simba_b_);
    sabre_.root().attach(1, 1, &simba_a_);
    Simba::pair(simba_a_, simba_b_);
    simba_a_.secondary().attach(1, 0, &ebus_);
    simba_a_.secondary().attach(1, 1, &hme_);  // the RIO's Ethernet
    simba_a_.secondary().attach(2, 0, &mach64_);  // the on-board graphics
    simba_a_.secondary().attach(3, 0, &ide_);

    // The UPA slot: a graphics card at port 0x1E.
    if (!opt_.creator3d_fcode.empty()) {
        ffb_ = std::make_unique<Ffb>(sched_, opt_.cpu_hz, opt_.creator3d_fcode);
        bus_.map_device(kUpaSlot, Ffb::kSpace, ffb_.get());
        ffb_->set_interrupt_sink([this] { sabre_.pulse_upa_interrupt(0); });
    }

    // EBus devices
    ebus_.set_prom(&flash_, static_cast<u32>(std::max<std::size_t>(opt_.prom.size(), 1)));
    ebus_.add(kEbusNvram, M48t59::kSize, &nvram_);
    ebus_.add(kEbusSuperIo + 0x15C, 2, &superio_);  // configuration ports at ISA 0x15C/0x15D
    ebus_.add(kEbusKeyboard, 8, &su_kbd_);
    ebus_.add(kEbusMouse, 8, &su_mouse_);
    ebus_.add(kEbusSerial, 0x80, &se_);
    auto block = [this](const char* name, u32 offset, u32 size, u8 fill = 0) {
        blocks_.push_back(std::make_unique<RegisterBlock>(name, size, fill));
        ebus_.add(offset, size, blocks_.back().get());
        return blocks_.back().get();
    };
    ebus_.add(kEbusAudio, Cs4231::kSize, &audio_);
    block("audio-aux", kEbusAudioAux, 4);  // bit 0: the codec's power-down (polarity unknown)
    ebus_.add(kEbusFloppy, 8, &fdc_);
    block("floppy-aux", kEbusFloppyAux, 4);  // bit 0: density sense
    // The parallel port in compatible mode, nothing connected (PC87332):
    // data, the status register (read-only; 0x7F as a real Ultra 10's), the
    // control register (bits 7:5 read 1).
    block("ecpp", kEbusParallel, 8)->fix({0x00, 0x7F, 0xE0}, {0xFF, 0x00, 0x1F});
    block("smartcard", kEbusSmartCard, 0x100);
    block("pll", kEbusPll, 0x100);
    ebus_.add(kEbusEcppDma, EbusDma::kSize, &ecpp_dma_, false);
    ebus_.add(kEbusAudioPlayDma, EbusDma::kSize, &audio_play_dma_, false);
    ebus_.add(kEbusAudioRecDma, EbusDma::kSize, &audio_rec_dma_, false);
    ebus_.add(kEbusFloppyDma, EbusDma::kSize, &fd_dma_, false);
    ebus_.add(kEbusPower, PowerControl::kSize, &power_);
    // (Last: what the SuperIO's chip selects leave undecoded floats.)
    ebus_.add(kEbusSuperIo, kSuperIoWindow, &superio_float_);
    auxio_led_ = block("auxio", kEbusAuxio[0], 4);
    auxio_led_->fix({kAuxioValue[0]}, {0x01, 0, 0, 0});
    for (unsigned i = 1; i < std::size(kEbusAuxio); ++i) block("auxio", kEbusAuxio[i], 4)->fix({kAuxioValue[i]}, {0, 0, 0, 0});

    su_kbd_.set_scratch(0x30);  // what a real Ultra 10 reads (no software writes it)
    su_kbd_.set_irq_sink([this](bool a) { sabre_.set_interrupt(kInoKeyboard, a); });
    su_mouse_.set_irq_sink([this](bool a) { sabre_.set_interrupt(kInoMouse, a); });
    se_.set_irq_sink([this](bool a) { sabre_.set_interrupt(kInoSerial, a); });
    // The floppy controller's interrupt reaches the system through its DMA
    // channel (INT_PEND, gated by INT_EN), as do the other EBus DMA devices'.
    fdc_.set_irq_sink([this](bool a) { fd_dma_.set_device_irq(a); });
    fd_dma_.set_irq_sink([this](bool a) { sabre_.set_interrupt(kInoFloppy, a); });
    ecpp_dma_.set_irq_sink([this](bool a) { sabre_.set_interrupt(kInoParallel, a); });
    audio_play_dma_.set_irq_sink([this](bool a) { sabre_.set_interrupt(kInoAudioPlay, a); });
    audio_rec_dma_.set_irq_sink([this](bool a) { sabre_.set_interrupt(kInoAudioRecord, a); });
    // The codec's interrupt line reaches both of its channels.
    audio_.set_dma(&audio_play_dma_, &audio_rec_dma_);
    audio_.set_irq_sink([this](bool a) {
        audio_play_dma_.set_device_irq(a);
        audio_rec_dma_.set_device_irq(a);
    });
    for (EbusDma* d : {&ecpp_dma_, &audio_play_dma_, &audio_rec_dma_, &fd_dma_})
        d->set_dma_port([this](u32 a, u8* p, std::size_t n) { return sabre_.dma_read(a, p, n); },
                        [this](u32 a, const u8* p, std::size_t n) { return sabre_.dma_write(a, p, n); });

    // The on-board graphics' interrupt.
    mach64_.set_interrupt_sink([this](bool a) { sabre_.set_interrupt(kInoGraphics, a); });
    // The on-board Ethernet: its interrupt, DMA and the host's side.
    hme_.set_interrupt_sink([this](bool a) { sabre_.set_interrupt(kInoRio, a); });
    hme_.set_dma_port([this](u32 a, u8* d, std::size_t n) { return sabre_.dma_read(a, d, n); },
                      [this](u32 a, const u8* d, std::size_t n) { return sabre_.dma_write(a, d, n); });
    hme_.set_network(opt_.network);
    if (opt_.keyboard) {
        keyboard_ = std::make_unique<SunKeyboard>(sched_, kbd_line_, opt_.cpu_hz);
        keyboard_->set_layout(opt_.keyboard_layout);
        mouse_ = std::make_unique<SunMouse>(mouse_line_);
    }

    // IDE: the disk (if any) on the primary channel, the CD-ROM drive as
    // master of the secondary.
    ide_.set_interrupt_sink([this](bool a) { sabre_.set_interrupt(kInoIde, a); });
    ide_.set_dma_port([this](u32 a, u8* d, std::size_t n) { return sabre_.dma_read(a, d, n); },
                      [this](u32 a, const u8* d, std::size_t n) { return sabre_.dma_write(a, d, n); });
    if (!opt_.disk_path.empty()) {
        const bool ok = opt_.disk_create_bytes ? disk_image_.create(opt_.disk_path, opt_.disk_create_bytes)
                                               : disk_image_.open(opt_.disk_path, false);
        if (ok) {
            disk_ = std::make_unique<AtaDisk>(sched_, opt_.cpu_hz, disk_image_);
            ide_.channel(0).attach(0, disk_.get());
        } else {
            UV_LOG(Error, "ultra5_10", "cannot open disk image %s", opt_.disk_path.c_str());
        }
    }
    ide_.channel(1).attach(0, &cdrom_);
    apply_fast_io();
    if (!opt_.cdrom_path.empty() && !insert_cdrom(opt_.cdrom_path))
        UV_LOG(Error, "ultra5_10", "cannot open CD image %s", opt_.cdrom_path.c_str());

    // NVRAM: the saved contents, or (none, or a blank image) a new one.
    if (opt_.nvram_path.empty() || !nvram_.load(opt_.nvram_path) || nvram_blank()) write_new_nvram();

    bus_.set_rom_write_handler([this](u64 pa, unsigned size, u64 value) {
        if (prom_map_size_ && pa - prom_pa_ < prom_map_size_) flash_.write(pa - prom_pa_, size, value);
    });
    flash_.on_mode_change = [this] { update_prom_mapping(); };
    flash_.set_write_enabled(options.flash_write_enable);
    ebus_.on_config_write = [this] { update_prom_mapping(); };
    simba_a_.on_config_write = [this] { update_prom_mapping(); };
    update_prom_mapping();

    // A reset the processor asks for with a store to Sabre's reset control
    // register comes after the store, between instructions (see run()).
    // Switched off by the system (OpenBoot's power-off, Solaris's poweroff):
    // the processor stops after the store, and the machine stays off.
    power_.on_power_off = [this] {
        switched_off_ = true;
        cpu_.stop_run();
    };
    sabre_.on_soft_reset = [this](bool por) {
        soft_reset_ = por ? SoftReset::Por : SoftReset::Xir;
        cpu_.stop_run();
    };
}

Ultra5_10::~Ultra5_10() { save_nvram(); }

void Ultra5_10::save_nvram() {
    if (!opt_.nvram_path.empty() && nvram_.dirty()) nvram_.save(opt_.nvram_path);
}

void Ultra5_10::reset(bool power_on) {
    switched_off_ = false;
    power_.reset();
    sabre_.reset(power_on);
    if (power_on) map_memory(false);  // Mem_Control0's power-on value: 10-bit columns
    update_prom_mapping();
    superio_.reset();
    su_kbd_.reset();
    su_mouse_.reset();
    if (keyboard_) keyboard_->reset(), mouse_->reset();
    se_.reset();
    fdc_.reset();
    for (EbusDma* d : {&ecpp_dma_, &audio_play_dma_, &audio_rec_dma_, &fd_dma_}) d->reset();
    audio_.reset();
    if (ffb_) ffb_->reset();
    for (auto& b : blocks_) b->reset();
    cpu_.reset();
}

void Ultra5_10::press_power_button() {
    // A pulse: the drivers clear nothing in the device, only the interrupt.
    sabre_.set_interrupt(kInoPower, true);
    sabre_.set_interrupt(kInoPower, false);
}

bool Ultra5_10::nvram_blank() const {
    const u8* p = nvram_.data();
    return std::all_of(p, p + kNvramIdprom + 16, [](u8 b) { return b == 0; });
}

// A new NVRAM, as a machine that has been set up before: the pattern a
// real Ultra 10's part holds wherever OpenBoot has not written (a real Ultra 10:
// 0x55, 0x00, 0xAA and 0xFF, 64 bytes each, over and over), OpenBoot
// 3.31's configuration defaults as it writes them over it, then the boot
// from the disk alone (not the network after it: a machine with nothing to
// boot stops at ok), the diagnostics at their least and the diagnostic
// switch off, as setenv writes them; a checksum OpenBoot accepts; an
// IDPROM with a Sun Ethernet address and host ID.
//
// OpenBoot keeps its variables at fixed places: byte 0 a checksum (0x17
// XOR every byte from 0x0C to 0x1CD7), byte 1 diag-switch? (0 or 0xFF),
// byte 2 diag-level (0x20 min, 0x40 max, 0x80 menus), bytes 8-0x0B the
// power-cycle count (#power-cycles, 32 bits); strings as a length, the text and a 0; flags 0 or
// 0xFF. Above 0x1CD8, outside the checksum, are its own records, which
// it writes as it boots.
// DRAM geometry: 16 bytes per column, the bank's columns as its DRAMs have
// them, the rest of its address in the rows.
void Ultra5_10::map_memory(bool col11) {
    col11_ = col11;
    const unsigned drive = col11 ? 11 : 10;
    const u64 decoded = col11 ? kBankWindow : kBankWindow / 2;  // the pair's share of the window
    for (unsigned b = 0; b < kBanks; ++b) {
        const u64 window = b * kBankWindow;
        const Bank& bank = banks_[b];
        if (decoded < kBankWindow) bus_.map_device(window + decoded, kBankWindow - decoded, &empty_bank_);  // pairs 1 and 3
        if (!bank.size) {
            bus_.map_device(window, decoded, &empty_bank_);
        } else if (bank.cols == drive) {
            // Rows and columns in address order: the bank, repeated.
            for (u64 off = 0; off < decoded; off += std::min(bank.size, decoded))
                bus_.map_ram(window + off, std::min(bank.size, decoded), bank.host);
        } else {
            MismatchedBank& m = mismatched_[b];
            m.host = bank.host, m.size = bank.size, m.drive_cols = drive, m.dram_cols = bank.cols;
            bus_.map_device(window, decoded, &m);
        }
    }
}

u64 Ultra5_10::MismatchedBank::cell(u64 offset) const {
    const u64 col = (offset >> 4) & ((u64{1} << drive_cols) - 1), row = offset >> (4 + drive_cols);
    // A wider column the DRAM ignores the top bit of; a narrower one leaves
    // the DRAM's top column bit 0.
    const u64 dcol = col & ((u64{1} << dram_cols) - 1);
    const u64 rows = size >> (4 + dram_cols);
    return (((row & (rows - 1)) << dram_cols | dcol) << 4) | (offset & 15);
}

bool Ultra5_10::MismatchedBank::read(u64 offset, unsigned n, u64& value) {
    value = 0;
    for (unsigned i = 0; i < n; ++i) value = value << 8 | host[cell(offset + i)];
    return true;
}

bool Ultra5_10::MismatchedBank::write(u64 offset, unsigned n, u64 value) {
    for (unsigned i = 0; i < n; ++i) host[cell(offset + i)] = static_cast<u8>(value >> (8 * (n - 1 - i)));
    return true;
}

void Ultra5_10::write_new_nvram() {
    u8* p = nvram_.data();
    static constexpr u8 kFill[4] = {0x55, 0x00, 0xAA, 0xFF};
    for (unsigned i = 0; i < kNvramIdprom; ++i) p[i] = kFill[(i >> 6) & 3];
    // OpenBoot 3.31's defaults, as it writes them (found by letting it set
    // them over the pattern): runs of zeros, then the values.
    static const struct {
        unsigned at, size;
    } kZeros[] = {{0x001, 3}, {0x010, 8}, {0x0B7, 0x89}, {0x180, 0xC0}, {0x280, 0x4A}, {0x3CC, 2}, {0x5D2, 1},
                  {0x6A8, 0x98}, {0x780, 0xB2}};
    for (const auto& z : kZeros) std::fill(p + z.at, p + z.at + z.size, u8{0});
    static const struct {
        unsigned at;
        std::vector<u8> bytes;
    } kDefaults[] = {
        {0x001, {0xFF, 0x40}},                                 // diag-switch? true, diag-level max
        {0x2C3, {0x22, 0x00, 0x00, 0x00, 0x50, 0xFF, 0x00}},  // (numbers and flags: 34 rows, 80 columns, ...)
        {0x2CA, {0x08, 'd', 'i', 's', 'k', ' ', 'n', 'e', 't', 0x00}},  // boot-device
        {0x44E, {0x03, 'n', 'e', 't'}},                                  // diag-device
        {0x5D3, {0xFF, 0x04, 'b', 'o', 'o', 't', 0x00}},                 // auto-boot? true, boot-command
        {0x616, {0x00, 0x00, 0x40, 0x00, 0x08, 'k', 'e', 'y', 'b', 'o', 'a', 'r', 'd', 0x00}},  // input-device
        {0x63C, {0x06, 's', 'c', 'r', 'e', 'e', 'n', 0x00}},                                  // output-device
        {0x65F, {0x05, '1', ',', '2', ',', '3', 0x00}},                                       // pcib-probe-list
        {0x671, {0x07, '1', ',', '2', ',', '3', ',', '4', 0x00}},                             // pcia-probe-list
        {0x683, {0x0C, '9', '6', '0', '0', ',', '8', ',', 'n', ',', '1', ',', '-', 0x00}},    // ttyb-mode
        {0x695, {0x0C, '9', '6', '0', '0', ',', '8', ',', 'n', ',', '1', ',', '-', 0x00}},    // ttya-mode
        {0x6A7, {0xFF, 0x00, 0xFF}},  // (the serial ports' flags)
        {0x832, {0x07, 0xFF}},
    };
    for (const auto& d : kDefaults) std::copy(d.bytes.begin(), d.bytes.end(), p + d.at);
    // The starting point, as setenv leaves it: boot from the disk (boot
    // cdrom from ok installs; the rest of the old value stays behind the 0).
    static const char kBootDevice[] = "disk";
    p[0x2CA] = sizeof kBootDevice - 1;
    std::copy(kBootDevice, kBootDevice + sizeof kBootDevice, p + 0x2CB);
    p[1] = 0x00;  // diag-switch? false
    p[2] = 0x20;  // diag-level min

    std::array<u8, 6> mac{0x08, 0x00, 0x20, 0, 0, 0};
    if (opt_.mac) {
        mac = *opt_.mac;
    } else {
        std::random_device rd;
        for (int i = 3; i < 6; ++i) mac[static_cast<std::size_t>(i)] = static_cast<u8>(rd());
    }
    const u32 serial = static_cast<u32>(mac[3]) << 16 | static_cast<u32>(mac[4]) << 8 | mac[5];

    // What the factory programs and the defaults leave alone, as a real Ultra 10
    // has them: a power-cycle count from 0 (powerd will not shut an idle
    // machine down once it is high for the board's age),
    // system-board-date (YYYYMMDD) and system-board-serial#, at 0x50, and
    // the same date in the IDPROM.
    std::fill(p + 0x08, p + 0x0C, u8{0});
    unsigned at = 0x50;
    for (const std::string& v : {std::string(kBoardDate), "WSS200#" + std::to_string(10000 + serial % 90000)}) {
        p[at] = static_cast<u8>(v.size());
        std::copy(v.begin(), v.end(), p + at + 1);
        p[at + 1 + v.size()] = 0;
        at += static_cast<unsigned>(v.size()) + 2;
    }
    u8 sum = 0x17;
    for (unsigned i = 0x0C; i < kNvramConfigEnd; ++i) sum ^= p[i];
    p[0] = sum;
    nvram_.write_idprom(kMachineType, mac.data(), kBoardDateBcd, serial);
}

void Ultra5_10::set_timing(Pacer::Mode mode) {
    opt_.timing = mode;
    pacer_.set_mode(mode);
    cpu_.set_delay_cycles_per_step(pacer_.delay_cycles_per_instruction());
    apply_fast_io();
}

void Ultra5_10::set_fast_disk(bool fast) {
    opt_.fast_disk = fast;
    apply_fast_io();
}

void Ultra5_10::apply_fast_io() {
    const bool fast = opt_.fast_disk || opt_.timing == Pacer::Mode::Max;
    for (unsigned ch = 0; ch < 2; ++ch) ide_.channel(ch).set_fast(fast);
    if (disk_) disk_->set_fast(fast);
    cdrom_.set_fast(fast);
    hme_.set_fast(opt_.timing == Pacer::Mode::Max);  // (the network only with Max: no setting of its own)
}

bool Ultra5_10::insert_cdrom(const std::string& path) {
    cdrom_.insert(nullptr);
    cd_image_.close();
    if (path.empty()) return true;
    if (!cd_image_.open(path, true)) return false;
    cdrom_.insert(&cd_image_);
    return true;
}

void Ultra5_10::update_prom_mapping() {
    if (prom_map_size_) {
        sabre_.map_mem_window(prom_pa_, prom_map_size_);
        prom_map_size_ = 0;
    }
    const u32 bar0 = ebus_.bar_address(0);
    const bool decoded = (ebus_.command() & pci::kCmdMemory) && (simba_a_.command() & pci::kCmdMemory) &&
                         ((simba_a_.config_read(Simba::kMemMap, 1) >> (bar0 >> 29)) & 1);
    std::vector<u8>& image = flash_.image();
    if (!decoded || !flash_.array_mode() || image.empty() || (image.size() & (Bus::kPageSize - 1))) return;
    prom_pa_ = Sabre::kMemBase + bar0;
    prom_map_size_ = image.size();
    bus_.map_ram(prom_pa_, prom_map_size_, image.data(), false);
}

void Ultra5_10::post_key(u8 code, bool down) {
    std::lock_guard<std::mutex> lock(input_mutex_);
    input_.push_back({InputEvent::Key, code, down, 0, 0, 0});
    input_waiting_.store(true, std::memory_order_release);
}

void Ultra5_10::post_release_keys() {
    std::lock_guard<std::mutex> lock(input_mutex_);
    input_.push_back({InputEvent::ReleaseKeys, 0, false, 0, 0, 0});
    input_waiting_.store(true, std::memory_order_release);
}

void Ultra5_10::post_mouse(int dx, int dy, unsigned buttons) {
    std::lock_guard<std::mutex> lock(input_mutex_);
    input_.push_back({InputEvent::Mouse, 0, false, dx, dy, buttons});
    input_waiting_.store(true, std::memory_order_release);
}

void Ultra5_10::post_keyboard_plugged(bool plugged) {
    std::lock_guard<std::mutex> lock(input_mutex_);
    input_.push_back({InputEvent::Plug, 0, plugged, 0, 0, 0});
    input_waiting_.store(true, std::memory_order_release);
}

void Ultra5_10::take_input() {
    std::vector<InputEvent> events;
    {
        std::lock_guard<std::mutex> lock(input_mutex_);
        events.swap(input_);
        input_waiting_.store(false, std::memory_order_relaxed);
    }
    if (!keyboard_) return;
    for (const InputEvent& e : events) {
        switch (e.kind) {
            case InputEvent::Key: keyboard_->key(e.code, e.down); break;
            case InputEvent::ReleaseKeys: keyboard_->release_all(); break;
            case InputEvent::Mouse:
                if (!keyboard_->plugged()) break;  // (the mouse plugs into the keyboard)
                mouse_->move(e.dx, e.dy);
                mouse_->set_buttons(e.buttons);
                break;
            case InputEvent::Plug:
                keyboard_->set_plugged(e.down);
                poll_serial();
                break;
        }
    }
}

void Ultra5_10::poll_serial() {
    su_kbd_.poll_line();
    su_mouse_.poll_line();
    se_.poll_lines();
}

// Guest time in PCI clocks (what the APB's tick register counts): polled
// often, so a multiply by a fixed-point ratio.
u64 Ultra5_10::pci_ticks() const {
    u64 hi, lo;
    mul_u64(cpu_.cycles(), pci_ticks_fx_, hi, lo);
    return hi;
}

// Whole seconds of guest time (for the TOD clock, which Solaris polls):
// by the reciprocal, then put right.
u64 Ultra5_10::guest_seconds() const {
    const u64 c = cpu_.cycles();
    u64 q, lo;
    mul_u64(c, per_second_fx_, q, lo);
    while (c - q * hz_ >= hz_) ++q;
    return q;
}

void Ultra5_10::run(u64 instructions) {
    const u64 end = cpu_.instructions() + instructions;
    const u64 pace_every = static_cast<u64>(opt_.cpu_hz / 1000);  // a millisecond of guest time
    while (cpu_.instructions() < end && !cpu_.halted() && !switched_off_) {
        const u64 next = sched_.next_event();
        u64 n = cpu_.steps_until(next);
        n = std::min<u64>({n, end - cpu_.instructions(), 4096});
        cpu_.run(n);
        if (switched_off_) break;
        if (soft_reset_ != SoftReset::None) {
            const SoftReset r = soft_reset_;
            soft_reset_ = SoftReset::None;
            if (r == SoftReset::Por) reset(false);
            else cpu_.externally_initiated_reset();
        }
        // An idle loop: round it at once up to the next event (with the
        // pacer, at most a millisecond of guest time, for input and frames
        // from the host to be taken that often; without it, within the
        // instructions asked for).
        if (cpu_.idle_period() && !input_waiting_.load(std::memory_order_relaxed)) {
            const bool free = opt_.timing == Pacer::Mode::Free;
            const u64 event = sched_.next_event();
            cpu_.skip_idle(free ? event : std::min(event, cpu_.cycles() + pace_every),
                           free ? end - std::min(end, cpu_.instructions()) : ~u64{0});
        }
        const u64 now = cpu_.cycles();
        sched_.run_due(now);
        hme_.poll();  // frames from the host
        if (input_waiting_.load(std::memory_order_acquire)) take_input();
        if (mouse_) {
            mouse_->poll();
            su_kbd_.poll_line();
            su_mouse_.poll_line();
        }
        // Guest time to the wall clock: the pacer waits while the guest is
        // ahead, and sets the time each step is charged from what the host
        // manages. Delay loops and idle loops run at once take next to no
        // host time for the guest time they are charged: to the pacer, that
        // time is idle.
        if (opt_.timing != Pacer::Mode::Free && now - paced_at_ >= pace_every) {
            paced_at_ = now;
            cpu_.set_cycles_per_step(pacer_.update(now,
                                                   cpu_.instructions() - cpu_.delay_steps() - cpu_.idle_instructions(),
                                                   false, cpu_.delay_cycles() + cpu_.idle_cycles()));
        }
    }
}

std::string Ultra5_10::describe() const {
    static const char* const kMode[] = {"cycle", "realtime", "max", "free"};
    std::string out;
    const double hz = opt_.cpu_hz;
    appendf(out, "machine: %.0f MHz, %u MB, timing %s, %.4f cycles a step, %.3f s behind the wall clock\n", hz / 1e6,
            opt_.ram_mb, kMode[static_cast<int>(pacer_.mode())], pacer_.cycles_per_instruction(), pacer_.lag());
    const u64 next = sched_.next_event(), now = sched_.now();
    if (next == Scheduler::kNever)
        appendf(out, "events: none pending\n");
    else
        appendf(out, "events: next in %.6f s\n", next > now ? static_cast<double>(next - now) / hz : 0.0);
    appendf(out, "activity: disk %llu, cd-rom %llu, network %llu\n", static_cast<unsigned long long>(disk_image_.accesses()),
            static_cast<unsigned long long>(cd_image_.accesses()), static_cast<unsigned long long>(hme_.frames()));
    cpu_.describe(out, hz);
    sabre_.describe(out);
    ide_.describe(out);
    return out;
}

}  // namespace uv
