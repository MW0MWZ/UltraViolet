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
	post_suspect("DMMU");
	pages_unmapped();
	mmus_on(LSU_DM);
	post_compare(dtag_after(VA_MEM), VA_MEM, "DMMU miss");
	post_compare(dtag_after(VA_MEM), 0, "DMMU hit");
	mmus_off();
}

/* A routine (retl; nop) in a page not yet run. */
void post_immu_hit_miss(void)
{
	post_suspect("IMMU");
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
	post_suspect("DMMU");
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
	u32 asi_ld32(u64 va, u32 asi);
	u32 asi_ld16(u64 va, u32 asi);
	u32 asi_ld8(u64 va, u32 asi);

	post_suspect("CPU (Basic)");
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

	post_suspect("FPU");
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

	post_suspect("Ecache");
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

u32 asi_ld8(u64 va, u32 asi);
void asi_st8(u64 va, u32 asi, u32 v);
u32 asi_ld16(u64 va, u32 asi);
void asi_st16(u64 va, u32 asi, u32 v);

void post_data_access(void)
{
	u32 map0, map1;

	post_suspect("CPU (Basic)");
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
 * All Sabre IOMMU Error Reporting Tests: PIO reads the PCI bus ends in
 * an abort, with APB B's I/O and memory enabled and its memory map 8f
 * (PCI memory 0-7fff.ffff and e000.0000-ffff.ffff; APB A's map without
 * c000.0000 and e000.0000). A master abort: a read of c000.0000, which nothing
 * claims, the processor's AFSR PRIV and TO, the Sabre's received master
 * abort. A target abort: with APB B's bridge control in master-abort
 * mode (bit 5; with parity and SERR# reporting), a read of e000.1000,
 * which nothing behind it claims: APB B answers with a target abort
 * (it signalled one, status bit 11), the Sabre received one (bit 12) and
 * the processor's AFSR has PRIV and BERR. Each status is cleared by
 * writing 1, APB B's PIO AFSR (e8) too, and the bridge put back.
 */
#define PCI_ABORT	0x1ffe0001000ULL
#define SABRE_STATUS	0x1fe01000006ULL
#define AFSR_PRIV	(1ULL << 31)
#define AFSR_TO		(1ULL << 27)
#define AFSR_BERR	(1ULL << 26)
#define APB_COMMAND	0x04
#define APB_STATUS	0x06
#define APB_SEC_STATUS	0x1e
#define APB_BRIDGE_CTL	0x3e

static u32 apb_cmd, apb_ctl, apb_map0, apb_map1;

static void apb_set(void)
{
	apb_cmd = asi_ld16(APB_CONFIG(1, APB_COMMAND), 0x1d);
	apb_ctl = asi_ld8(APB_CONFIG(1, APB_BRIDGE_CTL), 0x1d);
	apb_map0 = asi_ld8(APB_CONFIG(0, APB_MEM_MAP), 0x1d);
	apb_map1 = asi_ld8(APB_CONFIG(1, APB_MEM_MAP), 0x1d);
	asi_st16(APB_CONFIG(1, APB_COMMAND), 0x1d, apb_cmd | 0x147);
	asi_st8(APB_CONFIG(0, APB_MEM_MAP), 0x1d, apb_map0 & ~0xc0);
	asi_st8(APB_CONFIG(1, APB_MEM_MAP), 0x1d, 0x8f);
}

static void apb_clear(void)
{
	asi_stx(APB_CONFIG(1, 0xe8), 0x1d, ~0ULL);
	asi_st16(APB_CONFIG(1, APB_STATUS), 0x1d, 0xf900);
	asi_st16(APB_CONFIG(1, APB_SEC_STATUS), 0x1d, 0xf900);
	asi_st16(SABRE_STATUS, 0x1d, 0xf900);
}

static void apb_back(void)
{
	apb_clear();
	asi_st8(APB_CONFIG(1, APB_BRIDGE_CTL), 0x1d, apb_ctl);
	asi_st8(APB_CONFIG(0, APB_MEM_MAP), 0x1d, apb_map0);
	asi_st8(APB_CONFIG(1, APB_MEM_MAP), 0x1d, apb_map1);
	asi_st16(APB_CONFIG(1, APB_COMMAND), 0x1d, apb_cmd);
}

/* Init Sabre, and its error status clear: the UE and CE AFSRs, the PCI
   AFSR and its PCI status. */
void post_init_sabre_err(void)
{
	post_init_sabre();
	asi_stx(0x1fe00000030ULL, 0x15, ~0ULL);
	asi_stx(0x1fe00000040ULL, 0x15, ~0ULL);
	asi_stx(0x1fe00002010ULL, 0x15, ~0ULL);
	asi_st16(SABRE_STATUS, 0x1d, 0xf900);
	afsr_clear();
}

static void pio_abort(u64 pa, u64 afsr, u32 sabre_bit, const char *msg)
{
	afsr_clear();
	acc_pa = pa;
	acc_asi = 0x15;
	post_compare(trap_run(ldx_pa), 0x32, "Data access error not taken");
	post_compare(afsr_get() & 0xffffffffULL, afsr, "Sabre AFSR");
	post_compare(asi_ld16(SABRE_STATUS, 0x1d) & sabre_bit, sabre_bit, msg);
	afsr_clear();
}

void post_pio_master_abort(void)
{
	post_suspect("Sabre IOMMU (Functional)");
	apb_set();
	apb_clear();
	pio_abort(PCI_NOTHING, AFSR_PRIV | AFSR_TO, 0x2000, "Received master abort");
	apb_back();
}

void post_pio_target_abort(void)
{
	post_suspect("Sabre IOMMU (Functional)");
	apb_set();
	asi_st8(APB_CONFIG(1, APB_BRIDGE_CTL), 0x1d, 0x23);
	apb_clear();
	pio_abort(PCI_ABORT, AFSR_PRIV | AFSR_BERR, 0x1000, "Received target abort");
	post_compare(asi_ld16(APB_CONFIG(1, APB_STATUS), 0x1d) & 0x800, 0x800, "APB signalled target abort");
	apb_back();
}

/* Loads and stores not on their size's boundary (mem_address_not_aligned,
   34). */
static void ldw_acc(void) { u32 asi_ld32(u64, u32); asi_ld32(acc_va, acc_asi); }
static void stw_acc(void) { void asi_st32(u64, u32, u32); asi_st32(acc_va, acc_asi, 0); }
static void ldh_acc(void) { u32 asi_ld16(u64, u32); asi_ld16(acc_va, acc_asi); }
static void sth_acc(void) { void asi_st16(u64, u32, u32); asi_st16(acc_va, acc_asi, 0); }

static void align_one(void (*fn)(void), u32 offset)
{
	acc_va = VA_MEM + offset;
	acc_asi = 0x80;
	post_compare(trap_run(fn), 0x34, "Address alignment trap not taken");
}

void post_align(void)
{
	u32 i;

	post_suspect("CPU (Basic)");
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
	post_suspect("DMMU");
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
	post_suspect("DMMU");
	pages_unmapped();
	mmus_on(LSU_DM);
	acc_va = VA_RO;
	acc_asi = 0x80;
	post_compare(trap_run(stx_acc), 0x6c, "Write protection not trapped");
	post_compare(asi_ldx(0x30, 0x58) & ~(u64)(PAGE - 1), VA_RO, "DMMU Tag Access");
	sfsr_clear();
	mmus_off();
}
