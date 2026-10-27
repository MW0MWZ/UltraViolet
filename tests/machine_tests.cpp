// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
#include <ctime>
#include <string>
#include <vector>

#include "core/endian.h"
#include "devices/fdc82077.h"
#include "devices/m48t59.h"
#include "devices/ns16550.h"
#include "devices/sab82532.h"
#include "machine/ultra5_10/ultra5_10.h"
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
    CHECK_EQ(v & 0x21, 0x21u);  // XPR and ALLS
    CHECK(!irq);
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
