/*
 * Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
 * OpenBoot release it is part of (see LICENSE).
 *
 * The POST's tests of the Sabre (the UltraSPARC IIi's PCI bus module):
 * its registers (through the bypass ASI), its own PCI configuration
 * (little-endian), its IOMMU's TLB through its diagnostic accesses, the
 * PIO path's decoding and byte enables, its interrupt maps and states.
 * Each register is put back as it was found.
 */
#include "post.h"

u32 asi_ld8(u64 va, u32 asi);
u32 asi_ld16(u64 va, u32 asi);
u32 asi_ld32(u64 va, u32 asi);
void asi_st8(u64 va, u32 asi, u32 v);
void asi_st16(u64 va, u32 asi, u32 v);
void asi_st32(u64 va, u32 asi, u32 v);

#define SABRE_PA	0x1fe00000000ULL
#define CONFIG_PA	0x1fe01000000ULL	/* PCI configuration, bus 0 */
#define BYPASS		0x15
#define LITTLE		0x1d			/* bypass, little-endian */

static u64 sx(u32 off) { return asi_ldx(SABRE_PA + off, BYPASS); }
static void sx_set(u32 off, u64 v) { asi_stx(SABRE_PA + off, BYPASS, v); }

/* A register written with a pattern and read back, then put back, with
   nothing else between; the bits mask keeps compared. */
static void swap(u64 pattern, u32 off, u64 mask, const char *msg)
{
	post_compare(asi_swap(SABRE_PA + off, BYPASS, pattern) & mask, pattern & mask, msg);
}

static void patterns(u32 off, u64 mask, const char *msg)
{
	swap(0, off, mask, msg);
	swap(~0ULL, off, mask, msg);
	swap(0xaaaaaaaaaaaaaaaaULL, off, mask, msg);
	swap(0x5555555555555555ULL, off, mask, msg);
}

/* ---------------------------------------------------------------------
 * The IOMMU.
 */
#define IOMMU_CTL	0x200
#define IOMMU_TSB	0x208
#define IOMMU_FLUSH	0x210
#define TAG_DIAG	0xa580		/* the TLB's tags */
#define DATA_DIAG	0xa600		/* and data */
#define TAG_MASK	0x1ffffffULL	/* ERRSTS, ERR, W, S, SIZE, VPN<31:13> */
#define DATA_MASK	0x701fffffULL	/* V, U, C, PA<33:13> */
#define CTL_ERRS	0x7000000ULL	/* error bits (cleared by writing 1) */
#define CTL_DE		2ULL		/* MMU_DE: the TLB's diagnostics */

static u64 iommu_ctl;

static void iommu_de(void)
{
	iommu_ctl = sx(IOMMU_CTL);
	sx_set(IOMMU_CTL, iommu_ctl | CTL_DE);
}

/* Each entry invalid (the TLB is a cache of the TSB), the control
   register put back. */
static void iommu_clear(void)
{
	u32 i;

	for (i = 0; i < 16; i++) {
		sx_set(TAG_DIAG + i * 8, 0);
		sx_set(DATA_DIAG + i * 8, 0);
	}
	sx_set(IOMMU_CTL, iommu_ctl & ~CTL_ERRS);
}

/* Init Sabre: the IOMMU's TLB invalid, its diagnostics off. */
void post_init_sabre(void)
{
	iommu_ctl = sx(IOMMU_CTL) & ~CTL_DE;
	sx_set(IOMMU_CTL, iommu_ctl | CTL_DE);
	iommu_clear();
}

/* The control register's TSB_SIZE and TBW_SIZE (its error bits written
   0), and the TSB base (PA<40:13>). */
void post_iommu_regs(void)
{
	static const u64 ctl_p[4] = { 0, 0x70004ULL, 0x20004ULL, 0x50000ULL };
	static const u64 tsb_p[4] = { 0, 0x1ffffffe000ULL, 0xaaaaaaa000ULL, 0x15555554000ULL };
	u64 ctl, tsb;
	u32 p;

	post_suspect("Sabre");
	ctl = sx(IOMMU_CTL) & ~CTL_ERRS;
	for (p = 0; p < 4; p++) {
		sx_set(IOMMU_CTL, (ctl & ~0x70004ULL) | ctl_p[p]);
		post_compare(sx(IOMMU_CTL) & 0x70004ULL, ctl_p[p], "IOMMU Control");
	}
	sx_set(IOMMU_CTL, ctl);
	tsb = sx(IOMMU_TSB);
	for (p = 0; p < 4; p++) {
		sx_set(IOMMU_TSB, tsb_p[p]);
		post_compare(sx(IOMMU_TSB), tsb_p[p], "IOMMU TSB Base");
	}
	sx_set(IOMMU_TSB, tsb);
}

static u64 index_pattern(u32 i) { return 0x0101010101010101ULL * i; }

static void iommu_array(u32 base, u64 mask, const char *msg)
{
	static const u64 pat[2] = { 0xaaaaaaaaaaaaaaaaULL, 0x5555555555555555ULL };
	u32 i, p;

	for (i = 0; i < 16; i++)
		sx_set(base + i * 8, index_pattern(i));
	for (i = 0; i < 16; i++)
		post_compare(sx(base + i * 8) & mask, index_pattern(i) & mask, msg);
	for (p = 0; p < 2; p++) {
		for (i = 0; i < 16; i++)
			sx_set(base + i * 8, pat[p]);
		for (i = 0; i < 16; i++)
			post_compare(sx(base + i * 8) & mask, pat[p] & mask, msg);
	}
}

void post_iommu_ram(void)
{
	post_suspect("Sabre");
	iommu_de();
	iommu_array(DATA_DIAG, DATA_MASK, "IOMMU RAM mismatch");
	iommu_clear();
}

void post_iommu_cam(void)
{
	post_suspect("Sabre");
	iommu_de();
	iommu_array(TAG_DIAG, TAG_MASK, "IOMMU CAM mismatch");
	iommu_clear();
}

/* Entry n's tag VPN n+1 (8K pages): the comparator (a408) with VA n+1 (at
   a400) then shows bit n; a flush (210) of that VA then leaves entry n
   invalid, the others valid. */
static void iommu_tags(void)
{
	u32 i;

	for (i = 0; i < 16; i++) {
		sx_set(TAG_DIAG + i * 8, i + 1);
		sx_set(DATA_DIAG + i * 8, 0x40000000ULL | i);
	}
}

void post_iommu_compare(void)
{
	u32 i;

	post_suspect("Sabre");
	iommu_de();
	iommu_tags();
	for (i = 0; i < 16; i++) {
		sx_set(0xa400, (u64)(i + 1) << 13);
		post_compare(sx(0xa408) & 0xffff, 1U << i, "IOMMU tag compare");
	}
	iommu_clear();
}

void post_iommu_flush(void)
{
	u32 i, j;

	post_suspect("Sabre");
	iommu_de();
	for (j = 0; j < 16; j++) {
		iommu_tags();
		sx_set(IOMMU_FLUSH, (u64)(j + 1) << 13);
		for (i = 0; i < 16; i++)
			post_compare(sx(DATA_DIAG + i * 8) & 0x40000000ULL,
				     i == j ? 0 : 0x40000000ULL, "IOMMU flush");
	}
	iommu_clear();
}

/* ---------------------------------------------------------------------
 * PBM A's configuration header: Sun's vendor and the Sabre's device ID,
 * a host bridge; the PCI control register's fields (MRLM_EN, ARB_PARK,
 * CPU_PRIO, ARB_PRIO, ERRINT_EN, RETRY_WAIT_EN, ARB_EN), the PCI
 * diagnostic register's (DIS_RETRY, the parity inversions), the PCI
 * target address space register and the IOMMU's virtual address
 * diagnostic register (VA<31:13>).
 */
void post_pbm_config(void)
{
	post_suspect("Sabre");
	post_compare(asi_ld32(CONFIG_PA, LITTLE), 0xa000108e, "Vendor/Device ID");
	post_compare(asi_ld32(CONFIG_PA + 8, LITTLE) >> 8, 0x60000, "Class Code");
}

void post_pbm_ctl(void) { post_suspect("Sabre"); patterns(0x2000, 0x10003f018fULL, "PCI Control/Status"); }
void post_pbm_diag(void) { post_suspect("Sabre"); patterns(0x2020, 0x4e, "PCI Diag"); }

void post_sabre_io(void)
{
	post_suspect("Sabre");
	patterns(0x2028, 0xff, "PCI Target Address Space");
	patterns(0xa400, 0xffffe000ULL, "IOMMU Virtual Address Diag");
}

/* ---------------------------------------------------------------------
 * The PIO decoder: the Sabre's configuration (its latency timer) and its
 * IOMMU's TLB data and tags (MMU_DE set) each written with its own value,
 * then all read back, so that an address decoded to the wrong register
 * shows; the latency timer put back, the TLB left invalid.
 */
static const u32 pio_data[16] = {
	0x00022222, 0x10133333, 0x40044444, 0x50155555,
	0x40066666, 0x50177777, 0x00088888, 0x10199999,
	0x000aaaaa, 0x101bbbbb, 0x400ccccc, 0x501ddddd,
	0x400eeeee, 0x501fffff, 0x10111110, 0x00022221,
};
static const u32 pio_tag[16] = {
	0x01333332, 0x00444443, 0x01555554, 0x00666665,
	0x01777776, 0x00888887, 0x01999998, 0x00aaaaa9,
	0x01bbbbba, 0x00cccccb, 0x01dddddc, 0x00eeeeed,
	0x01fffffe, 0x0111110f, 0x00222220, 0x01333331,
};
#define LATENCY 0x0d

void post_pio_decoder(void)
{
	u32 i, latency;

	post_suspect("Sabre");
	iommu_de();
	latency = asi_ld8(CONFIG_PA + LATENCY, LITTLE);
	asi_st8(CONFIG_PA + LATENCY, LITTLE, 0x10);
	for (i = 0; i < 16; i++) {
		sx_set(DATA_DIAG + i * 8, pio_data[i]);
		sx_set(TAG_DIAG + i * 8, pio_tag[i]);
	}
	post_compare(asi_ld8(CONFIG_PA + LATENCY, LITTLE), 0x10, "PCI Latency Timer");
	for (i = 0; i < 16; i++)
		post_compare(sx(DATA_DIAG + i * 8) & DATA_MASK, pio_data[i], "IOMMU RAM decode");
	for (i = 0; i < 16; i++)
		post_compare(sx(TAG_DIAG + i * 8) & TAG_MASK, pio_tag[i], "IOMMU CAM decode");
	asi_st8(CONFIG_PA + LATENCY, LITTLE, latency);
	iommu_clear();
}

/* The byte enables: an EBus word (the NVRAM's first scratch word, saved
   and put back) read as bytes and halfwords after a word write, and read
   as a word after byte and halfword writes. */
#define BE_PA (NVRAM_PA + 0x1d00)
#define BE_PATTERN 0x01234567

static void be_reset(void)
{
	asi_st32(BE_PA, BYPASS, BE_PATTERN);
	post_compare(asi_ld32(BE_PA, BYPASS), BE_PATTERN, "Word write/read");
}

void post_byte_enable(void)
{
	u32 saved, i;

	post_suspect("Sabre");
	saved = asi_ld32(BE_PA, BYPASS);
	be_reset();
	for (i = 0; i < 4; i++)
		post_compare(asi_ld8(BE_PA + i, BYPASS), BE_PATTERN >> (3 - i) * 8 & 0xff, "Byte read");
	be_reset();
	post_compare(asi_ld16(BE_PA, BYPASS), 0x0123, "Halfword read");
	post_compare(asi_ld16(BE_PA + 2, BYPASS), 0x4567, "Halfword read");
	be_reset();
	asi_st8(BE_PA, BYPASS, 0xfe);
	post_compare(asi_ld32(BE_PA, BYPASS), 0xfe234567, "Byte 0 write");
	asi_st8(BE_PA + 1, BYPASS, 0xdc);
	post_compare(asi_ld32(BE_PA, BYPASS), 0xfedc4567, "Byte 1 write");
	asi_st8(BE_PA + 2, BYPASS, 0xba);
	post_compare(asi_ld32(BE_PA, BYPASS), 0xfedcba67, "Byte 2 write");
	asi_st8(BE_PA + 3, BYPASS, 0x98);
	post_compare(asi_ld32(BE_PA, BYPASS), 0xfedcba98, "Byte 3 write");
	be_reset();
	asi_st16(BE_PA, BYPASS, 0xfedc);
	post_compare(asi_ld32(BE_PA, BYPASS), 0xfedc4567, "Halfword 0 write");
	asi_st16(BE_PA + 2, BYPASS, 0xba98);
	post_compare(asi_ld32(BE_PA, BYPASS), 0xfedcba98, "Halfword 1 write");
	asi_st32(BE_PA, BYPASS, saved);
}

/* ---------------------------------------------------------------------
 * The interrupt maps: PCI's (c00-c38) and the on-board devices' (1000
 * on), their TID fields (30:26), each written (left invalid) and read
 * back, then put back.
 */
static void imap_test(u32 off)
{
	static const u64 pat[4] = { 0x7c000000ULL, 0, 0x28000000ULL, 0x54000000ULL };
	u64 saved = sx(off);
	u32 p;

	for (p = 0; p < 4; p++) {
		sx_set(off, (saved & ~0xfc000000ULL) | pat[p]);
		post_compare(sx(off) & 0x7c000000ULL, pat[p], "Interrupt Map");
	}
	sx_set(off, saved);
}

void post_imap(void)
{
	u32 i;

	post_suspect("Sabre");
	for (i = 0; i < 8; i++)
		imap_test(0xc00 + i * 8);
	for (i = 0; i < 32; i++)
		imap_test(0x1000 + i * 8);
}

/* The PCI interrupts' clear registers (1400 on): each one whose map is
   invalid and whose state is idle set received (1) then pending (3) and
   back to idle (0), its state read in the interrupt state diagnostic
   register (a800, two bits an interrupt). */
static u32 int_state(u32 ino) { return (u32)(sx(0xa800) >> (ino * 2)) & 3; }

void post_iclr(void)
{
	static const u32 states[3] = { 0, 3, 1 };
	u32 i, s;

	post_suspect("Sabre");
	for (i = 0; i < 32; i++) {
		if ((sx(0xc00 + (i >> 2) * 8) & 0x80000000ULL) || int_state(i))
			continue;
		for (s = 3; s-- > 0;) {
			sx_set(0x1400 + i * 8, states[s]);
			post_compare(int_state(i), states[s], "Interrupt state");
		}
	}
}
