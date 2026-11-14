/*
 * Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
 * OpenBoot release it is part of (see LICENSE).
 *
 * The power-on selftest's start. The reset code (reset.fth) enters it at
 * a hardware power-on in diagnostic mode, at +0x20 of its place in the
 * flash PROM, with the MMU and caches off, TL 0, its trap base set to the
 * POST's start and %o0 = 1 | diag-level << 8 | mfg-mode << 16. The image
 * begins with its trap table (TL 0, then TL > 0): the reset code finds
 * "POST" at +0x10, enters at +0x20 (the power-on reset's slot) and, after
 * a reset the POST makes itself, at +0xC0.
 *
 * Here: the memory the POST runs in (0 to 2 MiB) checked, the image copied
 * to its place there (1 MiB), its BSS cleared, the trap base moved to the
 * copy, a stack below 1 MiB; then post_main (post.c). On its return, back
 * to the reset code (the PROM's +0xC0) with %o0 = the status (0 passed),
 * %o1 = the physical address of the message naming the part suspected (in
 * the PROM, as the copy in memory does not survive the reset), %o2 = the
 * board descriptor.
 */

#define ASI_BYPASS	0x15		/* physical, not cached */
#define RAM_TOP		0x200000	/* the memory the POST uses */
#define STACK_TOP	0x0f0000
#define PROM_HI		0x1ff		/* the reset code's return entry: */
#define PROM_RESET_LO	0xf00000c0	/* 1ff.f000.00c0 */

	.section .text.start, "ax"
	.global	_start

	/* A trap table slot: 8 instructions. */
	.macro	slot target
	ba,a	\target
	nop; nop; nop; nop; nop; nop; nop
	.endm
	.macro	slots target, count
	.rept	\count
	slot	\target
	.endr
	.endm

/* The trap table, TL 0. */
_start:
	.ascii	"U510"			/* +0x00 */
	.word	1			/* +0x04: layout version */
	.word	_image_end - _start	/* +0x08: the image's size */
	.word	0
	.ascii	"POST"			/* +0x10: what the reset code looks for */
	.word	0, 0, 0
	slot	post_entry		/* +0x20: power-on (tt 1) */
	slots	post_trap, 4		/* tt 2-5 */
	slot	post_entry		/* +0xc0: after the POST's own reset */
	slots	post_trap, 0x80 - 7	/* tt 7-7f */
	/* tt 80: spill_0_normal (32-bit frames, as this code makes them) */
	srl	%sp, 0, %sp
	stw	%l0, [%sp + 0]
	stw	%l1, [%sp + 4]
	stw	%l2, [%sp + 8]
	stw	%l3, [%sp + 12]
	stw	%l4, [%sp + 16]
	stw	%l5, [%sp + 20]
	stw	%l6, [%sp + 24]
	stw	%l7, [%sp + 28]
	stw	%i0, [%sp + 32]
	stw	%i1, [%sp + 36]
	stw	%i2, [%sp + 40]
	stw	%i3, [%sp + 44]
	stw	%i4, [%sp + 48]
	stw	%i5, [%sp + 52]
	stw	%i6, [%sp + 56]
	stw	%i7, [%sp + 60]
	saved
	retry
	.rept	13
	nop
	.endr
	slots	post_trap, 0xc0 - 0x84	/* tt 84-bf */
	/* tt c0: fill_0_normal */
	srl	%sp, 0, %sp
	lduw	[%sp + 0], %l0
	lduw	[%sp + 4], %l1
	lduw	[%sp + 8], %l2
	lduw	[%sp + 12], %l3
	lduw	[%sp + 16], %l4
	lduw	[%sp + 20], %l5
	lduw	[%sp + 24], %l6
	lduw	[%sp + 28], %l7
	lduw	[%sp + 32], %i0
	lduw	[%sp + 36], %i1
	lduw	[%sp + 40], %i2
	lduw	[%sp + 44], %i3
	lduw	[%sp + 48], %i4
	lduw	[%sp + 52], %i5
	lduw	[%sp + 56], %i6
	lduw	[%sp + 60], %i7
	restored
	retry
	.rept	13
	nop
	.endr
	slots	post_trap, 0x200 - 0xc4	/* tt c4-1ff */
/* TL > 0 */
	slots	post_trap, 0x200

/*
 * The entry, from the PROM (%o0 = the reset code's word, kept in %l7).
 * %l4 = the image's address in the PROM.
 */
post_entry:
	mov	%o0, %l7
1:	rd	%pc, %l4
	set	1b - _start, %l5
	sub	%l4, %l5, %l4

	/* The memory probed, from here (the PROM: the column mode changes
	   how addresses reach the DRAMs, so nothing runs from memory yet),
	   and the column mode set as the tests will have it: %i0 bank 0,
	   %i1 bank 2 (bit 0 present, 1 11-bit columns, 2 a second side,
	   31:8 a DIMM's size in MiB with 10-bit columns), %i2 the mode. */
	call	pm_mode
	clr	%o0
	clr	%l5			/* the bank: 0, then 1 (bank 2) */
pm_bank:
	sll	%l5, 28, %l0		/* its base: 0, 256 MiB */
	clr	%l6
	call	pm_present
	mov	%l0, %o0
	brz	%o0, pm_next
	nop
	call	pm_present
	add	%l0, 8, %o0
	brz	%o0, pm_next
	nop
	or	%l6, 1, %l6
	call	pm_size
	mov	%l0, %o0
	srl	%o0, 20, %o0
	sll	%o0, 8, %o0
	or	%l6, %o0, %l6
	call	pm_col11
	mov	%l0, %o0
	brz	%o0, 1f
	nop
	call	pm_col11
	add	%l0, 8, %o0
	brz	%o0, 1f
	nop
	or	%l6, 2, %l6
1:	sethi	%hi(0x20000000), %l1
	or	%l0, %l1, %l1
	call	pm_present
	mov	%l1, %o0
	brz	%o0, pm_next
	nop
	call	pm_present
	add	%l1, 8, %o0
	brz	%o0, pm_next
	nop
	or	%l6, 4, %l6
pm_next:
	brnz	%l5, 2f
	nop
	mov	%l6, %i0
	ba	pm_bank
	mov	1, %l5
2:	mov	%l6, %i1
	/* 11-bit only if every bank present has 11-bit DIMMs (and one is). */
	clr	%i2
	or	%i0, %i1, %g1
	andcc	%g1, 1, %g0
	be	3f
	and	%i0, 3, %l0
	cmp	%l0, 1
	be	3f
	and	%i1, 3, %l1
	cmp	%l1, 1
	be	3f
	nop
	mov	1, %i2
3:	call	pm_mode
	mov	%i2, %o0

	/* The memory used, 0 to 2 MiB: each word its own address, then its
	   complement, read back (through no cache). */
	set	RAM_TOP, %l0
	clr	%l1
2:	stxa	%l1, [%l1] ASI_BYPASS
	add	%l1, 8, %l1
	cmp	%l1, %l0
	bl	2b
	nop
	clr	%l1
3:	ldxa	[%l1] ASI_BYPASS, %l2
	cmp	%l2, %l1
	bne,pn	%xcc, post_ram_bad
	xnor	%l1, %g0, %l3
	stxa	%l3, [%l1] ASI_BYPASS
	ldxa	[%l1] ASI_BYPASS, %l2
	cmp	%l2, %l3
	bne,pn	%xcc, post_ram_bad
	add	%l1, 8, %l1
	cmp	%l1, %l0
	bl	3b
	nop

	/* The image copied to its place, its BSS cleared. */
	set	_start, %l0
	set	_image_end, %l1
	sub	%l1, %l0, %l1		/* size, a multiple of 8 */
	clr	%l2
4:	lduwa	[%l4 + %l2] ASI_BYPASS, %l3
	stwa	%l3, [%l0 + %l2] ASI_BYPASS
	add	%l2, 4, %l2
	cmp	%l2, %l1
	bl	4b
	nop
	set	__bss_start, %l0
	set	__bss_end, %l1
5:	cmp	%l0, %l1
	bge	6f
	nop
	stxa	%g0, [%l0] ASI_BYPASS
	ba	5b
	add	%l0, 8, %l0
6:
	/* Running from the copy: traps to its table, a stack. */
	set	7f, %g1
	jmp	%g1
	nop
7:	set	_start, %g1
	wrpr	%g1, 0, %tba
	set	STACK_TOP, %sp
	clr	%fp
	set	post_prom_base, %g1
	srlx	%l4, 32, %g2		/* (kept for the message's address) */
	st	%g2, [%g1]
	st	%l4, [%g1 + 4]
	set	probe_result, %g1	/* the memory probe's, for post.c */
	st	%i0, [%g1]
	st	%i1, [%g1 + 4]
	st	%i2, [%g1 + 8]
	wrpr	%g0, 0x1c, %pstate	/* PEF, AM (32-bit addresses), PRIV */
	call	post_main
	mov	%l7, %o0
	wrpr	%g0, 0x14, %pstate	/* (full addresses again) */

	/* Back to the reset code: %o0 status, %o1 message (PROM), %o2 board. */
	set	post_message, %g1
	lduw	[%g1], %o1
	brz,pn	%o1, 8f
	nop
	set	_start, %g2
	sub	%o1, %g2, %o1
	set	post_prom_base, %g1	/* (%l4 may have lost its upper half) */
	ldx	[%g1], %g2
	add	%o1, %g2, %o1		/* its address in the PROM */
8:	set	post_board, %g1
	ldx	[%g1], %o2
	ba	post_return
	nop

/* The memory the POST would run in is bad: said so, and back. */
post_ram_bad:
	set	ram_bad_msg - _start, %o1
	add	%o1, %l4, %o1		/* (still running from the PROM) */
	mov	1, %o0
	clr	%o2
	ba	post_return
	nop

post_return:
	wrpr	%g0, 0, %tba
	mov	PROM_HI, %g1
	sllx	%g1, 32, %g1
	set	PROM_RESET_LO, %g2
	or	%g1, %g2, %g1
	jmp	%g1
	nop

/* A trap: one a test expects (trap_armed) to trap_hook (cpu.S); else
   unexpected: said so (post.c), and back as a failure. */
post_trap:
	rdpr	%tt, %g1		/* an MMU miss: mmu_miss (cpu.S) */
	cmp	%g1, 0x68
	be	mmu_miss
	cmp	%g1, 0x64
	be	mmu_miss
	nop
	set	trap_armed, %g1
	lduw	[%g1], %g1
	brnz,pt	%g1, trap_hook
	nop
	.global	post_trap_fatal
post_trap_fatal:
	rdpr	%tt, %o0
	rdpr	%tpc, %o2
	srlx	%o2, 32, %o1		/* (64 bits: %o1 high, %o2 low) */
	wrpr	%g0, 0, %tl
	wrpr	%g0, 0x1c, %pstate
	set	STACK_TOP - 0x400, %sp
	call	post_unexpected_trap
	nop
	wrpr	%g0, 0x14, %pstate
	set	post_message, %g1
	lduw	[%g1], %o1
	set	_start, %g2
	sub	%o1, %g2, %o1
	set	post_prom_base, %g1
	ldx	[%g1], %g2
	add	%o1, %g2, %o1
	mov	1, %o0
	set	post_board, %g1		/* (post_unexpected_trap's) */
	ldx	[%g1], %o2
	ba	post_return
	nop

/*
 * The memory probe's routines (leaves, run from the PROM; %o0 the
 * argument and result; physical, through no cache).
 */

/* Mem_Control0's column mode (%o0 1: 11-bit): refresh off, then on with
   the mode set; then a wait (auxio reads). */
pm_mode:
	setx	0x1fe0000f010, %o5, %o1
	ldxa	[%o1] ASI_BYPASS, %o2
	setx	0x80000000, %o5, %o3
	andn	%o2, %o3, %o2
	stxa	%o2, [%o1] ASI_BYPASS
	membar	#Sync
	sethi	%hi(0x1000), %o4
	andn	%o2, %o4, %o2
	brz	%o0, 1f
	nop
	or	%o2, %o4, %o2
1:	or	%o2, %o3, %o2
	stxa	%o2, [%o1] ASI_BYPASS
	membar	#Sync
	setx	0x1fff1726000, %o5, %o1
	set	1000, %o2
2:	lduwa	[%o1] ASI_BYPASS, %g0
	subcc	%o2, 1, %o2
	bne	2b
	nop
	retl
	nop

/* A DIMM at %o0: patterns at +0, +8 MiB, +16 MiB, the first read back
   (1 if it is). The memory put back as found. */
pm_present:
	sethi	%hi(0x800000), %o4
	sethi	%hi(0x1000000), %o5
	add	%o0, %o4, %g4
	add	%o0, %o5, %g5
	ldxa	[%o0] ASI_BYPASS, %g1
	ldxa	[%g4] ASI_BYPASS, %g2
	ldxa	[%g5] ASI_BYPASS, %g3
	setx	0xaaaaaaaa55555555, %o1, %o2
	stxa	%o2, [%o0] ASI_BYPASS
	xnor	%o2, %g0, %o3
	stxa	%o3, [%g4] ASI_BYPASS
	setx	0xa5a5a5a5a5a5a5a5, %o1, %o3
	stxa	%o3, [%g5] ASI_BYPASS
	ldxa	[%o0] ASI_BYPASS, %o3
	stxa	%g3, [%g5] ASI_BYPASS
	stxa	%g2, [%g4] ASI_BYPASS
	stxa	%g1, [%o0] ASI_BYPASS
	cmp	%o3, %o2
	clr	%o0
	retl
	move	%xcc, 1, %o0

/* Its size (with 10-bit columns): 64 MiB at +0, 32 MiB at +64 MiB, 16 MiB
   at +32 MiB; what +0 then holds. */
pm_size:
	sethi	%hi(0x4000000), %o4	/* 64 MiB */
	sethi	%hi(0x2000000), %o5	/* 32 MiB */
	add	%o0, %o4, %g4
	add	%o0, %o5, %g5
	ldxa	[%o0] ASI_BYPASS, %g1
	ldxa	[%g4] ASI_BYPASS, %g2
	ldxa	[%g5] ASI_BYPASS, %g3
	stxa	%o4, [%o0] ASI_BYPASS
	stxa	%o5, [%g4] ASI_BYPASS
	sethi	%hi(0x1000000), %o3	/* 16 MiB */
	stxa	%o3, [%g5] ASI_BYPASS
	ldxa	[%o0] ASI_BYPASS, %o3
	stxa	%g3, [%g5] ASI_BYPASS
	stxa	%g2, [%g4] ASI_BYPASS
	stxa	%g1, [%o0] ASI_BYPASS
	retl
	mov	%o3, %o0

/* Its DRAMs' columns: with 11-bit columns, 2 at +0 and 1 at +16 KiB; 1
   if +0 still holds 2 (11-bit), 0 if the second landed on it. */
pm_col11:
	mov	%o7, %g5
	mov	%o0, %g4
	call	pm_mode
	mov	1, %o0
	set	0x4000, %o1
	add	%g4, %o1, %g3
	ldxa	[%g4] ASI_BYPASS, %g1
	ldxa	[%g3] ASI_BYPASS, %g2
	mov	2, %o2
	stxa	%o2, [%g4] ASI_BYPASS
	mov	1, %o2
	stxa	%o2, [%g3] ASI_BYPASS
	ldxa	[%g4] ASI_BYPASS, %o3
	stxa	%g2, [%g3] ASI_BYPASS
	stxa	%g1, [%g4] ASI_BYPASS
	mov	%o3, %g1
	call	pm_mode
	clr	%o0
	cmp	%g1, 2
	clr	%o0
	jmp	%g5 + 8
	move	%xcc, 1, %o0

ram_bad_msg:				/* (beside the code: read from the PROM) */
	.asciz	"Memory (POST area)"
	.align	4

/*
 * Physical access (through no cache) and processor registers, for C. A
 * physical address is a 64-bit value: in the 32-bit ABI, %o0 (high word)
 * and %o1 (low word).
 */
	.text
#define PA	sllx %o0, 32, %o0; srl %o1, 0, %o1; or %o0, %o1, %o0
/* (64-bit addresses: PSTATE.AM off for the access, as it was after) */
#define AM_OFF	rdpr %pstate, %o5; andn %o5, 8, %o4; wrpr %o4, 0, %pstate
#define AM_BACK	wrpr %o5, 0, %pstate

	.global	phys_ld8, phys_st8, phys_ld32, phys_st32
phys_ld8:
	PA
	AM_OFF
	lduba	[%o0] ASI_BYPASS, %o0
	retl
	AM_BACK
phys_st8:
	PA
	AM_OFF
	stba	%o2, [%o0] ASI_BYPASS
	retl
	AM_BACK
phys_ld32:
	PA
	AM_OFF
	lduwa	[%o0] ASI_BYPASS, %o0
	retl
	AM_BACK
phys_st32:
	PA
	AM_OFF
	stwa	%o2, [%o0] ASI_BYPASS
	retl
	AM_BACK

	.global	rd_tick, rd_ver
rd_tick:				/* (long long: %o0 high, %o1 low) */
	rd	%tick, %o1
	srlx	%o1, 32, %o0
	retl
	srl	%o1, 0, %o1
rd_ver:
	rdpr	%ver, %o1
	srlx	%o1, 32, %o0
	retl
	srl	%o1, 0, %o1
