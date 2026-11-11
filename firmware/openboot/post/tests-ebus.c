/*
 * Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
 * OpenBoot release it is part of (see LICENSE).
 *
 * All Advanced Sabre IOMMU Tests: DMA through the Sabre's IOMMU, made by
 * an EBus DMA channel (the parallel port's, which has its own FIFO) in
 * its diagnostic mode, with no device: DIAG_EN with EN_DMA makes one
 * burst of the FIFO (16 entries of 8 bytes) from memory into the FIFO,
 * or with WRITE from the FIFO to memory, and shows it done in
 * DIAG_RD_DONE or DIAG_WR_DONE. The FIFO is read and written through
 * its window, entry n's first word at 4n and its second at 0x100 + 4n.
 *
 * A translation is checked where the IOMMU's TLB shows it (tags at
 * a580, data at a600, the control register's MMU_DE set); a hit is told
 * from a miss by the TSB entry made invalid after the first transfer.
 * The IOMMU, its TSB base and the PCI target address space are put back
 * as they were found.
 */
#include "post.h"

u32 asi_ld16(u64 va, u32 asi);
void asi_st16(u64 va, u32 asi, u32 v);
u32 asi_ld32(u64 va, u32 asi);
void asi_st32(u64 va, u32 asi, u32 v);

#define SABRE_PA	0x1fe00000000ULL
#define BYPASS		0x15
#define LITTLE		0x1d			/* bypass, little-endian */
#define CACHED		0x14			/* physical, through the caches */

#define IOMMU_CTL	0x200
#define IOMMU_TSB	0x208
#define IOMMU_FLUSH	0x210
#define TAG_DIAG	0xa580
#define DATA_DIAG	0xa600
#define PCI_TASR	0x2028			/* each bit a 512 MiB region */
#define CTL_EN		1ULL
#define CTL_DE		2ULL
#define CTL_LCKEN	(1ULL << 23)
#define CTL_LCKPTR(n)	((u64)(n) << 19)
#define CTL_ERRS	0x7000000ULL

/* The TSB (1K entries, 8 KiB pages: 8 MiB of DVMA) and the pages DMA
   reaches, just past the POST's own 8 MiB of bank 0. */
#define TSB_PA		0x800000ULL
#define PAGE		0x2000U
#define DATA_PA(n)	(TSB_PA + PAGE * (1 + (n)))
#define DVMA(n)		(0xdf800000U + PAGE * (n))	/* TSB entry n */
#define TTE_V		(1ULL << 63)
#define TTE_C		(1ULL << 4)
#define TTE_W		(1ULL << 1)
#define TAG_W		(1ULL << 21)
#define DATA_V		(1ULL << 30)
#define DATA_C		(1ULL << 28)

/* The parallel port's EBus DMA channel (little-endian registers). */
#define DMA_CSR		0x1fff1700000ULL
#define DMA_ADDR	0x1fff1700004ULL
#define FIFO		0x1fff1730000ULL
#define CSR_RESET	(1U << 7)
#define CSR_WRITE	(1U << 8)
#define CSR_EN_DMA	(1U << 9)
#define CSR_RD_DONE	(1U << 11)
#define CSR_WR_DONE	(1U << 12)
#define CSR_DIAG_EN	(1U << 20)
#define FIFO_ENTRIES	16
#define BURST		(FIFO_ENTRIES * 8)

static u64 sx(u32 off) { return asi_ldx(SABRE_PA + off, BYPASS); }
static void sx_set(u32 off, u64 v) { asi_stx(SABRE_PA + off, BYPASS, v); }

/* APB B's command register (I/O, memory, master, parity and SERR#
   enables), and the EBus's (memory and master), for the DMA's path. */
#define APB_B_COMMAND	0x1fe01000904ULL
#define EBUS_COMMAND	0x1fe01010804ULL

static u64 saved_ctl, saved_tsb, saved_tasr;
static u32 saved_apb, saved_ebus;

static void tlb_invalid(void)
{
	u32 i;

	for (i = 0; i < 16; i++) {
		sx_set(TAG_DIAG + i * 8, 0);
		sx_set(DATA_DIAG + i * 8, 0);
	}
}

/* The IOMMU on, the TSB's entries invalid, the TLB empty; ctl adds
   locking. Region 6 (c000.0000) is the module's, and region 0 too for
   pass-through. */
static void iommu_on(u64 lock)
{
	u32 i;

	saved_ctl = sx(IOMMU_CTL) & ~CTL_ERRS;
	saved_tsb = sx(IOMMU_TSB);
	saved_tasr = sx(PCI_TASR);
	saved_apb = asi_ld16(APB_B_COMMAND, LITTLE);
	saved_ebus = asi_ld16(EBUS_COMMAND, LITTLE);
	asi_st16(APB_B_COMMAND, LITTLE, saved_apb | 0x147);
	asi_st16(EBUS_COMMAND, LITTLE, saved_ebus | 0x6);
	for (i = 0; i < 1024; i++)
		asi_stx(TSB_PA + i * 8, CACHED, 0);
	sx_set(IOMMU_CTL, 0);
	if (lock)
		sx_set(IOMMU_CTL, CTL_LCKEN);	/* a write of its own */
	sx_set(IOMMU_CTL, lock | CTL_DE);
	tlb_invalid();
	sx_set(IOMMU_TSB, TSB_PA);
	sx_set(PCI_TASR, saved_tasr | 0x41);
	sx_set(IOMMU_CTL, lock | CTL_EN);
}

static void iommu_back(void)
{
	sx_set(IOMMU_CTL, CTL_DE);
	tlb_invalid();
	sx_set(IOMMU_CTL, saved_ctl);
	sx_set(IOMMU_TSB, saved_tsb);
	sx_set(PCI_TASR, saved_tasr);
	asi_st16(EBUS_COMMAND, LITTLE, saved_ebus);
	asi_st16(APB_B_COMMAND, LITTLE, saved_apb);
}

/* The TLB's entry n as its diagnostic registers show it. */
static u64 tlb_tag(u32 n)
{
	u64 ctl = sx(IOMMU_CTL), v;

	sx_set(IOMMU_CTL, ctl | CTL_DE);
	v = sx(TAG_DIAG + n * 8) & 0x1ffffffULL;
	sx_set(IOMMU_CTL, ctl);
	return v;
}

static u64 tlb_data(u32 n)
{
	u64 ctl = sx(IOMMU_CTL), v;

	sx_set(IOMMU_CTL, ctl | CTL_DE);
	v = sx(DATA_DIAG + n * 8) & 0x701fffffULL;
	sx_set(IOMMU_CTL, ctl);
	return v;
}

/* The TLB entry a translation loads (the IOMMU's LRU picks it): the one
   whose tag is the address's page, else 16. */
static u32 tlb_find(u32 dvma)
{
	u32 n;

	for (n = 0; n < 16; n++)
		if ((tlb_data(n) & DATA_V) && (tlb_tag(n) & 0x7ffff) == (dvma >> 13 & 0x7ffff))
			return n;
	return 16;
}

static void check_tlb(u32 dvma, u64 pa, int writable)
{
	u32 n = tlb_find(dvma);

	post_compare(n < 16, 1, "IOMMU TLB entry not loaded");
	if (n >= 16)
		return;
	post_compare_at(tlb_tag(n), (writable ? TAG_W : 0) | (dvma >> 13 & 0x7ffff),
			SABRE_PA + TAG_DIAG + n * 8, "TLB CAM");
	post_compare_at(tlb_data(n), DATA_V | DATA_C | (pa >> 13),
			SABRE_PA + DATA_DIAG + n * 8, "TLB RAM");
}

/* The pattern for entry i's word w (0 or 1), a test's own (seed). */
static u32 pattern(u32 seed, u32 i, u32 w)
{
	return (seed + i * 0x01010101U) ^ (w ? 0xffffffffU : 0);
}

static void fifo_fill(u32 v)
{
	u32 i;

	for (i = 0; i < FIFO_ENTRIES; i++) {
		phys_st32(FIFO + 4 * i, v);
		phys_st32(FIFO + 0x100 + 4 * i, v);
	}
}

static void mem_fill(u64 pa, u32 seed)
{
	u32 i;

	for (i = 0; i < FIFO_ENTRIES; i++)
		asi_stx(pa + 8 * i, CACHED, (u64)pattern(seed, i, 0) << 32 | pattern(seed, i, 1));
}

static void dma_reset(void)
{
	asi_st32(DMA_CSR, LITTLE, CSR_RESET);
	asi_st32(DMA_CSR, LITTLE, 0);
}

/* One diagnostic burst at dvma: the channel reset, the address loaded
   (no count), DIAG_EN and EN_DMA (and WRITE), then its done bit. */
static void dma_burst(u32 dvma, int to_memory)
{
	u32 done = to_memory ? CSR_WR_DONE : CSR_RD_DONE, csr = 0, i;

	dma_reset();
	asi_st32(DMA_ADDR, LITTLE, dvma);
	asi_st32(DMA_CSR, LITTLE, CSR_DIAG_EN | CSR_EN_DMA | (to_memory ? CSR_WRITE : 0));
	for (i = 0; i < 100000; i++) {
		csr = asi_ld32(DMA_CSR, LITTLE);
		if (csr & done)
			break;
	}
	if (!(csr & done) && !post_failed()) {
		post_fail("Ebus Channel Engine\n\tDidn't Complete Dma Xfer");
		post_puts("\tEbus CSR Addr ");
		post_putx64(DMA_CSR);
		post_puts("\n\tEbus CSR \t ");
		post_putx(csr, 8);
		post_putc('\n');
	}
	dma_reset();
}

/* The FIFO against memory at pa. */
static void check_fifo(u32 seed, u64 pa)
{
	u32 i;

	for (i = 0; i < FIFO_ENTRIES; i++) {
		post_compare_at(phys_ld32(FIFO + 4 * i), pattern(seed, i, 0), pa + 8 * i, "Ebus FIFO");
		post_compare_at(phys_ld32(FIFO + 0x100 + 4 * i), pattern(seed, i, 1), pa + 8 * i + 4, "Ebus FIFO");
	}
}

/* Memory at pa against the FIFO's pattern (read past the caches). */
static void check_mem(u32 seed, u64 pa)
{
	u32 i;

	for (i = 0; i < FIFO_ENTRIES; i++)
		post_compare_at(asi_ldx(pa + 8 * i, BYPASS),
				(u64)pattern(seed, i, 0) << 32 | pattern(seed, i, 1), pa + 8 * i, "DMA data");
}

static void fifo_load(u32 seed)
{
	u32 i;

	for (i = 0; i < FIFO_ENTRIES; i++) {
		phys_st32(FIFO + 4 * i, pattern(seed, i, 0));
		phys_st32(FIFO + 0x100 + 4 * i, pattern(seed, i, 1));
	}
}

/* A read from memory: a TSB entry (consistent), the FIFO filled with
   ones, the burst, the FIFO and the TLB. With hit, the TSB entry made
   invalid and the burst made again, through the TLB alone. */
static void dma_read(int hit)
{
	const u32 seed = hit ? 0x5a00a500U : 0x10203040U;

	post_suspect("Sabre");
	iommu_on(0);
	mem_fill(DATA_PA(0), seed);
	asi_stx(TSB_PA, CACHED, TTE_V | TTE_C | DATA_PA(0));
	fifo_fill(0xffffffffU);
	dma_burst(DVMA(0), 0);
	if (!post_failed())
		check_tlb(DVMA(0), DATA_PA(0), 0);
	if (!post_failed())
		check_fifo(seed, DATA_PA(0));
	if (hit && !post_failed()) {
		asi_stx(TSB_PA, CACHED, 0);
		fifo_fill(0xffffffffU);
		dma_burst(DVMA(0), 0);
		if (!post_failed())
			check_fifo(seed, DATA_PA(0));
	}
	iommu_back();
}

/* A write to memory: a writable TSB entry, memory filled with ones, the
   FIFO loaded, the burst, memory and the TLB. */
static void dma_write(int hit)
{
	const u32 seed = hit ? 0xc3c33c3cU : 0x0f1e2d3cU;

	post_suspect("Sabre");
	iommu_on(0);
	mem_fill(DATA_PA(1), 0xffffffffU);
	asi_stx(TSB_PA + 8, CACHED, TTE_V | TTE_C | TTE_W | DATA_PA(1));
	fifo_load(seed);
	dma_burst(DVMA(1), 1);
	if (!post_failed())
		check_tlb(DVMA(1), DATA_PA(1), 1);
	if (!post_failed())
		check_mem(seed, DATA_PA(1));
	if (hit && !post_failed()) {
		asi_stx(TSB_PA + 8, CACHED, 0);
		mem_fill(DATA_PA(1), 0xffffffffU);
		fifo_load(~seed);
		dma_burst(DVMA(1), 1);
		if (!post_failed())
			check_mem(~seed, DATA_PA(1));
	}
	iommu_back();
}

void post_dma_rd_miss(void) { dma_read(0); }
void post_dma_rd_hit(void) { dma_read(1); }
void post_dma_wr_miss(void) { dma_write(0); }
void post_dma_wr_hit(void) { dma_write(1); }

/* Pass-through: the IOMMU off, a DMA address in region 0 is the
   physical address itself. */
void post_dma_pass_rd(void)
{
	post_suspect("Sabre");
	iommu_on(0);
	sx_set(IOMMU_CTL, 0);
	mem_fill(DATA_PA(2), 0x21436587U);
	fifo_fill(0xffffffffU);
	dma_burst((u32)DATA_PA(2), 0);
	if (!post_failed())
		check_fifo(0x21436587U, DATA_PA(2));
	iommu_back();
}

void post_dma_pass_wr(void)
{
	post_suspect("Sabre");
	iommu_on(0);
	sx_set(IOMMU_CTL, 0);
	mem_fill(DATA_PA(2), 0xffffffffU);
	fifo_load(0x78563412U);
	dma_burst((u32)DATA_PA(2), 1);
	if (!post_failed())
		check_mem(0x78563412U, DATA_PA(2));
	iommu_back();
}

/* Locking (the IIi manual's sequence): with LRU_LCKEN and LRU_LCKPTR
   set, three transfers through three TSB entries all load the one entry,
   which then holds the last; the others stay invalid. */
#define LOCKED 9

static void dma_locked(int to_memory)
{
	u32 i, n, seed;

	post_suspect("Sabre");
	iommu_on(CTL_LCKEN | CTL_LCKPTR(LOCKED));
	for (i = 0; i < 3; i++) {
		mem_fill(DATA_PA(i), to_memory ? 0xffffffffU : 0x01000000U * (i + 1));
		asi_stx(TSB_PA + 8 * i, CACHED, TTE_V | TTE_C | (to_memory ? TTE_W : 0) | DATA_PA(i));
	}
	for (i = 0; i < 3 && !post_failed(); i++) {
		seed = 0x01000000U * (i + 1);
		if (to_memory)
			fifo_load(seed);
		else
			fifo_fill(0xffffffffU);
		dma_burst(DVMA(i), to_memory);
		if (post_failed())
			break;
		if (to_memory)
			check_mem(seed, DATA_PA(i));
		else
			check_fifo(seed, DATA_PA(i));
	}
	if (!post_failed())
		post_compare_at(tlb_tag(LOCKED), (to_memory ? TAG_W : 0) | DVMA(2) >> 13,
				SABRE_PA + TAG_DIAG + LOCKED * 8, "TLB CAM");
	for (n = 0; n < 16 && !post_failed(); n++)
		if (n != LOCKED)
			post_compare_at(tlb_data(n) & DATA_V, 0, SABRE_PA + DATA_DIAG + n * 8, "TLB RAM");
	sx_set(IOMMU_CTL, sx(IOMMU_CTL) & ~CTL_LCKEN);
	iommu_back();
}

void post_dma_rd_lock(void) { dma_locked(0); }
void post_dma_wr_lock(void) { dma_locked(1); }
