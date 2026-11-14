/*
 * Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
 * OpenBoot release it is part of (see LICENSE).
 *
 * All Advanced Sabre IOMMU Tests: DMA through the Sabre's IOMMU, made by
 * an EBus DMA channel (the parallel port's, which has its own FIFO) in
 * its diagnostic mode, with no device: DIAG_EN with EN_DMA makes one
 * burst of the FIFO (16 entries of 8 bytes, 128 bytes) from memory into
 * the FIFO, or with WRITE from the FIFO to memory, and shows it done in
 * DIAG_RD_DONE or DIAG_WR_DONE. The FIFO is read and written through its
 * window, 32 words at 000-03c and 100-13c, as an Ultra 10's POST reaches
 * it: 32-bit little-endian accesses, each word the same pattern (55..),
 * so that a read must replace every word and a write must give memory
 * the pattern in every byte, whatever the window's order.
 *
 * Init Sabre sets the group up as an Ultra 10's POST does: APB B's
 * command, I/O and memory maps and bridge control, the Sabre's PCI target
 * address space, the IOMMU on with its TSB at 8 MiB and its TLB empty,
 * and the RIO EBus's configuration header. A translation is checked where
 * the IOMMU's TLB shows it (tags at a580, data at a600, the control
 * register's MMU_DE set); a hit is told from a miss by the TSB entry made
 * invalid after the first transfer.
 */
#include "post.h"

#define SABRE_PA	0x1fe00000000ULL
#define BYPASS		0x15
#define LITTLE		0x1d			/* bypass, little-endian */
#define CACHED		0x14			/* physical, through the caches */

#define IOMMU_CTL	0x200
#define IOMMU_TSB	0x208
#define TAG_DIAG	0xa580
#define DATA_DIAG	0xa600
#define PCI_CTL		0x2000
#define PCI_AFSR	0x2010
#define PCI_TASR	0x2028			/* each bit a 512 MiB region */
#define PCI_AFSR_ERRS	0xff00000000000000ULL
#define CTL_EN		1ULL
#define CTL_DE		2ULL
#define CTL_LCKEN	(1ULL << 23)
#define CTL_LCKPTR(n)	((u64)(n) << 19)

/* The TSB (1K entries, 8 KiB pages: 8 MiB of DVMA from df80.0000) and
   the pages DMA reaches, as an Ultra 10's POST places them: transfer n's
   page at DVMA df80.0000 + (n * 11.0000 + (n / 2) * 2000, within the
   8 MiB), mapped to physical 100.0000 + n * 2000; each transfer's buffer 80
   into its page. */
#define TSB_PA		0x800000ULL
#define PAGE		0x2000U
#define DATA_PA(n)	(0x1000000ULL + PAGE * (n))
#define DVMA_BASE	0xdf800000U
#define TSB_INDEX(n)	(((n) * 0x88 + ((n) >> 1)) & 0x3ff)
#define DVMA(n)		(DVMA_BASE + PAGE * TSB_INDEX(n))
#define BUF		0x80
#define TTE_V		(1ULL << 63)
#define TTE_C		(1ULL << 4)
#define TTE_W		(1ULL << 1)
#define TAG_W		(1ULL << 21)
#define DATA_V		(1ULL << 30)
#define DATA_C		(1ULL << 28)
/* The data word's bits the checks compare: valid, cacheable and the page;
   not the Used bit (29) the TLB's replacement keeps, nor bit 31. */
#define DATA_CHECKED	0x501fffffULL

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
#define DMA_POLLS	50
#define FIFO_WORDS	16			/* in each half of the window */
#define BURST		(FIFO_WORDS * 8)
#define PATTERN		0x5555555555555555ULL
#define ONES		0xffffffffU

static u64 sx(u32 off) { return asi_ldx(SABRE_PA + off, BYPASS); }
static void sx_set(u32 off, u64 v) { asi_stx(SABRE_PA + off, BYPASS, v); }

#define CONFIG(bus, dev, fn, reg) \
	(0x1fe01000000ULL | (bus) << 16 | (dev) << 11 | (fn) << 8 | (reg))
#define APB_B(reg)	CONFIG(0, 1, 1, reg)
#define EBUS(reg)	CONFIG(1, 1, 0, reg)

static void tlb_invalid(void)
{
	u64 ctl = sx(IOMMU_CTL);
	u32 i;

	sx_set(IOMMU_CTL, ctl | CTL_DE);
	for (i = 0; i < 16; i++) {
		sx_set(TAG_DIAG + i * 8, 0);
		sx_set(DATA_DIAG + i * 8, 0);
	}
	sx_set(IOMMU_CTL, ctl);
}

/* A configuration register written and read back. */
static void cfg32(u64 a, u32 v, const char *msg)
{
	asi_st32(a, LITTLE, v);
	post_compare_at(asi_ld32(a, LITTLE), v, a, msg);
}

static void cfg8(u64 a, u32 v, const char *msg)
{
	asi_st8(a, LITTLE, v);
	post_compare_at(asi_ld8(a, LITTLE), v, a, msg);
}

void post_init_sabre_dma(void)
{
	u32 r;

	asi_st16(APB_B(0x04), LITTLE, 0x0147);	/* I/O, memory, master, parity, SERR# */
	asi_st8(APB_B(0xe0), LITTLE, 0x0f);	/* I/O map */
	asi_st8(APB_B(0xdf), LITTLE, 0x80);	/* memory map */
	sx_set(PCI_TASR, 0xff);
	asi_st8(APB_B(0x3e), LITTLE, 0x23);	/* bridge control */
	sx_set(IOMMU_CTL, 0);
	tlb_invalid();
	sx_set(IOMMU_TSB, TSB_PA);
	sx_set(IOMMU_CTL, CTL_EN);

	/* The RIO EBus: memory and master, its status clear, its BARs (the
	   status is not compared, its bits being the device's). */
	asi_st16(EBUS(0x04), LITTLE, 0x0006);
	post_compare_at(asi_ld16(EBUS(0x04), LITTLE), 0x0006, EBUS(0x04), "Ebus Command");
	asi_st16(EBUS(0x06), LITTLE, 0xf900);
	cfg8(EBUS(0x0c), 0, "Ebus Cache Line Size");
	cfg8(EBUS(0x0d), 0x40, "Ebus Latency Timer");
	cfg32(EBUS(0x10), 0xf0000000U, "Ebus BAR0");
	cfg32(EBUS(0x14), 0xf1000000U, "Ebus BAR1");
	for (r = 0x18; r <= 0x24; r += 4)
		cfg32(EBUS(r), 0, "Ebus BAR");
}

/* The TSB's entries invalid, the TLB empty; lock adds locking. */
static void iommu_ready(u64 lock)
{
	u32 i;

	for (i = 0; i < 1024; i++)
		asi_stx(TSB_PA + i * 8, CACHED, 0);
	sx_set(IOMMU_CTL, 0);
	if (lock)
		sx_set(IOMMU_CTL, CTL_LCKEN);	/* a write of its own */
	sx_set(IOMMU_CTL, lock);
	tlb_invalid();
	sx_set(IOMMU_CTL, lock | CTL_EN);
}

static void iommu_done(void)
{
	sx_set(IOMMU_CTL, 0);
	tlb_invalid();
	sx_set(IOMMU_CTL, CTL_EN);
}

/* The TLB's entry n as its diagnostic registers show it. */
static u64 tlb_tag(u32 n)
{
	u64 ctl = sx(IOMMU_CTL), v;

	sx_set(IOMMU_CTL, ctl | CTL_DE);
	v = sx(TAG_DIAG + n * 8);
	sx_set(IOMMU_CTL, ctl);
	return v;
}

static u64 tlb_data(u32 n)
{
	u64 ctl = sx(IOMMU_CTL), v;

	sx_set(IOMMU_CTL, ctl | CTL_DE);
	v = sx(DATA_DIAG + n * 8) & DATA_CHECKED;
	sx_set(IOMMU_CTL, ctl);
	return v;
}

static void check_tlb(u32 last, int writable)
{
	u32 k;

	/* All 16 entries, in order, after each transfer, as an Ultra 10's POST
	   reads them: the TLB fills from empty in order, so entries 0..last
	   hold transfers 0..last and the rest are still empty. */
	for (k = 0; k < 16 && !post_failed(); k++) {
		const u64 tag = k <= last ? (writable ? TAG_W : 0) | DVMA(k) >> 13 : 0;
		const u64 data = k <= last ? DATA_V | DATA_C | DATA_PA(k) >> 13 : 0;

		post_compare_at(tlb_tag(k), tag, SABRE_PA + TAG_DIAG + k * 8, "TLB CAM");
		if (!post_failed())
			post_compare_at(tlb_data(k), data, SABRE_PA + DATA_DIAG + k * 8, "TLB RAM");
	}
}

/* After a transfer: no PCI error logged in the Sabre. */
static void check_sabre(void)
{
	post_compare_at(sx(PCI_AFSR) & PCI_AFSR_ERRS, 0, SABRE_PA + PCI_AFSR, "Sabre PCI AFSR");
}

/* The window's 32 words. */
static void fifo_fill(u32 v)
{
	u32 i;

	for (i = 0; i < FIFO_WORDS; i++) {
		asi_st32(FIFO + 4 * i, LITTLE, v);
		asi_st32(FIFO + 0x100 + 4 * i, LITTLE, v);
	}
}

static void check_fifo(void)
{
	u32 i;

	for (i = 0; i < FIFO_WORDS && !post_failed(); i++) {
		post_compare_at(asi_ld32(FIFO + 4 * i, LITTLE), (u32)PATTERN, FIFO + 4 * i, "Ebus FIFO");
		post_compare_at(asi_ld32(FIFO + 0x100 + 4 * i, LITTLE), (u32)PATTERN,
				FIFO + 0x100 + 4 * i, "Ebus FIFO");
	}
}

/* A buffer's 128 bytes, through the caches. */
static void mem_fill(u64 pa, u64 v)
{
	u32 i;

	for (i = 0; i < BURST; i += 8)
		asi_stx(pa + i, CACHED, v);
}

static void check_mem(u64 pa)
{
	u32 i;

	for (i = 0; i < BURST && !post_failed(); i += 8)
		post_compare_at(asi_ldx(pa + i, CACHED), PATTERN, pa + i, "DMA data");
}

/* One diagnostic burst at dvma, as an Ultra 10's POST makes it: the
   channel reset (and RESET cleared from what the CSR then reads), the
   address loaded (no count), DIAG_EN and EN_DMA (and WRITE) added to the
   CSR, its done bit polled for, the channel reset; then none of DIAG_EN,
   EN_DMA, RESET (or WRITE) left on. */
static void dma_burst(u32 dvma, int to_memory)
{
	const u32 done = to_memory ? CSR_WR_DONE : CSR_RD_DONE;
	const u32 left = CSR_DIAG_EN | CSR_EN_DMA | CSR_RESET | (to_memory ? CSR_WRITE : 0);
	u32 csr, i;

	asi_st32(DMA_CSR, LITTLE, CSR_RESET);
	asi_st32(DMA_CSR, LITTLE, asi_ld32(DMA_CSR, LITTLE) & ~CSR_RESET);
	csr = asi_ld32(DMA_CSR, LITTLE);
	asi_st32(DMA_ADDR, LITTLE, dvma);
	asi_st32(DMA_CSR, LITTLE, csr | CSR_DIAG_EN | CSR_EN_DMA | (to_memory ? CSR_WRITE : 0));
	for (i = 0; i < DMA_POLLS; i++) {
		csr = asi_ld32(DMA_CSR, LITTLE);
		if (csr & done)
			break;
	}
	asi_st32(DMA_CSR, LITTLE, CSR_RESET);
	asi_st32(DMA_CSR, LITTLE, 0);
	if (!(csr & done)) {
		post_fail("Ebus Channel Engine\n\tDidn't Complete Dma Xfer");
		post_puts("\tEbus CSR Addr ");
		post_putx(DMA_CSR, 16);
		post_puts("\n\tEbus CSR \t ");
		post_putx(csr, 8);
		post_putc('\n');
		return;
	}
	post_compare_at(asi_ld32(DMA_CSR, LITTLE) & left, 0, DMA_CSR, "Ebus CSR");
}

/* A read from memory: the buffer the pattern, the window all ones, the
   burst; the window, the TLB and the Sabre. */
static void read_at(u32 n, int hit)
{
	const u64 pa = DATA_PA(n) + BUF;
	const u32 dvma = DVMA(n) + BUF;

	mem_fill(pa, PATTERN);
	asi_stx(TSB_PA + 8 * TSB_INDEX(n), CACHED, TTE_V | TTE_C | DATA_PA(n));
	fifo_fill(ONES);
	dma_burst(dvma, 0);
	if (!post_failed())
		check_fifo();
	if (!post_failed())
		check_tlb(n, 0);
	if (!post_failed())
		check_sabre();
	if (hit && !post_failed()) {
		asi_stx(TSB_PA + 8 * TSB_INDEX(n), CACHED, 0);
		fifo_fill(ONES);
		dma_burst(dvma, 0);
		if (!post_failed())
			check_fifo();
		if (!post_failed())
			check_sabre();
	}
}

/* A write to memory: the buffer another pattern, the window the
   pattern, the burst; memory, the TLB and the Sabre. */
static void write_at(u32 n, int hit)
{
	const u64 pa = DATA_PA(n) + BUF;
	const u32 dvma = DVMA(n) + BUF;

	mem_fill(pa, ~PATTERN);
	asi_stx(TSB_PA + 8 * TSB_INDEX(n), CACHED, TTE_V | TTE_C | TTE_W | DATA_PA(n));
	fifo_fill((u32)PATTERN);
	dma_burst(dvma, 1);
	if (!post_failed())
		check_mem(pa);
	if (!post_failed())
		check_tlb(n, 1);
	if (!post_failed())
		check_sabre();
	if (hit && !post_failed()) {
		asi_stx(TSB_PA + 8 * TSB_INDEX(n), CACHED, 0);
		mem_fill(pa, ~PATTERN);
		dma_burst(dvma, 1);
		if (!post_failed())
			check_mem(pa);
		if (!post_failed())
			check_sabre();
	}
}

/* Misses: a transfer through each of 16 TSB entries, one a TLB entry.
   Hits: one entry, then through the TLB alone. */
static void dma_test(void (*at)(u32, int), int hit)
{
	u32 n;

	iommu_ready(0);
	for (n = 0; n < (hit ? 1 : 16) && !post_failed(); n++)
		at(n, hit);
	iommu_done();
}

void post_dma_rd_miss(void) { dma_test(read_at, 0); }
void post_dma_rd_hit(void) { dma_test(read_at, 1); }
void post_dma_wr_miss(void) { dma_test(write_at, 0); }
void post_dma_wr_hit(void) { dma_test(write_at, 1); }

/* Pass-through: the IOMMU off, a DMA address in region 0 is the
   physical address itself. */
void post_dma_pass_rd(void)
{
	const u64 pa = DATA_PA(2) + BUF;

	sx_set(IOMMU_CTL, 0);
	mem_fill(pa, PATTERN);
	fifo_fill(ONES);
	dma_burst((u32)pa, 0);
	if (!post_failed())
		check_fifo();
	if (!post_failed())
		check_sabre();
	iommu_done();
}

void post_dma_pass_wr(void)
{
	const u64 pa = DATA_PA(2) + BUF;

	sx_set(IOMMU_CTL, 0);
	mem_fill(pa, ~PATTERN);
	fifo_fill((u32)PATTERN);
	dma_burst((u32)pa, 1);
	if (!post_failed())
		check_mem(pa);
	if (!post_failed())
		check_sabre();
	iommu_done();
}

/* Locking (the IIi manual's sequence): with LRU_LCKEN and LRU_LCKPTR
   set, three transfers through three TSB entries all load the one entry,
   which then holds the last; the others stay invalid. */
#define LOCKED 9

static void dma_locked(int to_memory)
{
	u32 i, n;

	iommu_ready(CTL_LCKEN | CTL_LCKPTR(LOCKED));
	for (i = 0; i < 3; i++)
		asi_stx(TSB_PA + 8 * TSB_INDEX(i), CACHED,
			TTE_V | TTE_C | (to_memory ? TTE_W : 0) | DATA_PA(i));
	for (i = 0; i < 3 && !post_failed(); i++) {
		const u64 pa = DATA_PA(i) + BUF;

		if (to_memory) {
			mem_fill(pa, ~PATTERN);
			fifo_fill((u32)PATTERN);
		} else {
			mem_fill(pa, PATTERN);
			fifo_fill(ONES);
		}
		dma_burst(DVMA(i) + BUF, to_memory);
		if (post_failed())
			break;
		if (to_memory)
			check_mem(pa);
		else
			check_fifo();
	}
	if (!post_failed())
		post_compare_at(tlb_tag(LOCKED), (to_memory ? TAG_W : 0) | DVMA(2) >> 13,
				SABRE_PA + TAG_DIAG + LOCKED * 8, "TLB CAM");
	for (n = 0; n < 16 && !post_failed(); n++)
		if (n != LOCKED)
			post_compare_at(tlb_data(n) & DATA_V, 0, SABRE_PA + DATA_DIAG + n * 8, "TLB RAM");
	iommu_done();
}

void post_dma_rd_lock(void) { dma_locked(0); }
void post_dma_wr_lock(void) { dma_locked(1); }
