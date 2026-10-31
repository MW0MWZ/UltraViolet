// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
// Sun Ultra 5 / Ultra 10 ("Darwin", SUNW,Ultra-5_10): an UltraSPARC IIi
// whose PCI bus module leads to an Advanced PCI Bridge with two buses. Bus
// A carries the on-board devices: the PCIO (RIO) EBus at device 1, with
// the flash PROM, the M48T59 NVRAM and clock, the SuperIO (keyboard and
// mouse UARTs, floppy, parallel), the SAB82532 serial ports, audio and the
// auxio, power and clock-control registers; and the CMD646 IDE controller
// at device 3, with the disk on the primary channel and the CD-ROM drive
// as master of the secondary. Bus B carries the slots.
#pragma once

#include <array>
#include <atomic>
#include <ctime>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "core/host_memory.h"
#include "core/pacer.h"
#include "core/scheduler.h"
#include "cpu/ultrasparc/ultrasparc.h"
#include "devices/atapi_cdrom.h"
#include "devices/cmd646.h"
#include "devices/cs4231.h"
#include "devices/ebus_dma.h"
#include "devices/ebus_misc.h"
#include "devices/fdc82077.h"
#include "devices/hme.h"
#include "devices/mach64.h"
#include "devices/sun_keyboard.h"
#include "devices/m48t59.h"
#include "devices/ns16550.h"
#include "devices/rio_ebus.h"
#include "devices/sab82532.h"
#include "devices/sabre.h"
#include "devices/serial.h"
#include "devices/simba.h"
#include "mem/bus.h"
#include "net/net_backend.h"
#include "storage/disk_image.h"

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
        // The Ethernet address a new NVRAM gets (none: a random Sun address,
        // 08:00:20:xx:xx:xx, the host ID's serial the same last three bytes).
        std::optional<std::array<u8, 6>> mac;
        std::string disk_path;   // ATA disk image (primary master); empty: none
        u64 disk_create_bytes = 0;  // create the disk image at this size if it does not exist
        std::string cdrom_path;  // CD image in the drive (secondary master); empty: no disc
        // The time of day at power-on, in seconds since 1970 (0: the host's
        // clock at start). The clock then advances with the processor's
        // cycles, so it agrees with TICK and the other timers.
        std::time_t start_time = 0;
        // How guest time relates to the host's clock (core/pacer.h): Free
        // charges one cycle per instruction and never waits (reproducible
        // runs); Realtime keeps guest time to the wall clock, the processor
        // as fast as the host allows.
        Pacer::Mode timing = Pacer::Mode::Free;
        // The IDE drives act at once and DMA runs at host speed, rather than
        // with a real drive's command delays and the controller's transfer
        // rate (always so with Max timing).
        bool fast_disk = false;
        // The host side of the on-board Ethernet (HME); none: no cable.
        std::shared_ptr<net::NetBackend> network;
        // A Sun Type 5 keyboard and mouse on the keyboard port. Without
        // them OpenBoot takes ttya for input and output; with them, and a
        // display, the screen is the console.
        bool keyboard = false;
        u8 keyboard_layout = SunKeyboard::kLayoutUs;
        // The host clock guest time keeps to (default: the system's), as
        // the pacer takes it.
        Pacer::HostClock host_clock;
    };

    // On-board interrupt numbers (INO) of the EBus devices. The audio's
    // first interrupt (capture) is 0x23, its second (playback) 0x24, as the
    // Linux and OpenBSD drivers take them.
    static constexpr unsigned kInoParallel = 0x22, kInoAudioRecord = 0x23, kInoAudioPlay = 0x24, kInoPower = 0x25,
                              kInoFloppy = 0x27, kInoKeyboard = 0x29, kInoMouse = 0x2A, kInoSerial = 0x2B;
    // INTA of the bus A devices, from the Simba's interrupt-map: RIO (device 1,
    // its HME function), the Rage Pro (device 2) and the IDE controller (device 3).
    static constexpr unsigned kInoRio = 0x21, kInoGraphics = 0x0F, kInoIde = 0x20;

    explicit Ultra5_10(const Options& options);
    ~Ultra5_10();

    void reset();
    // Runs up to `instructions` instructions (stops early if the CPU halts
    // or the system switches itself off).
    void run(u64 instructions);
    // The system has switched itself off (the power control register): it
    // runs no further until reset.
    bool switched_off() const { return switched_off_; }
    void save_nvram();

    usparc::UltraSparc& cpu() { return cpu_; }
    Bus& bus() { return bus_; }
    Scheduler& scheduler() { return sched_; }
    Sabre& sabre() { return sabre_; }
    M48t59& nvram() { return nvram_; }
    Cmd646& ide() { return ide_; }
    AtapiCdrom& cdrom() { return cdrom_; }
    AtaDisk* disk() { return disk_.get(); }
    // Changes the disc in the CD-ROM drive (empty path: no disc). False if
    // the image cannot be opened.
    bool insert_cdrom(const std::string& path);

    // Serial lines: ttya and ttyb (the SAB82532), and the keyboard and mouse
    // ports (SuperIO UARTs).
    SerialLine& ttya() { return ttya_; }
    SerialLine& ttyb() { return ttyb_; }
    SerialLine& keyboard_line() { return kbd_line_; }
    SerialLine& mouse_line() { return mouse_line_; }
    // Call after delivering bytes to any of the lines.
    void poll_serial();

    // Keyboard and mouse input (any thread; taken at the next run slice):
    // a Sun key number going down or up, every key up, mouse movement (x
    // right, y down) and buttons (left 1, middle 2, right 4).
    void post_key(u8 code, bool down);
    void post_release_keys();
    void post_mouse(int dx, int dy, unsigned buttons);
    // The keyboard (and mouse, which plugs into it) unplugged or plugged in.
    void post_keyboard_plugged(bool plugged);
    // All mouse movement posted has reached the guest's mouse port (none
    // waiting to be taken, gathered or sent down the line).
    bool mouse_idle() const { return !input_waiting_.load(std::memory_order_acquire) && (!mouse_ || mouse_->idle()); }
    SunKeyboard* keyboard() { return keyboard_.get(); }

    // For the window: how busy the disk, CD-ROM and network have been, the
    // pacer (speed, lag), changing the timing while running (turbo), and
    // the disc in the CD-ROM drive.
    struct Activity {
        u64 disk = 0, cdrom = 0, net = 0;
    };
    Activity activity() const { return {disk_image_.accesses(), cd_image_.accesses(), hme_.frames()}; }
    const Pacer& pacer() const { return pacer_; }
    void set_timing(Pacer::Mode mode);
    void set_fast_disk(bool fast);
    std::string disc() const { return cd_image_.is_open() ? cd_image_.path() : std::string(); }
    // A diagnostic report: the timing, the CPU, pending interrupts and the
    // IDE controller, as they are now (for a guest that has stopped making
    // progress). Call with the machine stopped.
    std::string describe() const;
    Mach64& graphics() { return mach64_; }
    Cs4231& audio() { return audio_; }
    // Where the audio's playback goes (16-bit stereo frames at a rate).
    void set_audio_sink(Cs4231::AudioSink sink) { audio_.set_audio_sink(std::move(sink)); }
    // A press of the front-panel power button: its interrupt, which asks
    // the system to shut down (Solaris, Linux and OpenBSD switch off).
    void press_power_button();
    // The system LED in the auxio register (on: the system has started).
    bool system_led() const { return auxio_led_ && (auxio_led_->data()[0] & 1); }

private:
    // Maps the flash image straight onto the processor's bus while reads of
    // it return the array and the path to it (Simba A, the EBus BAR0) is
    // decoded, so instruction fetches skip the PCI path.
    void update_prom_mapping();
    void apply_fast_io();
    bool nvram_blank() const;
    void write_new_nvram();
    u64 prom_pa_ = 0, prom_map_size_ = 0;

    Options opt_;
    Pacer pacer_;
    u64 paced_at_ = 0;  // guest time of the last pacer update
    enum class SoftReset { None, Por, Xir } soft_reset_ = SoftReset::None;  // asked for, to come between instructions
    bool switched_off_ = false;
    u64 pci_ticks_fx_ = 0;  // PCI clocks per CPU cycle, as a 0.64 fraction
    u64 pci_ticks() const;
    u64 hz_ = 0, per_second_fx_ = 0;  // the CPU clock, and 2^64 / it
    u64 guest_seconds() const;
    Bus bus_;
    Scheduler sched_;
    HostMemory ram_;
    usparc::UltraSparc cpu_;
    Sabre sabre_;
    Simba simba_b_, simba_a_;
    RioEbus ebus_;
    SerialLine ttya_, ttyb_, kbd_line_, mouse_line_;
    FlashProm flash_;
    std::time_t start_time_ = 0;
    M48t59 nvram_;
    SuperIoConfig superio_;
    Ns16550 su_kbd_, su_mouse_;
    Sab82532 se_;
    Fdc82077 fdc_;
    // EBus DMA channels: parallel port, audio playback and capture, floppy.
    EbusDma ecpp_dma_{"ecpp-dma"}, audio_play_dma_{"audio-play-dma"}, audio_rec_dma_{"audio-rec-dma"}, fd_dma_{"fd-dma"};
    Cmd646 ide_;
    Hme hme_;
    Mach64 mach64_;
    std::unique_ptr<SunKeyboard> keyboard_;
    std::unique_ptr<SunMouse> mouse_;
    // Input from the host, waiting for the emulation thread.
    struct InputEvent {
        enum Kind : u8 { Key, ReleaseKeys, Mouse, Plug } kind;
        u8 code;
        bool down;
        int dx, dy;
        unsigned buttons;
    };
    void take_input();
    std::mutex input_mutex_;
    std::vector<InputEvent> input_;
    std::atomic<bool> input_waiting_{false};
    DiskImage disk_image_, cd_image_;
    std::unique_ptr<AtaDisk> disk_;
    AtapiCdrom cdrom_;
    std::vector<std::unique_ptr<RegisterBlock>> blocks_;
    RegisterBlock* auxio_led_ = nullptr;
    PowerControl power_;
    Cs4231 audio_;
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
