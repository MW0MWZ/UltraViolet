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
 *
 * Each test has a suspect (the part a failure names, by its code), a class
 * and, for the processor's, a sub-code: as an Ultra 10's POST, the first
 * failure updates the board descriptor by the class, shows the suspect on
 * the keyboard's LEDs and ends the run. The POST's working data is in the
 * NVRAM past the configuration variables (post.h).
 */
#include "post.h"

const char *post_message;
u64 post_board;
u64 post_prom_base;
volatile u32 trap_armed, trap_tt;

int post_max;
u32 ecache_size;
static u32 post_status;		/* 0: passed */
static const char *test_name;
static u32 test_id;		/* the running test's (ID below) */
static u32 cpu_clock;		/* MHz */
static u64 fail_pa;		/* a miscompare's address (post_compare_at) */
static int fail_at;

/* ---------------------------------------------------------------------
 * The NVRAM's wider fields.
 */
void nvram_st(u32 offset, u64 v, int bytes)
{
	while (bytes--)
		nvram_st8(offset + bytes, (u8)v), v >>= 8;
}

u64 nvram_ld(u32 offset, int bytes)
{
	u64 v = 0;
	int i;

	for (i = 0; i < bytes; i++)
		v = v << 8 | nvram_ld8(offset + i);
	return v;
}

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

/* Each character counted in the NVRAM's output column, 0 at a newline. */
static u32 column;

void post_putc(int c)
{
	ttya_tx(c);
	if (c == '\n')
		ttya_tx('\r');
	column = c == '\n' ? 0 : (column + 1) & 0xff;
	nvram_st8(NV_COLUMN, column);
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
 * The keyboard's LEDs (the Sun keyboard on the SuperIO's first UART):
 * SETLED (0e) and the mask, each byte once the transmitter is empty.
 */
#define KBD_PA		0x1fff13003f8ULL
#define LSR_THRE	0x20
#define SETLED		0x0e
#define LED_NUM		0x01
#define LED_COMPOSE	0x02
#define LED_SCROLL	0x04
#define LED_CAPS	0x08

static void kbd_tx(u32 b)
{
	u32 i;

	for (i = 0; i < 100000 && !(phys_ld8(KBD_PA + 5) & LSR_THRE); i++)
		;
	phys_st8(KBD_PA, b);
}

static void leds(u32 mask)
{
	kbd_tx(SETLED);
	kbd_tx(mask);
}

/* A pause the LEDs can be seen in (a fifth of a second: the length an
   Ultra 10 waits is not known). */
static void led_pause(void)
{
	u64 t0 = rd_tick(), n = (u64)(cpu_clock ? cpu_clock : 500) * 200000;

	while (rd_tick() - t0 < n)
		;
}

static u32 caps;			/* the heartbeat's Caps Lock */

static void heartbeat(void)
{
	caps ^= LED_CAPS;
	leds(caps);
}

/* The LEDs that name a suspect. */
static u32 suspect_leds(u32 code)
{
	static const u8 dimm[0x25 - 0x1b] = {
		0x0c, 0x01, 0x04, 0x00, 0x00,		/* No DIMM, DIMM1-4 */
		0x05, 0x02, 0x00, 0x00, 0x03,		/* Memory Bank 0-3, System Failure */
	};

	if (code <= 0x1a)
		return LED_CAPS | LED_NUM;
	return code < 0x25 ? dimm[code - 0x1b] : 0;	/* (BMX: none, a guess) */
}

/* ---------------------------------------------------------------------
 * Tests and their failures, laid out as an Ultra 10 lays them out.
 *
 * The suspects, by code.
 */
static const char *const suspects[] = {
	"CPU (Basic) ", "CPU (Functional) ", "CPU (Coherency) ", "FPU (Basic) ",
	"FPU (Functional) ", "DMMU (Basic) ", "DMMU (Functional) ", "IMMU (Basic) ",
	"IMMU (Functional) ", "ICache (Basic) ", "ICache (Functional) ", "DCache (Basic) ",
	"DCache (Functional) ", "ECache ", "ECache Data ", "ECache Tag ",
	"SDB ", "Saber IOMMU (Basic) ", "Sabre IOMMU (Functional) ", "Cheerio U24",
	"System Controller ", "CMD PCI0646U U22", "CS4231 U14", "SuperIO U21",
	"Siemens (tty a/b) U9", "NVRAM U13", "Flash PROM U16", "No DIMM",
	"DIMM1", "DIMM2", "DIMM3", "DIMM4",
	"Memory Bank 0", "Memory Bank 1", "Memory Bank 2", "Memory Bank 3",
	"System Failure",
};
#define SUSPECT_LAST	0x34		/* 25-34: "BMX " */

static const char *suspect_text(u32 code)
{
	return code < sizeof(suspects) / sizeof(suspects[0]) ? suspects[code] : "BMX ";
}

/* A test's identity: its suspect code, its class and (the processor's
   class) its sub-code. */
#define ID(code, class, sub)	((code) | (class) << 8 | (sub) << 12)
#define CL_NONE		0
#define CL_CPU		1		/* the processor module */
#define CL_CHEERIO	2		/* the RIO */
#define CL_NVRAM	3
#define CL_SC		4		/* the system controller */
#define CL_SABRE	5		/* the Sabre (functional) */
#define CL_MEMORY	6
#define CPU(code, sub)	ID(code, CL_CPU, sub)	/* sub: 0 IU, 1 FPU, 2 D-cache, */
						/* 3 I-cache, 4 D-MMU, 5 I-MMU, */
						/* 6 E-cache, 7 Sabre */
#define NVRAM		ID(0x19, CL_NVRAM, 0)
#define MEMORY		ID(0x1c, CL_MEMORY, 0)
#define CHEERIO		ID(0x13, CL_CHEERIO, 0)
#define SABRE(code)	ID(code, CL_SABRE, 0)
#define NONE(code)	ID(code, CL_NONE, 0)

/* The board descriptor: at the run's start, the Ultra 10's (module 0's
   bits 4:0 1b, the RIO's bit 25, the DIMMs' bits 47:32, the NVRAM's bit
   49); a failure clears its part's bit. */
#define BOARD_START	0x000233330200001bULL
#define BD_CPU		(1ULL << 4)	/* module 0 (4 + 4m for module m) */
#define BD_SC		(1ULL << 20)
#define BD_SABRE	(1ULL << 22)	/* the group's argument 1f's bit */
#define BD_CHEERIO	(1ULL << 25)
#define BD_DIMM(n)	(1ULL << (32 + (n)))
#define BD_NVRAM	(1ULL << 49)

static void board_failed(void)
{
	u64 bd = nvram_ld(NV_BOARD, 8);
	u32 n;

	switch (test_id >> 8 & 15) {
	case CL_CPU:
		bd = (bd & ~BD_CPU) | (test_id >> 12 & 7);
		break;
	case CL_CHEERIO:
		bd &= ~BD_CHEERIO;
		break;
	case CL_NVRAM:
		bd &= ~BD_NVRAM;
		break;
	case CL_SC:
		bd &= ~BD_SC;
		break;
	case CL_SABRE:
		bd &= ~BD_SABRE;
		break;
	case CL_MEMORY:
		/* A miscompare's DIMM (pair and side, then which DIMM of the
		   pair by address bit 3): its bit, and the suspect from DIMM1
		   on. */
		if (!fail_at)
			break;
		n = 2 * (u32)(fail_pa >> 28) + (u32)(fail_pa >> 3 & 1);
		if (n < 16)
			bd &= ~BD_DIMM(n);
		n += nvram_ld8(NV_SUSPECT);
		nvram_st8(NV_SUSPECT, n > SUSPECT_LAST ? SUSPECT_LAST : n);
		break;
	}
	nvram_st(NV_BOARD, bd, 8);
	nvram_st8(NV_PASS, 0);
}

int post_failed(void) { return post_status != 0; }

void post_fail(const char *msg)
{
	u32 code;

	if (post_failed())
		return;
	board_failed();
	code = nvram_ld8(NV_SUSPECT);
	leds(suspect_leds(code));
	led_pause();
	leds(0);
	post_puts("\n***********************\n");
	post_puts("STATUS =FAILED\n");
	post_puts("TEST   =");
	post_puts(test_name);
	post_puts("\nSUSPECT=");
	post_puts(suspect_text(code));
	post_puts("\nMESSAGE=");
	post_puts(msg);
	post_putc('\n');
	post_status = 1;
	post_message = suspect_text(code);
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
	fail_pa = adr;
	fail_at = 1;
	post_fail(msg);
	post_puts("\tAddress  ");
	post_putx64(adr);
	post_puts("\n\tExpected ");
	post_putx64(expected);
	post_puts("\n\tObserved ");
	post_putx64(observed);
	post_putc('\n');
}

/* A group: the keyboard's heartbeat, its name. */
static void group(const char *name)
{
	if (post_failed())
		return;
	heartbeat();
	post_puts(name);
	post_putc('\n');
}

/* What a test is, in the NVRAM once its line is out. */
static void running(const char *name, u32 id)
{
	test_name = name;
	test_id = id;
	fail_at = 0;
	nvram_st8(NV_SUSPECT, id & 0xff);
	nvram_st(NV_TEST, (u32)name, 8);
}

static void test(void (*fn)(void), const char *name, u32 id)
{
	if (post_failed())
		return;
	post_putc('\t');
	post_puts(name);
	post_puts(" Test\n");
	running(name, id);
	fn();
}

static void max_test(void (*fn)(void), const char *name, u32 id)
{
	if (post_max)
		test(fn, name, id);
}

/* A group that is itself a test (named with " Test", no tab). */
static void group_test(void (*fn)(void), const char *name, u32 id)
{
	if (post_failed())
		return;
	heartbeat();
	post_puts(name);
	post_puts(" Test\n");
	running(name, id);
	fn();
}

/* A step that is not a test (named as it is). */
static void step(void (*fn)(void), const char *name, u32 id)
{
	if (post_failed())
		return;
	post_putc('\t');
	post_puts(name);
	post_putc('\n');
	running(name, id);
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
	post_fail("Unexpected trap");
	post_puts("\tTT  ");
	post_putx(tt, 3);
	post_puts("\n\tTPC ");
	post_putx64(tpc);
	post_putc('\n');
	post_board = nvram_ld(NV_BOARD, 8);
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
	cpu_clock = (u32)((rd_tick() - t0 + 500000) / 1000000);
	nvram_st(NV_MHZ, cpu_clock, 4);
	return cpu_clock;
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
 * The steps that are the runner's own.
 */
#define CONFIG_PA(bus, dev, fn, reg) \
	(0x1fe01000000ULL | (bus) << 16 | (dev) << 11 | (fn) << 8 | (reg))
#define LITTLE		0x1d		/* bypass, little-endian */
#define EBUS_BASE	0xf1000000U	/* the RIO's BAR1: the EBus */
#define POST_MEMORY	0x100000U	/* where the image runs (post.ld) */
#define POST_STACK	0x0f0000U	/* (start.S) */

/* At each entry: the Sabre's bus numbers, the APB bridges' (A: 0, 80-ff;
   B: 0, 1-7f) and the RIO EBus's BAR1, so that the POST reaches the
   devices whatever was there before. */
static void pci_buses(void)
{
	asi_st8(CONFIG_PA(0, 0, 0, 0x40), LITTLE, 0);
	asi_st8(CONFIG_PA(0, 0, 0, 0x41), LITTLE, 0);
	asi_st8(CONFIG_PA(0, 1, 0, 0x18), LITTLE, 0);
	asi_st8(CONFIG_PA(0, 1, 0, 0x19), LITTLE, 0x80);
	asi_st8(CONFIG_PA(0, 1, 0, 0x1a), LITTLE, 0xff);
	asi_st8(CONFIG_PA(0, 1, 1, 0x18), LITTLE, 0);
	asi_st8(CONFIG_PA(0, 1, 1, 0x19), LITTLE, 1);
	asi_st8(CONFIG_PA(0, 1, 1, 0x1a), LITTLE, 0x7f);
	asi_st32(CONFIG_PA(1, 1, 0, 0x14), LITTLE, EBUS_BASE);
}

/* Init System BSS: the NVRAM's working fields. */
static void init_bss(void)
{
	nvram_st(NV_MEM_BASE, 0, 8);
	nvram_st(NV_MALLOC, 0, 8);
	nvram_st(NV_FLAGS, 0, 2);
	nvram_st(NV_EBUS, EBUS_BASE, 4);
}

static void probe_ec(void) { nvram_st(NV_ECACHE, ecache_size, 4); }

/* The POST's memory as the fields name it: from 0, the image's place
   (ours; an Ultra 10's are its own). */
static void malloc_post(void)
{
	nvram_st(NV_MEM_BASE, 0, 8);
	nvram_st(NV_MALLOC, POST_MEMORY, 8);
}

static void stack_pointers(void) { nvram_st(NV_STACK, POST_STACK, 8); }

/* ---------------------------------------------------------------------
 * The run. arg: 1 | diag-level << 8 | mfg-mode << 16, from the reset code:
 * bit 13 min, bit 14 max, bit 16 the chamber (manufacturing) mode.
 */
#define OPT_MAX		0x4000
#define OPT_CHAMBER	0x10000

u32 post_main(u32 arg)
{
	int i;

	nvram_st(NV_OPTIONS, arg, 8);
	pci_buses();
	post_status = 0;
	post_message = 0;
	post_board = 0;
	test_name = "";
	test_id = NONE(0x24);
	post_max = (arg & OPT_MAX) != 0;

	/* The run's start: auxio's LED on, the audio's auxiliary register
	   0, the test control flags clear. */
	asi_st32(0x1fff1726000ULL, LITTLE, 1);
	asi_st32(0x1fff1722000ULL, LITTLE, 0);
	nvram_st(NV_FLAGS, 0, 2);

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

	/* Caps Lock three times, a pause each (where in the start an Ultra
	   10 does this, and the pause's length, are not known). */
	for (i = 0; i < 3; i++) {
		heartbeat();
		led_pause();
	}

	/* The board descriptor as a passing run leaves it, and no failure
	   yet. */
	nvram_st(NV_BOARD, BOARD_START, 8);
	nvram_st8(NV_PASS, 1);

	group("Init POST BSS");
	step(init_bss, "Init System BSS", NVRAM);
	group("NVRAM");
	test(post_battery, "NVRAM Battery Detect", NVRAM);
	test(post_nv_addr, "NVRAM Scratch Addr", NVRAM);
	test(post_nv_data, "NVRAM Scratch Data", NVRAM);
	group("DMMU TLB Tags");
	test(post_dtlb_tags, "DMMU TLB Tag Access", CPU(0, 4));
	group("DMMU TLB RAM");
	test(post_dtlb_data, "DMMU TLB RAM Access", CPU(0, 4));
	group("Probe Ecache");
	step(probe_ec, "Probe Ecache", CPU(0x0d, 6));
	group("Ecache Tests");
	test(post_ec_ram_addr, "Ecache RAM Addr", CPU(0x0e, 6));
	test(post_ec_tag_addr, "Ecache Tag Addr", CPU(0x0f, 6));
	max_test(post_ec_ram, "Ecache RAM", CPU(0x0e, 6));
	max_test(post_ec_tag, "Ecache Tag", CPU(0x0f, 6));
	group("All CPU Basic Tests");
	test(post_v9, "V9 Instruction", CPU(0, 0));
	max_test(post_tick, "CPU Tick and Tick Compare Reg", CPU(0, 0));
	test(post_soft_trap, "CPU Soft Trap", CPU(0, 0));
	test(post_softint, "CPU Softint Reg and Int", CPU(0, 0));
	group("All Basic MMU Tests");
	test(post_pctx, "DMMU Primary Context Reg", CPU(0, 4));
	test(post_sctx, "DMMU Secondary Context Reg", CPU(0, 4));
	test(post_dtsb, "DMMU TSB Reg", CPU(0, 4));
	test(post_dtag, "DMMU Tag Access Reg", CPU(0, 4));
	max_test(post_vawp, "DMMU VA Watchpoint Reg", CPU(0, 4));
	max_test(post_pawp, "DMMU PA Watchpoint Reg", CPU(0, 4));
	test(post_itsb, "IMMU TSB Reg", CPU(0, 5));
	test(post_itag, "IMMU Tag Access Reg", CPU(0, 5));
	group("All Basic Cache Tests");
	test(post_dcache_ram, "Dcache RAM", CPU(0, 2));
	max_test(post_dcache_tag, "Dcache Tag", CPU(0, 2));
	test(post_icache_ram, "Icache RAM", CPU(0, 3));
	max_test(post_icache_tag, "Icache Tag", CPU(0, 3));
	max_test(post_icache_next, "Icache Next", CPU(0, 3));
	max_test(post_icache_predecode, "Icache Predecode", CPU(0, 3));
	group("Memory Probe");
	step(post_banks, "Probe Memory", NONE(0x20));
	group("Sabre MCU Control & Status Regs Init and Tests");
	step(post_mcu_init, "Init Sabre MCU Control & Status Regs", ID(0x14, CL_SC, 0));
	group("Memory Init");
	test(post_ec_access, "Ecache Access", CPU(0x0d, 6));
	step(malloc_post, "Malloc Post Memory", NONE(0x24));
	step(post_area_addr, "Memory Addr with Ecache", MEMORY);
	step(0, "Load Post In Memory", MEMORY);
	step(post_load, "Run POST from MEM", MEMORY);	/* (as Load Post In Memory: a guess) */
	step(post_map, "Map PROM/STACK/NVRAM in DMMU", CPU(0, 0));
	step(stack_pointers, "Update Master Stack/Frame Pointers", CPU(0, 0));
	group("All FPU Basic Tests");
	test(post_fpu_regs, "FPU Regs", CPU(0, 1));
	test(post_fpu_move, "FPU Move Regs", CPU(0, 1));
	max_test(post_fsr, "FPU State Reg", CPU(0, 1));
	max_test(post_fpu_func, "FPU Functional", CPU(0, 1));
	max_test(post_fpu_trap, "FPU Trap", CPU(0, 1));
	group_test(post_upa, "UPA Data Bus Line", NONE(0x10));	/* (SDB, no class: a guess) */
	group("Memory Tests");
	step(post_init_memory, "Init Memory", NONE(0x24));
	max_test(post_mem_addr_ec, "Memory Addr with Ecache", MEMORY);
	test(post_mem_addr_ecc, "ECC Memory Addr", MEMORY);
	max_test(post_blk_addr, "Block Memory Addr", MEMORY);
	max_test(post_blk, "Block Memory", MEMORY);
	max_test(post_ecc_blk, "ECC Blk Memory", MEMORY);
	group("All Basic Sabre MMU Tests");
	step(post_init_sabre, "Init Sabre", CPU(0, 7));
	max_test(post_pio_decoder, "PIO Decoder and BCT", CPU(0, 7));
	max_test(post_byte_enable, "PCI Byte Enable", CPU(0, 7));
	test(post_imap, "Interrupt Map (short) Reg", CPU(0, 7));
	test(post_iclr, "Interrupt Set/Clr Reg", CPU(0, 7));
	test(post_iommu_regs, "Sabre IOMMU Regs", CPU(0, 7));
	test(post_iommu_ram, "Sabre IOMMU RAM Address", CPU(0, 7));
	test(post_iommu_cam, "Sabre IOMMU CAM Address", CPU(0, 7));
	max_test(post_iommu_compare, "IOMMU TLB Compare", CPU(0, 7));
	max_test(post_iommu_flush, "IOMMU TLB Flush", CPU(0, 7));
	test(post_pbm_config, "PBMA PCI Config Space Regs", SABRE(0x11));
	test(post_pbm_ctl, "PBMA Control/Status Reg", CPU(0, 7));
	test(post_pbm_diag, "PBMA Diag Reg", CPU(0, 7));
	test(post_sabre_io, "Sabre IO Regs", CPU(0, 7));
	group("All Advanced CPU Tests");
	max_test(post_dmmu_hit_miss, "DMMU Hit/Miss", CPU(0, 4));
	max_test(post_immu_hit_miss, "IMMU Hit/Miss", CPU(0, 5));
	max_test(post_little, "DMMU Little Endian", CPU(1, 4));
	test(post_iu_asi, "IU ASI Access", CPU(0, 0));
	test(post_fpu_asi, "FPU ASI Access", CPU(0, 0));
	max_test(post_ec_thrash, "Ecache Thrash", CPU(0x0d, 6));
	group("All CPU Error Reporting Tests");
	test(post_data_access, "CPU Data Access Trap", CPU(1, 0));
	test(post_align, "CPU Addr Align Trap", CPU(1, 0));
	test(post_priv, "DMMU Access Priv Page", CPU(1, 4));
	test(post_protect, "DMMU Write Protected Page", CPU(1, 4));
	group("All Advanced Sabre IOMMU Tests");
	step(post_init_sabre_dma, "Init Sabre", CPU(0, 7));
	test(post_dma_rd_miss, "Consist DMA Rd, IOMMU miss Ebus", CPU(0, 7));
	max_test(post_dma_rd_hit, "Consist DMA Rd, IOMMU hit Ebus", CPU(0, 7));
	max_test(post_dma_wr_miss, "Consist DMA Wr, IOMMU miss Ebus", CPU(0, 7));
	max_test(post_dma_wr_hit, "Consist DMA Wr, IOMMU hit Ebus", CPU(0, 7));
	max_test(post_dma_pass_rd, "Pass-Thru DMA Rd, Ebus device", CPU(0, 7));
	max_test(post_dma_pass_wr, "Pass-Thru DMA Wr, Ebus device", CPU(0, 7));
	max_test(post_dma_rd_lock, "Consist DMA Rd, IOMMU LRU Lock Ebus", CPU(0, 7));
	max_test(post_dma_wr_lock, "Consist DMA Wr, IOMMU LRU Locked Ebus", CPU(0, 7));
	group("All Basic Cheerio Tests");
	test(post_rio_ebus_config, "Cheerio Ebus PCI Config Space", CHEERIO);
	test(post_rio_net_config, "Cheerio Ethernet PCI Config Space", CHEERIO);
	step(0, "Cheerio Init", CHEERIO);	/* (an Ultra 10's sets up nothing here) */
	group("All Sabre IOMMU Error Reporting Tests");
	step(post_init_sabre_err, "Init Sabre", SABRE(0x11));
	test(post_pio_master_abort, "PIO Read, Master Abort", SABRE(0x12));
	test(post_pio_target_abort, "PIO Read, Target Abort", SABRE(0x12));

	/* The result: the status by the pass flag, the board descriptor as
	   the NVRAM holds it; on the LEDs, a processor's failure (0e) or
	   another module's (09). */
	post_board = nvram_ld(NV_BOARD, 8);
	post_status = nvram_ld8(NV_PASS) != 1;
	if (!(post_board & BD_CPU))
		leds(0x0e);
	else if (!(post_board & 1))
		leds(0x09);

	post_puts("Status of this POST run:\t");
	post_puts(post_status ? "FAIL\n" : "PASS\n");
	post_puts("manfacturing mode=");
	post_puts(arg & OPT_CHAMBER ? "CHAMBER\n" : "OFF\n");
	post_time();
	post_putc('\n');
	return post_status;
}
