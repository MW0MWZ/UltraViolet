/*
 * Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
 * OpenBoot release it is part of (see LICENSE).
 *
 * The processor's registers and address spaces, for the tests (post.h).
 * In the 32-bit ABI a 64-bit argument is a register pair (high word
 * first) and a 64-bit result is %o0 (high) and %o1 (low).
 */

#define X64(hi, lo, r)	sllx hi, 32, r; srl lo, 0, lo; or r, lo, r
#define RET64(r)	srlx r, 32, %o0; retl; srl r, 0, %o1
/* PSTATE.AM off (64-bit addresses) and back; %g4 kept the old value. */
#define AM_OFF		rdpr %pstate, %g4; andn %g4, 8, %g3; wrpr %g3, 0, %pstate
#define AM_BACK		wrpr %g4, 0, %pstate

	.text

/* u64 asi_ldx(u64 va, u32 asi) */
	.global	asi_ldx
asi_ldx:
	X64(%o0, %o1, %o3)
	wr	%o2, 0, %asi
	AM_OFF
	ldxa	[%o3] %asi, %o3
	membar	#Sync
	AM_BACK
	RET64(%o3)

/* void asi_stx(u64 va, u32 asi, u64 v) */
	.global	asi_stx
asi_stx:
	X64(%o0, %o1, %o5)
	X64(%o3, %o4, %o3)
	wr	%o2, 0, %asi
	AM_OFF
	stxa	%o3, [%o5] %asi
	membar	#Sync
	retl
	AM_BACK

/* u64 asi_swap(u64 va, u32 asi, u64 pattern): the pattern written, read
   back, and what was there put back. */
	.global	asi_swap
asi_swap:
	X64(%o0, %o1, %o5)
	X64(%o3, %o4, %o3)
	wr	%o2, 0, %asi
	AM_OFF
	ldxa	[%o5] %asi, %g1
	membar	#Sync
	stxa	%o3, [%o5] %asi
	membar	#Sync
	ldxa	[%o5] %asi, %o3
	membar	#Sync
	stxa	%g1, [%o5] %asi
	membar	#Sync
	AM_BACK
	RET64(%o3)

/* void dtlb_swap(u32 index, u64 tag, u64 data, u64 *out): the D-TLB
   entry's tag (through Tag Access) and data written, read back to
   out[0] and out[1], and the entry and Tag Access put back. */
	.global	dtlb_swap
dtlb_swap:
	sll	%o0, 3, %g5		/* the entry's address */
	X64(%o1, %o2, %g1)		/* tag */
	X64(%o3, %o4, %g2)		/* data */
	mov	0x30, %g3
	ldxa	[%g3] 0x58, %o0		/* Tag Access, as it was */
	ldxa	[%g5] 0x5e, %o1		/* the entry's tag */
	ldxa	[%g5] 0x5d, %o2		/* and data */
	stxa	%g1, [%g3] 0x58
	membar	#Sync
	stxa	%g2, [%g5] 0x5d
	membar	#Sync
	ldxa	[%g5] 0x5e, %g1
	ldxa	[%g5] 0x5d, %g2
	stxa	%o1, [%g3] 0x58		/* the entry put back */
	membar	#Sync
	stxa	%o2, [%g5] 0x5d
	membar	#Sync
	stxa	%o0, [%g3] 0x58		/* and Tag Access */
	membar	#Sync
	srl	%o5, 0, %o5
	stx	%g1, [%o5]
	retl
	stx	%g2, [%o5 + 8]

/* u64 dtlb_entry(u32 index, u32 which): the entry's tag (0) or data (1). */
	.global	dtlb_entry
dtlb_entry:
	sll	%o0, 3, %o0
	brnz	%o1, 1f
	nop
	ldxa	[%o0] 0x5e, %o3
	RET64(%o3)
1:	ldxa	[%o0] 0x5d, %o3
	RET64(%o3)

/* u32 softint_setclr(u32 bit, u32 *after_clear): the bit set (SOFTINT as
   it then is returned), then cleared (as it then is to *after_clear);
   what was pending before set again. */
	.global	softint_setclr
softint_setclr:
	rd	%softint, %g1
	wr	%o0, 0, %set_softint
	rd	%softint, %o2
	wr	%o0, 0, %clear_softint
	rd	%softint, %o3
	wr	%g1, 0, %set_softint
	srl	%o1, 0, %o1
	st	%o3, [%o1]
	retl
	mov	%o2, %o0

/* void softint_int(u32 level): with the PIL just below the level and
   interrupts on, its soft interrupt set (taken here: trap_hook). */
	.global	softint_int
softint_int:
	rdpr	%pil, %g4
	sub	%o0, 1, %g1
	wrpr	%g1, 0, %pil
	mov	1, %g1
	sll	%g1, %o0, %g1
	wr	%g1, 0, %set_softint
	rdpr	%pstate, %g2
	or	%g2, 2, %g3
	wrpr	%g3, 0, %pstate		/* IE */
	nop
	nop
	nop
	nop
	wrpr	%g2, 0, %pstate
	retl
	wrpr	%g4, 0, %pil

/* void tick_test(u64 pattern, u64 out[3]): the tick, the pattern written
   to TICK_CMPR and read back, the tick again; TICK_CMPR put back. */
	.global	tick_test
tick_test:
	X64(%o0, %o1, %o3)
	srl	%o2, 0, %o2
	rd	%asr23, %g1
	rd	%tick, %g2
	wr	%o3, 0, %asr23
	rd	%asr23, %g3
	rd	%tick, %g4
	wr	%g1, 0, %asr23
	stx	%g2, [%o2]
	stx	%g3, [%o2 + 8]
	retl
	stx	%g4, [%o2 + 16]

/* Software traps: taken (171), and with a false condition, not. */
	.global	do_ta, do_tn
do_ta:
	ta	0x71
	retl
	nop
do_tn:
	tn	0x71
	retl
	nop

/* The LSU control register. */
	.global	lsu_get, lsu_set
lsu_get:
	ldxa	[%g0] 0x45, %o3
	RET64(%o3)
lsu_set:
	X64(%o0, %o1, %o3)
	stxa	%o3, [%g0] 0x45
	membar	#Sync
	retl
	nop

/*
 * A trap a test expects (trap_armed set): its type left in trap_tt, and on
 * past the instruction; a soft interrupt (41-4f) cleared, and the
 * instruction run. (From the trap table's post_trap.)
 */
	.global	trap_hook
trap_hook:
	rdpr	%tt, %g3
	set	trap_tt, %g1
	st	%g3, [%g1]
	sub	%g3, 0x41, %g2
	cmp	%g2, 0x4f - 0x41
	bgu	1f
	nop
	add	%g2, 1, %g2
	mov	1, %g1
	sll	%g1, %g2, %g1
	wr	%g1, 0, %clear_softint
	retry
1:	done

/* void v9_ops(u64 out[10]): the V9 instruction test's operations, each
   by its own instruction: MULX, UDIVX (and the remainder by MULX, SUB),
   SDIVX, SLLX, SRLX, SRAX, XOR, AND, OR. */
	.global	v9_ops
v9_ops:
	srl	%o0, 0, %o0
	setx	0x1234567890abcdef, %g1, %o1
	setx	0x000fedcba9876543, %g1, %o2
	mulx	%o1, %o2, %o3
	stx	%o3, [%o0]
	udivx	%o1, %o2, %o3
	stx	%o3, [%o0 + 8]
	mulx	%o3, %o2, %o4
	sub	%o1, %o4, %o4
	stx	%o4, [%o0 + 16]
	setx	0x0123456789abcdef, %g1, %o4
	sub	%g0, %o4, %o4
	setx	0x12345, %g1, %o5
	sdivx	%o4, %o5, %o4
	stx	%o4, [%o0 + 24]
	sllx	%o1, 13, %o4
	stx	%o4, [%o0 + 32]
	srlx	%o1, 17, %o4
	stx	%o4, [%o0 + 40]
	setx	0xf000000000000123, %g1, %o4
	srax	%o4, 9, %o4
	stx	%o4, [%o0 + 48]
	setx	0x5555555555555555, %g1, %o4
	xor	%o1, %o4, %o4
	stx	%o4, [%o0 + 56]
	setx	0x0ff00ff00ff00ff0, %g1, %o4
	and	%o1, %o4, %o4
	stx	%o4, [%o0 + 64]
	setx	0x8000000000000001, %g1, %o4
	or	%o1, %o4, %o4
	retl
	stx	%o4, [%o0 + 72]

/*
 * Memory: a range (pa, len, both multiples of 8; of 64 for blocks)
 * through an ASI (14 through the E-cache, 15 around it), PSTATE.AM off.
 */

/* void mem_fill(u64 pa, u32 len, u64 x, u32 asi) */
	.global	mem_fill
mem_fill:
	X64(%o0, %o1, %o0)
	X64(%o3, %o4, %o3)
	wr	%o5, 0, %asi
	AM_OFF
1:	subcc	%o2, 8, %o2
	add	%o0, %o2, %g1
	bne,pt	%icc, 1b
	stxa	%o3, [%g1] %asi
	membar	#Sync
	retl
	AM_BACK

/* u32 mem_check(u64 pa, u32 len, u64 x, u32 asi, u64 *out): 0, or 1 with
   the first doubleword not x (out[0]) and its address (out[1]). */
	.global	mem_check
mem_check:
	ld	[%sp + 92], %g5		/* (the 6th argument: out) */
	X64(%o0, %o1, %o0)
	X64(%o3, %o4, %o3)
	wr	%o5, 0, %asi
	AM_OFF
	clr	%g1
1:	add	%o0, %g1, %g3
	ldxa	[%g3] %asi, %g2
	cmp	%g2, %o3
	bne,pn	%xcc, 2f
	add	%g1, 8, %g1
	cmp	%g1, %o2
	blu,pt	%icc, 1b
	nop
	AM_BACK
	retl
	clr	%o0
2:	sub	%g1, 8, %g1
	add	%o0, %g1, %g1
	AM_BACK
	stx	%g2, [%g5]
	stx	%g1, [%g5 + 8]
	retl
	mov	1, %o0

/* void mem_fill_addr(u64 pa, u32 len, u32 asi): each doubleword its
   own address. */
	.global	mem_fill_addr
mem_fill_addr:
	X64(%o0, %o1, %o0)
	wr	%o3, 0, %asi
	AM_OFF
1:	subcc	%o2, 8, %o2
	add	%o0, %o2, %g1
	bne,pt	%icc, 1b
	stxa	%g1, [%g1] %asi
	membar	#Sync
	retl
	AM_BACK

/* u32 mem_check_addr(u64 pa, u32 len, u32 asi, u64 *out) */
	.global	mem_check_addr
mem_check_addr:
	X64(%o0, %o1, %o0)
	wr	%o3, 0, %asi
	AM_OFF
	clr	%g1
1:	add	%o0, %g1, %g3
	ldxa	[%g3] %asi, %g2
	cmp	%g2, %g3
	bne,pn	%xcc, 2f
	add	%g1, 8, %g1
	cmp	%g1, %o2
	blu,pt	%icc, 1b
	nop
	AM_BACK
	retl
	clr	%o0
2:	AM_BACK
	stx	%g2, [%o4]
	stx	%g3, [%o4 + 8]
	retl
	mov	1, %o0

/* Block stores and loads (64 bytes, through the FPU's registers, ASI f0
   with the D-MMU off: physical). buf: 64 bytes, 64-byte aligned. */

/* void blk_fill(u64 pa, u32 len, u64 *buf): the block in buf stored to
   each block of the range. */
	.global	blk_fill
blk_fill:
	X64(%o0, %o1, %o0)
	wr	%g0, 4, %fprs
	ldda	[%o3] 0xf0, %f0
	membar	#Sync
1:	subcc	%o2, 0x40, %o2
	bne,pt	%icc, 1b
	stda	%f0, [%o0 + %o2] 0xf0
	membar	#Sync
	retl
	wr	%g0, 0, %fprs

/* u32 blk_check(u64 pa, u32 len, u64 x, u64 *buf, u64 *out): each block
   loaded, stored to buf, its doublewords against x; 0, or 1 with the
   first not x (out[0]) and its address (out[1]). */
	.global	blk_check
blk_check:
	ld	[%sp + 92], %g5		/* out */
	X64(%o0, %o1, %o0)
	X64(%o3, %o4, %o3)
	wr	%g0, 4, %fprs
	clr	%g1
1:	ldda	[%o0 + %g1] 0xf0, %f0
	membar	#Sync
	stda	%f0, [%o5] 0xf0
	membar	#Sync
	clr	%g3
2:	ldx	[%o5 + %g3], %g2
	cmp	%g2, %o3
	bne,pn	%xcc, 3f
	add	%g3, 8, %g3
	cmp	%g3, 0x40
	blu,pt	%icc, 2b
	nop
	add	%g1, 0x40, %g1
	cmp	%g1, %o2
	blu,pt	%icc, 1b
	nop
	wr	%g0, 0, %fprs
	retl
	clr	%o0
3:	add	%g1, %g3, %g1
	sub	%g1, 8, %g1
	add	%o0, %g1, %g1
	stx	%g2, [%g5]
	stx	%g1, [%g5 + 8]
	wr	%g0, 0, %fprs
	retl
	mov	1, %o0

/* void blk_fill_addr(u64 pa, u32 len, u64 *buf): each block its
   doublewords' addresses, built in buf and stored. */
	.global	blk_fill_addr
blk_fill_addr:
	X64(%o0, %o1, %o0)
	wr	%g0, 4, %fprs
	clr	%g1
1:	add	%o0, %g1, %g2
	stx	%g2, [%o3]
	add	%g2, 8, %g2
	stx	%g2, [%o3 + 8]
	add	%g2, 8, %g2
	stx	%g2, [%o3 + 16]
	add	%g2, 8, %g2
	stx	%g2, [%o3 + 24]
	add	%g2, 8, %g2
	stx	%g2, [%o3 + 32]
	add	%g2, 8, %g2
	stx	%g2, [%o3 + 40]
	add	%g2, 8, %g2
	stx	%g2, [%o3 + 48]
	add	%g2, 8, %g2
	stx	%g2, [%o3 + 56]
	ldda	[%o3] 0xf0, %f0
	membar	#Sync
	stda	%f0, [%o0 + %g1] 0xf0
	add	%g1, 0x40, %g1
	cmp	%g1, %o2
	blu,pt	%icc, 1b
	nop
	membar	#Sync
	retl
	wr	%g0, 0, %fprs

/* u32 blk_check_addr(u64 pa, u32 len, u64 *buf, u64 *out) */
	.global	blk_check_addr
blk_check_addr:
	X64(%o0, %o1, %o0)
	wr	%g0, 4, %fprs
	clr	%g1
1:	ldda	[%o0 + %g1] 0xf0, %f0
	membar	#Sync
	stda	%f0, [%o3] 0xf0
	membar	#Sync
	clr	%g3
2:	ldx	[%o3 + %g3], %g2
	add	%o0, %g1, %g4
	add	%g4, %g3, %g4		/* its address */
	cmp	%g2, %g4
	bne,pn	%xcc, 3f
	add	%g3, 8, %g3
	cmp	%g3, 0x40
	blu,pt	%icc, 2b
	nop
	add	%g1, 0x40, %g1
	cmp	%g1, %o2
	blu,pt	%icc, 1b
	nop
	wr	%g0, 0, %fprs
	retl
	clr	%o0
3:	stx	%g2, [%o4]
	stx	%g4, [%o4 + 8]
	wr	%g0, 0, %fprs
	retl
	mov	1, %o0

/* The processor's asynchronous fault status: read, and cleared (each bit
   written back). */
	.global	afsr_get, afsr_clear
afsr_get:
	ldxa	[%g0] 0x4c, %o3
	RET64(%o3)
afsr_clear:
	ldxa	[%g0] 0x4c, %o3
	stxa	%o3, [%g0] 0x4c
	membar	#Sync
	retl
	nop

/*
 * The FPU, through a buffer (u64 buf[...], 8-byte aligned).
 */

/* void fregs(u64 *buf): every double register loaded from buf[0], then
   each stored to buf[1..32]. */
	.global	fregs
fregs:
	wr	%g0, 4, %fprs
	ldd	[%o0], %f0
	ldd	[%o0], %f2
	ldd	[%o0], %f4
	ldd	[%o0], %f6
	ldd	[%o0], %f8
	ldd	[%o0], %f10
	ldd	[%o0], %f12
	ldd	[%o0], %f14
	ldd	[%o0], %f16
	ldd	[%o0], %f18
	ldd	[%o0], %f20
	ldd	[%o0], %f22
	ldd	[%o0], %f24
	ldd	[%o0], %f26
	ldd	[%o0], %f28
	ldd	[%o0], %f30
	ldd	[%o0], %f32
	ldd	[%o0], %f34
	ldd	[%o0], %f36
	ldd	[%o0], %f38
	ldd	[%o0], %f40
	ldd	[%o0], %f42
	ldd	[%o0], %f44
	ldd	[%o0], %f46
	ldd	[%o0], %f48
	ldd	[%o0], %f50
	ldd	[%o0], %f52
	ldd	[%o0], %f54
	ldd	[%o0], %f56
	ldd	[%o0], %f58
	ldd	[%o0], %f60
	ldd	[%o0], %f62
	std	%f0, [%o0 + 8]
	std	%f2, [%o0 + 16]
	std	%f4, [%o0 + 24]
	std	%f6, [%o0 + 32]
	std	%f8, [%o0 + 40]
	std	%f10, [%o0 + 48]
	std	%f12, [%o0 + 56]
	std	%f14, [%o0 + 64]
	std	%f16, [%o0 + 72]
	std	%f18, [%o0 + 80]
	std	%f20, [%o0 + 88]
	std	%f22, [%o0 + 96]
	std	%f24, [%o0 + 104]
	std	%f26, [%o0 + 112]
	std	%f28, [%o0 + 120]
	std	%f30, [%o0 + 128]
	std	%f32, [%o0 + 136]
	std	%f34, [%o0 + 144]
	std	%f36, [%o0 + 152]
	std	%f38, [%o0 + 160]
	std	%f40, [%o0 + 168]
	std	%f42, [%o0 + 176]
	std	%f44, [%o0 + 184]
	std	%f46, [%o0 + 192]
	std	%f48, [%o0 + 200]
	std	%f50, [%o0 + 208]
	std	%f52, [%o0 + 216]
	std	%f54, [%o0 + 224]
	std	%f56, [%o0 + 232]
	std	%f58, [%o0 + 240]
	std	%f60, [%o0 + 248]
	std	%f62, [%o0 + 256]
	retl
	wr	%g0, 0, %fprs

/* void fsr_test(u64 *buf): the FSR as it was to buf[2], buf[0] loaded,
   stored to buf[1], put back. */
	.global	fsr_test
fsr_test:
	wr	%g0, 4, %fprs
	stx	%fsr, [%o0 + 16]
	ldx	[%o0], %fsr
	stx	%fsr, [%o0 + 8]
	ldx	[%o0 + 16], %fsr
	retl
	wr	%g0, 0, %fprs

/* void fops(u64 *buf): a (buf[0]) and b (buf[1]), doubles: to buf[2] on,
   a+b, a*b, a/b, sqrt b, -a, |-a| and a moved. */
	.global	fops
fops:
	wr	%g0, 4, %fprs
	ldd	[%o0], %f0
	ldd	[%o0 + 8], %f2
	faddd	%f0, %f2, %f4
	fmuld	%f0, %f2, %f6
	fdivd	%f0, %f2, %f8
	fsqrtd	%f2, %f10
	fnegd	%f0, %f12
	fabsd	%f12, %f14
	fmovd	%f0, %f16
	std	%f4, [%o0 + 16]
	std	%f6, [%o0 + 24]
	std	%f8, [%o0 + 32]
	std	%f10, [%o0 + 40]
	std	%f12, [%o0 + 48]
	std	%f14, [%o0 + 56]
	std	%f16, [%o0 + 64]
	retl
	wr	%g0, 0, %fprs

/* void fdiv0(u64 *buf): a division by zero with buf[0] as the FSR (its
   trap enabled: an IEEE 754 exception, trap 21), buf[1] 1.0, buf[2] 0.0;
   the FSR as it was to buf[3], as it is after to buf[4]. */
	.global	fdiv0
fdiv0:
	wr	%g0, 4, %fprs
	stx	%fsr, [%o0 + 24]
	ldx	[%o0], %fsr
	ldd	[%o0 + 8], %f0
	ldd	[%o0 + 16], %f2
	fdivd	%f0, %f2, %f4
	nop
	stx	%fsr, [%o0 + 32]
	ldx	[%o0 + 24], %fsr
	retl
	wr	%g0, 0, %fprs

/* 8-, 16- and 32-bit accesses through an ASI: u32 asi_ld{8,16,32}(u64 va,
   u32 asi); void asi_st{8,16,32}(u64 va, u32 asi, u32 v). */
	.global	asi_ld8, asi_ld16, asi_ld32, asi_st8, asi_st16, asi_st32
asi_ld8:
	X64(%o0, %o1, %o3)
	wr	%o2, 0, %asi
	AM_OFF
	lduba	[%o3] %asi, %o0
	retl
	AM_BACK
asi_ld16:
	X64(%o0, %o1, %o3)
	wr	%o2, 0, %asi
	AM_OFF
	lduha	[%o3] %asi, %o0
	retl
	AM_BACK
asi_ld32:
	X64(%o0, %o1, %o3)
	wr	%o2, 0, %asi
	AM_OFF
	lduwa	[%o3] %asi, %o0
	retl
	AM_BACK
asi_st8:
	X64(%o0, %o1, %o4)
	wr	%o2, 0, %asi
	AM_OFF
	stba	%o3, [%o4] %asi
	membar	#Sync
	retl
	AM_BACK
asi_st16:
	X64(%o0, %o1, %o4)
	wr	%o2, 0, %asi
	AM_OFF
	stha	%o3, [%o4] %asi
	membar	#Sync
	retl
	AM_BACK
asi_st32:
	X64(%o0, %o1, %o4)
	wr	%o2, 0, %asi
	AM_OFF
	stwa	%o3, [%o4] %asi
	membar	#Sync
	retl
	AM_BACK

/* void tlb_load(u32 immu, u32 va, u64 tte): an entry loaded through Tag
   Access and Data In (context 0): the I-MMU's (immu 1) or the D-MMU's. */
	.global	tlb_load
tlb_load:
	X64(%o2, %o3, %o2)
	mov	0x30, %g1
	brnz	%o0, 1f
	nop
	stxa	%o1, [%g1] 0x58
	membar	#Sync
	stxa	%o2, [%g0] 0x5c
	membar	#Sync
	retl
	nop
1:	stxa	%o1, [%g1] 0x50
	membar	#Sync
	stxa	%o2, [%g0] 0x54
	membar	#Sync
	retl
	nop

/* u64 dtag_after(u32 va): Tag Access cleared, a load from va (through the
   D-MMU), Tag Access then (the page on a miss, 0 on a hit). */
	.global	dtag_after
dtag_after:
	mov	0x30, %g1
	stxa	%g0, [%g1] 0x58
	membar	#Sync
	ldx	[%o0], %g0
	ldxa	[%g1] 0x58, %o3
	RET64(%o3)

/* u64 itag_after(u32 code): the same through the I-MMU, by a call of
   the code at that address (retl; nop). */
	.global	itag_after
itag_after:
	mov	%o7, %g4
	mov	0x30, %g1
	stxa	%g0, [%g1] 0x50
	membar	#Sync
	jmpl	%o0, %o7
	nop
	mov	0x30, %g1
	ldxa	[%g1] 0x50, %o3
	mov	%g4, %o7
	RET64(%o3)

/* void fasi(u32 src, u32 asi, u32 dst, u32 asi2): a double loaded through
   asi from src to %f0, stored through asi2 to dst. */
	.global	fasi
fasi:
	wr	%g0, 4, %fprs
	wr	%o1, 0, %asi
	ldda	[%o0] %asi, %f0
	membar	#Sync
	wr	%o3, 0, %asi
	stda	%f0, [%o2] %asi
	membar	#Sync
	retl
	wr	%g0, 0, %fprs

/*
 * An MMU miss (fast_data/instruction_access_MMU_miss, 68/64) on a page of
 * the tests' table (mmu_pages, the VAs; mmu_ttes, their TTEs): its TTE
 * loaded, and the access tried again. (From the trap table's post_trap, MMU globals.)
 */
	.global	mmu_miss
mmu_miss:
	rdpr	%tt, %g1
	mov	0x30, %g2
	cmp	%g1, 0x68
	bne	1f
	nop
	ldxa	[%g2] 0x58, %g3		/* the D-MMU's Tag Access */
	ba	2f
	mov	0x5c, %g1
1:	ldxa	[%g2] 0x50, %g3		/* the I-MMU's */
	mov	0x54, %g1
2:	srlx	%g3, 13, %g3
	sllx	%g3, 13, %g3		/* the page */
	set	mmu_pages, %g2
	clr	%g4
3:	lduw	[%g2 + %g4], %g5
	cmp	%g5, %g3
	be	4f
	add	%g4, 4, %g4
	cmp	%g4, 32
	bne	3b
	nop
	ba	post_trap_fatal
	nop
4:	sub	%g4, 4, %g4
	sll	%g4, 1, %g4
	set	mmu_ttes, %g2
	ldx	[%g2 + %g4], %g3	/* its TTE */
	cmp	%g1, 0x5c
	bne	5f
	nop
	stxa	%g3, [%g0] 0x5c
	membar	#Sync
	retry
5:	stxa	%g3, [%g0] 0x54
	membar	#Sync
	retry
