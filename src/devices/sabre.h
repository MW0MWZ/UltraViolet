// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
// The UltraSPARC IIi's PCI bus module ("Sabre", pci108e,a000), as the
// processor's physical address space sees it:
//   0x1FE.0000.0000  control registers: IOMMU, interrupt mapping and
//                    clearing, timers, PCI control and error status,
//                    memory control, reset control
//   0x1FE.0100.0000  PCI configuration space (bus << 16 | device << 11 |
//                    function << 8 | register)
//   0x1FE.0200.0000  PCI I/O space (16 MiB)
//   0x1FF.0000.0000  PCI memory space (4 GiB)
// PCI is little-endian: values crossing to it are byte-swapped, so a
// big-endian access sees a register's bytes in address order and a
// little-endian ASI sees its value. Interrupts from PCI slots and on-board
// devices are mapped through the IMAP registers into interrupt vectors
// (INR = IGN << 6 | INO) sent to the processor, and held until software
// returns their clear register to idle.
#pragma once

#include <array>
#include <functional>
#include <map>

#include "core/scheduler.h"
#include "devices/pci.h"
#include "mem/bus.h"

namespace uv {

class Sabre {
public:
    static constexpr u64 kRegsBase = 0x1FE00000000ull, kConfigBase = 0x1FE01000000ull, kIoBase = 0x1FE02000000ull,
                         kMemBase = 0x1FF00000000ull;
    static constexpr unsigned kIgn = 0x1F;  // the interrupt group number of the IIi's bus module
    static constexpr unsigned kNumIno = 0x40;

    // `send_vector`: delivers an interrupt vector (its first data word) to
    // the processor; false if its receive register is busy.
    Sabre(Bus& bus, Scheduler& scheduler, double cpu_hz, std::function<bool(u64)> send_vector);
    ~Sabre();

    void map();
    // Returns [pa, pa + size) to the PCI memory window (after a fast path
    // mapped it elsewhere).
    void map_mem_window(u64 pa, u64 size);
    void reset(bool power_on);
    pci::Bus& root() { return root_; }

    // Interrupt input `ino` (0x00-0x1F: PCI slots, slot << 2 | INTx; 0x20 and
    // up: on-board devices) at level `asserted`.
    void set_interrupt(unsigned ino, bool asserted);

    // Bus-master DMA from PCI: DVMA address through the IOMMU to memory.
    bool dma_read(u32 dvma, u8* data, std::size_t length);
    bool dma_write(u32 dvma, const u8* data, std::size_t length);
    // The physical address for a DVMA address (false: no valid translation).
    bool translate(u32 dvma, bool write, u64& pa) const;

    // Called for a software power-on or externally initiated reset written
    // to the reset control register.
    std::function<void(bool por)> on_soft_reset;

    u64 reg(u32 offset) const;

private:
    class Window;
    class HostFunction;

    bool regs_read(u64 offset, unsigned size, u64& value);
    bool regs_write(u64 offset, unsigned size, u64 value);
    bool config_read(u64 offset, unsigned size, u64& value);
    bool config_write(u64 offset, unsigned size, u64 value);
    bool io_read(u64 offset, unsigned size, u64& value);
    bool io_write(u64 offset, unsigned size, u64 value);
    bool mem_read(u64 offset, unsigned size, u64& value);
    bool mem_write(u64 offset, unsigned size, u64 value);
    void pio_error(u64 bit);

    u64& imap(unsigned ino);
    void dispatch(unsigned ino);
    void dispatch_all();
    void timer_update(unsigned n);

    Bus& bus_;
    Scheduler& sched_;
    double cpu_hz_;
    std::function<bool(u64)> send_vector_;
    pci::Bus root_;
    std::unique_ptr<HostFunction> host_;
    std::unique_ptr<Window> regs_win_, config_win_, io_win_, mem_win_;

    std::map<u32, u64> regs_;               // registers without behaviour of their own
    std::array<u64, 8> pci_imap_{};         // one per slot: four INOs each
    std::array<u64, 32> obio_imap_{};       // INO 0x20 + n
    std::array<u8, kNumIno> state_{};       // clear register state: 0 idle, 1 received, 3 pending
    std::array<bool, kNumIno> level_{};
    Timer retry_timer_;
    u64 iommu_ctl_ = 0, iommu_tsb_ = 0;
    u64 pci_ctl_ = 0, pci_afsr_ = 0, pci_afar_ = 0, pci_diag_ = 0, pci_tasr_ = 0;
    u64 reset_ctl_ = 0;
    // Timer/counters: 29-bit counts at 1 MHz; the limit register holds the
    // interrupt enable (bit 31), clear-at-limit (bit 30), restart (bit 29)
    // and the limit.
    struct TimerCounter {
        u64 base_cycle = 0;  // cycle at which the count was last zero
        u64 limit = 0;
    };
    u64 tc_count(unsigned n) const;
    std::array<TimerCounter, 2> tc_{};
    std::array<std::unique_ptr<Timer>, 2> tc_timer_;
};

}  // namespace uv
