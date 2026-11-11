/*
 * Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
 * OpenBoot release it is part of (see LICENSE).
 *
 * All Basic Cheerio Tests: the PCI configuration headers of the RIO
 * ("Cheerio") on APB B's bus (bus 1, device 1, the bus number the reset
 * code gives it): function 0 the EBus, function 1 the Ethernet. Their
 * identity (vendor, device, class, header type) stays as it is when
 * written; the latency timer and interrupt line keep what is written to
 * them (bits 6:0), and are put back after.
 */
#include "post.h"

u32 asi_ld8(u64 va, u32 asi);
u32 asi_ld16(u64 va, u32 asi);
void asi_st8(u64 va, u32 asi, u32 v);
void asi_st16(u64 va, u32 asi, u32 v);

#define LITTLE		0x1d
#define RIO_CONFIG(fn, reg) (0x1fe01000000ULL | 1 << 16 | 1 << 11 | (fn) << 8 | (reg))

static void read_only16(u32 fn, u32 reg, u32 expected, const char *msg)
{
	u64 a = RIO_CONFIG(fn, reg);

	post_compare(asi_ld16(a, LITTLE), expected, msg);
	asi_st16(a, LITTLE, ~expected & 0xffff);
	post_compare(asi_ld16(a, LITTLE), expected, msg);
}

static void read_only8(u32 fn, u32 reg, u32 expected, const char *msg)
{
	u64 a = RIO_CONFIG(fn, reg);

	post_compare(asi_ld8(a, LITTLE), expected, msg);
	asi_st8(a, LITTLE, ~expected & 0xff);
	post_compare(asi_ld8(a, LITTLE), expected, msg);
}

/* Seven bits of each are tested, as an Ultra 10's POST tests them (bit 7
   left alone). */
static void read_write8(u32 fn, u32 reg, const char *msg)
{
	static const u8 pat[] = { 0x55, 0x2a, 0x33, 0x4c, 0x0f, 0x70, 0, 0x7f };
	u64 a = RIO_CONFIG(fn, reg);
	u32 was = asi_ld8(a, LITTLE), i;

	for (i = 0; i < sizeof(pat); i++) {
		asi_st8(a, LITTLE, pat[i]);
		post_compare(asi_ld8(a, LITTLE) & 0x7f, pat[i], msg);
	}
	for (i = 0; i < 7; i++) {
		asi_st8(a, LITTLE, 1U << i);
		post_compare(asi_ld8(a, LITTLE) & 0x7f, 1U << i, msg);
		asi_st8(a, LITTLE, 0x7f & ~(1U << i));
		post_compare(asi_ld8(a, LITTLE) & 0x7f, 0x7f & ~(1U << i), msg);
	}
	asi_st8(a, LITTLE, was);
}

static void header(u32 fn, u32 device, u32 class)
{
	read_only16(fn, 0x00, 0x108e, "Vendor ID");
	read_only16(fn, 0x02, device, "Device ID");
	read_only8(fn, 0x09, class & 0xff, "Programming Interface");
	read_only8(fn, 0x0a, class >> 8 & 0xff, "Sub-Class Code");
	read_only8(fn, 0x0b, class >> 16, "Base Class Code");
	read_only8(fn, 0x0e, 0x80, "Header Type");
	read_write8(fn, 0x0d, "Latency Timer");
	read_write8(fn, 0x3c, "Interrupt Line");
}

void post_rio_ebus_config(void)
{
	post_suspect("Cheerio");
	header(0, 0x1000, 0x068000);	/* a bridge, other */
}

void post_rio_net_config(void)
{
	post_suspect("Cheerio");
	header(1, 0x1001, 0x020000);	/* Ethernet */
}
