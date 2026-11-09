/*
 * Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
 * OpenBoot release it is part of (see LICENSE).
 *
 * The power-on selftest (POST), a program of its own in the flash PROM's
 * second half, as the Ultra 10 has one: the reset code runs it at a
 * hardware power-on with diag-switch? true and diag-level not off, and it
 * reports on ttya, whatever the console. It runs its groups of tests, each
 * a line and its tests a tab in, stops at the first failure with the test,
 * the part suspected and why, gives the run's status and the time, and
 * returns to the reset code, which shows the status and starts again with
 * a software power-on reset; OpenBoot then has the status, the part
 * suspected and the board descriptor (show-post-results), and a failure
 * keeps it from booting by itself. diag-level min runs the tests an Ultra
 * 10 runs then, max all of them.
 */
#include "post.h"

const char *post_message;
u64 post_board;
u64 post_prom_base;
volatile u32 trap_armed, trap_tt;

int post_max;
u32 ecache_size;
static u32 post_status;		/* 0: passed */
static const char *test_name, *suspect;

/* ---------------------------------------------------------------------
 * Output: ttya, by the SAB82532's transmit-immediate register.
 */
#define STAR	0x20		/* status */
#define TIC	0x35		/* transmit immediate */
#define STAR_TEC 0x08		/* TIC executing */

static void ttya_tx(int c)
{
	while (phys_ld8(TTYA_PA + STAR) & STAR_TEC)
		;
	phys_st8(TTYA_PA + TIC, (u8)c);
}

void post_putc(int c)
{
	ttya_tx(c);
	if (c == '\n')
		ttya_tx('\r');
}

void post_puts(const char *s)
{
	while (*s)
		post_putc(*s++);
}

void post_putd(u32 n, int width, int zero)
{
	char buf[12];
	int i = 0;

	do {
		buf[i++] = '0' + n % 10;
		n /= 10;
	} while (n);
	while (i < width--)
		post_putc(zero ? '0' : ' ');
	while (i)
		post_putc(buf[--i]);
}

void post_putx(u64 n, int digits)
{
	while (digits--)
		post_putc("0123456789abcdef"[(n >> (4 * digits)) & 15]);
}

void post_putx64(u64 n)
{
	post_putx(n >> 32, 8);
	post_putc('.');
	post_putx(n, 8);
}

/* ---------------------------------------------------------------------
 * Tests and their failures, laid out as an Ultra 10 lays them out.
 */
void post_suspect(const char *part) { suspect = part; }
int post_failed(void) { return post_status != 0; }

void post_fail(const char *msg)
{
	if (post_failed())
		return;
	post_puts("***********************\n");
	post_puts("STATUS =FAILED\n");
	post_puts("TEST   =");
	post_puts(test_name);
	post_puts("\nSUSPECT=");
	post_puts(suspect);
	post_puts(" \nMESSAGE=");
	post_puts(msg);
	post_putc('\n');
	post_status = 1;
	post_message = suspect;
}

void post_compare(u64 observed, u64 expected, const char *msg)
{
	if (observed == expected || post_failed())
		return;
	post_fail(msg);
	post_puts("\tExpected ");
	post_putx64(expected);
	post_puts("\n\tObserved ");
	post_putx64(observed);
	post_putc('\n');
}

void post_compare_at(u64 observed, u64 expected, u64 adr, const char *msg)
{
	if (observed == expected || post_failed())
		return;
	post_fail(msg);
	post_puts("\tAddress  ");
	post_putx64(adr);
	post_puts("\n\tExpected ");
	post_putx64(expected);
	post_puts("\n\tObserved ");
	post_putx64(observed);
	post_putc('\n');
}

static void group(const char *name)
{
	if (post_failed())
		return;
	post_puts(name);
	post_putc('\n');
}

static void test(void (*fn)(void), const char *name)
{
	if (post_failed())
		return;
	test_name = name;
	post_putc('\t');
	post_puts(name);
	post_puts(" Test\n");
	fn();
}

static void max_test(void (*fn)(void), const char *name)
{
	if (post_max)
		test(fn, name);
}

/* A group that is itself a test (named with " Test", no tab). */
static void group_test(void (*fn)(void), const char *name)
{
	if (post_failed())
		return;
	test_name = name;
	post_puts(name);
	post_puts(" Test\n");
	fn();
}

/* A step that is not a test (named as it is). */
static void step(void (*fn)(void), const char *name)
{
	if (post_failed())
		return;
	post_putc('\t');
	post_puts(name);
	post_putc('\n');
	if (fn)
		fn();
}

u32 trap_run(void (*fn)(void))
{
	trap_tt = 0;
	trap_armed = 1;
	fn();
	trap_armed = 0;
	return trap_tt;
}

/* A trap the POST did not expect: what and where, as a failure. */
void post_unexpected_trap(u32 tt, u64 tpc)
{
	if (!suspect || !*suspect)
		suspect = "CPU";
	post_fail("Unexpected trap");
	post_puts("\tTT  ");
	post_putx(tt, 3);
	post_puts("\n\tTPC ");
	post_putx64(tpc);
	post_putc('\n');
}

/* ---------------------------------------------------------------------
 * The clock: the M48T59's registers at the NVRAM's top, read with the
 * READ bit set (so they hold still), in BCD; the year counted from 1968.
 */
#define CLK_CTL	 0x1ff8
#define CLK_READ 0x40

static u32 bcd(u8 b) { return (b >> 4) * 10 + (b & 15); }

static void post_time(void)
{
	u32 sec, min, hour, date, month, year;

	nvram_st8(CLK_CTL, nvram_ld8(CLK_CTL) | CLK_READ);
	sec = bcd(nvram_ld8(0x1ff9) & 0x7f);
	min = bcd(nvram_ld8(0x1ffa) & 0x7f);
	hour = bcd(nvram_ld8(0x1ffb) & 0x3f);
	date = bcd(nvram_ld8(0x1ffd) & 0x3f);
	month = bcd(nvram_ld8(0x1ffe) & 0x1f);
	year = bcd(nvram_ld8(0x1fff));
	nvram_st8(CLK_CTL, nvram_ld8(CLK_CTL) & ~CLK_READ);
	year += 1968;			/* (as Solaris keeps it) */

	post_puts("Time Stamp [hour:min:sec] ");
	post_putd(hour, 2, 1);
	post_putc(':');
	post_putd(min, 2, 1);
	post_putc(':');
	post_putd(sec, 2, 1);
	post_puts("  [month/date year] ");
	post_putd(month, 2, 1);
	post_putc('/');
	post_putd(date, 2, 1);
	post_putc(' ');
	post_putd(year, 0, 0);
	post_putc('\n');
}

/* The processor's clock: the tick counted over a second of the clock's. */
static u8 clock_second(void) { return nvram_ld8(0x1ff9) & 0x7f; }

static u32 cpu_mhz(void)
{
	u8 s = clock_second();
	u64 t0;

	while (clock_second() == s)
		;
	s = clock_second();
	t0 = rd_tick();
	while (clock_second() == s)
		;
	return (u32)((rd_tick() - t0 + 500000) / 1000000);
}

/* The E-cache's size: through its diagnostic data access, written at
   2 MiB, 1 MiB, 512 KiB and 256 KiB above 0 and read back at 0. */
static u32 probe_ecache(void)
{
	const u64 data = 1ULL << 39;
	u32 n;

	asi_stx(data, 0x76, 0);
	for (n = 0x200000; n >= 0x40000; n >>= 1)
		asi_stx(data + n, 0x76, n);
	return (u32)asi_ldx(data, 0x7e);
}

/* The name the Ultra 10's POST gives the processor (its version's
   implementation, 0x12 for the IIi). */
static const char *cpu_name(void)
{
	return (rd_ver() >> 32 & 0xffff) == 0x12 ? "UltraSPARC-LC" : "UltraSPARC";
}

/* ---------------------------------------------------------------------
 * The run. arg: 1 | diag-level << 8 | mfg-mode << 16, from the reset code.
 */
u32 post_main(u32 arg)
{
	u32 mfg = (arg >> 16) & 0xff;

	post_status = 0;
	post_message = 0;
	post_board = 0;
	suspect = "";
	post_max = ((arg >> 8) & 0xff) == 0x40;
	ecache_size = probe_ecache();

	post_puts("@(#) Ultra 5/10 POST 0.1, OpenBoot 4.23.0, built " POST_DATE "\n");
	post_putc('\n');
	post_puts("CPU: ");
	post_puts(cpu_name());
	post_puts(" (Clock Frequency: ");
	post_putd(cpu_mhz(), 0, 0);
	post_puts("MHz, Ecache Size: ");
	post_putd(ecache_size >> 10, 0, 0);
	post_puts("KB)\n");
	post_putc('\n');
	group("Init POST BSS");
	step(0, "Init System BSS");

	group("NVRAM");
	test(post_battery, "NVRAM Battery Detect");
	test(post_nv_addr, "NVRAM Scratch Addr");
	test(post_nv_data, "NVRAM Scratch Data");
	group("DMMU TLB Tags");
	test(post_dtlb_tags, "DMMU TLB Tag Access");
	group("DMMU TLB RAM");
	test(post_dtlb_data, "DMMU TLB RAM Access");
	group("Probe Ecache");
	step(post_probe_ec, "Probe Ecache");
	group("Ecache Tests");
	test(post_ec_ram_addr, "Ecache RAM Addr");
	test(post_ec_tag_addr, "Ecache Tag Addr");
	max_test(post_ec_ram, "Ecache RAM");
	max_test(post_ec_tag, "Ecache Tag");
	group("All CPU Basic Tests");
	test(post_v9, "V9 Instruction");
	max_test(post_tick, "CPU Tick and Tick Compare Reg");
	test(post_soft_trap, "CPU Soft Trap");
	test(post_softint, "CPU Softint Reg and Int");
	group("All Basic MMU Tests");
	test(post_pctx, "DMMU Primary Context Reg");
	test(post_sctx, "DMMU Secondary Context Reg");
	test(post_dtsb, "DMMU TSB Reg");
	test(post_dtag, "DMMU Tag Access Reg");
	max_test(post_vawp, "DMMU VA Watchpoint Reg");
	max_test(post_pawp, "DMMU PA Watchpoint Reg");
	test(post_itsb, "IMMU TSB Reg");
	test(post_itag, "IMMU Tag Access Reg");
	group("All Basic Cache Tests");
	test(post_dcache_ram, "Dcache RAM");
	max_test(post_dcache_tag, "Dcache Tag");
	test(post_icache_ram, "Icache RAM");
	max_test(post_icache_tag, "Icache Tag");
	max_test(post_icache_next, "Icache Next");
	max_test(post_icache_predecode, "Icache Predecode");
	group("Memory Probe");
	step(post_banks, "Probe Memory");
	group("Sabre MCU Control & Status Regs Init and Tests");
	step(post_mcu_init, "Init Sabre MCU Control & Status Regs");
	group("Memory Init");
	test(post_ec_access, "Ecache Access");
	step(0, "Malloc Post Memory");
	step(post_area_addr, "Memory Addr with Ecache");
	step(0, "Load Post In Memory");
	step(post_load, "Run POST from MEM");
	step(post_map, "Map PROM/STACK/NVRAM in DMMU");
	step(0, "Update Master Stack/Frame Pointers");
	group("All FPU Basic Tests");
	test(post_fpu_regs, "FPU Regs");
	test(post_fpu_move, "FPU Move Regs");
	max_test(post_fsr, "FPU State Reg");
	max_test(post_fpu_func, "FPU Functional");
	max_test(post_fpu_trap, "FPU Trap");
	group_test(post_upa, "UPA Data Bus Line");
	group("Memory Tests");
	step(post_init_memory, "Init Memory");
	max_test(post_mem_addr_ec, "Memory Addr with Ecache");
	test(post_mem_addr_ecc, "ECC Memory Addr");
	max_test(post_blk_addr, "Block Memory Addr");
	max_test(post_blk, "Block Memory");
	max_test(post_ecc_blk, "ECC Blk Memory");
	group("All Basic Sabre MMU Tests");
	step(post_init_sabre, "Init Sabre");
	max_test(post_pio_decoder, "PIO Decoder and BCT");
	max_test(post_byte_enable, "PCI Byte Enable");
	test(post_imap, "Interrupt Map (short) Reg");
	test(post_iclr, "Interrupt Set/Clr Reg");
	test(post_iommu_regs, "Sabre IOMMU Regs");
	test(post_iommu_ram, "Sabre IOMMU RAM Address");
	test(post_iommu_cam, "Sabre IOMMU CAM Address");
	max_test(post_iommu_compare, "IOMMU TLB Compare");
	max_test(post_iommu_flush, "IOMMU TLB Flush");
	test(post_pbm_config, "PBMA PCI Config Space Regs");
	test(post_pbm_ctl, "PBMA Control/Status Reg");
	test(post_pbm_diag, "PBMA Diag Reg");
	test(post_sabre_io, "Sabre IO Regs");
	group("All Advanced CPU Tests");
	max_test(post_dmmu_hit_miss, "DMMU Hit/Miss");
	max_test(post_immu_hit_miss, "IMMU Hit/Miss");
	max_test(post_little, "DMMU Little Endian");
	test(post_iu_asi, "IU ASI Access");
	test(post_fpu_asi, "FPU ASI Access");
	max_test(post_ec_thrash, "Ecache Thrash");
	group("All CPU Error Reporting Tests");
	test(post_data_access, "CPU Data Access Trap");
	test(post_align, "CPU Addr Align Trap");
	test(post_priv, "DMMU Access Priv Page");
	test(post_protect, "DMMU Write Protected Page");

	post_puts("Status of this POST run:\t");
	post_puts(post_status ? "FAIL\n" : "PASS\n");
	post_puts("manfacturing mode=");
	post_puts(mfg ? "ON\n" : "OFF\n");
	post_time();
	post_putc('\n');
	return post_status;
}
