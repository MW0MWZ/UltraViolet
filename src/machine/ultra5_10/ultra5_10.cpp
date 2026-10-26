// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
#include "machine/ultra5_10/ultra5_10.h"

#include <algorithm>

#include "core/log.h"

namespace uv {

namespace {

usparc::UltraSparc::Options cpu_options(const Ultra5_10::Options& o) {
    usparc::UltraSparc::Options c;
    c.clock_hz = o.cpu_hz;
    c.ecache_bytes = o.ecache_bytes;
    return c;
}

// EBus device space (BAR1) offsets, as the Ultra 5/10 OpenBoot device tree
// places them.
constexpr u32 kEbusNvram = 0x000000, kEbusAudio = 0x200000, kEbusSuperIo = 0x300000, kEbusFloppy = 0x3023F0,
              kEbusParallel = 0x3043BC, kEbusMouse = 0x3062F8, kEbusKeyboard = 0x3083F8, kEbusSerial = 0x400000,
              kEbusSmartCard = 0x500000, kEbusPll = 0x504000, kEbusDma = 0x700000, kEbusPower = 0x724000,
              kEbusAuxio = 0x726000;

constexpr double kPciHz = 66.0e6;      // the APB's primary bus clock, which its tick register counts
constexpr double kSuperIoUartHz = 1843200.0;
constexpr u8 kSuperIoSid = 0xD0;       // SuperIO identification register value
constexpr u8 kMachineType = 0x80;      // IDPROM machine type: sun4u

}  // namespace

Ultra5_10::Ultra5_10(const Options& options)
    : opt_(options),
      ram_(static_cast<std::size_t>(options.ram_mb) << 20),
      cpu_(bus_, cpu_options(options)),
      sabre_(bus_, sched_, options.cpu_hz, [this](u64 inr) { return cpu_.deliver_interrupt_vector(inr, 0, 0); }),
      simba_b_(false, [this] { return static_cast<u64>(static_cast<double>(cpu_.cycles()) * kPciHz / opt_.cpu_hz); }),
      simba_a_(true, [this] { return static_cast<u64>(static_cast<double>(cpu_.cycles()) * kPciHz / opt_.cpu_hz); }),
      flash_(options.prom, 0x01, 0xD5, 0x10000),
      superio_(kSuperIoSid),
      su_kbd_(sched_, kbd_line_, kSuperIoUartHz, options.cpu_hz),
      su_mouse_(sched_, mouse_line_, kSuperIoUartHz, options.cpu_hz),
      se_(sched_, ttya_, ttyb_, options.cpu_hz),
      fdc_(sched_) {
    sched_.set_clock([this] { return cpu_.cycles(); });

    bus_.add_host_region(ram_.get(), ram_.size());
    u64 placed = 0;
    static constexpr unsigned kFillOrder[kBanks] = {0, 2, 1, 3};
    for (unsigned b : kFillOrder) {
        const u64 window = b * kBankWindow;
        u64 size = std::min<u64>(ram_.size() - placed, kBankWindow);
        while (size & (size - 1)) size &= size - 1;  // a bank is a power of two
        if (size == 0) {
            bus_.map_device(window, kBankWindow, &empty_bank_);
            continue;
        }
        for (u64 off = 0; off < kBankWindow; off += size) bus_.map_ram(window + off, size, ram_.get() + placed);
        placed += size;
    }
    if (placed < ram_.size())
        UV_LOG(Warn, "ultra5_10", "%llu MiB of memory left unused (banks are powers of two)",
               static_cast<unsigned long long>((ram_.size() - placed) >> 20));
    sabre_.map();

    // PCI topology: the APB's function 0 is bus B, function 1 bus A.
    sabre_.root().attach(1, 0, &simba_b_);
    sabre_.root().attach(1, 1, &simba_a_);
    simba_a_.secondary().attach(1, 0, &ebus_);

    // EBus devices
    ebus_.set_prom(&flash_, static_cast<u32>(std::max<std::size_t>(opt_.prom.size(), 1)));
    ebus_.add(kEbusNvram, M48t59::kSize, &nvram_);
    ebus_.add(kEbusSuperIo + 0x398, 2, &superio_);
    ebus_.add(kEbusKeyboard, 8, &su_kbd_);
    ebus_.add(kEbusMouse, 8, &su_mouse_);
    ebus_.add(kEbusSerial, 0x80, &se_);
    auto block = [this](const char* name, u32 offset, u32 size, u8 fill = 0) {
        blocks_.push_back(std::make_unique<RegisterBlock>(name, size, fill));
        ebus_.add(offset, size, blocks_.back().get());
        return blocks_.back().get();
    };
    block("cs4231", kEbusAudio, 0x100);
    ebus_.add(kEbusFloppy, 8, &fdc_);
    block("ecpp", kEbusParallel, 8);
    block("smartcard", kEbusSmartCard, 0x100);
    block("pll", kEbusPll, 0x100);
    block("ebus-dma", kEbusDma, 0x20000);
    block("power", kEbusPower, 0x100);
    block("auxio", kEbusAuxio, 0x100);

    su_kbd_.set_irq_sink([this](bool a) { sabre_.set_interrupt(kInoKeyboard, a); });
    su_mouse_.set_irq_sink([this](bool a) { sabre_.set_interrupt(kInoMouse, a); });
    se_.set_irq_sink([this](bool a) { sabre_.set_interrupt(kInoSerial, a); });
    fdc_.set_irq_sink([this](bool a) { sabre_.set_interrupt(kInoFloppy, a); });

    // NVRAM: the saved contents, or a fresh one with a valid IDPROM.
    if (opt_.nvram_path.empty() || !nvram_.load(opt_.nvram_path)) nvram_.write_idprom(kMachineType, opt_.mac, 0x000001);

    bus_.set_rom_write_handler([this](u64 pa, unsigned size, u64 value) {
        if (prom_map_size_ && pa - prom_pa_ < prom_map_size_) flash_.write(pa - prom_pa_, size, value);
    });
    flash_.on_mode_change = [this] { update_prom_mapping(); };
    ebus_.on_config_write = [this] { update_prom_mapping(); };
    simba_a_.on_config_write = [this] { update_prom_mapping(); };
    update_prom_mapping();

    sabre_.on_soft_reset = [this](bool por) {
        if (por) reset();
        else cpu_.externally_initiated_reset();
    };
}

Ultra5_10::~Ultra5_10() { save_nvram(); }

void Ultra5_10::save_nvram() {
    if (!opt_.nvram_path.empty() && nvram_.dirty()) nvram_.save(opt_.nvram_path);
}

void Ultra5_10::reset() {
    sabre_.reset(true);
    update_prom_mapping();
    superio_.reset();
    su_kbd_.reset();
    su_mouse_.reset();
    se_.reset();
    fdc_.reset();
    cpu_.reset();
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

void Ultra5_10::poll_serial() {
    su_kbd_.poll_line();
    su_mouse_.poll_line();
    se_.poll_lines();
}

void Ultra5_10::run(u64 instructions) {
    const u64 end = cpu_.instructions() + instructions;
    while (cpu_.instructions() < end && !cpu_.halted()) {
        const u64 now = cpu_.cycles();
        const u64 next = sched_.next_event();
        u64 n = next > now ? next - now : 1;
        n = std::min<u64>({n, end - cpu_.instructions(), 4096});
        cpu_.run(n);
        sched_.run_due(cpu_.cycles());
    }
}

}  // namespace uv
