/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Andy Taylor
 *
 * ffbprom: read a Creator / Creator3D card's PROM (its FCode and logo) on
 * a running Solaris system, and log it for "sunrom log" to put back
 * together.
 *
 * The card's PROM holds a byte at each fourth address of its UPA space,
 * from the address its first "reg" entry gives (0x1fc00000000 for the
 * Ultra 5/10's UPA slot); the card's FCode maps 0x40000 bytes of it, 0x10000
 * PROM bytes, and this reads those. It reads nothing else and writes
 * nothing to the card. The work is done in _init, which then returns EIO,
 * so the module is never left loaded.
 *
 * Only SUNW,ffb devices (Creator, Creator3D) are read: the module walks the
 * device tree for them and takes the address from their own "reg"
 * properties.
 *
 * Build (64-bit kernel, "isainfo -kv"; for a 32-bit kernel leave out -m64
 * and -mcmodel=medlow, or -xarch=v9):
 *   gcc -m64 -mcmodel=medlow -mno-app-regs -fno-pic -ffreestanding \
 *       -D_KERNEL -O2 -c ffbprom.c && ld -r -o ffbprom ffbprom.o
 * or with Sun's compiler:
 *   cc -xarch=v9 -xregs=no%appl -D_KERNEL -O -c ffbprom.c && ld -r -o ffbprom ffbprom.o
 * Run, as root:
 *   modload ./ffbprom          ("I/O error" is expected)
 *   sunrom log /var/adm/messages ffb0 ffb-prom.bin
 */
#include <sys/types.h>
#include <sys/errno.h>
#include <sys/modctl.h>
#include <sys/cmn_err.h>
#include <sys/systm.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>

/* A byte load by physical address, through the I/O space (sun4u). */
extern uint8_t ldbphysio(uint64_t physaddr);

#define PROM_BYTES 0x10000 /* PROM bytes, a byte each fourth address */
#define PER_LINE   64

static struct modlmisc modlmisc = {&mod_miscops, "ffbprom"};
static struct modlinkage modlinkage = {MODREV_1, {(void *)&modlmisc, NULL}};

static const char hex[] = "0123456789abcdef";

/* One card's PROM, as lines of PER_LINE bytes: a start line with the
 * address, the bytes, and an end line. */
static void
dump(int unit, uint64_t pa)
{
	char line[PER_LINE * 2 + 1];
	int at, i;
	uint8_t b;

	cmn_err(CE_CONT, "?sunrom: ffb%d start %x%08x %x\n", unit, (uint_t)(pa >> 32), (uint_t)pa, PROM_BYTES);
	for (at = 0; at < PROM_BYTES; at += PER_LINE) {
		for (i = 0; i < PER_LINE; i++) {
			b = ldbphysio(pa + 4ULL * (uint64_t)(at + i));
			line[2 * i] = hex[b >> 4];
			line[2 * i + 1] = hex[b & 15];
		}
		line[PER_LINE * 2] = '\0';
		cmn_err(CE_CONT, "?sunrom: ffb%d %04x %s\n", unit, at, line);
		delay(drv_usectohz(2000)); /* (so syslog keeps up) */
	}
	cmn_err(CE_CONT, "?sunrom: ffb%d end\n", unit);
}

int
_init(void)
{
	dev_info_t *dip;
	int *reg;
	uint_t n;
	int unit = 0;
	uint64_t pa;

	/* The cards are children of the root node, named SUNW,ffb. */
	for (dip = ddi_get_child(ddi_root_node()); dip != NULL; dip = ddi_get_next_sibling(dip)) {
		if (strcmp(ddi_node_name(dip), "SUNW,ffb") != 0) continue;
		if (ddi_prop_lookup_int_array(DDI_DEV_T_ANY, dip, DDI_PROP_DONTPASS, "reg", &reg, &n) !=
		    DDI_PROP_SUCCESS)
			continue;
		if (n >= 4) {
			/* The first entry: the PROM, at the card's base. */
			pa = ((uint64_t)(uint_t)reg[0] << 32) | (uint_t)reg[1];
			dump(unit++, pa);
		}
		ddi_prop_free(reg);
	}
	if (unit == 0) cmn_err(CE_CONT, "?sunrom: no SUNW,ffb device found\n");
	return (EIO);
}

int
_fini(void)
{
	return (EBUSY);
}

int
_info(struct modinfo *mip)
{
	return (mod_info(&modlinkage, mip));
}
