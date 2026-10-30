// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <vector>

#include "core/endian.h"
#include "devices/ata.h"
#include "devices/atapi_cdrom.h"
#include "machine/ultra5_10/ultra5_10.h"
#include "storage/disk_image.h"
#include "test.h"

using namespace uv;

namespace {

// An image whose block n starts with n (big-endian) and is filled with n's low byte.
std::string make_image(const char* name, unsigned blocks, unsigned block_size) {
    const std::string path = (std::filesystem::temp_directory_path() / name).string();
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    std::vector<u8> b(block_size);
    for (unsigned n = 0; n < blocks; ++n) {
        std::memset(b.data(), static_cast<int>(n & 0xFF), b.size());
        store_be<u32>(b.data(), n);
        f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    }
    return path;
}

// One channel at 1 MHz (a microsecond per cycle), with 1 MiB of memory for DMA.
struct Rig {
    u64 now = 0;
    Scheduler sched;
    std::vector<u8> mem = std::vector<u8>(1u << 20);
    AtaChannel ch;
    bool irq = false;

    Rig() : ch(sched, 1e6) {
        sched.set_clock([this] { return now; });
        ch.set_irq_sink([this](bool a) { irq = a; });
        ch.set_dma(
            [this](u32 a, u8* d, std::size_t n) {
                if (a + n > mem.size()) return false;
                std::memcpy(d, mem.data() + a, n);
                return true;
            },
            [this](u32 a, const u8* d, std::size_t n) {
                if (a + n > mem.size()) return false;
                std::memcpy(mem.data() + a, d, n);
                return true;
            });
    }
    void run(u64 cycles) {
        now += cycles;
        sched.run_due(now);
    }
    void wait() {
        for (int i = 0; i < 100000 && (ch.read_control(2) & ata::kBsy); ++i) run(10);
    }
    void reg(unsigned r, u8 v) { ch.write_command(r, 1, v); }
    u8 reg(unsigned r) { return static_cast<u8>(ch.read_command(r, 1)); }
    u16 data() { return static_cast<u16>(ch.read_command(0, 2)); }
    void data(u16 v) { ch.write_command(0, 2, v); }
    void lba28(u32 lba, u8 count) {
        reg(6, static_cast<u8>(0xE0 | (lba >> 24)));
        reg(2, count);
        reg(3, static_cast<u8>(lba));
        reg(4, static_cast<u8>(lba >> 8));
        reg(5, static_cast<u8>(lba >> 16));
    }
    // A PRD table at 0x1000 with one entry for `bytes` at `addr`.
    void prd(u32 addr, u32 bytes) {
        store_le<u32>(mem.data() + 0x1000, addr);
        store_le<u32>(mem.data() + 0x1004, 0x80000000u | (bytes & 0xFFFF));
        for (unsigned i = 0; i < 4; ++i) ch.bm_write(4 + i, static_cast<u8>(0x1000 >> (8 * i)));
    }
    // ATAPI: sends a packet; PIO data-in with the given byte count limit.
    void packet(std::initializer_list<u8> cdb, u16 limit = 0xFFFE, bool dma = false) {
        reg(1, dma ? 1 : 0);
        reg(4, static_cast<u8>(limit));
        reg(5, static_cast<u8>(limit >> 8));
        reg(7, 0xA0);
        wait();
        u8 b[12] = {};
        std::size_t i = 0;
        for (u8 v : cdb) b[i++] = v;
        for (i = 0; i < 6; ++i) data(static_cast<u16>(b[2 * i] | (b[2 * i + 1] << 8)));
        wait();
    }
    // Reads every PIO block the device offers.
    std::vector<u8> read_all() {
        std::vector<u8> out;
        while (reg(7) & ata::kDrq) {
            const unsigned n = reg(4) | (reg(5) << 8);
            for (unsigned i = 0; i < n; i += 2) {
                const u16 w = data();
                out.push_back(static_cast<u8>(w));
                out.push_back(static_cast<u8>(w >> 8));
            }
            wait();
        }
        return out;
    }
};

u16 word(const std::vector<u8>& b, unsigned w) { return static_cast<u16>(b[2 * w] | (b[2 * w + 1] << 8)); }

}  // namespace

TEST(ata_disk_identifies_and_reads_by_pio) {
    DiskImage img;
    CHECK(img.open(make_image("uv-ata1.img", 2048, 512), false));
    Rig r;
    AtaDisk disk(r.sched, 1e6, img);
    r.ch.attach(0, &disk);
    // Signature after reset.
    CHECK_EQ(r.reg(2), 1u);
    CHECK_EQ(r.reg(3), 1u);
    CHECK_EQ(r.reg(4), 0u);
    CHECK_EQ(r.reg(5), 0u);
    r.reg(6, 0xA0);
    r.reg(7, 0xEC);  // IDENTIFY DEVICE
    CHECK(r.reg(7) & ata::kBsy);
    r.wait();
    CHECK(r.irq);
    CHECK_EQ(r.ch.read_control(2) & 0x89, 0x08u);  // DRQ, not busy (alternate status leaves the interrupt)
    CHECK(r.irq);
    CHECK_EQ(r.reg(7) & 0x89, 0x08u);
    CHECK(!r.irq);
    std::vector<u8> id;
    for (int i = 0; i < 256; ++i) {
        const u16 w = r.data();
        id.push_back(static_cast<u8>(w));
        id.push_back(static_cast<u8>(w >> 8));
    }
    CHECK_EQ(word(id, 0), 0x0040u);
    CHECK_EQ(word(id, 27), static_cast<u16>(('U' << 8) | 'l'));
    CHECK_EQ(word(id, 60) | (u32{word(id, 61)} << 16), 2048u);
    u8 sum = 0;
    for (u8 b : id) sum = static_cast<u8>(sum + b);
    CHECK_EQ(sum, 0u);
    CHECK_EQ(r.reg(7) & 0x89, 0x00u);
    // READ SECTORS: two from LBA 5, an interrupt per sector.
    r.lba28(5, 2);
    r.reg(7, 0x20);
    r.wait();
    for (u32 s = 5; s < 7; ++s) {
        CHECK(r.irq);
        CHECK_EQ(r.reg(7) & 0x89, 0x08u);
        const u16 w0 = r.data(), w1 = r.data();
        CHECK_EQ(w0, 0u);
        CHECK_EQ(w1, static_cast<u16>(((s & 0xFF) << 8) | 0));  // bytes 0..3 hold n big-endian
        for (int i = 2; i < 256; ++i) CHECK_EQ(r.data(), static_cast<u16>((s & 0xFF) * 0x0101));
        r.wait();
    }
    CHECK_EQ(r.reg(7) & 0x89, 0x00u);
    CHECK_EQ(r.reg(3), 6u);  // the registers end on the last sector read
    // Out of range: ID not found.
    r.lba28(2047, 2);
    r.reg(7, 0x20);
    r.wait();
    CHECK_EQ(r.reg(7) & 0x01, 0x01u);
    CHECK_EQ(r.reg(1), ata::kIdnf);
}

TEST(ata_disk_writes_by_pio_and_reads_back_by_dma) {
    DiskImage img;
    CHECK(img.open(make_image("uv-ata2.img", 64, 512), false));
    Rig r;
    AtaDisk disk(r.sched, 1e6, img);
    r.ch.attach(0, &disk);
    r.lba28(10, 1);
    r.reg(7, 0x30);  // WRITE SECTORS: DRQ at once, no interrupt for the first block
    r.wait();
    CHECK(!r.irq);
    CHECK_EQ(r.ch.read_control(2) & 0x89, 0x08u);
    for (int i = 0; i < 256; ++i) r.data(static_cast<u16>(0xA500 | i));
    r.wait();
    CHECK(r.irq);
    CHECK_EQ(r.reg(7) & 0x89, 0x00u);
    // READ DMA of sectors 9-10 to 0x2000.
    r.prd(0x2000, 1024);
    r.lba28(9, 2);
    r.reg(7, 0xC8);
    r.ch.bm_write(0, 0x09);  // start, towards memory
    r.wait();
    r.run(1000);
    CHECK(r.irq);
    CHECK_EQ(r.ch.bm_read(2) & 0x07, 0x04u);  // interrupt, not active, no error
    CHECK_EQ(r.mem[0x2003], 9u);
    CHECK_EQ(r.mem[0x2200], 0x00u);  // the word written as 0xA500: low byte first
    CHECK_EQ(r.mem[0x2201], 0xA5u);
    CHECK_EQ(r.mem[0x2200 + 0x1FC], 0xFEu);
    r.ch.bm_write(2, 0x04);
    CHECK_EQ(r.ch.bm_read(2) & 0x04, 0u);
    CHECK_EQ(r.reg(7) & 0x89, 0x00u);
}

TEST(atapi_cdrom_reports_unit_attention_then_reads) {
    DiskImage img;
    CHECK(img.open(make_image("uv-cd.iso", 100, 2048), true));
    Rig r;
    AtapiCdrom cd(r.sched, 1e6);
    r.ch.attach(0, &cd);
    cd.insert(&img);
    CHECK_EQ(r.reg(4), 0x14u);  // packet device signature
    CHECK_EQ(r.reg(5), 0xEBu);
    r.reg(7, 0xEC);  // IDENTIFY DEVICE: refused, signature kept
    r.wait();
    CHECK_EQ(r.reg(7) & 0x01, 0x01u);
    CHECK_EQ(r.reg(5), 0xEBu);
    // INQUIRY works through a pending unit attention.
    r.packet({0x12, 0, 0, 0, 36, 0});
    std::vector<u8> inq = r.read_all();
    CHECK_EQ(inq.size(), 36u);
    CHECK_EQ(inq[0], 0x05u);
    CHECK_EQ(inq[1], 0x80u);
    CHECK_EQ(r.reg(2), 0x03u);  // status phase
    // TEST UNIT READY: the medium changed.
    r.packet({0x00});
    CHECK_EQ(r.reg(7) & 0x01, 0x01u);
    CHECK_EQ(r.reg(1) >> 4, 0x06u);
    r.packet({0x00});
    CHECK_EQ(r.reg(7) & 0x01, 0x00u);
    // READ CAPACITY
    r.packet({0x25});
    std::vector<u8> cap = r.read_all();
    CHECK_EQ(load_be<u32>(cap.data()), 99u);
    CHECK_EQ(load_be<u32>(cap.data() + 4), 2048u);
    // READ (10) of blocks 3 and 4, 2 KiB per DRQ block.
    r.packet({0x28, 0, 0, 0, 0, 3, 0, 0, 2, 0}, 2048);
    std::vector<u8> d = r.read_all();
    CHECK_EQ(d.size(), 4096u);
    CHECK_EQ(load_be<u32>(d.data()), 3u);
    CHECK_EQ(load_be<u32>(d.data() + 2048), 4u);
    CHECK_EQ(r.reg(7) & 0x01, 0x00u);
    // Past the end: illegal request, LBA out of range.
    r.packet({0x28, 0, 0, 0, 0, 99, 0, 0, 2, 0});
    CHECK_EQ(r.reg(7) & 0x01, 0x01u);
    r.packet({0x03, 0, 0, 0, 18, 0});
    std::vector<u8> sense = r.read_all();
    CHECK_EQ(sense[2], 0x05u);
    CHECK_EQ(sense[12], 0x21u);
    // READ TOC: track 1 at 0, lead-out at 100.
    r.packet({0x43, 0, 0, 0, 0, 0, 0, 0, 100, 0});
    std::vector<u8> toc = r.read_all();
    CHECK_EQ(toc.size(), 20u);
    CHECK_EQ(toc[6], 1u);
    CHECK_EQ(toc[14], 0xAAu);
    CHECK_EQ(load_be<u32>(toc.data() + 16), 100u);
}

TEST(atapi_cdrom_read_cd_builds_raw_sectors_and_dma_reads) {
    DiskImage img;
    CHECK(img.open(make_image("uv-cd2.iso", 20, 2048), true));
    Rig r;
    AtapiCdrom cd(r.sched, 1e6);
    r.ch.attach(0, &cd);
    cd.insert(&img);
    r.packet({0x03, 0, 0, 0, 18, 0});  // clears the unit attention
    r.read_all();
    // READ CD of LBA 7 with sync, header, user data and EDC/ECC: 2352 bytes.
    r.packet({0xBE, 0x08, 0, 0, 0, 7, 0, 0, 1, 0xB8, 0});
    std::vector<u8> s = r.read_all();
    CHECK_EQ(s.size(), 2352u);
    CHECK_EQ(s[0], 0x00u);
    CHECK_EQ(s[1], 0xFFu);
    CHECK_EQ(s[11], 0x00u);
    CHECK_EQ(s[12], 0x00u);  // 00:02:07 in BCD
    CHECK_EQ(s[13], 0x02u);
    CHECK_EQ(s[14], 0x07u);
    CHECK_EQ(s[15], 0x01u);
    CHECK_EQ(load_be<u32>(s.data() + 16), 7u);
    // The EDC: the CRC over sync, header, data and the EDC itself is zero.
    u32 crc = 0;
    for (unsigned i = 0; i < 0x814; ++i) {
        crc ^= s[i];
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ ((crc & 1) ? 0xD8018001u : 0);
    }
    CHECK_EQ(crc, 0u);
    // READ (10) by DMA.
    r.prd(0x4000, 2048);
    r.packet({0x28, 0, 0, 0, 0, 12, 0, 0, 1, 0}, 0xFFFE, true);
    r.ch.bm_write(0, 0x09);
    r.run(10000);
    CHECK(r.irq);
    CHECK_EQ(load_be<u32>(r.mem.data() + 0x4000), 12u);
    CHECK_EQ(r.reg(2), 0x03u);
    CHECK_EQ(r.reg(7) & 0x89, 0x00u);
}

TEST(cmd646_answers_on_bus_a_and_latches_interrupts) {
    Ultra5_10::Options o;
    o.ram_mb = 64;
    o.prom.assign(1u << 20, 0);
    store_be<u32>(o.prom.data() + 0x20, 0x10800000);
    store_be<u32>(o.prom.data() + 0x24, 0x01000000);
    Ultra5_10 m(o);
    auto cfg = [](unsigned bus, unsigned dev, unsigned reg) {
        return Sabre::kConfigBase | (bus << 16) | (dev << 11) | reg;
    };
    m.bus().write(cfg(0, 1, 0x100 + 0x18), byteswap(u32{0x00010100}));  // bus A is bus 1
    u32 v = 0;
    m.bus().read(cfg(1, 3, 0), v);
    CHECK_EQ(byteswap(v), 0x06461095u);
    m.bus().read(cfg(1, 3, 8), v);
    CHECK_EQ(byteswap(v), 0x01018A03u);  // both channels in compatibility mode at power-on
    m.bus().write(cfg(1, 3, 9), u8{0x8F});  // OBP switches both to native mode
    m.bus().read(cfg(1, 3, 8), v);
    CHECK_EQ(byteswap(v), 0x01018F03u);
    // The bus-master registers in configuration space: the PRD pointer.
    m.bus().write(cfg(1, 3, 0x74), byteswap(u32{0x12345678}));
    m.bus().read(cfg(1, 3, 0x74), v);
    CHECK_EQ(byteswap(v), 0x12345678u & ~3u);
    // The CD-ROM drive (secondary master) asserts INTRQ: CFR/ARTTIM23 and
    // MRDMODE latch it until cleared.
    AtaChannel& sec = m.ide().channel(1);
    sec.write_command(6, 1, 0xA0);
    sec.write_command(7, 1, 0xE5);  // CHECK POWER MODE
    m.run(100000);
    CHECK(sec.interrupt());
    u8 b = 0;
    m.bus().read(cfg(1, 3, 0x57), b);
    CHECK_EQ(b & 0x10, 0x10u);
    m.bus().read(cfg(1, 3, 0x71), b);
    CHECK_EQ(b & 0x0C, 0x08u);
    sec.read_command(7, 1);  // acknowledge at the drive
    CHECK(!sec.interrupt());
    m.bus().read(cfg(1, 3, 0x71), b);
    CHECK_EQ(b & 0x08, 0x08u);  // still latched
    m.bus().write(cfg(1, 3, 0x71), u8{0x08});
    m.bus().read(cfg(1, 3, 0x57), b);
    CHECK_EQ(b & 0x10, 0x00u);
}

// With device 1 absent, selecting it reads zero in every register,
// device/head included (device 0 does not answer for it), and alternate
// status too; selecting device 0 again finds it as it was.
TEST(ata_absent_device_reads_zero_in_every_register) {
    DiskImage img;
    CHECK(img.open(make_image("uv-ata-absent.img", 64, 512), false));
    Rig r;
    AtaDisk disk(r.sched, 1e6, img);
    r.ch.attach(0, &disk);
    r.reg(6, 0xB0);  // device 1
    for (unsigned reg = 1; reg <= 7; ++reg) CHECK_EQ(r.reg(reg), 0u);
    CHECK_EQ(r.ch.read_control(2), 0u);
    r.reg(6, 0xA0);  // device 0
    CHECK_EQ(r.reg(6), 0xA0u);
    CHECK_EQ(r.ch.read_control(2) & 0xC0, 0x40u);  // ready, not busy
}

// Fast: a 64 KiB READ DMA completes in about 70 us instead of the
// controller's 16.6 MB/s (about 4 ms), and the command is acted on within
// 2 us instead of 20; the same data, status and interrupt, in the same
// order.
TEST(ata_fast_disk_shortens_the_delays_not_the_sequence) {
    DiskImage img;
    CHECK(img.open(make_image("uv-ata-fast.img", 256, 512), false));
    auto read_time = [&](bool fast) {
        Rig r;  // (1 MHz: a cycle a microsecond)
        AtaDisk disk(r.sched, 1e6, img);
        r.ch.attach(0, &disk);
        disk.set_fast(fast);
        r.ch.set_fast(fast);
        r.prd(0x10000, 0x10000);
        r.lba28(0, 128);
        r.reg(7, 0xC8);
        CHECK(r.ch.read_control(2) & ata::kBsy);  // busy straight after the command either way
        r.ch.bm_write(0, 0x09);
        u64 t = 0;
        while (!r.irq && t < 100000) r.run(1), ++t;
        CHECK(r.irq);
        CHECK_EQ(r.ch.bm_read(2) & 0x07, 0x04u);
        CHECK_EQ(r.mem[0x10003], 0u);  // sector 0's own number (make_image's fill)
        CHECK_EQ(r.mem[0x10000 + 127 * 512 + 3], 127u);
        return t;
    };
    const u64 slow = read_time(false), fast = read_time(true);
    CHECK(slow > 3900);  // 65536 bytes at 16.6 MB/s, and 20 us
    CHECK(fast < 100);
}
