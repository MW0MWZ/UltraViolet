// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
#include <algorithm>
#include <cstring>
#include <ctime>
#include <string>
#include <tuple>
#include <vector>

#include "core/endian.h"
#include "devices/ebus_dma.h"
#include "devices/fdc82077.h"
#include "devices/m48t59.h"
#include "devices/ns16550.h"
#include "devices/pci.h"
#include "devices/sab82532.h"
#include "devices/sun_keyboard.h"
#include "machine/ultra5_10/ultra5_10.h"
#include "sparc_asm.h"
#include "test.h"

using namespace uv;

namespace {

// A machine with a flash image whose first bytes are recognisable.
Ultra5_10::Options small_options() {
    Ultra5_10::Options o;
    o.ram_mb = 64;
    o.prom.assign(1u << 20, 0);
    o.prom[0] = 0x4F;
    o.prom[1] = 0x42;
    // At the power-on reset vector (+0x20): ba . ; nop
    store_be<u32>(o.prom.data() + 0x20, 0x10800000);
    store_be<u32>(o.prom.data() + 0x24, 0x01000000);
    return o;
}

// PCI configuration address for the processor: bus, device, function, register.
constexpr u64 cfg(unsigned bus, unsigned dev, unsigned fn, unsigned reg) {
    return Sabre::kConfigBase | (bus << 16) | (dev << 11) | (fn << 8) | reg;
}

u32 cfg_read32(Ultra5_10& m, u64 pa) {
    u32 v = 0;
    m.bus().read(pa, v);
    return byteswap(v);  // PCI is little-endian: a big-endian access sees the bytes swapped
}

void cfg_write32(Ultra5_10& m, u64 pa, u32 v) { m.bus().write(pa, byteswap(v)); }

// A function whose BARs answer with who was reached: id, BAR and offset.
// Function 1 refuses reads at offset 0xFC (so the bus tries the others); if
// `special` is set, also answers I/O cycles there outside its BARs.
struct ProbeFunction : pci::Function {
    ProbeFunction(u8 id_, std::initializer_list<std::pair<u32, bool>> bars) : Function(0x1234, id_, 0, 0xFF0000), id(id_) {
        unsigned i = 0;
        for (auto [size, io] : bars) define_bar(i++, size, io);
    }
    const char* name() const override { return "probe"; }
    bool bar_read(unsigned bar, u32 offset, unsigned, u64& value) override {
        if (id == 1 && offset == 0xFC) return false;
        value = (u32{id} << 24) | (bar << 16) | offset;
        return true;
    }
    bool bar_write(unsigned bar, u32 offset, unsigned, u64 value) override {
        last_write = (u64{bar} << 32) | (offset << 8) | (value & 0xFF);
        return true;
    }
    Claim claim(u32 addr, bool io, Decoded& d) const override {
        if (io && special && (addr & ~3u) == special) return Claim::Special;
        return Function::claim(addr, io, d);
    }
    bool io_read(u32 addr, unsigned size, u32& value) override {
        if (special && addr == special) {
            value = 0x5EC1A1;
            return true;
        }
        return Function::io_read(addr, size, value);
    }
    void set_bar(unsigned bar, u32 base) { config_write(static_cast<u8>(pci::kBar0 + 4 * bar), 4, base); }
    void enable(u16 cmd) { config_write(pci::kCommand, 2, cmd); }
    u8 id;
    u32 special = 0;
    u64 last_write = 0;
};

struct ProbeBridge : pci::Bridge {
    ProbeBridge() : Bridge(0x1234, 0x99, 0) {}
    const char* name() const override { return "probe-bridge"; }
};

u64 bus_read(pci::Bus& b, u32 addr) {
    u64 v = 0;
    return b.mem_read(addr, 4, v) ? v : ~0ull;
}

u32 bus_in(pci::Bus& b, u32 addr) {
    u32 v = 0;
    return b.io_read(addr, 4, v) ? v : ~0u;
}

}  // namespace

TEST(machine_pci_topology_answers_configuration_cycles) {
    Ultra5_10 m(small_options());
    CHECK_EQ(cfg_read32(m, cfg(0, 0, 0, 0)), 0xA000108Eu);   // Sabre
    CHECK_EQ(cfg_read32(m, cfg(0, 1, 0, 0)), 0x5000108Eu);   // APB function 0 (bus B)
    CHECK_EQ(cfg_read32(m, cfg(0, 1, 1, 0)), 0x5000108Eu);   // APB function 1 (bus A)
    CHECK_EQ(cfg_read32(m, cfg(0, 2, 0, 0)), 0xFFFFFFFFu);   // nothing there
    // Number bus A as bus 1, then the EBus answers there at device 1.
    cfg_write32(m, cfg(0, 1, 1, 0x18), 0x00010100);           // primary 0, secondary 1, subordinate 1
    CHECK_EQ(cfg_read32(m, cfg(1, 1, 0, 0)), 0x1000108Eu);
    CHECK_EQ(cfg_read32(m, cfg(1, 1, 0, 0x14)), 0xF1000000u); // BAR1 at its power-on address
}

// Memory and I/O cycles reach the same function and offset however often
// they repeat, as configuration moves BARs, turns decoding off and on and
// moves bridge windows.
TEST(pci_decoding_follows_every_configuration_change) {
    pci::Bus root;
    ProbeFunction a(1, {{0x100, false}, {0x10, true}, {4, true}});
    ProbeFunction c(3, {{4, true}});
    ProbeBridge bridge;
    ProbeFunction b(2, {{0x1000, false}});
    root.attach(1, 0, &a);
    root.attach(2, 0, &bridge);
    root.attach(3, 0, &c);
    bridge.secondary().attach(0, 0, &b);
    a.set_bar(0, 0x80001000);
    a.set_bar(1, 0x400);
    a.set_bar(2, 0x414);
    c.set_bar(0, 0x410);  // the 4-byte I/O BARs of two functions share 16 bytes
    b.set_bar(0, 0x90000000);
    a.enable(pci::kCmdMemory | pci::kCmdIo);
    c.enable(pci::kCmdIo);
    b.enable(pci::kCmdMemory);
    for (int pass = 0; pass < 2; ++pass) {  // the second pass is served from the cache
        CHECK(bus_read(root, 0x80001010) == 0x01000010);
        CHECK(bus_read(root, 0x80001100) == ~0ull);
        CHECK(bus_in(root, 0x408) == 0x01010008);
        CHECK(bus_in(root, 0x410) == 0x03000000);
        CHECK(bus_in(root, 0x414) == 0x01020000);
        CHECK(bus_read(root, 0x90000010) == ~0ull);  // the bridge forwards nothing yet
    }
    // A BAR moved: the old address stops answering, the new one starts.
    a.set_bar(0, 0x80002000);
    CHECK(bus_read(root, 0x80001010) == ~0ull);
    CHECK(bus_read(root, 0x80002010) == 0x01000010);
    // Memory decoding off, then on again.
    a.enable(pci::kCmdIo);
    CHECK(bus_read(root, 0x80002010) == ~0ull);
    CHECK(bus_in(root, 0x408) == 0x01010008);
    a.enable(pci::kCmdMemory | pci::kCmdIo);
    CHECK(bus_read(root, 0x80002010) == 0x01000010);
    // A bridge window opened over the secondary BAR, then closed.
    bridge.config_write(pci::kMemBase, 4, 0x90009000);
    bridge.config_write(pci::kCommand, 2, pci::kCmdMemory);
    CHECK(bus_read(root, 0x90000010) == 0x02000010);
    CHECK(bus_read(root, 0x90000010) == 0x02000010);
    bridge.config_write(pci::kMemBase, 4, 0x90109010);
    CHECK(bus_read(root, 0x90000010) == ~0ull);
    bridge.config_write(pci::kMemBase, 4, 0x90009000);
    b.set_bar(0, 0x90004000);
    CHECK(bus_read(root, 0x90000010) == ~0ull);
    CHECK(bus_read(root, 0x90004010) == 0x02000010);
    // Writes reach the function the reads do.
    CHECK(root.mem_write(0x80002020, 1, 0x5A) && a.last_write == 0x205A);
    CHECK(root.io_write(0x410, 1, 0x77) && c.last_write == 0x77);
    // A refused cycle goes on to an overlapping function behind it.
    bridge.config_write(pci::kMemBase, 4, 0x80008000);
    b.set_bar(0, 0x80002000);
    for (int pass = 0; pass < 2; ++pass) {
        CHECK(bus_read(root, 0x80002010) == 0x01000010);
        CHECK(bus_read(root, 0x800020FC) == 0x020000FC);
    }
    // An address claimed outside the BARs is never served from the cache: a
    // BAR of c's covers it, but a answers first.
    c.set_bar(0, 0x500);
    CHECK(bus_in(root, 0x500) == 0x03000000);
    a.special = 0x500;
    a.enable(pci::kCmdMemory | pci::kCmdIo);  // a configuration write, as when it would be set up
    for (int pass = 0; pass < 2; ++pass) {
        CHECK(bus_in(root, 0x500) == 0x5EC1A1);
        CHECK(bus_in(root, 0x502) == 0x03000002);  // within its granule, a does not answer
    }
    // Reset restores power-on decoding.
    CHECK(bus_read(root, 0x80002010) == 0x01000010);
    root.reset();
    CHECK(bus_read(root, 0x80002010) == ~0ull);
}

// OpenBoot's reset-all stores the soft power-on reset bit to Sabre's reset
// control register: the reset comes after the store, between instructions,
// and the instruction after the store never runs (it used to run in the
// reset state, which took OpenBoot to error_state).
// OpenBoot's power-off writes bit 0 of the power control register (EBus
// 0x724000, its first byte): the system switches off after the store and
// runs no further.
TEST(machine_ver_mask_follows_the_processor_module) {
    using namespace uv::test::sparc;
    for (const auto& [mhz, ecache, mask] : {std::tuple{300.0, 512u << 10, 0x13u}, {360.0, 2u << 20, 0x13u},
                                           {360.0, 256u << 10, 0x91u}, {440.0, 2u << 20, 0x91u}}) {
        Ultra5_10::Options o = small_options();
        o.cpu_hz = mhz * 1e6;
        o.ecache_bytes = ecache;
        store_be<u32>(o.prom.data() + 0x20, RDPR(g1, prVER));
        Ultra5_10 m(o);
        m.run(1);
        CHECK_EQ((m.cpu().reg(g1) >> 24) & 0xFF, mask);
        CHECK_EQ(m.cpu().reg(g1) >> 32, 0x00170012u);  // TI, the IIi
    }
}

TEST(machine_switches_itself_off_through_the_power_register) {
    using namespace uv::test::sparc;
    Ultra5_10::Options o = small_options();
    const u32 code[] = {
        SETHI(g2, 0x01000000),  // (the first byte, as a 32-bit big-endian store puts it)
        MOVi(g1, 0x1FF),
        SLLXi(g1, g1, 32),
        SETHI(g3, 0xF1724000),
        OR(g1, g1, g3),         // the power register through the EBus at power-on, 0x1FFF1724000
        STWA(g2, g1, g0, 0x15),
        ADDi(g5, g5, 1),        // never reached
    };
    for (std::size_t i = 0; i < std::size(code); ++i) store_be<u32>(o.prom.data() + 0x20 + 4 * i, code[i]);
    Ultra5_10 m(o);
    m.run(100);
    CHECK(m.switched_off());
    CHECK_EQ(m.cpu().reg(g5), 0u);
    const u64 at = m.cpu().instructions();
    m.run(100);
    CHECK_EQ(m.cpu().instructions(), at);  // off
    m.reset();                             // the power switch
    CHECK(!m.switched_off());
    m.run(3);
    CHECK_EQ(m.cpu().instructions(), at + 3);
}

TEST(machine_soft_resets_come_after_the_store_that_asks_for_them) {
    using namespace uv::test::sparc;
    for (const auto& [bit, vector] : {std::pair<u32, u64>{0x40000000, 0x20}, {0x20000000, 0x60}}) {
        Ultra5_10::Options o = small_options();
        const u32 code[] = {
            WRPRi(7, g0, 0),  // TL 0 (an XIR at MAXTL is error_state)
            SETHI(g2, bit),
            MOVi(g1, 0x1FE),
            SLLXi(g1, g1, 32),
            SETHI(g3, 0xF000),
            ORi(g3, g3, 0x20),
            OR(g1, g1, g3),       // Sabre's reset control register, 0x1FE0000F020
            STXA(g2, g1, g0, 0x14),
            ADDi(g5, g5, 1),      // never reached
        };
        for (std::size_t i = 0; i < std::size(code); ++i) store_be<u32>(o.prom.data() + 0x20 + 4 * i, code[i]);
        Ultra5_10 m(o);
        m.run(std::size(code) - 1);  // up to the store
        CHECK_EQ(m.cpu().pc(), usparc::kRstvAddr + vector);
        CHECK_EQ(m.cpu().reg(g5), 0u);
        CHECK(!m.cpu().halted());
        if (vector != 0x20) continue;
        m.run(100);  // round again, and again
        CHECK_EQ(m.cpu().reg(g5), 0u);
        CHECK(!m.cpu().halted());
    }
}

// A device the processor gives work to in the middle of a run acts when
// it is due, not when the run ends: the count of Sabre's timer 0, set to
// go back to zero at its limit (8 us after the store), read 4500 steps on,
// is the same whether the machine runs in one go or a step at a time.
TEST(machine_events_scheduled_by_a_store_come_on_time) {
    using namespace uv::test::sparc;
    Ultra5_10::Options o = small_options();
    const u32 code[] = {
        MOVi(g1, 0x1FE),
        SLLXi(g1, g1, 32),
        SETHI(g3, 0x1C00),
        OR(g1, g1, g3),  // timer 0's count, 0x1FE00001C00
        ADDi(g4, g1, 8),  // and its limit
        SETHI(g2, 0x40000000),
        ORi(g2, g2, 8),  // back to zero at 8 us
        STXA(g0, g1, g0, 0x14),
        STXA(g2, g4, g0, 0x14),
        SETHI(l0, 1024),
        ADDi(l0, l0, 476),  // 1500 times round, 4500 steps
        SUBcci(l0, l0, 1),
        Bicc(cNE, -1),
        NOP,
        LDXA(g5, g1, g0, 0x14),
        Bicc(cA, 0),
        NOP,
    };
    for (std::size_t i = 0; i < std::size(code); ++i) store_be<u32>(o.prom.data() + 0x20 + 4 * i, code[i]);
    constexpr u64 kRun = 4600;
    Ultra5_10 whole(o), stepped(o);
    whole.run(kRun);
    for (u64 i = 0; i < kRun; ++i) stepped.run(1);
    CHECK_EQ(whole.cpu().pc(), stepped.cpu().pc());
    CHECK_EQ(whole.cpu().reg(l0), 0u);
    CHECK_EQ(whole.cpu().reg(g5), stepped.cpu().reg(g5));
    // (4500 steps at 440 MHz, about 10 us, less the 8 us before it went back)
    CHECK_EQ(whole.cpu().reg(g5), 2u);
}

TEST(machine_boot_prom_and_nvram_are_reachable_at_power_on) {
    Ultra5_10 m(small_options());
    u16 v = 0;
    CHECK(m.bus().read(0x1FFF0000000ull, v));  // the reset vector's flash, through the fast path
    CHECK_EQ(v, 0x4F42u);
    // The NVRAM through the PCI path (EBus BAR1), bytes in address order.
    m.nvram().data()[0x10] = 0x12;
    m.nvram().data()[0x11] = 0x34;
    u16 n = 0;
    CHECK(m.bus().read(0x1FFF1000010ull, n));
    CHECK_EQ(n, 0x1234u);
}

TEST(machine_wide_ebus_accesses_reach_byte_devices_as_byte_cycles) {
    Ultra5_10 m(small_options());
    // A 16-bit store to the serial controller's MODE/TIMR pair writes both
    // registers, in address order, as the EBus splits it into byte cycles.
    CHECK(m.bus().write(0x1FFF1400022ull, u16{0x1234}));
    u8 mode = 0, timr = 0;
    CHECK(m.bus().read(0x1FFF1400022ull, mode));
    CHECK(m.bus().read(0x1FFF1400023ull, timr));
    CHECK_EQ(mode, 0x12u);
    CHECK_EQ(timr, 0x34u);
    u16 both = 0;
    CHECK(m.bus().read(0x1FFF1400022ull, both));
    CHECK_EQ(both, 0x1234u);
}

TEST(machine_power_on_registers_read_as_on_a_real_ultra_10) {
    Ultra5_10 m(small_options());
    u64 port_id = 0, imap = 1;
    CHECK(m.bus().read(Sabre::kRegsBase, port_id));
    CHECK_EQ(port_id, 0x8E10002000000000ull);
    CHECK(m.bus().read(Sabre::kRegsBase + 0x1098, imap));  // graphics interrupt map
    CHECK_EQ(imap, 0u);
    CHECK(m.bus().read(Sabre::kRegsBase + 0x10A8, imap));
    CHECK_EQ(imap, 0x7C0u);
    cfg_write32(m, cfg(0, 1, 1, 0x18), 0x00010100);  // bus A as bus 1
    CHECK_EQ(cfg_read32(m, cfg(1, 3, 0, 0x50)), 0xC000EC40u);  // CMD646 CFR, CNTRL, CMDTIM, ARTTIM0
    CHECK_EQ(cfg_read32(m, cfg(1, 3, 0, 0x54)), 0xCC00C000u);
    CHECK_EQ(cfg_read32(m, cfg(1, 3, 0, 0x58)), 0x00004000u);
    CHECK_EQ(cfg_read32(m, cfg(1, 3, 0, 0x70)) >> 24, 0xF0u);  // UDIDETCR0
    cfg_write32(m, cfg(1, 3, 0, 0x78), 0x05000000);              // UDIDETCR1, as Solaris sets its modes
    CHECK_EQ(cfg_read32(m, cfg(1, 3, 0, 0x78)) >> 24, 0xF0u);  // reads as the real one does after Solaris
    // SuperIO: OBP's FER value reads back with bits 7:6 set; SID 0x90.
    const u64 sio = 0x1FFF130015Cull;
    CHECK(m.bus().write(sio, u8{0}));
    CHECK(m.bus().write(sio + 1, u8{0x0F}));  // (a single write changes nothing)
    u8 b = 0;
    CHECK(m.bus().read(sio + 1, b));
    CHECK_EQ(b, 0xC0u);
    CHECK(m.bus().write(sio + 1, u8{0x0F}));  // the second consecutive write takes
    CHECK(m.bus().read(sio + 1, b));
    CHECK_EQ(b, 0xCFu);
    CHECK(m.bus().write(sio, u8{8}));
    CHECK(m.bus().read(sio + 1, b));
    CHECK_EQ(b, 0x90u);
    // EBus DMA channels report device ID 0xC.
    u32 csr = 0;
    CHECK(m.bus().read(0x1FFF1706000ull, csr));
    CHECK_EQ(byteswap(csr) >> 28, 0xCu);
}

TEST(simba_has_no_base_and_limit_registers_and_resets_its_maps_as_the_apb_manual_says) {
    Ultra5_10 m(small_options());
    for (unsigned fn : {0u, 1u}) {
        cfg_write32(m, cfg(0, 1, fn, 0x1C), 0xFFFFFFFFu);  // I/O base and limit, secondary status
        CHECK_EQ(cfg_read32(m, cfg(0, 1, fn, 0x1C)) & 0xFFFF, 0u);
        cfg_write32(m, cfg(0, 1, fn, 0x20), 0xFFF0FFF0u);  // memory base and limit
        CHECK_EQ(cfg_read32(m, cfg(0, 1, fn, 0x20)), 0u);
        cfg_write32(m, cfg(0, 1, fn, 0x3C), 0xFFFFFFFFu);  // interrupt line and pin, bridge control
        CHECK_EQ(cfg_read32(m, cfg(0, 1, fn, 0x3C)), 0x00630000u);
    }
    // At reset only the boot bus (function 1) forwards memory; no I/O.
    CHECK_EQ(cfg_read32(m, cfg(0, 1, 1, 0xDC)) >> 16, 0xFF00u);  // memory map FF, I/O map 0
    CHECK_EQ(cfg_read32(m, cfg(0, 1, 0, 0xDC)) >> 16, 0u);
}

TEST(machine_memory_banks_mirror_and_empty_banks_read_zero) {
    Ultra5_10 m(small_options());  // 64 MiB in bank 0
    CHECK(m.bus().write(0x100ull, u64{0x1122334455667788ull}));
    u64 v = 0;
    CHECK(m.bus().read(0x4000100ull, v));  // 64 MiB higher: the same DIMMs
    CHECK_EQ(v, 0x1122334455667788ull);
    CHECK(m.bus().read(0x20000100ull, v));  // bank 2: empty
    CHECK_EQ(v, 0u);
}

TEST(machine_simba_tick_counts_at_the_pci_clock) {
    Ultra5_10 m(small_options());
    const u64 pa = cfg(0, 1, 0, 0xB0);
    m.bus().write(pa, u32{0});
    m.run(4400);  // spinning at the reset vector
    u32 lo = 0;
    m.bus().read(pa, lo);
    const u32 ticks = byteswap(lo);
    CHECK(ticks > 0 && ticks <= 700);  // 66 MHz against a 440 MHz CPU: about 0.15 per cycle
}

TEST(sabre_maps_interrupts_to_vectors_and_holds_them_until_cleared) {
    Ultra5_10 m(small_options());
    auto& cpu = m.cpu();
    // Map INO 0x2B (serial): valid.
    m.bus().write(Sabre::kRegsBase + 0x1058, u64{0x80000000ull});
    m.sabre().set_interrupt(0x2B, true);
    // The vector is waiting in the processor's receive register.
    cpu.set_pstate(usparc::pstate::PRIV);
    CHECK(!cpu.deliver_interrupt_vector(0, 0, 0));  // busy with the Sabre's vector
    CHECK_EQ(m.sabre().reg(0x1858), 3u);              // pending
    // Clear it while the level is still asserted: it is sent again (once
    // the processor has room).
    m.bus().write(Sabre::kRegsBase + 0x1858, u64{0});
    CHECK_EQ(m.sabre().reg(0x1858), 0u);
}

TEST(machine_ebus_audio_auxio_and_power_button_answer_where_openboot_puts_them) {
    Ultra5_10 m(small_options());
    constexpr u64 ebus = 0x1FFF1000000ull;  // EBus BAR1 at power-on
    // The codec: index, then data a word on.
    u8 b = 0;
    CHECK(m.bus().write(ebus + 0x200000, u8{12}));
    CHECK(m.bus().read(ebus + 0x200004, b));
    CHECK_EQ(b, 0x8Au);
    // The auxio registers (the system LED in the first), and the floppy's
    // and audio's auxiliary registers, read back what was written.
    for (u64 at : {0x726000ull, 0x728000ull, 0x72A000ull, 0x72C000ull, 0x72F000ull, 0x720000ull, 0x722000ull}) {
        u32 v = 0;
        CHECK(m.bus().write(ebus + at, byteswap(u32{0x01})));
        CHECK(m.bus().read(ebus + at, v));
        CHECK_EQ(byteswap(v), 0x01u);
    }
    CHECK(m.system_led());
    m.reset();
    CHECK(!m.system_led());
    // The power button interrupts through INO 0x25.
    m.bus().write(Sabre::kRegsBase + 0x1028, u64{0x80000000ull});
    CHECK_EQ(m.sabre().reg(0x1828), 0u);
    m.press_power_button();
    CHECK_EQ(m.sabre().reg(0x1828), 3u);  // pending
}

TEST(sabre_iommu_translates_dvma_through_the_tsb) {
    Ultra5_10 m(small_options());
    // TSB at 0x10000, 8 KiB pages, 1K entries: DVMA page n uses entry n.
    const u64 tsb = 0x10000;
    store_be<u64>(m.bus().host_write_ptr(tsb + 3 * 8), 0x8000000000000002ull | 0x200000);  // valid, writable -> PA 0x200000
    m.bus().write(Sabre::kRegsBase + 0x208, u64{tsb});
    m.bus().write(Sabre::kRegsBase + 0x200, u64{1});
    u64 pa = 0;
    // A 1K-entry TSB of 8 KiB pages covers the top 8 MiB: entry n is DVMA 0xFF80_0000 + n pages.
    CHECK(m.sabre().translate(0xFF800000u + 3 * 0x2000 + 0x123, true, pa));
    CHECK_EQ(pa, 0x200123u);
    CHECK(!m.sabre().translate(0xFF800000u + 4 * 0x2000, false, pa));  // invalid entry
}

TEST(sabre_logs_pio_writes_and_their_master_aborts_not_reads) {
    Ultra5_10 m(small_options());
    auto& b = m.bus();
    constexpr u64 nowhere = Sabre::kMemBase + 0x90000000ull;  // no device decodes it
    auto status = [&] { return cfg_read32(m, cfg(0, 0, 0, 4)) >> 16; };
    // A PIO write that completes: its address and byte mask, no error.
    CHECK(b.write(0x1FFF1400020ull, u8{0x01}));  // the SAB82532's command register
    CHECK_EQ(m.sabre().reg(0x2018), 0x1FFF1400020ull);
    CHECK_EQ(m.sabre().reg(0x2010), 0x100000000ull);  // byte 0
    // A read that master-aborts: a bus error to the processor, the PCI
    // status's received master abort, nothing in the PIO write AFSR.
    u32 v = 0;
    CHECK(!b.read(nowhere, v));
    CHECK_EQ(m.sabre().reg(0x2010), 0x100000000ull);
    CHECK(status() & 0x2000);
    cfg_write32(m, cfg(0, 0, 0, 4), 0x20000000u);  // cleared by writing 1
    CHECK(!(status() & 0x2000));
    // A write that master-aborts: logged, and the PCI error interrupt once
    // enabled and mapped.
    b.write(Sabre::kRegsBase + 0x1080, u64{0x80000000ull});  // PCI error map valid
    CHECK(b.write(nowhere + 4, u16{0x1234}));
    CHECK_EQ(m.sabre().reg(0x2010) >> 56, 0x80u);
    CHECK_EQ(m.sabre().reg(0x2018), nowhere + 4);
    CHECK_EQ((m.sabre().reg(0x2010) >> 32) & 0xFF, 0x30u);  // bytes 4 and 5
    CHECK_EQ(m.sabre().reg(0x1880), 0u);  // ERRINT_EN still clear
    b.write(Sabre::kRegsBase + 0x2000, u64{0x100});
    CHECK_EQ(m.sabre().reg(0x1880), 3u);
    // A second error is secondary; the primary's address stays.
    CHECK(b.write(nowhere + 8, u8{0}));
    CHECK_EQ(m.sabre().reg(0x2010) >> 56, 0x88u);
    CHECK_EQ(m.sabre().reg(0x2018), nowhere + 4);
    b.write(Sabre::kRegsBase + 0x2010, ~u64{0});  // the error bits clear when written with 1
    CHECK_EQ(m.sabre().reg(0x2010) >> 56, 0u);
}

TEST(sabre_dma_answers_only_its_target_space_and_logs_translation_errors) {
    Ultra5_10 m(small_options());
    auto& b = m.bus();
    u8 buf[4]{};
    // Nothing claimed until the target address space register enables a region.
    CHECK(!m.sabre().dma_read(0x1000, buf, 4));
    b.write(Sabre::kRegsBase + 0x2028, u64{0x41});  // 0x0000_0000 and 0xC000_0000 regions
    CHECK(m.sabre().dma_read(0x1000, buf, 4));      // pass-through with the IOMMU off
    CHECK(!m.sabre().dma_read(0x20001000u, buf, 4));
    CHECK_EQ(m.sabre().reg(0x30), 0u);  // not the module's: nothing logged
    // The IOMMU on, a 1K-entry TSB at 0x10000 with entry 2 valid but read-only.
    const u64 tsb = 0x10000;
    store_be<u64>(m.bus().host_write_ptr(tsb + 2 * 8), 0x8000000000000000ull | 0x200000);
    b.write(Sabre::kRegsBase + 0x208, u64{tsb});
    b.write(Sabre::kRegsBase + 0x200, u64{1});
    b.write(Sabre::kRegsBase + 0x1070, u64{0x80000000ull});  // UE map valid
    CHECK(m.sabre().dma_read(0xC0004000u, buf, 4));  // entry 2
    // A read through an invalid entry (3): a primary translation error on
    // a read, the TTE's address, an invalid-entry error in the IOMMU, a
    // target abort, and the UE interrupt.
    CHECK(!m.sabre().dma_read(0xC0006000u, buf, 4));
    const u64 afsr = m.sabre().reg(0x30);
    CHECK_EQ(afsr >> 56, 0x41u);                          // P_DRD, P_DTE
    CHECK_EQ(m.sabre().reg(0x38), tsb + 3 * 8);
    CHECK_EQ((afsr >> 29) & 7, 3u);                       // the doubleword in its 64 bytes
    CHECK_EQ((afsr >> 32) & 0xFFFF, 0xFF00u);
    CHECK_EQ(m.sabre().reg(0x200) & (7ull << 24), 3ull << 24);  // error, invalid
    CHECK(cfg_read32(m, cfg(0, 0, 0, 4)) >> 16 & 0x0800);  // signalled target abort
    CHECK_EQ(m.sabre().reg(0x1870), 3u);
    // A write to the read-only page: secondary bits only.
    CHECK(!m.sabre().dma_write(0xC0004000u, buf, 4));
    CHECK_EQ(m.sabre().reg(0x30) >> 56, 0x47u);  // + S_DWR, S_DTE
    CHECK_EQ(m.sabre().reg(0x38), tsb + 3 * 8);
    b.write(Sabre::kRegsBase + 0x30, ~u64{0});
    CHECK_EQ(m.sabre().reg(0x30) >> 56, 0u);
    // Now a protection error is the primary one.
    CHECK(!m.sabre().dma_write(0xC0004000u, buf, 4));
    CHECK_EQ(m.sabre().reg(0x30) >> 56, 0x21u);  // P_DWR, P_DTE
    CHECK_EQ(m.sabre().reg(0x200) & (7ull << 24), 1ull << 24);  // error, protection
}

TEST(m48t59_clock_reads_and_sets_in_bcd) {
    std::time_t host = 0;  // 1970-01-01 00:00:00 (a Thursday)
    M48t59 rtc([&] { return host; });
    auto rd = [&](unsigned a) {
        u64 v = 0;
        rtc.read(a, 1, v);
        return static_cast<unsigned>(v);
    };
    host = 86400 * 365 * 30 + 3661;  // 1999-12-25, 01:01:01 UTC give or take leap days
    CHECK_EQ(rd(0x1FF9) & 0x7F, 0x01u);
    CHECK_EQ(rd(0x1FFA), 0x01u);
    CHECK_EQ(rd(0x1FFB), 0x01u);
    // Set 2004-02-29 23:59:58 (year counted from 1968: 36).
    rtc.write(0x1FF8, 1, 0x80);
    rtc.write(0x1FF9, 1, 0x58);
    rtc.write(0x1FFA, 1, 0x59);
    rtc.write(0x1FFB, 1, 0x23);
    rtc.write(0x1FFD, 1, 0x29);
    rtc.write(0x1FFE, 1, 0x02);
    rtc.write(0x1FFF, 1, 0x36);
    rtc.write(0x1FF8, 1, 0x00);
    host += 3;  // three seconds later: 2004-03-01 00:00:01
    CHECK_EQ(rd(0x1FF9) & 0x7F, 0x01u);
    CHECK_EQ(rd(0x1FFB), 0x00u);
    CHECK_EQ(rd(0x1FFD), 0x01u);
    CHECK_EQ(rd(0x1FFE), 0x03u);
    CHECK_EQ(rd(0x1FFF), 0x36u);
}

TEST(m48t59_alarm_and_watchdog_raise_their_flags_and_irq) {
    Scheduler sched;
    u64 now = 0;
    constexpr double hz = 1e6;
    sched.set_clock([&] { return now; });
    std::time_t host = 86400 * 10;  // a midnight
    M48t59 rtc([&] { return host + static_cast<std::time_t>(now / hz); });
    rtc.set_next_second([&] { return (now / static_cast<u64>(hz) + 1) * static_cast<u64>(hz); });
    rtc.attach(sched, hz);
    bool irq = false;
    rtc.on_irq = [&](bool on) { irq = on; };
    auto run_to = [&](double seconds) {
        const u64 end = static_cast<u64>(seconds * hz);
        while (sched.next_event() <= end) {
            now = std::max(now, sched.next_event());
            sched.run_due(now);
        }
        now = end;
    };
    auto rd = [&](unsigned a) {
        u64 v = 0;
        rtc.read(a, 1, v);
        return static_cast<unsigned>(v);
    };
    auto wr = [&](unsigned a, unsigned v) { rtc.write(a, 1, v); };
    // Once a minute at 30 seconds (RPT1 clear, the rest set), the IRQ enabled.
    wr(0x1FF2, 0x30);
    wr(0x1FF3, 0x80);
    wr(0x1FF4, 0x80);
    wr(0x1FF5, 0x80);
    wr(0x1FF6, 0x80);
    run_to(29.5);
    CHECK(!irq);
    CHECK_EQ(rd(0x1FF0) & 0x40, 0u);
    run_to(30.01);
    CHECK(irq);
    CHECK_EQ(rd(0x1FF0) & 0x40, 0x40u);  // the read that clears it sees it set
    CHECK(!irq);
    CHECK_EQ(rd(0x1FF0) & 0x40, 0u);
    run_to(90.01);  // and a minute later
    CHECK(irq);
    rd(0x1FF0);
    // Without AFE the flag rises alone.
    wr(0x1FF6, 0);
    run_to(150.01);
    CHECK(!irq);
    CHECK_EQ(rd(0x1FF0) & 0x40, 0x40u);
    wr(0x1FF5, 0);  // to disable the alarm: date 0, RPT bits clear
    wr(0x1FF4, 0);
    wr(0x1FF3, 0);
    wr(0x1FF2, 0);
    // The watchdog: 3 x 1 second to the IRQ (WDS clear), restarted by a write.
    wr(0x1FF7, 3 << 2 | 2);
    run_to(152.5);
    wr(0x1FF7, 3 << 2 | 2);
    run_to(155.4);
    CHECK(!irq);
    run_to(155.6);
    CHECK(irq);
    CHECK_EQ(rd(0x1FF0) & 0x80, 0x80u);
    CHECK(!irq);
    // Steered to the reset output: a pulse, and the watchdog, FT and AFE cleared.
    bool reset = false;
    rtc.on_watchdog_reset = [&] { reset = true; };
    wr(0x1FF6, 0x80);
    wr(0x1FF7, 0x80 | 1 << 2 | 0);  // 1/16 s
    run_to(155.7);
    CHECK(reset);
    CHECK_EQ(rd(0x1FF7), 0u);
    CHECK_EQ(rd(0x1FF6) & 0x80, 0u);
    CHECK(!irq);
}

TEST(m48t59_idprom_checksum) {
    M48t59 rtc;
    const u8 mac[6] = {8, 0, 0x20, 1, 2, 3};
    rtc.write_idprom(0x80, mac, 0x123456);
    const u8* p = rtc.data() + 0x1FD8;
    u8 sum = 0;
    for (int i = 0; i < 16; ++i) sum ^= p[i];
    CHECK_EQ(sum, 0u);
    CHECK_EQ(p[1], 0x80u);
    CHECK_EQ(p[7], 3u);
}

TEST(ns16550_transmits_receives_and_identifies_interrupts) {
    Scheduler s;
    u64 now = 0;
    s.set_clock([&] { return now; });
    SerialLine line;
    std::string out;
    line.transmit = [&](u8 b) { out.push_back(static_cast<char>(b)); };
    Ns16550 u(s, line, 1843200.0, 1843200.0 * 16);  // one cycle per bit time x16 for easy numbers
    bool irq = false;
    u.set_irq_sink([&](bool a) { irq = a; });
    auto rd = [&](unsigned r) {
        u64 v = 0;
        u.read(r, 1, v);
        return static_cast<unsigned>(v);
    };
    u.write(3, 1, 0x80);  // DLAB
    u.write(0, 1, 1);     // divisor 1
    u.write(3, 1, 0x03);  // 8N1
    u.write(4, 1, 0x08);  // OUT2: interrupts reach the system
    u.write(1, 1, 0x03);  // receive and THRE interrupts
    CHECK(irq);           // THR is empty
    CHECK_EQ(rd(2), 0x02u);
    CHECK(!irq);          // reading IIR acknowledged THRE
    u.write(0, 1, 'A');
    CHECK_EQ(rd(5) & 0x60, 0u);
    now += 5000;  // a character at the 115200 baud set above is 2560 cycles here
    s.run_due(now);
    CHECK(out == "A");
    CHECK_EQ(rd(5) & 0x60, 0x60u);
    line.receive('z');
    u.poll_line();
    now += 5000;
    s.run_due(now);
    CHECK(rd(5) & 1);
    CHECK_EQ(rd(2), 0x04u);  // received data available
    CHECK_EQ(rd(0), static_cast<unsigned>('z'));
    CHECK(!(rd(5) & 1));
}

// With the FIFO on, received data interrupts only at the trigger level, or
// with less after the line has been idle for four character times; the
// interrupt line follows IIR. (Raised for any data, it told the driver of
// a mouse packet below the trigger that IIR then denied, over and over.)
TEST(ns16550_receive_interrupt_follows_the_fifo_trigger_and_timeout) {
    Scheduler s;
    u64 now = 0;
    s.set_clock([&] { return now; });
    SerialLine line;
    Ns16550 u(s, line, 1843200.0, 1843200.0 * 16);
    bool irq = false;
    u.set_irq_sink([&](bool a) { irq = a; });
    auto rd = [&](unsigned r) {
        u64 v = 0;
        u.read(r, 1, v);
        return static_cast<unsigned>(v);
    };
    u.write(3, 1, 0x80);
    u.write(0, 1, 1);
    u.write(3, 1, 0x03);
    u.write(2, 1, 0x81);  // FIFO on, trigger at 8
    u.write(4, 1, 0x08);
    u.write(1, 1, 0x01);  // received data interrupts only
    auto feed = [&](const char* bytes) {
        for (const char* p = bytes; *p; ++p) line.receive(static_cast<u8>(*p));
        u.poll_line();
        for (std::size_t i = 0; i < std::strlen(bytes); ++i) {
            now += 2600;  // a character time at 115200 baud here
            s.run_due(now);
        }
    };
    feed("abcde");  // a mouse packet: below the trigger
    CHECK(rd(5) & 1);
    CHECK(!irq);
    CHECK_EQ(rd(2) & 0x0F, 0x01u);
    now += 5 * 2600;  // idle: the timeout
    s.run_due(now);
    CHECK(irq);
    CHECK_EQ(rd(2) & 0x0F, 0x0Cu);
    for (int i = 0; i < 5; ++i) rd(0);
    CHECK(!irq);
    feed("12345678");  // the trigger level: at once
    CHECK(irq);
    CHECK_EQ(rd(2) & 0x0F, 0x04u);
}

TEST(sab82532_transmit_fifo_fills_in_order_at_any_address) {
    Scheduler s;
    u64 now = 0;
    s.set_clock([&] { return now; });
    SerialLine a, b;
    std::string out;
    a.transmit = [&](u8 c) { out.push_back(static_cast<char>(c)); };
    Sab82532 se(s, a, b, Sab82532::kClockHz / 16.0);  // a bit time is one cycle at the reset baud rate
    bool irq = false;
    se.set_irq_sink([&](bool on) { irq = on; });
    u64 v = 0;
    se.read(0x3B, 1, v);  // clear the reset XPR/ALLS
    se.write(0x3B, 1, 0xFE);  // IMR1: XPR only
    for (char c : std::string("abc")) se.write(0x00, 1, static_cast<u8>(c));  // all to offset 0, as Solaris does
    se.write(0x20, 1, 0x08);  // CMDR: XF
    CHECK(!irq);
    for (int i = 0; i < 10; ++i) {  // each character is 10 bit times
        now += 11;
        s.run_due(now);
    }
    CHECK(out == "abc");
    CHECK(irq);
    se.read(0x3B, 1, v);
    CHECK_EQ(v, 0x01u);  // XPR; ALLS is masked, so not shown (IPC:VIS clear)
    CHECK(!irq);
}

TEST(sab82532_masked_statuses_stay_hidden_and_stored_unless_ipc_vis) {
    Scheduler s;
    u64 now = 0;
    s.set_clock([&] { return now; });
    SerialLine a, b;
    Sab82532 se(s, a, b, Sab82532::kClockHz / 16.0);
    bool irq = false;
    se.set_irq_sink([&](bool on) { irq = on; });
    u64 v = 0;
    se.read(0x3B, 1, v);      // clear the reset XPR/ALLS
    se.write(0x3A, 1, 0xFF);  // IMR0: receive statuses masked
    se.write(0x28, 1, 0x04);  // RFC: threshold 4, so a lone character ends in TIME
    a.receive('x');
    se.poll_lines();
    for (int i = 0; i < 40; ++i) {  // the character, then the line idle: TIME
        now += 11;
        s.run_due(now);
    }
    CHECK(!irq);
    se.read(0x3A, 1, v);  // IPC:VIS clear: the masked TIME is not shown...
    CHECK_EQ(v, 0u);
    se.write(0x3A, 1, 0xBF);  // ...and is still there when TIME is unmasked
    CHECK(irq);
    se.read(0x3A, 1, v);
    CHECK_EQ(v, 0x40u);
    CHECK(!irq);
    // With IPC:VIS set, masked statuses are shown and cleared by the read.
    se.write(0x39, 1, 0x80);
    se.write(0x3A, 1, 0xFF);
    se.write(0x20, 1, 0x20);  // CMDR: RFRD gives TCD for the byte still in the FIFO
    CHECK(!irq);
    se.read(0x3A, 1, v);
    CHECK_EQ(v, 0x80u);
    se.write(0x3A, 1, 0x00);
    CHECK(!irq);
}

TEST(serial_breaks_reach_both_uarts_in_their_own_ways) {
    // SAB82532: BRK, no character in the RFIFO, then BRKT a quarter second on.
    {
        Scheduler s;
        u64 now = 0;
        s.set_clock([&] { return now; });
        SerialLine a, b;
        constexpr double hz = 1e6;
        Sab82532 se(s, a, b, hz);
        u64 v = 0;
        se.read(0x3B, 1, v);      // clear the reset XPR/ALLS
        se.write(0x3B, 1, 0x3F);  // IMR1: BRK and BRKT unmasked
        a.receive_break();
        a.receive('x');
        se.poll_lines();
        auto run_to = [&](double t) {
            const u64 end = static_cast<u64>(t * hz);
            while (s.next_event() <= end) {
                now = std::max(now, s.next_event());
                s.run_due(now);
            }
            now = end;
        };
        run_to(0.01);
        se.read(0x3B, 1, v);
        CHECK_EQ(v & 0xC0, 0x80u);  // BRK
        se.read(0x3B, 1, v);
        CHECK_EQ(v & 0xC0, 0u);     // (cleared by the read)
        run_to(0.3);
        se.read(0x3B, 1, v);
        CHECK_EQ(v & 0x40, 0x40u);  // BRKT
        se.write(0x20, 1, 0x20);    // RFRD: the FIFO holds only the 'x'
        se.read(0x2A, 1, v);        // RBCL
        CHECK_EQ(v, 1u);
        se.read(0x00, 1, v);
        CHECK_EQ(v, static_cast<u64>('x'));
    }
    // NS16550: a zero character with BI and FE.
    {
        Scheduler s;
        u64 now = 0;
        s.set_clock([&] { return now; });
        SerialLine line;
        Ns16550 u(s, line, 1843200.0, 1843200.0 * 16);
        u.write(3, 1, 0x80);
        u.write(0, 1, 1);
        u.write(3, 1, 0x03);
        line.receive_break();
        u.poll_line();
        now += 100000;
        s.run_due(now);
        u64 v = 0;
        u.read(5, 1, v);
        CHECK_EQ(v & 0x19, 0x19u);  // BI, FE, DR
        u.read(0, 1, v);
        CHECK_EQ(v, 0u);
    }
}

TEST(sun_keyboard_unplugged_breaks_the_line_and_reports_itself_when_plugged_in) {
    Scheduler s;
    u64 now = 0;
    s.set_clock([&] { return now; });
    SerialLine line;
    SunKeyboard k(s, line, 1e6);
    k.set_plugged(false);
    CHECK(line.rx_break());
    line.rx_take();
    k.key(0x4D, true);  // (nothing: unplugged)
    CHECK(!line.rx_pending());
    k.set_plugged(true);
    now += 100000;
    s.run_due(now);
    std::vector<u8> got;
    while (line.rx_pending()) got.push_back(line.rx_take());
    CHECK(got == (std::vector<u8>{0xFF, 0x04, 0x7F}));
}

TEST(ebus_dma_routes_device_interrupts_and_counts_to_terminal_count) {
    EbusDma dma("test-dma");
    std::vector<u8> mem(64);
    dma.set_dma_port([&](u32 a, u8* d, std::size_t n) { std::copy_n(mem.begin() + a, n, d); return true; },
                     [&](u32 a, const u8* d, std::size_t n) { std::copy_n(d, n, mem.begin() + a); return true; });
    bool irq = false;
    dma.set_irq_sink([&](bool on) { irq = on; });
    // The registers are little-endian: the bus carries bytes in address order.
    auto put = [&](unsigned reg, u32 v) { dma.write(reg, 4, byteswap(v)); };
    auto get = [&](unsigned reg) {
        u64 v = 0;
        dma.read(reg, 4, v);
        return byteswap(static_cast<u32>(v));
    };
    dma.set_device_irq(true);
    CHECK(get(0) & EbusDma::kIntPend);
    CHECK(!irq);  // INT_EN is clear
    put(0, EbusDma::kIntEn);
    CHECK(irq);
    dma.set_device_irq(false);
    CHECK(!irq);
    CHECK(!(get(0) & EbusDma::kIntPend));
    // Four bytes to memory at 0x10, counted to terminal count.
    put(0, EbusDma::kIntEn | EbusDma::kWrite | EbusDma::kEnDma | EbusDma::kEnCnt);
    put(8, 4);  // the count, then the address, which loads the transfer
    CHECK(!(get(0) & EbusDma::kALoaded));
    put(4, 0x10);
    CHECK(get(0) & EbusDma::kALoaded);
    const u8 data[6] = {1, 2, 3, 4, 5, 6};
    CHECK_EQ(dma.to_memory(data, 6), 4u);
    CHECK(mem[0x10] == 1 && mem[0x13] == 4 && mem[0x14] == 0);
    CHECK_EQ(get(4), 0x14u);
    CHECK_EQ(get(8), 0u);
    CHECK((get(0) & (EbusDma::kTc | EbusDma::kIntPend | EbusDma::kALoaded)) == (EbusDma::kTc | EbusDma::kIntPend));
    CHECK(irq);
    CHECK_EQ(dma.to_memory(data, 6), 0u);  // nothing loaded
    put(0, get(0));  // writing the CSR back clears TC, as drivers do
    CHECK(!(get(0) & EbusDma::kTc));
    CHECK(!irq);
}

TEST(ebus_dma_chains_the_next_transfer_at_terminal_count) {
    EbusDma dma("test-dma");
    std::vector<u8> mem(64);
    for (unsigned i = 0; i < mem.size(); ++i) mem[i] = static_cast<u8>(i);
    dma.set_dma_port([&](u32 a, u8* d, std::size_t n) { std::copy_n(mem.begin() + a, n, d); return true; },
                     [&](u32 a, const u8* d, std::size_t n) { std::copy_n(d, n, mem.begin() + a); return true; });
    auto put = [&](unsigned reg, u32 v) { dma.write(reg, 4, byteswap(v)); };
    auto get = [&](unsigned reg) {
        u64 v = 0;
        dma.read(reg, 4, v);
        return byteswap(static_cast<u32>(v));
    };
    // From memory (a playback), as the audio drivers set it up: the first
    // buffer loaded before the channel is enabled, the next while it runs.
    put(0, EbusDma::kReset);
    put(0, 0);
    put(8, 4);
    put(4, 0x00);
    put(0, EbusDma::kIntEn | EbusDma::kEnDma | EbusDma::kEnCnt | EbusDma::kEnNext);
    CHECK((get(0) & (EbusDma::kALoaded | EbusDma::kNaLoaded)) == EbusDma::kALoaded);
    put(8, 3);
    put(4, 0x20);
    CHECK((get(0) & (EbusDma::kALoaded | EbusDma::kNaLoaded)) == (EbusDma::kALoaded | EbusDma::kNaLoaded));
    CHECK_EQ(get(4), 0x00u);  // the current address reads back
    CHECK_EQ(get(8), 4u);
    // Six bytes run through the first buffer into the next without a gap.
    u8 out[8]{};
    CHECK_EQ(dma.from_memory(out, 6), 6u);
    CHECK(out[3] == 3 && out[4] == 0x20 && out[5] == 0x21);
    CHECK((get(0) & (EbusDma::kTc | EbusDma::kALoaded | EbusDma::kNaLoaded)) == (EbusDma::kTc | EbusDma::kALoaded));
    CHECK_EQ(get(4), 0x22u);
    CHECK_EQ(get(8), 1u);
    put(0, get(0));  // the interrupt handler acknowledges the terminal count
    CHECK(!(get(0) & EbusDma::kTc));
    CHECK_EQ(dma.from_memory(out, 6), 1u);  // the end of the chain
    CHECK((get(0) & (EbusDma::kTc | EbusDma::kALoaded)) == EbusDma::kTc);
    // Without EN_NEXT a second address replaces the current transfer.
    put(0, EbusDma::kEnDma | EbusDma::kEnCnt);
    put(8, 2);
    put(4, 0x10);
    put(8, 2);
    put(4, 0x30);
    CHECK(!(get(0) & EbusDma::kNaLoaded));
    CHECK_EQ(get(4), 0x30u);
}

TEST(fdc82077_reset_reports_four_drives_and_version) {
    Scheduler s;
    u64 now = 0;
    s.set_clock([&] { return now; });
    Fdc82077 f(s);
    bool irq = false;
    f.set_irq_sink([&](bool a) { irq = a; });
    auto rd = [&](unsigned r) {
        u64 v = 0;
        f.read(r, 1, v);
        return static_cast<unsigned>(v);
    };
    f.write(2, 1, 0x00);  // hold in reset
    f.write(2, 1, 0x0C);  // release, interrupts enabled
    now += 2000;
    s.run_due(now);
    CHECK(irq);
    for (unsigned d = 0; d < 4; ++d) {
        CHECK_EQ(rd(4) & 0xC0, 0x80u);  // ready for a command
        f.write(5, 1, 0x08);           // sense interrupt status
        CHECK_EQ(rd(4) & 0xC0, 0xC0u); // result to read
        CHECK_EQ(rd(5), 0xC0u | d);
        CHECK_EQ(rd(5), 0u);
    }
    CHECK(!irq);
    f.write(5, 1, 0x10);  // version
    CHECK_EQ(rd(5), 0x90u);
}

// Through Sabre, the PCI bus and the EBus, devices say how long a read
// holds still: NVRAM always, the TOD clock until its next second, the
// keyboard UART's line status always (no error waiting), its receive
// buffer never (a read takes a byte); RAM always; the APB tick counts.
TEST(machine_devices_say_how_long_a_read_holds) {
    Ultra5_10 m(small_options());
    const u64 hz = 440000000;
    CHECK_EQ(m.bus().stable_until(0x1FFF1000010ull, 1), Device::kForever);
    CHECK_EQ(m.bus().stable_until(0x1FFF1001FF9ull, 1), (m.cpu().cycles() / hz + 1) * hz);
    CHECK_EQ(m.bus().stable_until(0x1FFF13083FDull, 1), Device::kForever);
    CHECK_EQ(m.bus().stable_until(0x1FFF13083F8ull, 1), 0u);
    CHECK_EQ(m.bus().stable_until(0x1000, 8), Device::kForever);
    // The APB's tick register: a clock, counting up as read little-endian
    // (configuration space is), whole or by halves.
    CHECK_EQ(m.bus().stable_until(cfg(0, 1, 0, 0xB0), 8), Device::kClockLittle);
    CHECK_EQ(m.bus().stable_until(cfg(0, 1, 0, 0xB0), 4), Device::kClockLittle);
    CHECK_EQ(m.bus().stable_until(cfg(0, 1, 0, 0x00), 4), 0u);  // (the rest promises nothing)
}

// The M48T59 clock registers hold until the next second, unless stopped or
// held for reading; the flags hold while no bit a read clears is set.
TEST(m48t59_says_how_long_a_read_holds) {
    M48t59 rtc([] { return std::time_t{1000000}; });
    CHECK_EQ(rtc.stable_until(0x1FF9, 1), 0u);  // (no next second known)
    rtc.set_next_second([] { return u64{1234}; });
    CHECK_EQ(rtc.stable_until(0x1FF9, 1), 1234u);
    CHECK_EQ(rtc.stable_until(0x0100, 8), Device::kForever);
    rtc.write(0x1FF8, 1, 0x40);  // hold for reading
    CHECK_EQ(rtc.stable_until(0x1FF9, 1), Device::kForever);
    rtc.write(0x1FF8, 1, 0x00);
    rtc.data()[0x1FF0] = 0x80;  // a watchdog flag, cleared by reading
    CHECK_EQ(rtc.stable_until(0x1FF0, 1), 0u);
    CHECK_EQ(rtc.stable_until(0x1FF1, 1), Device::kForever);
}

// The SAB82532 says how long a read holds: the status register always; an
// interrupt status register not while it has bits a read would clear.
TEST(sab82532_says_how_long_a_read_holds) {
    Scheduler s;
    u64 now = 0;
    s.set_clock([&] { return now; });
    SerialLine a, b;
    Sab82532 se(s, a, b, Sab82532::kClockHz / 16.0);
    CHECK_EQ(se.stable_until(0x3B, 1), Device::kForever);  // the reset XPR/ALLS, masked: a read clears nothing
    se.write(0x3B, 1, 0xFE);                               // IMR1: XPR shown
    CHECK_EQ(se.stable_until(0x3B, 1), 0u);
    u64 v = 0;
    se.read(0x3B, 1, v);
    CHECK_EQ(se.stable_until(0x3B, 1), Device::kForever);
    CHECK_EQ(se.stable_until(0x20, 1), Device::kForever);  // STAR
    CHECK_EQ(se.stable_until(0x00, 1), Device::kForever);  // RFIFO, empty
}

// A new NVRAM is a machine set up before: OpenBoot's configuration with
// the boot from the disk alone, auto-boot on, the least diagnostics and
// the diagnostic switch off, under a checksum OpenBoot accepts; a Sun
// Ethernet address (random unless given) and a host ID with the same last
// three bytes. A blank (all-zero) image is taken as new as well.
TEST(ultra5_10_new_nvram_is_a_sane_starting_point) {
    auto checksum_ok = [](const u8* p) {
        u8 sum = 0x17;
        for (unsigned i = 0x0C; i < 0x1FD8; ++i) sum ^= p[i];
        return sum == p[0];
    };
    Ultra5_10::Options o = small_options();
    o.mac = std::array<u8, 6>{0x08, 0x00, 0x20, 0xAB, 0xCD, 0xEF};
    {
        Ultra5_10 m(o);
        const u8* p = m.nvram().data();
        CHECK(checksum_ok(p));
        CHECK_EQ(p[1], 0x00u);    // diag-switch? false
        CHECK_EQ(p[2], 0x20u);    // diag-level min
        CHECK_EQ(p[0x5D3], 0xFFu);  // auto-boot? true
        CHECK_EQ(p[0x2CA], 4u);     // boot-device "disk"
        CHECK(std::equal(p + 0x2CB, p + 0x2CB + 4, "disk"));
        CHECK_EQ(p[0x618 + 2], 8u);  // input-device "keyboard"
        const u8* id = p + 0x1FD8;
        CHECK_EQ(id[0], 0x01u);
        CHECK_EQ(id[1], 0x80u);
        CHECK_EQ(id[5], 0xABu);
        CHECK_EQ(id[7], 0xEFu);
        CHECK_EQ(id[12], 0xABu);  // serial = the address's last three bytes
        CHECK_EQ(id[14], 0xEFu);
    }
    // Random addresses: Sun's prefix, and two machines unlike each other.
    o.mac.reset();
    Ultra5_10 a(o), b(o);
    const u8* ia = a.nvram().data() + 0x1FD8;
    const u8* ib = b.nvram().data() + 0x1FD8;
    CHECK_EQ(ia[2], 0x08u);
    CHECK_EQ(ia[3], 0x00u);
    CHECK_EQ(ia[4], 0x20u);
    CHECK(!std::equal(ia + 5, ia + 8, ib + 5));
    CHECK(checksum_ok(a.nvram().data()));
}
