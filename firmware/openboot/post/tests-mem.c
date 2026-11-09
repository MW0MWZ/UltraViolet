/*
 * Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
 * OpenBoot release it is part of (see LICENSE).
 *
 * The POST's memory, FPU and UPA data bus tests.
 *
 * The memory is probed as an Ultra 10's POST probes it: the two pairs of
 * DIMM sockets (banks 0 and 2, at 0 and 256 MiB), each DIMM (+0, +8)
 * looked for and sized with 10-bit columns, its DRAMs' column width found
 * with 11-bit columns (a marker 16 KiB up landing on +0: 10-bit), each
 * pair's second side (stack 1) 512 MiB above. 11-bit mode only if every
 * pair present has 11-bit DIMMs; a pair of each is a mode mismatch, run
 * in 10-bit mode with half the 11-bit pair. (The reset code chooses
 * again after the POST, for OpenBoot.) The POST keeps the first 8 MiB of
 * bank 0 for itself; the tests cover the rest, a megabyte at a time.
 */
#include "post.h"

#define MB		0x100000U
#define POST_AREA	(8 * MB)		/* bank 0's first 8 MiB */
#define STACK1		0x20000000ULL

/* start.S/cpu.S */
void mem_fill(u64 pa, u32 len, u64 x, u32 asi);
u32 mem_check(u64 pa, u32 len, u64 x, u32 asi, u64 *out);
void mem_fill_addr(u64 pa, u32 len, u32 asi);
u32 mem_check_addr(u64 pa, u32 len, u32 asi, u64 *out);
void blk_fill(u64 pa, u32 len, u64 *buf);
u32 blk_check(u64 pa, u32 len, u64 x, u64 *buf, u64 *out);
void blk_fill_addr(u64 pa, u32 len, u64 *buf);
u32 blk_check_addr(u64 pa, u32 len, u64 *buf, u64 *out);
u64 afsr_get(void);
void afsr_clear(void);
void fregs(u64 *buf);
void fsr_test(u64 *buf);
void fops(u64 *buf);
void fdiv0(u64 *buf);

static u64 ld64(u64 pa) { return asi_ldx(pa, 0x15); }
static void st64(u64 pa, u64 v) { asi_stx(pa, 0x15, v); }

/* ---------------------------------------------------------------------
 * The probe.
 */
struct bank {
	int present;		/* both DIMMs answer */
	int col11;		/* both have 11-bit columns */
	int stacked;		/* both have a second side */
	u32 dimm;		/* a DIMM's size with 10-bit columns (one side) */
	u32 side;		/* a side's size, as the mode makes it */
};
static struct bank banks[2];	/* banks 0 and 2 */
static int mode11;

static u64 bank_base(int b) { return (u64)(b * 2) << 27; }

/* The probe's result (start.S, which probes from the PROM before the
   POST runs from memory): per bank, bit 0 present, 1 11-bit columns, 2 a
   second side, 31:8 a DIMM's size in MiB with 10-bit columns; the mode. */
u32 probe_result[3];

static void putmb(u32 bytes, int width)
{
	post_putd(bytes / MB, width, 0);
	post_puts("MB");
}

static void probe_memory(void)
{
	int b, n = 0, n11 = 0;

	post_suspect("Memory");
	for (b = 0; b < 2; b++) {
		struct bank *k = &banks[b];
		u32 r = probe_result[b];

		k->present = r & 1;
		if (!k->present)
			continue;
		k->col11 = (r & 2) != 0;
		k->stacked = (r & 4) != 0;
		k->dimm = (r >> 8) * MB;
		n++;
		n11 += k->col11;
	}
	mode11 = probe_result[2];
	if (n == 2 && n11 == 1) {
		b = banks[0].col11 ? 0 : 1;
		post_puts("WARNING: MODE MISMATCH in bank 0, 2, Bank ");
		post_putd(b * 2, 0, 0);
		post_puts(" has 11 bit column \naddressing\n Programming MCU in 10 bit mode. "
			  "Only one half of bank ");
		post_putd(b * 2, 0, 0);
		post_puts(" is used \n\n");
	} else if (n == 2 && mode11) {
		post_puts("\t\tINFO: 11 bit column addressing detected\n");
	} else if (n && !mode11) {
		post_puts("\t\tINFO: All the memory banks in 10 bit column mode\n");
	}
	for (b = 0; b < 2; b++) {
		struct bank *k = &banks[b];
		u32 total;

		k->side = !k->present ? 0 : 2 * k->dimm * (mode11 ? 2 : 1);
		total = k->side * (k->stacked ? 2 : 1);
		if (total) {
			post_puts("\t\tINFO:  ");
			putmb(total, 5);
			post_puts(" Bank ");
		} else {
			post_puts("\t\tbank ");
		}
		post_putd(b * 2, 0, 0);
		if (!total) {
			post_puts(":\t");
			putmb(0, 3);
		}
		post_putc('\n');
	}
}

void post_banks(void) { probe_memory(); }

/* ---------------------------------------------------------------------
 * The memory controller and the POST's own memory.
 */
void post_mcu_init(void)
{
	post_puts("\t\tInitializing SC registers in SabreIO\n");
}

/* Its memory, through the E-cache: each doubleword its address, read
   back (the POST's area above its own image). */
void post_area_addr(void)
{
	u64 out[2];

	post_suspect("Memory");
	mem_fill_addr(2 * MB, POST_AREA - 2 * MB, 0x14);
	if (mem_check_addr(2 * MB, POST_AREA - 2 * MB, 0x14, out))
		post_compare_at(out[0], out[1], out[1], "Memory address mismatch");
}

extern char _start[], _image_end[];

/* The image, as it runs, against the PROM's: a dot for each tenth. */
void post_load(void)
{
	u32 size = _image_end - _start, i, w, mark = size / 10, dots = 0;

	post_suspect("Memory");
	post_putc('\t');
	for (i = 0; i < size; i += 4) {
		w = *(u32 *)(_start + i);
		if (w != phys_ld32(post_prom_base + i)) {
			post_putc('\n');
			post_compare_at(w, phys_ld32(post_prom_base + i), (u32)(_start + i),
					"POST image mismatch");
			return;
		}
		if (i + 4 >= mark && dots < 10) {
			post_putc('.');
			dots++;
			mark += size / 10;
		}
	}
	post_puts("\nloaded POST in memory \n");
}

/* ---------------------------------------------------------------------
 * The FPU.
 */
static u64 fbuf[40] __attribute__((aligned(8)));

void post_fpu_regs(void)
{
	static const u64 pat[4] = {
		0x5555555555555555ULL, 0xaaaaaaaaaaaaaaaaULL, 0, ~0ULL
	};
	u32 p, i;

	post_suspect("FPU");
	for (p = 0; p < 4; p++) {
		fbuf[0] = pat[p];
		fregs(fbuf);
		for (i = 0; i < 32; i++)
			post_compare(fbuf[i + 1], pat[p], "FP register");
	}
}

static void fops_run(u64 a, u64 b)
{
	fbuf[0] = a;
	fbuf[1] = b;
	fops(fbuf);
}

static void fop(u32 n, u64 expected, const char *msg)
{
	post_compare(fbuf[n + 2], expected, msg);
}

void post_fpu_move(void)
{
	post_suspect("FPU");
	fops_run(0x3ff8000000000000ULL, 0x4002000000000000ULL);	/* 1.5, 2.25 */
	fop(4, 0xbff8000000000000ULL, "FNEGd");
	fop(5, 0x3ff8000000000000ULL, "FABSd");
	fop(6, 0x3ff8000000000000ULL, "FMOVd");
}

/* FSR: RD, TEM, fcc0-3, aexc and cexc are kept. */
#define FSR_MASK 0x3fcf800fffULL

void post_fsr(void)
{
	static const u64 pat[4] = { 0x154a800aaaULL, 0x2a85000555ULL, FSR_MASK, 0 };
	u32 p;

	post_suspect("FPU");
	for (p = 0; p < 4; p++) {
		fbuf[0] = pat[p];
		fsr_test(fbuf);
		post_compare(fbuf[1] & FSR_MASK, pat[p], "FSR");
	}
}

void post_fpu_func(void)
{
	post_suspect("FPU");
	fops_run(0x3ff8000000000000ULL, 0x4002000000000000ULL);	/* 1.5, 2.25 */
	fop(0, 0x400e000000000000ULL, "FADDd");
	fop(1, 0x400b000000000000ULL, "FMULd");
	fop(2, 0x3fe5555555555555ULL, "FDIVd");
	fop(3, 0x3ff8000000000000ULL, "FSQRTd");
	fops_run(0x3ff0000000000000ULL, 0x4000000000000000ULL);	/* 1, 2 */
	fop(3, 0x3ff6a09e667f3bcdULL, "FSQRTd");
}

/* A division by zero with its trap enabled (FSR TEM.DZM, bit 24): an
   IEEE 754 exception (trap 21), the FSR's cexc then DZ. */
static void fdiv0_run(void) { fdiv0(fbuf); }

void post_fpu_trap(void)
{
	post_suspect("FPU");
	fbuf[0] = 0x1000000;
	fbuf[1] = 0x3ff0000000000000ULL;
	fbuf[2] = 0;
	post_compare(trap_run(fdiv0_run), 0x21, "FP exception trap not taken");
	post_compare(fbuf[4] & 0x1f, 2, "FSR cexc");
}

/* ---------------------------------------------------------------------
 * The UPA data bus: a doubleword of the POST's memory, around the
 * E-cache, with each line on its own (walking ones and zeros).
 */
#define UPA_SCRATCH (POST_AREA - 64)

void post_upa(void)
{
	u64 x;
	u32 i;

	post_suspect("UPA Data Bus");
	for (i = 0; i < 64; i++) {
		x = 1ULL << i;
		st64(UPA_SCRATCH, x);
		post_compare(ld64(UPA_SCRATCH), x, "UPA data bus line");
		st64(UPA_SCRATCH, ~x);
		post_compare(ld64(UPA_SCRATCH), ~x, "UPA data bus line");
	}
}

/* ---------------------------------------------------------------------
 * The memory tests: each bank's sides ("stacks"), as INFO lines, the
 * memory a megabyte at a time (bank 0's first 8 MiB the POST's).
 */
typedef void (*range_fn)(u64 pa, u32 len);
static int dots;

static void info(int b, int s, u32 size)
{
	post_puts("\t\tINFO:  ");
	putmb(size, 5);
	post_puts(" at bank ");
	post_putd(b * 2, 0, 0);
	post_puts(" stack ");
	post_putd(s, 0, 0);
	if (size)
		post_puts(" (2 dimms per bank)");
	post_putc('\n');
}

static void chunks(u64 pa, u32 size, range_fn fn)
{
	u32 off;

	for (off = 0; off < size && !post_failed(); off += MB) {
		fn(pa + off, MB);
		if (dots)
			post_putc('.');
	}
}

/* fn over a side's memory (bank b, stack s). */
static void side_range(int b, int s, range_fn fn)
{
	u64 pa = bank_base(b) | (s ? STACK1 : 0);
	u32 size = banks[b].side;

	if (b == 0 && s == 0) {
		pa += POST_AREA;
		size -= POST_AREA;
	}
	chunks(pa, size, fn);
}

static int side_present(int b, int s)
{
	return banks[b].present && (s == 0 || banks[b].stacked);
}

/* Each side: its INFO line, then (if present) per-side work. */
static void per_side(void (*fn)(int b, int s))
{
	int b, s;

	for (b = 0; b < 2; b++)
		for (s = 0; s < 2; s++) {
			if (post_failed())
				return;
			info(b, s, side_present(b, s) ? banks[b].side : 0);
			if (side_present(b, s))
				fn(b, s);
		}
}

/* Written then read back with xw and xr. */
static range_fn pass_w, pass_r;

static void side_pass(int b, int s)
{
	side_range(b, s, pass_w);
	if (dots)
		post_putc('\n');
	if (dots && !post_failed())
		post_puts("Read ");
	side_range(b, s, pass_r);
	if (dots)
		post_putc('\n');
}

static u64 pattern;
static u32 mem_asi;
static u64 blk_buf[16] __attribute__((aligned(64)));

static void w_fill(u64 pa, u32 len) { mem_fill(pa, len, pattern, mem_asi); }
static void w_addr(u64 pa, u32 len) { mem_fill_addr(pa, len, mem_asi); }
static void r_addr(u64 pa, u32 len)
{
	u64 out[2];

	if (mem_check_addr(pa, len, mem_asi, out))
		post_compare_at(out[0], out[1], out[1], "Memory address mismatch");
}
static void w_blk(u64 pa, u32 len) { blk_fill(pa, len, blk_buf); }
static void r_blk(u64 pa, u32 len)
{
	u64 out[2];

	if (blk_check(pa, len, pattern, blk_buf + 8, out))
		post_compare_at(out[0], pattern, out[1], "Memory data mismatch");
}
static void w_blk_addr(u64 pa, u32 len) { blk_fill_addr(pa, len, blk_buf); }
static void r_blk_addr(u64 pa, u32 len)
{
	u64 out[2];

	if (blk_check_addr(pa, len, blk_buf, out))
		post_compare_at(out[0], out[1], out[1], "Memory address mismatch");
}

/* Init Memory: every doubleword written (its ECC with it), around the
   E-cache, a dot a megabyte. */
static void side_init(int b, int s)
{
	side_range(b, s, w_fill);
	post_putc('\n');
}

void post_init_memory(void)
{
	post_suspect("Memory");
	pattern = 0;
	mem_asi = 0x15;
	dots = 1;
	per_side(side_init);
	dots = 0;
}

static void addr_test(u32 asi)
{
	post_suspect("Memory");
	mem_asi = asi;
	dots = 0;
	pass_w = w_addr;
	pass_r = r_addr;
	per_side(side_pass);
}

void post_mem_addr_ec(void) { addr_test(0x14); }

/* Around the E-cache, so that the memory's ECC is checked: then no error
   in the processor's AFSR (UE 21, CE 20). */
void post_mem_addr_ecc(void)
{
	afsr_clear();
	addr_test(0x15);
	post_compare(afsr_get() & 0x300000, 0, "ECC error");
}

void post_blk_addr(void)
{
	post_suspect("Memory");
	dots = 0;
	pass_w = w_blk_addr;
	pass_r = r_blk_addr;
	per_side(side_pass);
}

static const u64 *blk_patterns;

static void side_blk(int b, int s)
{
	u32 p, i;

	for (p = 0; p < 4 && !post_failed(); p++) {
		pattern = blk_patterns[p];
		for (i = 0; i < 8; i++)
			blk_buf[i] = pattern;
		post_puts("Write 0x");
		post_putx64(pattern);
		post_putc(' ');
		side_pass(b, s);
	}
	post_puts("\n\n");
}

static void blk_test(const u64 *patterns)
{
	post_suspect("Memory");
	blk_patterns = patterns;
	pass_w = w_blk;
	pass_r = r_blk;
	dots = 1;
	per_side(side_blk);
	dots = 0;
}

void post_blk(void)
{
	static const u64 p[4] = {
		0x3333333333333333ULL, 0x5555555555555555ULL,
		0xccccccccccccccccULL, 0xaaaaaaaaaaaaaaaaULL,
	};

	blk_test(p);
}

void post_ecc_blk(void)
{
	static const u64 p[4] = {
		0xa5a5a5a5a5a5a5a5ULL, 0x9696969696969696ULL,
		0xbbbbbbbbbbbbbbbbULL, 0xddddddddddddddddULL,
	};

	afsr_clear();
	blk_test(p);
	post_compare(afsr_get() & 0x300000, 0, "ECC error");
}

/* The E-cache in use: a doubleword of the POST's memory stored through it
   and read back, then its line's tag (PA<29:18>, state modified). */
void post_ec_access(void)
{
	const u64 pa = POST_AREA - 128, x = 0x123456789abcdef0ULL;
	u64 tag;

	post_suspect("Ecache");
	asi_stx(pa, 0x14, x);
	post_compare(asi_ldx(pa, 0x14), x, "Ecache data mismatch");
	asi_ldx((2ULL << 39) | (pa & (ecache_size - 1)), 0x7e);
	tag = asi_ldx(0, 0x4e) & 0xcfff;
	post_compare(tag, ((pa >> 18) & 0xfff) | 0xc000, "Ecache tag mismatch");
	asi_stx(pa, 0x15, 0);
}
