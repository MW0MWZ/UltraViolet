/*
 * Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
 * OpenBoot release it is part of (see LICENSE).
 *
 * The POST's tests of the NVRAM, the D-TLB, the E-cache, the processor,
 * the MMUs' registers and the L1 caches. The POST runs with the MMUs and
 * the L1 caches off, so nothing of its own is in the caches or the TLBs:
 * each test writes the arrays it tests freely, then leaves them as they
 * were (the TLBs) or invalid (the caches).
 */
#include "post.h"

void v9_ops(u64 *out);

/* ---------------------------------------------------------------------
 * NVRAM: the M48T59's battery, and its scratch bytes: where an Ultra 10's
 * selftest tests them, 1d00-1d3f (in the selftest's own area past the
 * configuration variables). Each test leaves them as an Ultra 10's does:
 * sixteen words 00000000, 11111111 ... ffffffff.
 */
#define NV_BASE	NV_SCRATCH
#define NV_SIZE	0x40

static void nv_leave(void)
{
	u32 i;

	for (i = 0; i < NV_SIZE / 4; i++)
		nvram_st(NV_BASE + 4 * i, 0x11111111U * i, 4);
}

void post_battery(void)
{
	if (nvram_ld8(0x1ff0) & 0x10)
		post_fail("Battery Low");
}

void post_nv_addr(void)
{
	u32 i;

	for (i = 0; i < NV_SIZE; i++)
		nvram_st8(NV_BASE + i, i);
	for (i = 0; i < NV_SIZE && !post_failed(); i++)
		post_compare_at(nvram_ld8(NV_BASE + i), i, NV_BASE + i, "Scratch address mismatch");
	nv_leave();
}

void post_nv_data(void)
{
	static const u8 pat[4] = { 0x00, 0xff, 0x55, 0xaa };
	u32 i, p;

	for (i = 0; i < NV_SIZE && !post_failed(); i++)
		for (p = 0; p < 4; p++) {
			nvram_st8(NV_BASE + i, pat[p]);
			post_compare_at(nvram_ld8(NV_BASE + i), pat[p], NV_BASE + i,
					"Scratch data mismatch");
		}
	nv_leave();
}

/* ---------------------------------------------------------------------
 * The D-MMU's TLB: each entry's tag (VA<63:13> and context, through Tag
 * Access) and data (the TTE as Data Access reads it: soft fields as 0,
 * the Diag field's used and size bits) written and read back, then put
 * back.
 */
#define TTE_MASK 0xf80001ffffffe07fULL	/* the bits Data Access keeps */

void post_dtlb_tags(void)
{
	static const u64 pat[2] = { 0x0000055555554aaaULL, 0xfffffaaaaaaab555ULL };
	u64 out[2];
	u32 i, p;

	for (i = 0; i < 64 && !post_failed(); i++)
		for (p = 0; p < 2; p++) {
			dtlb_swap(i, pat[p], dtlb_entry(i, 1), out);
			post_compare_at(out[0], pat[p], i, "DMMU TLB tag");
		}
}

void post_dtlb_data(void)
{
	static const u64 pat[2] = { 0x5555555555555555ULL, 0xaaaaaaaaaaaaaaaaULL };
	u64 out[2];
	u32 i, p;

	for (i = 0; i < 64 && !post_failed(); i++)
		for (p = 0; p < 2; p++) {
			dtlb_swap(i, dtlb_entry(i, 0), pat[p] & TTE_MASK, out);
			post_compare_at(out[1] & TTE_MASK, pat[p] & TTE_MASK, i, "DMMU TLB data");
		}
}

/* ---------------------------------------------------------------------
 * The E-cache: its data (VA<40:39> 1) and tags (2, through the staging
 * register at ASI 4e) through the diagnostic ASIs (76 to write, 7e to
 * read), each written whole and read back: a pattern, or each its own
 * offset (in the high word, and its line in the low). Every tag left
 * invalid (state 0, its parity bits right) after.
 */
#define EC_DATA	(1ULL << 39)
#define EC_TAGS	(2ULL << 39)
#define EC_TAG_MASK 0x3cfffULL	/* parity, state, tag */
#define EC_TAG_INVALID 0x30000ULL


static u64 ec_value(u64 pattern, int addr, u32 offset)
{
	return addr ? (u64)offset << 32 | offset >> 6 : pattern;
}

static void ec_invalidate(void)
{
	u32 off;

	asi_stx(0, 0x4e, EC_TAG_INVALID);
	for (off = 0; off < ecache_size; off += 0x40)
		asi_stx(EC_TAGS + off, 0x76, EC_TAG_INVALID);
}

static void ec_data(u64 pattern, int addr, const char *msg)
{
	u32 off;

	for (off = 0; off < ecache_size; off += 8)
		asi_stx(EC_DATA + off, 0x76, ec_value(pattern, addr, off));
	for (off = 0; off < ecache_size && !post_failed(); off += 8)
		post_compare_at(asi_ldx(EC_DATA + off, 0x7e), ec_value(pattern, addr, off), off, msg);
	ec_invalidate();
}

static void ec_tags(u64 pattern, int addr, const char *msg)
{
	u32 off;
	u64 v;

	for (off = 0; off < ecache_size; off += 0x40) {
		asi_stx(0, 0x4e, ec_value(pattern, addr, off));
		asi_stx(EC_TAGS + off, 0x76, 0);
	}
	for (off = 0; off < ecache_size && !post_failed(); off += 0x40) {
		asi_ldx(EC_TAGS + off, 0x7e);
		v = asi_ldx(0, 0x4e);		/* the tag, staged */
		post_compare_at(v & EC_TAG_MASK, ec_value(pattern, addr, off) & EC_TAG_MASK, off, msg);
	}
	ec_invalidate();
}

void post_ec_ram_addr(void) { ec_data(0, 1, "Ecache RAM address mismatch"); }
void post_ec_tag_addr(void) { ec_tags(0, 1, "Ecache tag address mismatch"); }

void post_ec_ram(void)
{
	ec_data(0x5555555555555555ULL, 0, "Ecache RAM data mismatch");
	ec_data(0xaaaaaaaaaaaaaaaaULL, 0, "Ecache RAM data mismatch");
}

void post_ec_tag(void)
{
	ec_tags(0x15555, 0, "Ecache tag data mismatch");
	ec_tags(0x2aaaa, 0, "Ecache tag data mismatch");
}

/* ---------------------------------------------------------------------
 * The processor.
 */
void post_v9(void)
{
	static const struct { u64 value; const char *msg; } want[10] = {
		{ 0x71c977b3053f308dULL, "MULX" },
		{ 0x124ULL, "UDIVX" },
		{ 0x91a2b323c4d83ULL, "UDIVX remainder" },
		{ 0xfffffeffffa4ffe0ULL, "Signed divide" },	/* (SDIVX truncates) */
		{ 0x8acf121579bde000ULL, "SLLX" },
		{ 0x91a2b3c4855ULL, "SRLX" },
		{ 0xfff8000000000000ULL, "SRAX" },
		{ 0x4761032dc5fe98baULL, "XOR" },
		{ 0x0230067000a00de0ULL, "AND" },
		{ 0x9234567890abcdefULL, "OR" },
	};
	u64 out[10];
	u32 i;

	v9_ops(out);
	for (i = 0; i < 10; i++)
		post_compare(out[i], want[i].value, want[i].msg);
}

/* TICK and TICK_CMPR: the tick read before and after a pattern (with its
   interrupt disabled) is written to TICK_CMPR and read back. */
void post_tick(void)
{
	static const u64 pat[2] = { 0x8000000055555555ULL, 0xaaaaaaaaaaaaaaaaULL };
	u64 out[3];
	u32 p;

	for (p = 0; p < 2; p++) {
		tick_test(pat[p], out);
		if (out[2] <= out[0])
			post_fail("Tick not counting");
		post_compare(out[1], pat[p], "Tick Compare");
	}
}

/* A software trap (Tcc, trap 171), taken, and one whose condition is
   false, not. */
void post_soft_trap(void)
{
	post_compare(trap_run(do_ta), 0x171, "Trap not taken");
	post_compare(trap_run(do_tn), 0, "Trap taken");
}

/* SOFTINT: each level's bit set and cleared; then, with the PIL below it
   and interrupts on, its interrupt taken (interrupt_level_n). */
static u32 softint_level;
static void softint_one(void) { softint_int(softint_level); }

void post_softint(void)
{
	u32 level, set, cleared;

	for (level = 1; level < 14; level++) {
		set = softint_setclr(1 << level, &cleared);
		if (cleared & (1 << level))
			post_fail("Softint bit not cleared");
		if (!(set & (1 << level)))
			post_fail("Softint bit not set");
	}
	for (level = 1; level < 14; level++) {
		softint_level = level;
		post_compare(trap_run(softint_one), 0x40 + level, "Softint interrupt not taken");
	}
}

/* ---------------------------------------------------------------------
 * The MMUs' registers: each written with patterns (of the bits it keeps)
 * and read back, then put back.
 */
#define DMMU 0x58
#define IMMU 0x50

static void reg(u64 va, u32 asi, u64 pattern, const char *msg)
{
	post_compare(asi_swap(va, asi, pattern), pattern, msg);
}

static void mmu_reg(u64 va, u32 asi, u64 mask, const char *msg)
{
	reg(va, asi, 0x5555555555555555ULL & mask, msg);
	reg(va, asi, 0xaaaaaaaaaaaaaaaaULL & mask, msg);
}

void post_pctx(void) { mmu_reg(8, DMMU, 0x1fff, "Primary Context"); }
void post_sctx(void) { mmu_reg(0x10, DMMU, 0x1fff, "Secondary Context"); }
void post_dtsb(void) { mmu_reg(0x28, DMMU, ~0xff8ULL, "DMMU TSB"); }
void post_itsb(void) { mmu_reg(0x28, IMMU, ~0xff8ULL, "IMMU TSB"); }
void post_pawp(void) { mmu_reg(0x40, DMMU, 0x1fffffffff8ULL, "PA Watchpoint"); }

/* Tag access: VA<63:13> (sign-extended from bit 43) and a context; the VA
   watchpoint: VA<43:3>, sign-extended. */
void post_dtag(void)
{
	reg(0x30, DMMU, 0x0000055555554aaaULL, "DMMU Tag Access");
	reg(0x30, DMMU, 0xfffffaaaaaaab555ULL, "DMMU Tag Access");
}

void post_itag(void)
{
	reg(0x30, IMMU, 0x0000055555554aaaULL, "IMMU Tag Access");
	reg(0x30, IMMU, 0xfffffaaaaaaab555ULL, "IMMU Tag Access");
}

void post_vawp(void)
{
	reg(0x38, DMMU, 0x0000055555555550ULL, "VA Watchpoint");
	reg(0x38, DMMU, 0xfffffaaaaaaaaaa8ULL, "VA Watchpoint");
}

/* ---------------------------------------------------------------------
 * The L1 caches (off while the POST runs): their arrays written and read
 * back through the diagnostic ASIs, then every line invalid.
 */
struct arr { u32 asi, step, n; u64 mask; };

static u64 index_pattern(u32 i) { return 0x0101010101010101ULL * (i & 0xff); }

static void arr_addr(const struct arr *a, const char *msg)
{
	u32 i;

	for (i = 0; i < a->n; i++)
		asi_stx((u64)i * a->step, a->asi, index_pattern(i));
	for (i = 0; i < a->n && !post_failed(); i++)
		post_compare_at(asi_ldx((u64)i * a->step, a->asi) & a->mask,
				index_pattern(i) & a->mask, (u64)i * a->step, msg);
}

static void arr_pattern(const struct arr *a, u64 pattern, const char *msg)
{
	u32 i;

	for (i = 0; i < a->n; i++)
		asi_stx((u64)i * a->step, a->asi, pattern);
	for (i = 0; i < a->n && !post_failed(); i++)
		post_compare_at(asi_ldx((u64)i * a->step, a->asi) & a->mask,
				pattern & a->mask, (u64)i * a->step, msg);
}

static void arr_test(const struct arr *a, const char *addr_msg, const char *data_msg)
{
	arr_addr(a, addr_msg);
	arr_pattern(a, 0x5555555555555555ULL, data_msg);
	arr_pattern(a, 0xaaaaaaaaaaaaaaaaULL, data_msg);
}

static void arr_clear(u32 asi, u32 step, u32 n)
{
	u32 i;

	for (i = 0; i < n; i++)
		asi_stx((u64)i * step, asi, 0);
}

void post_dcache_ram(void)
{
	static const struct arr a = { 0x46, 8, 2048, ~0ULL };

	arr_test(&a, "Dcache RAM address mismatch", "Dcache RAM data mismatch");
	arr_clear(0x47, 0x20, 512);
}

/* D-cache tags: PA<40:13> in 29:2, a valid bit for each half line. */
void post_dcache_tag(void)
{
	static const struct arr a = { 0x47, 0x20, 512, 0x3fffffffULL };

	arr_test(&a, "Dcache tag address mismatch", "Dcache tag data mismatch");
	arr_clear(0x47, 0x20, 512);
}

/* I-cache: instruction pairs (VA<13> the way), tags (valid 36, PA<40:13>
   in 35:8), predecode bits (a quad's, the pair VA<3> names) and next
   fields (set prediction, next field, branch: 23:8). */
void post_icache_ram(void)
{
	static const struct arr a = { 0x66, 8, 2048, ~0ULL };

	arr_test(&a, "Icache RAM address mismatch", "Icache RAM data mismatch");
	arr_clear(0x67, 0x20, 512);
}

void post_icache_tag(void)
{
	static const struct arr a = { 0x67, 0x20, 512, 0x1fffffff00ULL };

	arr_test(&a, "Icache tag address mismatch", "Icache tag data mismatch");
	arr_clear(0x67, 0x20, 512);
}

void post_icache_next(void)
{
	static const struct arr a = { 0x6f, 0x10, 1024, 0xffff00ULL };

	arr_test(&a, "Icache next address mismatch", "Icache next data mismatch");
	arr_clear(0x67, 0x20, 512);
}

/* (Written a quad's 16 bits at a time; read a pair's 8: pair 0 the high
   byte.) */
void post_icache_predecode(void)
{
	u32 i, p, pat[3];

	for (i = 0; i < 1024 && !post_failed(); i++) {
		pat[0] = 0x55aa;
		pat[1] = 0xaa55;
		pat[2] = i ^ 0x3c3c;
		for (p = 0; p < 3; p++) {
			asi_stx(i * 0x10, 0x6e, pat[p]);
			post_compare_at(asi_ldx(i * 0x10, 0x6e), pat[p] >> 8 & 0xff, i * 0x10,
					"Icache predecode mismatch");
			post_compare_at(asi_ldx(i * 0x10 + 8, 0x6e), pat[p] & 0xff, i * 0x10 + 8,
					"Icache predecode mismatch");
		}
	}
	arr_clear(0x67, 0x20, 512);
}
