// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
// Sun Ultra 5 / Ultra 10 ("Darwin", SUNW,Ultra-5_10): an UltraSPARC IIi
// whose PCI bus module leads to an Advanced PCI Bridge with two buses. Bus
// A carries the on-board devices: the PCIO (RIO) EBus at device 1, with
// the flash PROM, the M48T59 NVRAM and clock, the SuperIO (keyboard and
// mouse UARTs, floppy, parallel), the SAB82532 serial ports, audio and the
// auxio, power and clock-control registers. Bus B carries the slots.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "core/host_memory.h"
#include "core/scheduler.h"
#include "cpu/ultrasparc/ultrasparc.h"
#include "devices/ebus_misc.h"
#include "devices/fdc82077.h"
#include "devices/m48t59.h"
#include "devices/ns16550.h"
#include "devices/rio_ebus.h"
#include "devices/sab82532.h"
#include "devices/sabre.h"
#include "devices/serial.h"
#include "devices/simba.h"
#include "mem/bus.h"

namespace uv {

class Ultra5_10 {
public:
    // Memory: four banks of up to 256 MiB, at physical 0, 0x1000_0000,
    // 0x2000_0000 and 0x3000_0000. A pair of DIMMs fills bank 0 or 2 (and
    // with double-sided DIMMs, bank 1 or 3 as well). Address bits above a
    // bank's size are not decoded, so a smaller bank repeats through its
    // window; an empty bank reads as zero.
    static constexpr u64 kBankWindow = 256ull << 20;
    static constexpr unsigned kBanks = 4;

    struct Options {
        u32 ram_mb = 256;  // 32 to 1024; each bank's share is a power of two
        double cpu_hz = 440e6;
        u32 ecache_bytes = 2u << 20;
        std::vector<u8> prom;    // the flash image (1 MiB: OpenBoot, then POST)
        std::string nvram_path;  // NVRAM contents, loaded at start and saved on stop
        u8 mac[6] = {0x08, 0x00, 0x20, 0x55, 0x56, 0x01};
    };

    // On-board interrupt numbers (INO) of the EBus devices.
    static constexpr unsigned kInoParallel = 0x22, kInoAudioRecord = 0x23, kInoAudioPlay = 0x24, kInoPower = 0x25,
                              kInoFloppy = 0x27, kInoKeyboard = 0x29, kInoMouse = 0x2A, kInoSerial = 0x2B;

    explicit Ultra5_10(const Options& options);
    ~Ultra5_10();

    void reset();
    // Runs up to `instructions` instructions (stops early if the CPU halts).
    void run(u64 instructions);
    void save_nvram();

    usparc::UltraSparc& cpu() { return cpu_; }
    Bus& bus() { return bus_; }
    Scheduler& scheduler() { return sched_; }
    Sabre& sabre() { return sabre_; }
    M48t59& nvram() { return nvram_; }

    // Serial lines: ttya and ttyb (the SAB82532), and the keyboard and mouse
    // ports (SuperIO UARTs).
    SerialLine& ttya() { return ttya_; }
    SerialLine& ttyb() { return ttyb_; }
    SerialLine& keyboard_line() { return kbd_line_; }
    SerialLine& mouse_line() { return mouse_line_; }
    // Call after delivering bytes to any of the lines.
    void poll_serial();

private:
    // Maps the flash image straight onto the processor's bus while reads of
    // it return the array and the path to it (Simba A, the EBus BAR0) is
    // decoded, so instruction fetches skip the PCI path.
    void update_prom_mapping();
    u64 prom_pa_ = 0, prom_map_size_ = 0;

    Options opt_;
    Bus bus_;
    Scheduler sched_;
    HostMemory ram_;
    usparc::UltraSparc cpu_;
    Sabre sabre_;
    Simba simba_b_, simba_a_;
    RioEbus ebus_;
    SerialLine ttya_, ttyb_, kbd_line_, mouse_line_;
    FlashProm flash_;
    M48t59 nvram_;
    SuperIoConfig superio_;
    Ns16550 su_kbd_, su_mouse_;
    Sab82532 se_;
    Fdc82077 fdc_;
    std::vector<std::unique_ptr<RegisterBlock>> blocks_;
    // An empty memory bank: reads as zero, writes are lost (no bus error).
    class EmptyBank final : public Device {
    public:
        const char* name() const override { return "empty-bank"; }
        bool read(u64, unsigned, u64& value) override {
            value = 0;
            return true;
        }
        bool write(u64, unsigned, u64) override { return true; }
    } empty_bank_;
};

}  // namespace uv
