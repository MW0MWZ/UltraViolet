/*
 * Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
 * OpenBoot release it is part of (see LICENSE).
 *
 * The POST's advanced processor tests (the MMUs' hits and misses, byte
 * order, the ASIs, the E-cache in use) and the errors the processor
 * reports. The POST's own 8 MiB is mapped to itself in both MMUs, locked
 * ("Map PROM/STACK/NVRAM in DMMU"); the tests' pages (a scratch page as
 * memory, read-only and in the other byte order, and a page of code) are
 * entered by the POST's MMU miss handler (cpu.S) from mmu_pages. The MMUs
 * are on only while a test needs them.
 */
#include "post.h"

void tlb_load(u32 immu, u32 va, u64 tte);
u64 dtag_after(u32 va);
u64 itag_after(u32 code);
void fasi(u32 src, u32 asi, u32 dst, u32 asi2);
u64 afsr_get(void);
void afsr_clear(void);

#define MB		0x100000U
#define PAGE		0x2000U

/* TTE bits (sun4u). */
#define TTE_V		(1ULL << 63)
#define TTE_4M		(3ULL << 61)
#define TTE_IE		(1ULL << 59)
#define TTE_L		(1ULL << 6)
#define TTE_CP		(1ULL << 5)
#define TTE_P		(1ULL << 2)
#define TTE_W		(1ULL << 1)

/* The tests' pages: the scratch page (in the POST's area) and its views. */
#define SCRATCH_PA	(7 * MB)
#define CODE_PA		(SCRATCH_PA + PAGE)
#define VA_MEM		0x40000000U
#define VA_RO		(VA_MEM + PAGE)
#define VA_IE		(VA_MEM + 2 * PAGE)
#define VA_CODE		(VA_MEM + 3 * PAGE)

u32 mmu_pages[8];			/* (mmu_miss: the VAs, then the TTEs) */
u64 mmu_ttes[8];

#define LSU_IM	4ULL
#define LSU_DM	8ULL

/* The POST's own 8 MiB mapped to itself, locked, in both MMUs. */
void post_map(void)
{
	u32 va;

	for (va = 0; va < 8 * MB; va += 4 * MB) {
		u64 tte = TTE_V | TTE_4M | va | TTE_L | TTE_CP | TTE_P | TTE_W;

		tlb_load(0, va, tte);
		tlb_load(1, va, tte);
	}
	mmu_pages[0] = VA_MEM;
	mmu_ttes[0] = TTE_V | SCRATCH_PA | TTE_CP | TTE_P | TTE_W;
	mmu_pages[1] = VA_RO;
	mmu_ttes[1] = TTE_V | SCRATCH_PA | TTE_CP | TTE_P;
	mmu_pages[2] = VA_IE;
	mmu_ttes[2] = TTE_V | TTE_IE | SCRATCH_PA | TTE_CP | TTE_P | TTE_W;
	mmu_pages[3] = VA_CODE;
	mmu_ttes[3] = TTE_V | CODE_PA | TTE_CP | TTE_P;
}

static u64 lsu;
static void mmus_on(u64 which) { lsu = lsu_get(); lsu_set(lsu | which); }
static void mmus_off(void) { lsu_set(lsu); }

/* The test pages' TLB entries (not locked) demapped (each page, primary
   context: ASI 5f, and 57 for the I-MMU), so that the next access
   misses. */
static void pages_unmapped(void)
{
	u32 i;

	for (i = 0; i < 4; i++) {
		asi_stx(mmu_pages[i] | 0x00, 0x5f, 0);
		asi_stx(mmu_pages[i] | 0x00, 0x57, 0);
	}
}

static u64 ld(u32 va, u32 asi) { return asi_ldx(va, asi); }
static void st(u32 va, u32 asi, u64 v) { asi_stx(va, asi, v); }

/* ---------------------------------------------------------------------
 * All Advanced CPU Tests.
 */

/* An access with Tag Access cleared first: Tag Access after (the page on
   a miss, 0 on a hit). */
void post_dmmu_hit_miss(void)
{
	pages_unmapped();
	mmus_on(LSU_DM);
	post_compare(dtag_after(VA_MEM), VA_MEM, "DMMU miss");
	post_compare(dtag_after(VA_MEM), 0, "DMMU hit");
	mmus_off();
}

/* A routine (retl; nop) in a page not yet run. */
void post_immu_hit_miss(void)
{
	phys_st32(CODE_PA, 0x81c3e008);
	phys_st32(CODE_PA + 4, 0x01000000);
	pages_unmapped();
	mmus_on(LSU_IM | LSU_DM);
	post_compare(itag_after(VA_CODE), VA_CODE, "IMMU miss");
	post_compare(itag_after(VA_CODE), 0, "IMMU hit");
	mmus_off();
}

/* The other byte order: the page mapped with IE, and the little-endian
   ASI (88). */
void post_little(void)
{
	pages_unmapped();
	mmus_on(LSU_DM);
	st(VA_MEM, 0x80, 0x0102030405060708ULL);
	post_compare(ld(VA_IE, 0x80), 0x0807060504030201ULL, "Little endian page");
	post_compare(ld(VA_MEM, 0x88), 0x0807060504030201ULL, "Little endian ASI");
	post_compare(ld(VA_IE, 0x88), 0x0102030405060708ULL, "Little endian page and ASI");
	mmus_off();
}

/* The same doubleword through the primary, nucleus and physical ASIs, in
   each size; and stored through each. */
void post_iu_asi(void)
{
	pages_unmapped();
	mmus_on(LSU_DM);
	st(VA_MEM, 0x80, 0x0123456789abcdefULL);
	post_compare(ld(VA_MEM, 0x80), 0x0123456789abcdefULL, "ASI 80");
	post_compare(ld(VA_MEM, 0x04), 0x0123456789abcdefULL, "ASI 04");
	post_compare(ld(SCRATCH_PA, 0x14), 0x0123456789abcdefULL, "ASI 14");
	post_compare(asi_ld32(VA_MEM + 4, 0x80), 0x89abcdef, "ASI 80 word");
	post_compare(asi_ld16(VA_MEM + 2, 0x80), 0x4567, "ASI 80 halfword");
	post_compare(asi_ld8(VA_MEM + 7, 0x80), 0xef, "ASI 80 byte");
	post_compare(asi_ld32(SCRATCH_PA + 4, 0x14), 0x89abcdef, "ASI 14 word");
	post_compare(asi_ld32(VA_MEM + 4, 0x88), 0xefcdab89, "ASI 88 word");
	post_compare(asi_ld16(VA_MEM + 2, 0x88), 0x6745, "ASI 88 halfword");
	st(VA_MEM, 0x80, 0xfedcba9876543210ULL);
	post_compare(ld(VA_MEM, 0x80), 0xfedcba9876543210ULL, "ASI 80 store");
	st(SCRATCH_PA, 0x14, 0x1111222233334444ULL);
	post_compare(ld(VA_MEM, 0x80), 0x1111222233334444ULL, "ASI 14 store");
	st(VA_MEM, 0x88, 0x0123456789abcdefULL);
	post_compare(ld(VA_MEM, 0x80), 0xefcdab8967452301ULL, "ASI 88 store");
	mmus_off();
}

/* The FPU's loads and stores through the ASIs: a double through the
   primary, little-endian and physical ASIs, to and from %f0. */
static u64 fbuf[2] __attribute__((aligned(8)));

void post_fpu_asi(void)
{
	u32 buf = (u32)fbuf;

	pages_unmapped();
	mmus_on(LSU_DM);
	st(VA_MEM, 0x80, 0x0123456789abcdefULL);
	fasi(VA_MEM, 0x80, buf, 0x80);
	post_compare(fbuf[0], 0x0123456789abcdefULL, "FPU ASI 80");
	fasi(VA_MEM, 0x88, buf, 0x80);
	post_compare(fbuf[0], 0xefcdab8967452301ULL, "FPU ASI 88");
	fasi(SCRATCH_PA, 0x14, buf, 0x80);
	post_compare(fbuf[0], 0x0123456789abcdefULL, "FPU ASI 14");
	fasi(buf, 0x80, VA_MEM + 8, 0x88);
	post_compare(ld(VA_MEM + 8, 0x80), 0xefcdab8967452301ULL, "FPU ASI 88 store");
	mmus_off();
}

/* Four doublewords an E-cache's size apart (one E-cache line), written
   through the E-cache in turn and read back, for lines across the cache
   (in the memory past the POST's 8 MiB). */
void post_ec_thrash(void)
{
	const u64 base = 8 * MB;
	u32 off, i;

	for (off = 0; off < ecache_size && !post_failed(); off += 0x10040) {
		for (i = 0; i < 4; i++)
			asi_stx(base + (u64)i * ecache_size + off, 0x14,
				0x0101010101010101ULL * ((i + off) & 0xff));
		for (i = 0; i < 4; i++)
			post_compare_at(asi_ldx(base + (u64)i * ecache_size + off, 0x14),
					0x0101010101010101ULL * ((i + off) & 0xff),
					base + (u64)i * ecache_size + off, "Ecache thrash mismatch");
	}
}

/* ---------------------------------------------------------------------
 * All CPU Error Reporting Tests (a handler at the trap taken).
 */
static u32 acc_va, acc_asi;
static void ldx_acc(void) { asi_ldx(acc_va, acc_asi); }
static void stx_acc(void) { asi_stx(acc_va, acc_asi, 0); }

static u64 acc_pa;
static void ldx_pa(void) { asi_ldx(acc_pa, acc_asi); }

static void sfsr_clear(void) { asi_stx(0x18, 0x58, 0); }

/* A load through an ASI the processor has none of (data_access_exception,
   30); one from a PCI address no device answers (data_access_error, 32,
   the processor's AFSR then TO or BERR): PCI memory at c000.0000, taken
   out of both APB bridges' memory maps (each bit 512 MiB, bit 6 that
   one) for the test, so that nothing claims it on the bus module's own
   bus (a master abort); the maps then put back, and the Sabre's received
   master abort (its PCI status, bit 13, cleared by writing 1). */
#define PCI_NOTHING 0x1ffc0000000ULL
#define APB_CONFIG(fn, reg) (0x1fe01000000ULL | 1 << 11 | (fn) << 8 | (reg))
#define APB_MEM_MAP 0xdf


void post_data_access(void)
{
	u32 map0, map1;

	pages_unmapped();
	mmus_on(LSU_DM);
	acc_va = VA_MEM;
	acc_asi = 0x31;
	post_compare(trap_run(ldx_acc), 0x30, "Data access exception not taken");
	sfsr_clear();
	mmus_off();
	map0 = asi_ld8(APB_CONFIG(0, APB_MEM_MAP), 0x1d);
	map1 = asi_ld8(APB_CONFIG(1, APB_MEM_MAP), 0x1d);
	asi_st8(APB_CONFIG(0, APB_MEM_MAP), 0x1d, map0 & ~0x40);
	asi_st8(APB_CONFIG(1, APB_MEM_MAP), 0x1d, map1 & ~0x40);
	afsr_clear();
	acc_pa = PCI_NOTHING;
	acc_asi = 0x15;
	post_compare(trap_run(ldx_pa), 0x32, "Data access error not taken");
	post_compare((afsr_get() & 0x0c000000ULL) != 0, 1, "AFSR TO/BERR");
	afsr_clear();
	asi_st8(APB_CONFIG(0, APB_MEM_MAP), 0x1d, map0);
	asi_st8(APB_CONFIG(1, APB_MEM_MAP), 0x1d, map1);
	asi_st16(0x1fe01000006ULL, 0x1d, 0x2000);	/* the Sabre's status */
}

/* ---------------------------------------------------------------------
 * All Sabre IOMMU Error Reporting Tests: a 32-bit PIO read that the PCI
 * bus ends in an abort. A master abort: APB B's memory map 8f (PCI memory
 * c000.0000-dfff.ffff not passed to its bus), a read of c001.0000. A
 * target abort: APB B's bridge control 23 (parity and SERR# responses,
 * master-abort mode), a read of e000.1000, which nothing behind APB B
 * claims, so that APB B ends it in a target abort. Each must trap
 * (data_access_error, 32) and leave exactly PRIV and TO (or BERR) in the
 * processor's AFSR, the address in its AFAR, nothing in the Sabre's UE,
 * CE or PCI AFSRs, its PCI control as set and its PCI status with the
 * abort received; then the errors are cleared, as an Ultra 10's POST
 * clears them.
 */
#define SABRE_UE_AFSR	0x1fe00000030ULL
#define SABRE_CE_AFSR	0x1fe00000040ULL
#define SABRE_PCI_CTL	0x1fe00002000ULL
#define SABRE_PCI_AFSR	0x1fe00002010ULL
#define SABRE_CONFIG(r)	(0x1fe01000000ULL | (r))
#define SABRE_STATUS	SABRE_CONFIG(0x06)
#define PCI_CTL_SET	0x0000001c00200101ULL
#define PCI_CTL_READ	0x0000001000200101ULL	/* (bits 35:34 clear when written) */
#define PCI_AFSR_ERRS	0xff00000000000000ULL
#define AFSR_PRIV	(1ULL << 31)
#define AFSR_TO		(1ULL << 27)
#define AFSR_BERR	(1ULL << 26)
#define AFSR_CLEAR	0x1adf00000ULL		/* the error bits, written 1 */
#define ESTATE_ERRS	0x1bULL			/* NCEEN, CEEN and the others */
#define APB_STATUS	0x06
#define APB_SEC_STATUS	0x1e
#define APB_BRIDGE_CTL	0x3e
#define APB_PIO_AFSR	0xe8
#define PIO_MASTER	0x1ffc0010000ULL
#define PIO_TARGET	0x1ffe0001000ULL

static void estate_set(u64 v) { asi_stx(0, 0x4b, v); }
static u64 afar_get(void) { return asi_ldx(0, 0x4d); }

/* The Sabre's error state clear, its PCI control and configuration
   header set (each read back), the processor's error traps enabled. */
static void sabre_err_setup(void)
{
	u32 r;

	asi_stx(SABRE_UE_AFSR, 0x15, 0x6f00000000000000ULL);
	asi_ldx(SABRE_UE_AFSR, 0x15);
	asi_stx(SABRE_CE_AFSR, 0x15, 0xfc00000000000000ULL);
	asi_ldx(SABRE_CE_AFSR, 0x15);
	asi_stx(SABRE_PCI_AFSR, 0x15, PCI_AFSR_ERRS);
	post_compare(asi_ldx(SABRE_PCI_AFSR, 0x15) & PCI_AFSR_ERRS, 0, "Sabre PCI AFSR");
	asi_stx(SABRE_PCI_CTL, 0x15, PCI_CTL_SET);
	/* (the mask the read back is compared under: a guess) */
	post_compare(asi_ldx(SABRE_PCI_CTL, 0x15) & ~0xe00000000ULL, PCI_CTL_READ, "Sabre PCI Control");
	asi_st16(SABRE_CONFIG(0x04), 0x1d, 0x0006);	/* memory, master */
	asi_ld16(SABRE_CONFIG(0x04), 0x1d);
	asi_st16(SABRE_STATUS, 0x1d, 0xfba0);
	asi_ld16(SABRE_STATUS, 0x1d);
	asi_ld32(SABRE_CONFIG(0x00), 0x1d);
	asi_ld32(SABRE_CONFIG(0x08), 0x1d);
	asi_st8(SABRE_CONFIG(0x0c), 0x1d, 0);
	asi_ld8(SABRE_CONFIG(0x0c), 0x1d);
	asi_st8(SABRE_CONFIG(0x0d), 0x1d, 0x40);
	asi_ld8(SABRE_CONFIG(0x0d), 0x1d);
	for (r = 0x10; r <= 0x24; r += 4) {
		asi_st32(SABRE_CONFIG(r), 0x1d, 0);
		asi_ld32(SABRE_CONFIG(r), 0x1d);
	}
	asi_st8(SABRE_CONFIG(0x40), 0x1d, 0);
	asi_ld8(SABRE_CONFIG(0x40), 0x1d);
	asi_st8(SABRE_CONFIG(0x41), 0x1d, 0);
	asi_ld8(SABRE_CONFIG(0x41), 0x1d);
	estate_set(ESTATE_ERRS);
}

static void sabre_err_clear(void)
{
	estate_set(0);
	asi_stx(0, 0x4c, AFSR_CLEAR);
	asi_st16(SABRE_STATUS, 0x1d, 0xfba0);
	asi_stx(APB_CONFIG(1, APB_PIO_AFSR), 0x1d, 0xff00000000000000ULL);
	asi_st16(APB_CONFIG(1, APB_STATUS), 0x1d, 0xf900);
	asi_st16(APB_CONFIG(1, APB_SEC_STATUS), 0x1d, 0xf900);
}

/* Init Sabre: the IOMMU's TLB invalid, the error state set up. */
void post_init_sabre_err(void)
{
	post_init_sabre();
	sabre_err_setup();
}

static u32 pio_pa_hi, pio_pa_lo;
static void ldw_pio(void) { asi_ld32((u64)pio_pa_hi << 32 | pio_pa_lo, 0x15); }

/* The AFAR as compared: 33 bits on an UltraSPARC-IIi of mask 1.x. */
static u64 afar_mask(void)
{
	u64 ver = rd_ver();

	return (ver >> 32 & 0xffff) == 0x12 && (ver >> 28 & 15) == 1 ? 0x1ffffffffULL : ~0ULL;
}

static void pio_abort(u64 pa, u64 afsr, u32 status)
{
	u64 m = afar_mask();

	sabre_err_setup();
	pio_pa_hi = (u32)(pa >> 32);
	pio_pa_lo = (u32)pa;
	if (trap_run(ldw_pio) != 0x32) {
		post_fail("Didn't Generate Trap as Expected");
		sabre_err_clear();
		return;
	}
	post_compare(asi_ldx(0, 0x4c), afsr, "CPU AFSR");
	asi_stx(0, 0x4c, AFSR_CLEAR);
	post_compare(asi_ldx(0, 0x4c) & AFSR_CLEAR, 0, "CPU AFSR not cleared");
	post_compare(afar_get() & m, pa & m, "CPU AFAR");
	post_compare(asi_ldx(SABRE_UE_AFSR, 0x15) & 0x6f00000000000000ULL, 0, "Sabre UE AFSR");
	post_compare(asi_ldx(SABRE_CE_AFSR, 0x15) & 0x6c00000000000000ULL, 0, "Sabre CE AFSR");
	post_compare(asi_ldx(SABRE_PCI_CTL, 0x15) & ~(1ULL << 33), PCI_CTL_READ, "Sabre PCI Control");
	post_compare(asi_ldx(SABRE_PCI_AFSR, 0x15) & PCI_AFSR_ERRS, 0, "Sabre PCI AFSR");
	post_compare(asi_ld16(SABRE_STATUS, 0x1d), status, "Sabre PCI Status");
	asi_st16(SABRE_STATUS, 0x1d, 0xfba0);
	sabre_err_clear();
}

void post_pio_master_abort(void)
{
	asi_st8(APB_CONFIG(1, APB_MEM_MAP), 0x1d, 0x8f);
	pio_abort(PIO_MASTER, AFSR_PRIV | AFSR_TO, 0x22a0);
}

void post_pio_target_abort(void)
{
	asi_st8(APB_CONFIG(1, APB_BRIDGE_CTL), 0x1d, 0x23);
	pio_abort(PIO_TARGET, AFSR_PRIV | AFSR_BERR, 0x12a0);
}

/* Loads and stores not on their size's boundary (mem_address_not_aligned,
   34). */
static void ldw_acc(void) { asi_ld32(acc_va, acc_asi); }
static void stw_acc(void) { asi_st32(acc_va, acc_asi, 0); }
static void ldh_acc(void) { asi_ld16(acc_va, acc_asi); }
static void sth_acc(void) { asi_st16(acc_va, acc_asi, 0); }

static void align_one(void (*fn)(void), u32 offset)
{
	acc_va = VA_MEM + offset;
	acc_asi = 0x80;
	post_compare(trap_run(fn), 0x34, "Address alignment trap not taken");
}

void post_align(void)
{
	u32 i;

	pages_unmapped();
	mmus_on(LSU_DM);
	for (i = 1; i < 8; i++) {
		align_one(ldx_acc, i);
		align_one(stx_acc, i);
	}
	for (i = 1; i < 4; i++) {
		align_one(ldw_acc, i);
		align_one(stw_acc, i);
	}
	align_one(ldh_acc, 1);
	align_one(sth_acc, 1);
	sfsr_clear();
	mmus_off();
}

/* A load as if by a user (ASI 10) from a privileged page
   (data_access_exception, 30; the SFSR's privilege violation bit, 7). */
void post_priv(void)
{
	pages_unmapped();
	mmus_on(LSU_DM);
	acc_va = VA_MEM;
	acc_asi = 0x10;
	post_compare(trap_run(ldx_acc), 0x30, "Privilege violation not trapped");
	post_compare(asi_ldx(0x18, 0x58) & 0x80, 0x80, "SFSR privilege violation");
	sfsr_clear();
	mmus_off();
}

/* A store to a read-only page (fast_data_access_protection, 6c): Tag
   Access then the page. */
void post_protect(void)
{
	pages_unmapped();
	mmus_on(LSU_DM);
	acc_va = VA_RO;
	acc_asi = 0x80;
	post_compare(trap_run(stx_acc), 0x6c, "Write protection not trapped");
	post_compare(asi_ldx(0x30, 0x58) & ~(u64)(PAGE - 1), VA_RO, "DMMU Tag Access");
	sfsr_clear();
	mmus_off();
}
