/*
 * Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
 * OpenBoot release it is part of (see LICENSE).
 *
 * All Basic Cheerio Tests: the PCI configuration headers of the RIO
 * ("Cheerio") on APB B's bus (bus 1, device 1, the bus number the POST
 * gives it): function 0 the EBus, function 1 the Ethernet. Each register
 * is tested as an Ultra 10's POST tests it: its fixed bits (F) and the
 * bits it takes (M) written as (pattern & M) | F with 55.., aa.., 33..,
 * 66.., 99.., cc.., a walking one and a walking zero, each read back; the
 * register put back after, or at its first mismatch, and the next one
 * tested still. Identity registers (M 0) are written their own value;
 * the latency timer and interrupt line are written with bit 7 clear.
 */
#include "post.h"

#define LITTLE		0x1d
#define RIO_CONFIG(fn, reg) (0x1fe01000000ULL | 1 << 16 | 1 << 11 | (fn) << 8 | (reg))

struct reg {
	u8 offset, width;		/* 8 or 16 bits */
	u16 fixed, mask;
	const char *name;		/* the EBus's message's */
};

static u32 cfg_ld(u64 a, u32 w) { return w == 16 ? asi_ld16(a, LITTLE) : asi_ld8(a, LITTLE); }

static void cfg_st(u64 a, u32 w, u32 v)
{
	if (w == 16)
		asi_st16(a, LITTLE, v);
	else
		asi_st8(a, LITTLE, v);
}

/* The compare report: a 64-bit value as 16 digits. */
static void put_l(const char *label, u64 v)
{
	post_puts(label);
	post_putx(v, 16);
	post_putc('\n');
}

static int reg_test(u32 fn, const struct reg *r, int ebus)
{
	static const u16 pat[6] = { 0x5555, 0xaaaa, 0x3333, 0x6666, 0x9999, 0xcccc };
	const u64 a = RIO_CONFIG(fn, r->offset);
	const u32 all = r->width == 16 ? 0xffff : 0xff, was = cfg_ld(a, r->width);
	u32 i, v = 0, got = 0, ok = 1;

	for (i = 0; i < 6 + 2 * r->width && ok; i++) {
		if (i < 6)
			v = pat[i] & r->mask;
		else if (i < 6 + r->width)
			v = (1U << (i - 6)) & r->mask;
		else
			v = r->mask & ~(1U << (i - 6 - r->width));
		v = (v | r->fixed) & all;
		cfg_st(a, r->width, v);
		got = cfg_ld(a, r->width);
		ok = got == v;
	}
	cfg_st(a, r->width, was);
	if (ok)
		return 0;
	if (post_failed())		/* (a register before this one failed) */
		post_puts("Data miss compare\n");
	else
		post_fail("Data miss compare");
	put_l("\taddr ", a);
	put_l("\texp  ", v);
	put_l("\tobs  ", got);
	if (ebus) {
		post_puts("Cheerio Test failed at ");
		post_puts(r->name);
		post_puts(", U24\n");
	}
	return 1;
}

static void header(u32 fn, const struct reg *regs, u32 n)
{
	u32 i;

	for (i = 0; i < n; i++)
		reg_test(fn, &regs[i], fn == 0);
}

void post_rio_ebus_config(void)
{
	static const struct reg regs[] = {
		{ 0x00, 16, 0x108e, 0, "VendID" },
		{ 0x02, 16, 0x1000, 0, "DevID" },
		{ 0x09, 8, 0x00, 0, "ProgIF" },
		{ 0x0a, 8, 0x80, 0, "SubClass" },	/* a bridge, other */
		{ 0x0b, 8, 0x06, 0, "BaseClass" },
		{ 0x0d, 8, 0x00, 0x7f, "LatTimer" },
		{ 0x0e, 8, 0x80, 0, "HeadType" },
		{ 0x3c, 8, 0x00, 0x7f, "IntLine" },
	};

	header(0, regs, sizeof(regs) / sizeof(regs[0]));
}

void post_rio_net_config(void)
{
	static const struct reg regs[] = {
		{ 0x00, 16, 0x108e, 0, 0 },
		{ 0x02, 16, 0x1001, 0, 0 },
		{ 0x09, 8, 0x00, 0, 0 },
		{ 0x0a, 8, 0x00, 0, 0 },
		{ 0x0b, 8, 0x02, 0x02, 0 },		/* Ethernet */
		{ 0x0d, 8, 0x00, 0x7f, 0 },
		{ 0x0e, 8, 0x80, 0, 0 },
		{ 0x3c, 8, 0x00, 0x7f, 0 },
	};

	header(1, regs, sizeof(regs) / sizeof(regs[0]));
}
