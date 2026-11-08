\ Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
\ OpenBoot release it is part of (see LICENSE).
id: @(#)post.fth 1.1 26/11/08
purpose: The Ultra 5/10's power-on selftest
copyright: Copyright 2026 Andy Taylor

\ The power-on selftest (POST), run (as system-tests) at a hardware
\ power-on with diag-switch? true and diag-level not off, as OpenBoot
\ 3.x's POST is, and laid out as that reports itself: each group of
\ tests, then its tests a tab in; at the first failure the test, the part
\ suspected and why, and the run stops; then the run's status and the
\ time. diag-level min runs the tests OpenBoot 3.31 runs then, max all of
\ them (and shows the steps that are not tests). A failure keeps the
\ machine from booting by itself; show-post-results gives the last run's
\ status.
\
\ Each test leaves the machine as it found it: registers are put back,
\ the NVRAM's scratch bytes restored, the caches' and TLBs' entries put
\ back or invalidated, and only memory free for clients is written. The
\ processor's registers are tested at TL 1 with interrupts off (the
\ nucleus context is in use while the contexts are changed) and nothing
\ else between the write and the putting back.

headerless

0 value post-ran?		\ the POST ran at the last reset
0 value post-status		\ 0: passed; else failed
0 value post-test$		\ the test running (adr len)
0 value /post-test$
0 value post-suspect$		\ the part a failure points to (adr len)
0 value /post-suspect$
0 value post-max?		\ diag-level max: every test

: tab ( -- )  control I emit  ;
: post-suspect ( adr len -- )  to /post-suspect$ to post-suspect$  ;
: post-failed? ( -- flag )  post-status 0<>  ;

\ A failure: the report, as OpenBoot 3.x's POST gives it.
: post-fail ( msg$ -- )
   post-failed? if  2drop exit  then
   cr ." ***********************" cr
   ." STATUS =FAILED" cr
   ." TEST   =" post-test$ /post-test$ type cr
   ." SUSPECT=" post-suspect$ /post-suspect$ type space cr
   ." MESSAGE=" type cr
   1 to post-status
;
: .post-x ( x -- )
   push-hex
   <# u# u# u# u# u# u# u# u# ascii . hold u# u# u# u# u# u# u# u# u#> type
   pop-base
;
\ An observed value against the one expected; a failure gives both.
: post-compare ( observed expected msg$ -- )
   2over = if  2drop 2drop exit  then		( obs exp msg$ )
   post-failed? >r post-fail r> if  2drop exit  then
   tab ." Expected " .post-x cr
   tab ." Observed " .post-x cr
;
\ A failure at an address: the address, then the values.
: post-compare-at ( observed expected adr msg$ -- )
   2>r >r 2dup = if  r> 2r> 2drop 3drop exit  then	( obs exp ) ( r: adr msg$ )
   r> 2r> post-failed? >r post-fail r> if  3drop exit  then
   tab ." Address  " .post-x cr
   tab ." Expected " .post-x cr
   tab ." Observed " .post-x cr
;

\ A test, named without " Test" (as a failure names it); max? if only
\ diag-level max runs it.
: (post-test) ( xt name$ -- )
   post-failed? if  3drop exit  then
   2dup to /post-test$ to post-test$
   tab type ."  Test" cr
   execute
;
: post-test ( xt name$ -- )  (post-test)  ;
: post-max-test ( xt name$ -- )  post-max? if  (post-test)  else  3drop  then  ;
\ A group that is itself a test (named with " Test", no tab).
: post-group-test ( xt name$ -- )
   post-failed? if  3drop exit  then
   2dup to /post-test$ to post-test$
   type ."  Test" cr
   execute
;
\ A step that is not a test (named as it is): shown at diag-level max.
: post-step ( xt name$ -- )
   post-failed? if  3drop exit  then
   post-max? if  tab type cr  else  2drop  then  execute
;
: post-group ( name$ -- )  post-failed? if  2drop  else  type cr  then  ;

d# 512 buffer: post-buf

\ An internal register (through %asi, at va) written with a pattern and
\ read back, then put back as it was.
code post-asi-swap ( pattern va asi -- readback )
   sp 0 /n*  sc1  nget			\ va
   sp 1 /n*  sc2  nget			\ pattern
   sp 2 /n*  sp   add
   sc4			rdpstate
   sc5			rdtl
   %r3        sc3	rdasr
   %g0 h# 14		wrpstate	\ interrupts off
   %g0 1		wrtl		\ the nucleus context
   tos 0      %r3	wrasr
   #Sync  membar
   sc1 0 %asi  sc6	ldxa		\ as it was
   #Sync  membar
   sc2  sc1 0 %asi	stxa
   #Sync  membar
   sc1 0 %asi  tos	ldxa		\ as written
   #Sync  membar
   sc6  sc1 0 %asi	stxa		\ put back
   #Sync  membar
   sc3 0      %r3	wrasr
   sc5 0		wrtl
   sc4 0		wrpstate
c;

\ SOFTINT: a bit set, then cleared (what was pending before is set again).
code post-softint ( bit -- after-set after-clear )
   sc4			rdpstate
   %g0 h# 14		wrpstate
   %r22       sc3	rdasr
   tos 0      %r20	wrasr
   %r22       sc1	rdasr
   tos 0      %r21	wrasr
   %r22       sc2	rdasr
   sc3 0      %r20	wrasr
   sc4 0		wrpstate
   sc1        sp	push
   sc2        tos	move
c;

\ TICK and TICK_CMPR: the tick read before and after a pattern is written
\ to TICK_CMPR and read back (the pattern has its interrupt disabled).
code post-tick ( pattern -- tick0 readback tick1 )
   sc4			rdpstate
   %g0 h# 14		wrpstate
   %r23       sc3	rdasr
   sc1			rdtick
   tos 0      %r23	wrasr
   %r23       sc2	rdasr
   sc3 0      %r23	wrasr
   sc4 0		wrpstate
   sc1        sp	push
   sc2        sp	push
   tos			rdtick
c;

\ The FPU, through a buffer: its registers, FSR, and its operations.
also assembler definitions
: fpu-on ( -- )  %r6 sc5 rdasr   %g0 4 %r6 wrasr  ;
: fpu-off ( -- )  sc5 0 %r6 wrasr  ;
\ (The assembler's if, then and comparisons are in use here.)
: %d ( n -- reg )  dup 5 rshift +  d# 64 +  ;
\ Every double register loaded from buf+0, then stored to buf+8 on.
: fregs-load ( -- )  d# 64 0 do  tos 0  i %d  lddf  2 +loop  ;
: fregs-store ( -- )  d# 64 0 do  i %d  tos  i 4 * 8 +  stdf  2 +loop  ;
previous definitions

code post-fregs ( buf -- )
   fpu-on  fregs-load  fregs-store  fpu-off
   sp tos pop
c;

\ FSR: as it was to buf+16, buf+0 loaded, stored to buf+8, put back.
code post-fsr ( buf -- )
   fpu-on
   tos h# 10	stxfsr
   tos 0	ldxfsr
   tos 8	stxfsr
   tos h# 10	ldxfsr
   fpu-off
   sp tos pop
c;

\ a (buf+0) and b (buf+8), doubles: to buf+16 on, a+b, a*b, a/b, sqrt b,
\ -a, |-a| and a moved.
code post-fops ( buf -- )
   fpu-on
   tos 0      %f0	lddf
   tos 8      %f2	lddf
   %f0 %f2    %f4	faddd
   %f0 %f2    %f6	fmuld
   %f0 %f2    %f8	fdivd
   %f2        %f10	fsqrtd
   %f0        %f12	fnegd
   %f12       %f14	fabsd
   %f0        %f16	fmovd
   %f4   tos h# 10	stdf
   %f6   tos h# 18	stdf
   %f8   tos h# 20	stdf
   %f10  tos h# 28	stdf
   %f12  tos h# 30	stdf
   %f14  tos h# 38	stdf
   %f16  tos h# 40	stdf
   fpu-off
   sp tos pop
c;

\ Physical memory (free memory only), 8 bytes at a time through a
\ physical ASI (14 through the E-cache, 15 around it).
code post-fill ( pa len x asi -- )
   sc5			rdasi
   tos 0		wrasi
   sp 0 /n*   sc3	nget
   sp 1 /n*   sc2	nget
   sp 2 /n*   sc1	nget
   sp 3 /n*   sp	add
   begin
      sc2 8      sc2	subcc
      sc1 sc2    sc4	add
   0= until
      sc3 sc4 0 %asi	stxa
   #Sync  membar
   sc5 0		wrasi
   sp         tos	pop
c;
\ The first doubleword not holding x: its address and contents.
code post-check ( pa len x asi -- false | obs pa true )
   sc5			rdasi
   tos 0		wrasi
   sp 0 /n*   sc3	nget
   sp 1 /n*   sc2	nget
   sp 2 /n*   sc1	nget
   sp 1 /n*   sp	add
   begin
      sc2 8      sc2	sub
      sc1 sc2    sc6	add
      sc6 0 %asi  sc4	ldxa
      sc4 sc3    %g0	subcc
      0<> if  nop
         sc5 0		wrasi
         sc4  sp 1 /n*	nput
         sc6  sp 0 /n*	nput
         -1         tos	move
         next
      then
      sc2 %g0    %g0	subcc
   0= until
      nop
   sc5 0		wrasi
   sp 2 /n*   sp	add
   %g0        tos	move
c;
\ Each doubleword its own address.
code post-fill-addr ( pa len asi -- )
   sc5			rdasi
   tos 0		wrasi
   sp 0 /n*   sc2	nget
   sp 1 /n*   sc1	nget
   sp 2 /n*   sp	add
   begin
      sc2 8      sc2	subcc
      sc1 sc2    sc3	add
   0= until
      sc3 sc3 0 %asi	stxa
   #Sync  membar
   sc5 0		wrasi
   sp         tos	pop
c;
code post-check-addr ( pa len asi -- false | obs pa true )
   sc5			rdasi
   tos 0		wrasi
   sp 0 /n*   sc2	nget
   sp 1 /n*   sc1	nget
   sp 1 /n*   sp	add
   begin
      sc2 8      sc2	sub
      sc1 sc2    sc3	add
      sc3 0 %asi  sc4	ldxa
      sc4 sc3    %g0	subcc
      0<> if  nop
         sc5 0		wrasi
         sc4  sp 0 /n*	nput
         sc3        sp	push
         -1         tos	move
         next
      then
      sc2 %g0    %g0	subcc
   0= until
      nop
   sc5 0		wrasi
   sp 1 /n*   sp	add
   %g0        tos	move
c;

\ Block stores and loads (64 bytes, through the FPU's registers, ASI f0)
\ over mapped memory, buf a 64-byte aligned buffer of 128 bytes: the
\ block in buf stored to each block of the range; each block of the
\ range loaded and stored to buf+64, its doublewords against x.
code post-blk-fill ( va len buf -- )
   fpu-on
   tos %g0 h# f0  %f0	lddfa
   #Sync  membar
   sp 0 /n*   sc2	nget
   sp 1 /n*   sc1	nget
   begin
      sc2 h# 40  sc2	subcc
   0= until
      %f0 sc1 sc2 h# f0	stdfa
   #Sync  membar
   fpu-off
   sp 2 /n*   sp	add
   sp         tos	pop
c;
code post-blk-check ( va len x buf -- false | obs va' true )
   fpu-on
   sp 0 /n*   sc3	nget		\ x
   sp 1 /n*   sc2	nget		\ len
   sp 2 /n*   sc1	nget		\ va
   tos h# 40  sc4	add		\ the check area
   begin
      sc2 h# 40  sc2	sub
      sc1 sc2 h# f0  %f0	lddfa
      #Sync  membar
      %f0 sc4 %g0 h# f0	stdfa
      #Sync  membar
      %g0 h# 40  scr	add
      begin
         scr 8      scr	sub
         sc4 scr    sc6	nget
         sc6 sc3    %g0	subcc
         0<> if  nop
            fpu-off
            sc6  sp 2 /n*	nput
            sc1 sc2    sc6	add
            sc6 scr    sc6	add
            sc6  sp 1 /n*	nput
            sp 1 /n*   sp	add
            -1         tos	move
            next
         then
         scr %g0    %g0	subcc
      0= until
         nop
      sc2 %g0    %g0	subcc
   0= until
      nop
   fpu-off
   sp 3 /n*   sp	add
   %g0        tos	move
c;
\ Each block its doublewords' physical addresses (pa the range's), built
\ in buf and stored; checked the same way.
code post-blk-fill-addr ( va pa len buf -- )
   fpu-on
   sp 0 /n*   sc3	nget		\ len
   sp 1 /n*   sc2	nget		\ pa
   sp 2 /n*   sc1	nget		\ va
   %g0        sc4	move		\ the offset
   begin
      sc2 sc4    sc6	add
      %g0 h# 40  scr	add
      begin
         scr 8      scr	sub
         sc6 scr    %g1	add
      scr %g0    %g0	subcc
      0= until
         %g1  tos scr	stx
      #Sync  membar
      tos %g0 h# f0  %f0	lddfa
      #Sync  membar
      %f0 sc1 sc4 h# f0	stdfa
      sc4 h# 40  sc4	add
      sc4 sc3    %g0	subcc
   0= until
      nop
   #Sync  membar
   fpu-off
   sp 3 /n*   sp	add
   sp         tos	pop
c;
code post-blk-check-addr ( va pa len buf -- false | obs pa' true )
   fpu-on
   sp 0 /n*   sc3	nget		\ len
   sp 1 /n*   sc2	nget		\ pa
   sp 2 /n*   sc1	nget		\ va
   tos h# 40  sc4	add		\ the check area
   %g0        %o0	move		\ the offset
   begin
      sc1 %o0 h# f0  %f0	lddfa
      #Sync  membar
      %f0 sc4 %g0 h# f0	stdfa
      #Sync  membar
      %g0 h# 40  scr	add
      begin
         scr 8      scr	sub
         sc4 scr    sc6	nget
         sc2 %o0    %g1	add
         %g1 scr    %g1	add		\ expected: its address
         sc6 %g1    %g0	subcc
         0<> if  nop
            fpu-off
            sc6  sp 1 /n*	nput
            %g1  sp 0 /n*	nput
            sp 1 /n*   sp	add
            -1         tos	move
            next
         then
         scr %g0    %g0	subcc
      0= until
         nop
      %o0 h# 40  %o0	add
      %o0 sc3    %g0	subcc
   0= until
      nop
   fpu-off
   sp 3 /n*   sp	add
   %g0        tos	move
c;

\ ---------------------------------------------------------------------
\ Traps a test causes on purpose: the trap's type left in sc7 (of the
\ window the test runs in, which the handler shares), and on past the
\ instruction (done); an interrupt's handler clears its soft interrupt
\ and lets the instruction run (retry). A handler is in the trap table
\ only while its test runs; the table's entry is put back after.

label post-trap
   sc7			rdtt
   done
end-code

label post-int-trap
   sc7			rdtt
   sc7 h# 40	%g1	sub
   %g0 1	%g2	add
   %g2 %g1	%g2	sllx
   %g2 0	%r21	wrasr		\ CLEAR_SOFTINT
   retry
end-code

code post-tt0 ( -- )  %g0 sc7 move  c;
code post-tt@ ( -- tt )  tos sp push  sc7 tos move  c;

8 /l* constant /post-vector
/post-vector buffer: post-vector-save
0 value post-vector#
: post-trap-on ( handler tt -- )
   dup to post-vector#  vector-adr post-vector-save /post-vector move
   post-vector# vector!
;
: post-trap-off ( -- )
   post-vector# vector-adr  /post-vector 0 do		( entry )
      post-vector-save i + l@  over i + instruction!
   /l +loop  drop
;
\ xt run with post-trap at tt; the trap type it took (0 for none).
: post-trap-run ( xt tt -- tt' )
   post-trap swap post-trap-on  post-tt0 execute post-tt@  post-trap-off
;
\ The trap xt should take.
: post-expect-trap ( xt tt msg$ -- )
   2>r dup >r post-trap-run r> 2r> post-compare
;

\ The operands of an access a test makes (adr, asi; the value stored).
0 value post-adr
0 value post-asi
: (ldx) ( -- )  post-adr post-asi spacex@ drop  ;
: (ldl) ( -- )  post-adr post-asi spacel@ drop  ;
: (ldw) ( -- )  post-adr post-asi spacew@ drop  ;
: (stx) ( -- )  0 post-adr post-asi spacex!  ;
: (stl) ( -- )  0 post-adr post-asi spacel!  ;
: (stw) ( -- )  0 post-adr post-asi spacew!  ;

\ The D-MMU's fault status cleared after a fault taken on purpose; the
\ processor's asynchronous fault status after an error.
: dmmu-sfsr-clear ( -- )  0 h# 18 h# 58 spacex!  ;
: cpu-afsr-clear ( -- )  cpu-afsr@ cpu-afsr!  ;

\ ---------------------------------------------------------------------
\ Memory free for the tests: the start of the largest free range, from
\ the memory node's "available".

: memory-prop ( name$ -- adr len false | true )
   memory-node @ ihandle>phandle get-package-property
;
: decode-x ( adr len -- adr' len' x )  decode-int >r decode-int r> lxjoin  ;

0 value post-scratch		\ the physical address
0 value /post-scratch		\ its size
: post-find-scratch ( -- )
   0 to /post-scratch
   " available" memory-prop if  exit  then
   begin  dup  while					( xdr,len )
      decode-x >r decode-x r> swap			( xdr,len pa size )
      dup /post-scratch u> if  to /post-scratch to post-scratch  else  2drop  then
   repeat  2drop
;

\ A page of it mapped (mode as the MMU's map takes it: 1 writable, 20
\ cacheable, 100 the other byte order), and unmapped.
: post-map ( pa mode -- va )
   >r 0 pagesize 1 mmu-claim			( pa 0 va ) ( r: mode )
   dup >r pagesize r> r> swap >r mmu-map r>	( va )
;
: post-unmap ( va -- )  dup pagesize mmu-unmap  pagesize mmu-release  ;

0 value post-ec-size		\ the E-cache's size

\ ---------------------------------------------------------------------
\ NVRAM: the M48T59's battery, and its scratch bytes (between the keys
\ and the IDPROM), each saved and put back.

h# 1800 constant post-nv-base
h# 1fd8 post-nv-base - constant /post-nv
0 value post-nv-save

: post-battery ( -- )
   " NVRAM" post-suspect
   h# 1ff0 nvram-c@ h# 10 and if  " Battery Low" post-fail  then
;
: post-nv-saved ( -- )
   /post-nv alloc-mem to post-nv-save
   /post-nv 0 do  post-nv-base i + nvram-c@  post-nv-save i + c!  loop
;
: post-nv-restore ( -- )
   /post-nv 0 do  post-nv-save i + c@  post-nv-base i + nvram-c!  loop
   post-nv-save /post-nv free-mem
;
: post-nv-byte ( offset -- b )  dup 8 rshift xor h# ff and  ;
: post-nv-addr ( -- )
   " NVRAM" post-suspect  post-nv-saved
   /post-nv 0 do  i post-nv-byte  post-nv-base i + nvram-c!  loop
   /post-nv 0 do
      post-nv-base i + nvram-c@  i post-nv-byte  <> if
         " Scratch address mismatch" post-fail  leave
      then
   loop
   post-nv-restore
;
: post-nv-data ( -- )
   " NVRAM" post-suspect  post-nv-saved
   /post-nv 0 do
      h# aa h# 55 h# ff h# 00  4 0 do			( b .. )
         dup post-nv-base j + nvram-c!
         post-nv-base j + nvram-c@ <> if  " Scratch data mismatch" post-fail  then
      loop
   loop
   post-nv-restore
;

\ ---------------------------------------------------------------------
\ The D-MMU's TLB: each entry's tag (VA<63:13> and context, through Tag
\ Access) and data (the TTE as Data Access reads it: soft fields as 0,
\ the Diag field's used and size bits) written and read back, then put
\ back, with interrupts off and no other data access between.

code post-dtlb-swap ( tag data index -- tag' data' )
   tos 3      sc6	sllx		\ the entry's address
   sp 0 /n*   sc2	nget		\ data
   sp 1 /n*   sc1	nget		\ tag
   sp 1 /n*   sp	add
   sc4			rdpstate
   %g0 h# 14		wrpstate
   %g0 h# 30  scr	add
   %g0 scr h# 58  sc3	ldxa		\ Tag Access, as it was
   sc6 %g0 h# 5e  sc5	ldxa		\ the entry's tag
   sc6 %g0 h# 5d  sc7	ldxa		\ and data
   sc1 %g0 scr h# 58	stxa		\ the patterns written
   #Sync  membar
   sc2 sc6 %g0 h# 5d	stxa
   #Sync  membar
   sc6 %g0 h# 5e  sc1	ldxa		\ and read back
   sc6 %g0 h# 5d  sc2	ldxa
   sc5 %g0 scr h# 58	stxa		\ the entry put back
   #Sync  membar
   sc7 sc6 %g0 h# 5d	stxa
   #Sync  membar
   sc3 %g0 scr h# 58	stxa		\ and Tag Access
   #Sync  membar
   sc4 0		wrpstate
   sc1  sp 0 /n*	nput
   sc2        tos	move
c;
: dtlb-entry ( index -- tag data )
   3 lshift  dup h# 5e spacex@  swap h# 5d spacex@
;
\ The bits Data Access keeps: all but the soft fields and Diag.
h# f800.01ff.ffff.e07f constant tte-mask

: post-dtlb-tags ( -- )
   " DMMU" post-suspect
   d# 64 0 do
      h# 0000.0555.5555.4aaa h# ffff.faaa.aaaa.b555  2 0 do	( p .. )
         dup  j dtlb-entry nip  j post-dtlb-swap drop		( .. p tag' )
         swap " DMMU TLB tag" post-compare
      loop
   loop
;
: post-dtlb-data ( -- )
   " DMMU" post-suspect
   d# 64 0 do
      h# 5555.5555.5555.5555 h# aaaa.aaaa.aaaa.aaaa  2 0 do	( p .. )
         tte-mask and  j dtlb-entry drop over  j post-dtlb-swap nip
         tte-mask and  swap " DMMU TLB data" post-compare
      loop
   loop
;

\ ---------------------------------------------------------------------
\ The E-cache: its data (VA<40:39> 1) and tags (2, through the staging
\ register at ASI 4e) through the diagnostic ASIs (76 to write, 7e to
\ read), each written whole and read back (a pattern, or each its own
\ offset). The cache is in use, so each test is one run with interrupts
\ off and no other data access: first every line made clean (a line of
\ the scratch memory loaded into each), then the test, then every tag
\ invalid (state 0, its parity bits right), so nothing is lost and
\ nothing stale is read after. The run is made twice, first on the
\ scratch memory through ASI 14, so that the instructions of the run that
\ counts are in the I-cache before it changes the E-cache.

also assembler definitions
\ The first difference (sc5, xor'ed) at offset scr: scr+1 to sc7, the
\ value read (sc2) to %o7, while sc7 is 0.
: ec-note ( -- )
   %g0 sc5    sc6	sub
   sc6 sc5    sc6	or
   sc6 d# 63  sc6	srlx		\ different
   %g0 sc7    %g1	sub
   %g1 sc7    %g1	or
   %g1 d# 63  %g1	srlx		\ one already noted
   sc6 %g1    sc6	andn
   scr 1      %g1	add
   %g1 sc6    %g1	mulx
   sc7 %g1    sc7	add
   sc2 sc6    %g1	mulx
   %o7 %g1    %o7	add
;
\ %asi: 14 in the run on the scratch memory (%o4 0), else n.
: ec-asi ( n -- )
   %o4 swap h# 14 - %g1	mulx
   %g1 h# 14  %g1	add
   %g1 0		wrasi
;
\ %o5: the scratch memory (%o1), or the array (field VA<40:39>).
: ec-base ( field -- )
   %g0 swap   %g1	add
   %g1 d# 39  %g1	sllx
   %g1 %o1    %g1	sub
   %g1 %o4    %g1	mulx
   %g1 %o1    %o5	add
;
\ The value for offset scr to sc1: the pattern (%o2), or the offset's
\ (the offset in the high word and the line in the low, as %o3 says).
: ec-value ( -- )
   scr d# 32  sc1	sllx
   scr 6      %g1	srlx
   sc1 %g1    sc1	add
   sc1 %o2    sc1	sub
   sc1 %o3    sc1	mulx
   sc1 %o2    sc1	add
;
: ec-start ( -- )
   tos        %o1	move		\ the scratch memory
   sp 0 /n*   %o0	nget		\ the size
   sp 1 /n*   %o3	nget		\ 1: each its offset
   sp 2 /n*   %o2	nget		\ the pattern
   sp 3 /n*   sp	add
   sc3			rdpstate
   sc4			rdasi
   %g0 h# 14		wrpstate
   %g0        %o4	move
   %g0        sc7	move
   %g0        %o7	move
;
\ Each line made clean.
: ec-clean ( -- )
   %g0        scr	move
   begin
      %o1 scr h# 14  %g1	ldxa
      scr h# 40  scr	add
      scr %o0    %g0	subcc
   0= until
      nop
;
\ Each tag invalid.
: ec-invalidate ( -- )
   2 ec-base  h# 76 ec-asi
   h# 30000   sc1	set
   sc1 %g0 %g0 h# 4e	stxa
   #Sync  membar
   %g0        scr	move
   begin
      %o5 scr    %g1	add
      sc1 %g1 0 %asi	stxa
      scr h# 40  scr	add
      scr %o0    %g0	subcc
   0= until
      nop
   #Sync  membar
;
: ec-end ( -- )
   %o4 1      %o4	add
   %o4 2      %g0	subcc
   0= until
      nop
   sc4 0		wrasi
   sc3 0		wrpstate
   sc7 %g0    %g0	subcc
   0<> if  nop
      sc7 1      sc7	sub
      sc7        sp	push
      %o7        sp	push
      -1         tos	move
      next
   then
   %g0        tos	move
;
previous definitions

code post-ec-data ( pattern addr? size scratch -- 0 | offset observed true )
   ec-start
   begin
      1 ec-base  ec-clean
      h# 76 ec-asi
      %g0        scr	move
      begin
         ec-value
         %o5 scr    %g1	add
         sc1 %g1 0 %asi	stxa
         scr 8      scr	add
         scr %o0    %g0	subcc
      0= until
         nop
      #Sync  membar
      h# 7e ec-asi
      %g0        scr	move
      begin
         ec-value
         %o5 scr    %g1	add
         %g1 0 %asi  sc2	ldxa
         sc2 sc1    sc5	xor
         ec-note
         scr 8      scr	add
         scr %o0    %g0	subcc
      0= until
         nop
      ec-invalidate
   ec-end
c;

code post-ec-tags ( pattern addr? size scratch -- 0 | offset observed true )
   ec-start
   begin
      2 ec-base  ec-clean
      h# 76 ec-asi
      %g0        scr	move
      begin
         ec-value
         sc1 %g0 %g0 h# 4e	stxa
         #Sync  membar
         %o5 scr    %g1	add
         sc1 %g1 0 %asi	stxa
         scr h# 40  scr	add
         scr %o0    %g0	subcc
      0= until
         nop
      #Sync  membar
      h# 7e ec-asi
      %g0        scr	move
      begin
         ec-value
         %o5 scr    %g1	add
         %g1 0 %asi  sc2	ldxa
         #Sync  membar
         %g0 %g0 h# 4e  %g1	ldxa		\ the tag, staged
         %g1 sc2    %g1	sub
         %g1 %o4    %g1	mulx
         sc2 %g1    sc2	add		\ (the scratch memory's, the first run)
         sc2 sc1    sc5	xor
         h# 3cfff   %g1	set		\ parity, state, tag
         sc5 %g1    sc5	and
         ec-note
         scr h# 40  scr	add
         scr %o0    %g0	subcc
      0= until
         nop
      ec-invalidate
   ec-end
c;

\ The value at an offset, as the runs above make it.
: ec-value ( pattern addr? offset -- x )
   swap if  nip dup d# 32 lshift swap 6 rshift +  else  drop  then
;
: post-ec ( pattern addr? xt mask msg$ -- )
   " Ecache" post-suspect
   2>r >r >r  2dup post-ec-size post-scratch r> execute if	( p a? off obs ) ( r: mask msg$ )
      r@ and >r  ec-value r> swap r> and  2r> post-compare
   else  2drop r> drop 2r> 2drop  then
;
: post-ec-ram-addr ( -- )   0 1 ['] post-ec-data -1 " Ecache RAM address mismatch" post-ec  ;
: post-ec-tag-addr ( -- )   0 1 ['] post-ec-tags h# 3cfff " Ecache tag address mismatch" post-ec  ;
: post-ec-ram ( -- )
   h# 5555.5555.5555.5555 h# aaaa.aaaa.aaaa.aaaa  2 0 do
      0 ['] post-ec-data -1 " Ecache RAM data mismatch" post-ec
   loop
;
: post-ec-tag ( -- )
   h# 1.5555 h# 2.aaaa  2 0 do
      0 ['] post-ec-tags h# 3cfff " Ecache tag data mismatch" post-ec
   loop
;
: post-probe-ec ( -- )  ecache-size to post-ec-size  ;

\ The E-cache in use: a doubleword of the scratch memory stored through
\ it and read back, its line's tag then the line's (PA<29:18>), modified.
h# 100.0000.0000 constant ec-tags
: post-ec-access ( -- )
   " Ecache" post-suspect
   h# 1234.5678.9abc.def0  post-scratch h# 14 spacex!
   post-scratch h# 14 spacex@  h# 1234.5678.9abc.def0 " Ecache data mismatch" post-compare
   post-scratch post-ec-size 1- and ec-tags or h# 7e spacex@ drop
   0 h# 4e spacex@ h# cfff and
   post-scratch d# 18 rshift h# fff and  h# c000 or  " Ecache tag mismatch" post-compare
;

\ ---------------------------------------------------------------------
\ The processor.

: post-v9 ( -- )
   " CPU (Basic)" post-suspect
   h# 1234.5678.90ab.cdef h# f.edcb.a987.6543 *  h# 71c9.77b3.053f.308d
      " MULX" post-compare
   h# 1234.5678.90ab.cdef h# f.edcb.a987.6543 u/mod
      h# 124 " UDIVX" post-compare  h# 9.1a2b.323c.4d83 " UDIVX remainder" post-compare
   h# 123.4567.89ab.cdef negate h# 12345 /  h# ffff.feff.ffa4.ffdf " Signed divide" post-compare
   h# 1234.5678.90ab.cdef d# 13 lshift  h# 8acf.1215.79bd.e000 " SLLX" post-compare
   h# 1234.5678.90ab.cdef d# 17 rshift  h# 91a.2b3c.4855 " SRLX" post-compare
   h# f000.0000.0000.0123 9 >>a  h# fff8.0000.0000.0000 " SRAX" post-compare
   h# 1234.5678.90ab.cdef h# 5555.5555.5555.5555 xor  h# 4761.032d.c5fe.98ba
      " XOR" post-compare
   h# 1234.5678.90ab.cdef h# ff0.0ff0.0ff0.0ff0 and  h# 230.0670.00a0.0de0
      " AND" post-compare
   h# 1234.5678.90ab.cdef h# 8000.0000.0000.0001 or  h# 9234.5678.90ab.cdef
      " OR" post-compare
;

: post-tick-test ( -- )
   " CPU (Basic)" post-suspect
   h# 8000.0000.5555.5555 h# aaaa.aaaa.aaaa.aaaa  2 0 do	( pattern .. )
      dup post-tick					( pattern t0 readback t1 )
      rot u> 0= if  " Tick not counting" post-fail  then	( pattern readback )
      swap " Tick Compare" post-compare
   loop
;

\ A software trap (Tcc, trap 171), taken, and one whose condition is
\ false, not.
code post-ta ( -- )  %g0 h# 71 always trapif  c;
code post-tn ( -- )  %g0 h# 71 never trapif  c;
: post-soft-trap ( -- )
   " CPU (Basic)" post-suspect
   ['] post-ta h# 171 " Trap not taken" post-expect-trap
   ['] post-tn h# 171 post-trap-run 0 " Trap taken" post-compare
;

\ SOFTINT: each level's bit set and cleared; then, with the PIL below it
\ and interrupts on, its interrupt taken (interrupt_level_n).
0 value post-bit
code post-softint-int ( level -- )
   sc4			rdpstate
   sc5			rdpil
   %g0 h# 14		wrpstate
   tos 1      sc1	sub
   sc1 0		wrpil
   %g0 1      sc1	add
   sc1 tos    sc1	sllx
   sc1 0      %r20	wrasr		\ SET_SOFTINT
   %g0 h# 16		wrpstate	\ taken here
   nop  nop  nop  nop
   %g0 h# 14		wrpstate
   sc5 0		wrpil
   sc4 0		wrpstate
   sp         tos	pop
c;
0 value post-level
: (post-int) ( -- )  post-level post-softint-int  ;
: post-softint-test ( -- )
   " CPU (Basic)" post-suspect
   d# 14 1 do
      1 i lshift to post-bit
      post-bit post-softint				( after-set after-clear )
      post-bit and if  " Softint bit not cleared" post-fail  then
      post-bit and 0= if  " Softint bit not set" post-fail  then
   loop
   d# 14 1 do
      i to post-level
      post-int-trap i h# 40 + post-trap-on
      post-tt0 (post-int) post-tt@  post-trap-off
      i h# 40 +  " Softint interrupt not taken" post-compare
   loop
;

\ The MMUs' registers: each written with patterns (of the bits it keeps)
\ and read back.
h# 58 constant dmmu-asi
h# 50 constant immu-asi
0 value post-reg-va
0 value post-reg-asi
0 value post-reg$
0 value /post-reg$
: post-reg ( va asi msg$ -- )  to /post-reg$ to post-reg$ to post-reg-asi to post-reg-va  ;
: post-reg-pattern ( pattern -- )
   dup post-reg-va post-reg-asi post-asi-swap  swap post-reg$ /post-reg$ post-compare
;
0 value post-reg-mask
: post-mmu-reg ( va asi mask msg$ -- )
   2>r to post-reg-mask 2r> post-reg
   h# 5555.5555.5555.5555 post-reg-mask and post-reg-pattern
   h# aaaa.aaaa.aaaa.aaaa post-reg-mask and post-reg-pattern
;
: post-pctx ( -- )  " DMMU" post-suspect  8 dmmu-asi h# 1fff " Primary Context" post-mmu-reg  ;
: post-sctx ( -- )  " DMMU" post-suspect  h# 10 dmmu-asi h# 1fff " Secondary Context" post-mmu-reg  ;
: post-dtsb ( -- )  " DMMU" post-suspect  h# 28 dmmu-asi h# ff8 invert " DMMU TSB" post-mmu-reg  ;
: post-itsb ( -- )  " IMMU" post-suspect  h# 28 immu-asi h# ff8 invert " IMMU TSB" post-mmu-reg  ;
: post-pawp ( -- )
   " DMMU" post-suspect  h# 40 dmmu-asi h# 1ff.ffff.fff8 " PA Watchpoint" post-mmu-reg
;
\ Tag access: VA<63:13> (sign-extended from bit 43) and a context;
\ the VA watchpoint: VA<43:3>, sign-extended.
: post-dtag ( -- )
   " DMMU" post-suspect  h# 30 dmmu-asi " DMMU Tag Access" post-reg
   h# 0000.0555.5555.4aaa post-reg-pattern  h# ffff.faaa.aaaa.b555 post-reg-pattern
;
: post-itag ( -- )
   " IMMU" post-suspect  h# 30 immu-asi " IMMU Tag Access" post-reg
   h# 0000.0555.5555.4aaa post-reg-pattern  h# ffff.faaa.aaaa.b555 post-reg-pattern
;
: post-vawp ( -- )
   " DMMU" post-suspect  h# 38 dmmu-asi " VA Watchpoint" post-reg
   h# 0000.0555.5555.5550 post-reg-pattern  h# ffff.faaa.aaaa.aaa8 post-reg-pattern
;

\ ---------------------------------------------------------------------
\ The L1 caches, each turned off (the LSU control register's IC, bit 0,
\ and DC, bit 1) while its arrays are written and read back through the
\ diagnostic ASIs, then every line of it invalid and the cache on again.

0 value post-lsu
: post-cache-off ( bit -- )  lsucr@ dup to post-lsu  swap invert and lsucr!  ;
: post-cache-on ( -- )  post-lsu lsucr!  ;

\ An array of n entries step apart (through asi), of the bits mask
\ keeps: each written with the pattern, or with its own index.
0 value post-arr-asi
0 value post-arr-step
0 value post-arr-mask
: post-arr ( asi step mask -- )  to post-arr-mask to post-arr-step to post-arr-asi  ;
: post-arr-adr ( i -- adr )  post-arr-step *  ;
: post-arr-fill ( n pattern -- )
   swap 0 ?do  dup  i post-arr-adr post-arr-asi spacex!  loop  drop
;
: post-arr-check ( n pattern msg$ -- )
   2>r post-arr-mask and swap 0 ?do			( p ) ( r: msg$ )
      i post-arr-adr post-arr-asi spacex@ post-arr-mask and  over
      i post-arr-adr 2r@ post-compare-at
      post-failed? if  leave  then
   loop  drop 2r> 2drop
;
\ Index patterns: the index in each byte.
: post-index ( i -- x )  h# 0101.0101.0101.0101 *  ;
: post-arr-addr ( n msg$ -- )
   2>r  dup 0 ?do  i post-index  i post-arr-adr post-arr-asi spacex!  loop	( n )
   0 ?do
      i post-arr-adr post-arr-asi spacex@ post-arr-mask and
      i post-index post-arr-mask and  i post-arr-adr  2r@ post-compare-at
      post-failed? if  leave  then
   loop  2r> 2drop
;
0 value post-arr-n
: post-arr-pattern ( p msg$ -- )
   2>r  post-arr-n over post-arr-fill  post-arr-n swap 2r> post-arr-check
;
: post-arr-patterns ( n msg$ -- )
   rot to post-arr-n
   2dup h# 5555.5555.5555.5555 -rot post-arr-pattern
   h# aaaa.aaaa.aaaa.aaaa -rot post-arr-pattern
;
: post-arr-clear ( n -- )  0 post-arr-fill  ;

: post-dcache-ram ( -- )
   " Dcache" post-suspect  2 post-cache-off
   h# 46 8 -1 post-arr
   d# 2048 " Dcache RAM address mismatch" post-arr-addr
   d# 2048 " Dcache RAM data mismatch" post-arr-patterns
   h# 47 h# 20 -1 post-arr  d# 512 post-arr-clear
   post-cache-on
;
\ D-cache tags: PA<40:13> in 29:2, a valid bit for each half line.
: post-dcache-tag ( -- )
   " Dcache" post-suspect  2 post-cache-off
   h# 47 h# 20 h# 3fff.ffff post-arr
   d# 512 " Dcache tag address mismatch" post-arr-addr
   d# 512 " Dcache tag data mismatch" post-arr-patterns
   d# 512 post-arr-clear
   post-cache-on
;
\ I-cache: instruction pairs (VA<13> the way), tags (valid 36, PA<40:13>
\ in 35:8), predecode bits (a quad's, the pair VA<3> names) and next
\ fields (set prediction, next field, branch: 23:8).
: post-icache-invalid ( -- )  h# 67 h# 20 -1 post-arr  d# 512 post-arr-clear  ;
: post-icache-ram ( -- )
   " Icache" post-suspect  1 post-cache-off
   h# 66 8 -1 post-arr
   d# 2048 " Icache RAM address mismatch" post-arr-addr
   d# 2048 " Icache RAM data mismatch" post-arr-patterns
   post-icache-invalid  post-cache-on
;
: post-icache-tag ( -- )
   " Icache" post-suspect  1 post-cache-off
   h# 67 h# 20 h# 1f.ffff.ff00 post-arr
   d# 512 " Icache tag address mismatch" post-arr-addr
   d# 512 " Icache tag data mismatch" post-arr-patterns
   post-icache-invalid  post-cache-on
;
: post-icache-next ( -- )
   " Icache" post-suspect  1 post-cache-off
   h# 6f h# 10 h# ff.ff00 post-arr
   d# 1024 " Icache next address mismatch" post-arr-addr
   d# 1024 " Icache next data mismatch" post-arr-patterns
   post-icache-invalid  post-cache-on
;
\ (Written a quad's 16 bits at a time; read a pair's 8: pair 0 the high
\ byte.)
: post-icache-predecode ( -- )
   " Icache" post-suspect  1 post-cache-off
   d# 1024 0 do
      h# 55aa h# aa55 i h# 3c3c xor  3 0 do			( p .. )
         dup j h# 10 * h# 6e spacex!
         j h# 10 * h# 6e spacex@  over 8 rshift h# ff and
            j h# 10 * " Icache predecode mismatch" post-compare-at
         j h# 10 * 8 + h# 6e spacex@  swap h# ff and
            j h# 10 * 8 + " Icache predecode mismatch" post-compare-at
      loop
      post-failed? if  leave  then
   loop
   post-icache-invalid  post-cache-on
;

\ ---------------------------------------------------------------------
\ The FPU.

: post-fpu-regs ( -- )
   " FPU" post-suspect
   -1 0 h# aaaa.aaaa.aaaa.aaaa h# 5555.5555.5555.5555  4 0 do	( p .. )
      dup post-buf x!  post-buf post-fregs			( .. p )
      d# 32 0 do
         post-buf i 1+ 8 * + x@  over " FP register" post-compare
      loop  drop
   loop
;
\ FSR: RD, TEM, fcc0-3, aexc and cexc are kept.
h# 3f.cf80.0fff constant fsr-mask
: post-fsr-test ( -- )
   " FPU" post-suspect
   0 fsr-mask h# 2a.8500.0555 h# 15.4a80.0aaa  4 0 do	( p .. )
      dup post-buf x!  post-buf post-fsr
      post-buf 8 + x@ fsr-mask and  swap " FSR" post-compare
   loop
;
: post-fops-run ( a b -- )  post-buf 8 + x!  post-buf x!  post-buf post-fops  ;
\ Result n of post-fops against the one expected.
: post-fop ( n expected msg$ -- )
   2>r swap 2+ 8 * post-buf + x@ swap 2r> post-compare
;
: post-fpu-move ( -- )
   " FPU" post-suspect
   h# 3ff8.0000.0000.0000 h# 4002.0000.0000.0000 post-fops-run	\ 1.5, 2.25
   4 h# bff8.0000.0000.0000 " FNEGd" post-fop
   5 h# 3ff8.0000.0000.0000 " FABSd" post-fop
   6 h# 3ff8.0000.0000.0000 " FMOVd" post-fop
;
: post-fpu-func ( -- )
   " FPU" post-suspect
   h# 3ff8.0000.0000.0000 h# 4002.0000.0000.0000 post-fops-run	\ 1.5, 2.25
   0 h# 400e.0000.0000.0000 " FADDd" post-fop
   1 h# 400b.0000.0000.0000 " FMULd" post-fop
   2 h# 3fe5.5555.5555.5555 " FDIVd" post-fop
   3 h# 3ff8.0000.0000.0000 " FSQRTd" post-fop
   h# 3ff0.0000.0000.0000 h# 4000.0000.0000.0000 post-fops-run	\ 1, 2
   3 h# 3ff6.a09e.667f.3bcd " FSQRTd" post-fop
;
\ A division by zero with its trap enabled (FSR TEM.DZM, bit 24): an
\ IEEE 754 exception (trap 21), the FSR's cexc then DZ. buf: +0 the FSR
\ to load, +8 1.0, +10 0.0, +18 the FSR as it was, +20 the FSR after.
code post-fdiv0 ( buf -- )
   fpu-on
   tos h# 18	stxfsr
   tos 0	ldxfsr
   tos 8      %f0	lddf
   tos h# 10  %f2	lddf
   %f0 %f2    %f4	fdivd
   nop
   tos h# 20	stxfsr
   tos h# 18	ldxfsr
   fpu-off
   sp tos pop
c;
: (fdiv0) ( -- )  post-buf post-fdiv0  ;
: post-fpu-trap ( -- )
   " FPU" post-suspect
   h# 100.0000 post-buf x!  h# 3ff0.0000.0000.0000 post-buf 8 + x!  0 post-buf h# 10 + x!
   ['] (fdiv0) h# 21 " FP exception trap not taken" post-expect-trap
   post-buf h# 20 + x@ h# 1f and  2 " FSR cexc" post-compare
;

\ ---------------------------------------------------------------------
\ The UPA data bus: a doubleword of the scratch memory, around the
\ E-cache, with each line on its own (walking ones and zeros).
: post-upa ( -- )
   " UPA Data Bus" post-suspect
   d# 64 0 do
      1 i lshift  dup invert  2 0 do			( x .. )
         dup post-scratch h# 15 spacex!
         post-scratch h# 15 spacex@ swap " UPA data bus line" post-compare
      loop
   loop
;

\ ---------------------------------------------------------------------
\ Memory: each bank's size, then the free memory written and checked.

: post-banks ( -- )
   " Memory" post-suspect
   post-max? 0= if  exit  then
   " reg" memory-prop if  exit  then
   begin  dup  while					( xdr,len )
      decode-x >r decode-x				( xdr,len size ) ( r: pa )
      push-decimal
      tab tab ." INFO:  " d# 20 rshift 5 .r ." MB Bank "
      r> d# 28 rshift (.d) type cr
      pop-base
   repeat  2drop
;

\ xt ( pa size -- ) for each free range of memory.
0 value post-range-xt
: post-free-ranges ( xt -- )
   to post-range-xt
   " available" memory-prop if  exit  then
   begin  dup  while  post-failed? 0=  while		( xdr,len )
      decode-x >r decode-x r> swap			( xdr,len pa size )
      post-range-xt execute
   repeat then  2drop
;

\ xt ( pa len -- ) for each MB of a range, a dot for each if dots?.
1meg constant /post-chunk
0 value post-end
0 value post-chunk-xt
0 value post-dots?
: post-chunks ( pa size xt -- )
   to post-chunk-xt  over + to post-end		( pa )
   begin  dup post-end u<  post-failed? 0= and  while
      post-end over - /post-chunk min		( pa len )
      2dup post-chunk-xt execute  +  post-dots? if  ascii . emit  then
   repeat  drop
;

0 value post-pattern
0 value post-mem-asi
: (post-write) ( pa len -- )  post-pattern post-mem-asi post-fill  ;
: (post-read) ( pa len -- )
   post-pattern post-mem-asi post-check if		( obs pa )
      post-pattern swap " Memory data mismatch" post-compare-at
   then
;
: (post-addr-write) ( pa len -- )  post-mem-asi post-fill-addr  ;
: (post-addr-read) ( pa len -- )
   post-mem-asi post-check-addr if  dup " Memory address mismatch" post-compare-at  then
;

\ Block stores and loads: each MB mapped while it is written or read.
0 value post-blk-raw
0 value post-blk-buf
: post-blk-alloc ( -- )
   h# 100 alloc-mem dup to post-blk-raw  h# 3f + h# 3f invert and to post-blk-buf
;
: post-blk-free ( -- )  post-blk-raw h# 100 free-mem  ;
: post-chunk-map ( pa len -- va len )  tuck 0 swap memmap swap  ;
: post-chunk-unmap ( va len -- )  2dup mmu-unmap mmu-release  ;
: (post-blk-write) ( pa len -- )
   post-chunk-map  2dup post-blk-buf post-blk-fill  post-chunk-unmap
;
: (post-blk-read) ( pa len -- )
   over >r post-chunk-map  2dup post-pattern post-blk-buf post-blk-check	( va len ... )
   if								( va len obs va' )
      3 pick - r@ +  post-pattern swap " Memory data mismatch" post-compare-at
   then  r> drop post-chunk-unmap
;
: (post-blk-addr-write) ( pa len -- )
   over >r post-chunk-map  2dup r> swap post-blk-buf post-blk-fill-addr  post-chunk-unmap
;
: (post-blk-addr-read) ( pa len -- )
   over >r post-chunk-map  2dup r> swap post-blk-buf post-blk-check-addr
   if  dup " Memory address mismatch" post-compare-at  then
   post-chunk-unmap
;

\ A pass over the free memory: written with xt-w, read with xt-r.
0 value post-r-xt
0 value post-w-xt
: (post-w-range) ( pa size -- )  post-w-xt post-chunks  ;
: (post-r-range) ( pa size -- )  post-r-xt post-chunks  ;
: post-write-read ( xt-w xt-r -- )
   to post-r-xt to post-w-xt
   ['] (post-w-range) post-free-ranges  post-dots? if  cr  then
   post-failed? if  exit  then
   post-dots? if  ." Read "  then
   ['] (post-r-range) post-free-ranges  post-dots? if  cr  then
;

: post-pattern-pass ( x xt-w xt-r -- )
   rot to post-pattern
   ." Write 0x" post-pattern .post-x space  post-write-read
;
: post-mem-addr ( asi -- )
   " Memory" post-suspect  to post-mem-asi  false to post-dots?
   ['] (post-addr-write) ['] (post-addr-read) post-write-read
;
: post-mem-addr-ec ( -- )  h# 14 post-mem-addr  ;
\ Around the E-cache, so that the memory's ECC is checked: then no error
\ in the processor's AFSR (UE 21, CE 20).
: post-mem-addr-ecc ( -- )
   cpu-afsr-clear  h# 15 post-mem-addr
   cpu-afsr@ h# 30.0000 and 0 " ECC error" post-compare
;
: post-blk-addr ( -- )
   " Memory" post-suspect  false to post-dots?  post-blk-alloc
   ['] (post-blk-addr-write) ['] (post-blk-addr-read) post-write-read
   post-blk-free
;
: post-blk-patterns ( p0 p1 p2 p3 -- )
   " Memory" post-suspect  true to post-dots?  post-blk-alloc
   4 0 do
      post-failed? if  drop  else
         dup post-blk-buf 8 0 do  2dup i 8 * + x!  loop  2drop
         ['] (post-blk-write) ['] (post-blk-read) post-pattern-pass
      then
   loop
   post-blk-free
;
: post-blk ( -- )
   h# aaaa.aaaa.aaaa.aaaa h# cccc.cccc.cccc.cccc
   h# 5555.5555.5555.5555 h# 3333.3333.3333.3333  post-blk-patterns
;
: post-ecc-blk ( -- )
   h# dddd.dddd.dddd.dddd h# bbbb.bbbb.bbbb.bbbb
   h# 9696.9696.9696.9696 h# a5a5.a5a5.a5a5.a5a5  post-blk-patterns
   cpu-afsr@ h# 30.0000 and 0 " ECC error" post-compare
;

\ ---------------------------------------------------------------------
\ The Sabre: its registers (through the bypass ASI), its own PCI
\ configuration, its IOMMU's TLB through its diagnostic accesses.

h# 1fe.0000.0000 constant sabre-pa
: sabre-x@ ( offset -- x )  sabre-pa + bypass-asi spacex@  ;
: sabre-x! ( x offset -- )  sabre-pa + bypass-asi spacex!  ;
\ A register written with a pattern and read back, then put back, with
\ nothing else between; the bits mask keeps compared.
: sabre-swap ( pattern offset mask msg$ -- )
   2>r >r  over swap sabre-pa + bypass-asi post-asi-swap	( p read ) ( r: msg$ mask )
   r@ and  swap r> and  2r> post-compare
;
0 value sabre-off
0 value sabre-mask
: sabre-patterns ( offset mask msg$ -- )
   2swap to sabre-mask to sabre-off
   h# 5555.5555.5555.5555 h# aaaa.aaaa.aaaa.aaaa -1 0  4 0 do	( msg$ p .. )
      >r 2dup r> -rot 2>r sabre-off sabre-mask 2r> sabre-swap
   loop  2drop
;

\ The IOMMU control register's TSB_SIZE and TBW_SIZE (its error bits,
\ cleared by writing 1, written 0), and the TSB base (PA<40:13>).
h# 7.0004 constant iommu-ctl-mask
h# 1ff.ffff.e000 constant iommu-tsb-mask
: post-iommu-regs ( -- )
   " Sabre" post-suspect
   h# 200 sabre-x@ h# 700.0000 invert and		( ctl )
   h# 5.0000 h# 2.0004 iommu-ctl-mask 0  4 0 do		( ctl p .. )
      over iommu-ctl-mask invert and over or h# 200 sabre-x!
      h# 200 sabre-x@ iommu-ctl-mask and  swap " IOMMU Control" post-compare
   loop
   h# 200 sabre-x!
   h# 208 sabre-x@					( tsb )
   h# 155.5555.4000 h# aa.aaaa.a000 iommu-tsb-mask 0  4 0 do
      dup h# 208 sabre-x!  h# 208 sabre-x@ swap " IOMMU TSB Base" post-compare
   loop
   h# 208 sabre-x!
;

\ The IOMMU's TLB (16 entries), with its control register's MMU_DE (bit
\ 1) set: tags (ERRSTS, ERR, W, S, SIZE, VPN<31:13>: 24:0) at a580,
\ data (V, U, C, PA<33:13>) at a600; then each entry invalid (the TLB is
\ a cache of the TSB).
h# a580 constant iommu-tag-diag
h# a600 constant iommu-data-diag
h# 1ff.ffff constant iommu-tag-mask
h# 701f.ffff constant iommu-data-mask
0 value post-iommu-ctl
: post-iommu-de ( -- )  h# 200 sabre-x@ dup to post-iommu-ctl  2 or h# 200 sabre-x!  ;
: post-iommu-clear ( -- )
   d# 16 0 do  0 iommu-tag-diag i 8 * + sabre-x!  0 iommu-data-diag i 8 * + sabre-x!  loop
   post-iommu-ctl h# 700.0000 invert and h# 200 sabre-x!
;
0 value iommu-base
0 value iommu-mask
: post-iommu-array ( base mask msg$ -- )
   2swap to iommu-mask to iommu-base  2>r
   d# 16 0 do  i post-index  iommu-base i 8 * + sabre-x!  loop
   d# 16 0 do
      iommu-base i 8 * + sabre-x@ iommu-mask and  i post-index iommu-mask and  2r@ post-compare
   loop
   h# 5555.5555.5555.5555 h# aaaa.aaaa.aaaa.aaaa  2 0 do	( p .. )
      d# 16 0 do  dup iommu-base i 8 * + sabre-x!  loop
      d# 16 0 do
         iommu-base i 8 * + sabre-x@ iommu-mask and  over iommu-mask and  2r@ post-compare
      loop
      drop
   loop
   2r> 2drop
;
: post-iommu-ram ( -- )
   " Sabre" post-suspect  post-iommu-de
   iommu-data-diag iommu-data-mask " IOMMU RAM mismatch" post-iommu-array
   post-iommu-clear
;
: post-iommu-cam ( -- )
   " Sabre" post-suspect  post-iommu-de
   iommu-tag-diag iommu-tag-mask " IOMMU CAM mismatch" post-iommu-array
   post-iommu-clear
;
\ Entry n's tag VPN n+1 (8K pages): the comparator (a408) with VA n+1
\ (at a400) then shows bit n; a flush (210) of that VA then leaves entry
\ n invalid, the others valid.
: post-iommu-tags ( -- )
   d# 16 0 do
      i 1+ iommu-tag-diag i 8 * + sabre-x!
      h# 4000.0000 i or iommu-data-diag i 8 * + sabre-x!
   loop
;
: post-iommu-compare ( -- )
   " Sabre" post-suspect  post-iommu-de  post-iommu-tags
   d# 16 0 do
      i 1+ d# 13 lshift h# a400 sabre-x!
      h# a408 sabre-x@ h# ffff and  1 i lshift " IOMMU tag compare" post-compare
   loop
   post-iommu-clear
;
: post-iommu-flush ( -- )
   " Sabre" post-suspect  post-iommu-de
   d# 16 0 do
      post-iommu-tags
      i 1+ d# 13 lshift h# 210 sabre-x!
      d# 16 0 do
         iommu-data-diag i 8 * + sabre-x@ h# 4000.0000 and
         i j = if  0  else  h# 4000.0000  then  " IOMMU flush" post-compare
      loop
   loop
   post-iommu-clear
;

\ PBM A's configuration header (little-endian): Sun's vendor and the
\ Sabre's device ID, a host bridge.
h# 1fe.0100.0000 constant sabre-config-pa
: config-l@ ( bus dev fn reg -- l )
   swap 8 lshift or  swap d# 11 lshift or  swap d# 16 lshift or
   sabre-config-pa + h# 1d spacel@
;
: sabre-config-l@ ( offset -- l )  >r 0 0 0 r> config-l@  ;
: post-pbm-config ( -- )
   " Sabre" post-suspect
   0 sabre-config-l@ h# a000.108e " Vendor/Device ID" post-compare
   8 sabre-config-l@ 8 rshift h# 6.0000 " Class Code" post-compare
;
\ The PCI control register's fields (MRLM_EN, ARB_PARK, CPU_PRIO,
\ ARB_PRIO, ERRINT_EN, RETRY_WAIT_EN, ARB_EN) and the PCI diagnostic
\ register's (DIS_RETRY, the parity inversions).
: post-pbm-ctl ( -- )
   " Sabre" post-suspect  h# 2000 h# 10.003f.018f " PCI Control/Status" sabre-patterns
;
: post-pbm-diag ( -- )  " Sabre" post-suspect  h# 2020 h# 4e " PCI Diag" sabre-patterns  ;
\ The PCI target address space register (8 bits) and the IOMMU's virtual
\ address diagnostic register (VA<31:13>).
: post-sabre-io ( -- )
   " Sabre" post-suspect
   h# 2028 h# ff " PCI Target Address Space" sabre-patterns
   h# a400 h# ffff.e000 " IOMMU Virtual Address Diag" sabre-patterns
;

\ The PIO decoder: the Sabre's configuration (its latency timer) and its
\ IOMMU's TLB data and tags (MMU_DE set) each written with its own value,
\ then all read back, so that an address decoded to the wrong register
\ shows; the latency timer put back, the TLB left invalid.
create pio-data-patterns
   h# 0002.2222 l, h# 1013.3333 l, h# 4004.4444 l, h# 5015.5555 l,
   h# 4006.6666 l, h# 5017.7777 l, h# 0008.8888 l, h# 1019.9999 l,
   h# 000a.aaaa l, h# 101b.bbbb l, h# 400c.cccc l, h# 501d.dddd l,
   h# 400e.eeee l, h# 501f.ffff l, h# 1011.1110 l, h# 0002.2221 l,
create pio-tag-patterns
   h# 0133.3332 l, h# 0044.4443 l, h# 0155.5554 l, h# 0066.6665 l,
   h# 0177.7776 l, h# 0088.8887 l, h# 0199.9998 l, h# 00aa.aaa9 l,
   h# 01bb.bbba l, h# 00cc.cccb l, h# 01dd.dddc l, h# 00ee.eeed l,
   h# 01ff.fffe l, h# 0111.110f l, h# 0022.2220 l, h# 0133.3331 l,
: pio-pattern ( table i -- l )  la+ l@  ;
h# 0d constant sabre-latency
: sabre-config-c@ ( offset -- b )  sabre-config-pa + h# 1d spacec@  ;
: sabre-config-c! ( b offset -- )  sabre-config-pa + h# 1d spacec!  ;
: post-pio-decoder ( -- )
   " Sabre" post-suspect  post-iommu-de
   sabre-latency sabre-config-c@			( latency )
   h# 10 sabre-latency sabre-config-c!
   d# 16 0 do
      pio-data-patterns i pio-pattern  iommu-data-diag i 8 * + sabre-x!
      pio-tag-patterns i pio-pattern  iommu-tag-diag i 8 * + sabre-x!
   loop
   sabre-latency sabre-config-c@ h# 10 " PCI Latency Timer" post-compare
   d# 16 0 do
      iommu-data-diag i 8 * + sabre-x@ iommu-data-mask and
      pio-data-patterns i pio-pattern  " IOMMU RAM decode" post-compare
   loop
   d# 16 0 do
      iommu-tag-diag i 8 * + sabre-x@ iommu-tag-mask and
      pio-tag-patterns i pio-pattern  " IOMMU CAM decode" post-compare
   loop
   sabre-latency sabre-config-c!  post-iommu-clear
;

\ The byte enables: an EBus word (the NVRAM's first scratch word, saved
\ and put back) read as bytes and halfwords after a word write, and read
\ as a word after byte and halfword writes.
nvram-pa post-nv-base + constant post-be-pa
: pbe-l@ ( -- l )  post-be-pa bypass-asi spacel@  ;
: pbe-l! ( l -- )  post-be-pa bypass-asi spacel!  ;
: pbe-w@ ( offset -- w )  post-be-pa + bypass-asi spacew@  ;
: pbe-w! ( w offset -- )  post-be-pa + bypass-asi spacew!  ;
: pbe-c@ ( offset -- b )  post-be-pa + bypass-asi spacec@  ;
: pbe-c! ( b offset -- )  post-be-pa + bypass-asi spacec!  ;
h# 0123.4567 constant pbe-pattern
: pbe-reset ( -- )
   pbe-pattern pbe-l!  pbe-l@ pbe-pattern " Word write/read" post-compare
;
: post-byte-enable ( -- )
   " Sabre" post-suspect  pbe-l@				( saved )
   pbe-reset
   4 0 do  i pbe-c@  pbe-pattern 3 i - 8 * rshift h# ff and  " Byte read" post-compare  loop
   pbe-reset
   0 pbe-w@ h# 0123 " Halfword read" post-compare
   2 pbe-w@ h# 4567 " Halfword read" post-compare
   pbe-reset
   h# fe 0 pbe-c!  pbe-l@ h# fe23.4567 " Byte 0 write" post-compare
   h# dc 1 pbe-c!  pbe-l@ h# fedc.4567 " Byte 1 write" post-compare
   h# ba 2 pbe-c!  pbe-l@ h# fedc.ba67 " Byte 2 write" post-compare
   h# 98 3 pbe-c!  pbe-l@ h# fedc.ba98 " Byte 3 write" post-compare
   pbe-reset
   h# fedc 0 pbe-w!  pbe-l@ h# fedc.4567 " Halfword 0 write" post-compare
   h# ba98 2 pbe-w!  pbe-l@ h# fedc.ba98 " Halfword 1 write" post-compare
   pbe-l!
;

\ The interrupt maps: PCI's (c00-c38) and the on-board devices' (1000
\ on), their TID fields (30:26), each written (left invalid) and read
\ back, then put back.
\ (Each pattern's TID with the map's other fields, valid cleared.)
0 value imap-off
0 value imap-saved
: post-imap-test ( offset -- )
   dup to imap-off  sabre-x@ to imap-saved
   h# 5400.0000 h# 2800.0000 0 h# 7c00.0000  4 0 do	( p .. )
      imap-saved h# fc00.0000 invert and  over or  imap-off sabre-x!
      imap-off sabre-x@ h# 7c00.0000 and  swap " Interrupt Map" post-compare
   loop
   imap-saved imap-off sabre-x!
;
: post-imap ( -- )
   " Sabre" post-suspect
   8 0 do  h# c00 i 8 * + post-imap-test  loop
   d# 32 0 do  h# 1000 i 8 * + post-imap-test  loop
;
\ The PCI interrupts' clear registers (1400 on): each one whose map is
\ invalid and whose state is idle set received (1) then pending (3) and
\ back to idle (0), its state read in the interrupt state diagnostic
\ register (a800, two bits an interrupt).
: int-state ( ino -- state )  h# a800 sabre-x@ swap 2* rshift 3 and  ;
: post-iclr ( -- )
   " Sabre" post-suspect
   d# 32 0 do
      h# c00 i 2 rshift 8 * + sabre-x@ h# 8000.0000 and 0=
      i int-state 0= and if
         1 3 0  3 0 do
            dup h# 1400 j 8 * + sabre-x!
            j int-state swap " Interrupt state" post-compare
         loop
      then
   loop
;

\ ---------------------------------------------------------------------
\ Advanced: the processor's MMUs, its ASIs, the E-cache in use.

\ A page of the scratch memory mapped: as memory, read-only, in the
\ other byte order.
0 value post-va
0 value post-ro-va
0 value post-ie-va
: post-pages ( -- )
   post-scratch h# 21 post-map to post-va
   post-scratch h# 20 post-map to post-ro-va
   post-scratch h# 121 post-map to post-ie-va
;
: post-pages-free ( -- )  post-va post-unmap  post-ro-va post-unmap  post-ie-va post-unmap  ;

\ An access with Tag Access cleared first: Tag Access after it (the
\ page and context on a miss). The D-MMU's a load, the I-MMU's a call of
\ code there.
code post-dtag-after ( adr -- tag-access )
   sc4			rdpstate
   %g0 h# 14		wrpstate
   %g0 h# 30  scr	add
   %g0 %g0 scr h# 58	stxa
   #Sync  membar
   tos 0      sc1	ldx
   %g0 scr h# 58  tos	ldxa
   sc4 0		wrpstate
c;
code post-itag-after ( adr -- tag-access )
   sc4			rdpstate
   %g0 h# 14		wrpstate
   %g0 h# 30  scr	add
   %g0 %g0 scr h# 50	stxa
   #Sync  membar
   tos %g0    %o7	jmpl
   nop
   %g0 scr h# 50  tos	ldxa
   sc4 0		wrpstate
c;
: post-dmmu-hit-miss ( -- )
   " DMMU" post-suspect
   post-va post-dtag-after  post-va " DMMU miss" post-compare
   post-va post-dtag-after  0 " DMMU hit" post-compare
;
\ A routine (retl; nop) in a page not yet run.
0 value post-code
: post-immu-hit-miss ( -- )
   " IMMU" post-suspect
   pagesize 2* alloc-mem dup >r  pagesize + pagesize 1- invert and to post-code
   h# 81c3.e008 post-code instruction!  h# 0100.0000 post-code la1+ instruction!
   post-code post-itag-after  post-code " IMMU miss" post-compare
   post-code post-itag-after  0 " IMMU hit" post-compare
   r> pagesize 2* free-mem
;
\ The other byte order: the page mapped with IE, and the little-endian
\ ASI (88).
: post-little ( -- )
   " DMMU" post-suspect
   h# 0102.0304.0506.0708 post-va x!
   post-ie-va x@ h# 0807.0605.0403.0201 " Little endian page" post-compare
   post-va h# 88 spacex@ h# 0807.0605.0403.0201 " Little endian ASI" post-compare
   post-ie-va h# 88 spacex@ h# 0102.0304.0506.0708 " Little endian page and ASI" post-compare
;
\ The same doubleword through the primary, nucleus and physical ASIs,
\ in each size; and stored through each.
: post-iu-asi ( -- )
   " CPU (Basic)" post-suspect
   h# 0123.4567.89ab.cdef post-va x!
   post-va h# 80 spacex@  h# 0123.4567.89ab.cdef " ASI 80" post-compare
   post-va h# 04 spacex@  h# 0123.4567.89ab.cdef " ASI 04" post-compare
   post-scratch h# 14 spacex@  h# 0123.4567.89ab.cdef " ASI 14" post-compare
   post-va 4 + h# 80 spacel@  h# 89ab.cdef " ASI 80 word" post-compare
   post-va 2 + h# 80 spacew@  h# 4567 " ASI 80 halfword" post-compare
   post-va 7 + h# 80 spacec@  h# ef " ASI 80 byte" post-compare
   post-scratch 4 + h# 14 spacel@  h# 89ab.cdef " ASI 14 word" post-compare
   post-va 4 + h# 88 spacel@  h# efcd.ab89 " ASI 88 word" post-compare
   post-va 2 + h# 88 spacew@  h# 6745 " ASI 88 halfword" post-compare
   h# fedc.ba98.7654.3210 post-va h# 80 spacex!
   post-va x@  h# fedc.ba98.7654.3210 " ASI 80 store" post-compare
   h# 1111.2222.3333.4444 post-scratch h# 14 spacex!
   post-va x@  h# 1111.2222.3333.4444 " ASI 14 store" post-compare
   h# 0123.4567.89ab.cdef post-va h# 88 spacex!
   post-va x@  h# efcd.ab89.6745.2301 " ASI 88 store" post-compare
;
\ The FPU's loads and stores through the ASIs: a double through the
\ primary, little-endian and physical ASIs, to and from %f0.
code post-fasi ( src asi dst asi' -- )
   fpu-on
   sc4			rdasi
   sp 0 /n*   sc1	nget		\ dst
   sp 1 /n*   sc2	nget		\ asi
   sp 2 /n*   sc3	nget		\ src
   sc2 0		wrasi
   sc3 0 %asi  %f0	lddfa
   #Sync  membar
   tos 0		wrasi
   %f0 sc1 0 %asi	stdfa
   #Sync  membar
   sc4 0		wrasi
   fpu-off
   sp 3 /n*   sp	add
   sp         tos	pop
c;
: post-fpu-asi ( -- )
   " FPU" post-suspect
   h# 0123.4567.89ab.cdef post-va x!
   post-va h# 80  post-buf h# 80 post-fasi
   post-buf x@  h# 0123.4567.89ab.cdef " FPU ASI 80" post-compare
   post-va h# 88  post-buf h# 80 post-fasi
   post-buf x@  h# efcd.ab89.6745.2301 " FPU ASI 88" post-compare
   post-scratch h# 14  post-buf h# 80 post-fasi
   post-buf x@  h# 0123.4567.89ab.cdef " FPU ASI 14" post-compare
   post-buf h# 80  post-va 8 + h# 88 post-fasi
   post-va 8 + x@  h# efcd.ab89.6745.2301 " FPU ASI 88 store" post-compare
;
\ Four doublewords a cache size apart (one E-cache line), written
\ through the E-cache in turn and read back, for lines across the
\ cache.
: post-ec-thrash ( -- )
   " Ecache" post-suspect
   post-ec-size 4 * /post-scratch u> if  exit  then
   post-ec-size 0 do
      4 0 do  i j + post-index  post-scratch i post-ec-size * + j +  h# 14 spacex!  loop
      4 0 do
         post-scratch i post-ec-size * + j + h# 14 spacex@  i j + post-index
         post-scratch i post-ec-size * + j +  " Ecache thrash mismatch" post-compare-at
      loop
      post-failed? if  leave  then
   h# 1.0040 +loop
;

\ ---------------------------------------------------------------------
\ Errors the processor reports (a handler at the trap taken).

\ A load through an ASI the processor has none of (data_access_exception,
\ 30); one from a PCI address no device answers (data_access_error, 32,
\ the processor's AFSR then TO or BERR). (PCI memory at c000.0000 is
\ outside both bridges' windows as the probe assigns them, so nothing
\ answers on the bus module's own bus: a master abort.)
h# 1ff.c000.0000 constant pci-nothing
: post-data-access ( -- )
   " CPU (Basic)" post-suspect
   post-va to post-adr  h# 31 to post-asi
   ['] (ldx) h# 30 " Data access exception not taken" post-expect-trap
   dmmu-sfsr-clear
   cpu-afsr-clear  pci-nothing to post-adr  bypass-asi to post-asi
   ['] (ldx) h# 32 " Data access error not taken" post-expect-trap
   cpu-afsr@ h# 0c00.0000 and 0<> true " AFSR TO/BERR" post-compare
   cpu-afsr-clear
;
\ Loads and stores not on their size's boundary (mem_address_not_aligned,
\ 34).
: post-align-one ( xt offset -- )
   post-va + to post-adr  h# 80 to post-asi
   h# 34 " Address alignment trap not taken" post-expect-trap
;
: post-align ( -- )
   " CPU (Basic)" post-suspect
   8 1 do  ['] (ldx) i post-align-one  ['] (stx) i post-align-one  loop
   4 1 do  ['] (ldl) i post-align-one  ['] (stl) i post-align-one  loop
   ['] (ldw) 1 post-align-one  ['] (stw) 1 post-align-one
   dmmu-sfsr-clear
;
\ A load as if by a user (ASI 10) from a privileged page
\ (data_access_exception, 30; the SFSR's privilege violation bit, 7).
: post-priv ( -- )
   " DMMU" post-suspect
   post-va to post-adr  h# 10 to post-asi
   ['] (ldx) h# 30 " Privilege violation not trapped" post-expect-trap
   h# 18 h# 58 spacex@ h# 80 and  h# 80 " SFSR privilege violation" post-compare
   dmmu-sfsr-clear
;
\ A store to a read-only page (fast_data_access_protection, 6c): Tag
\ Access then the page.
: post-protect ( -- )
   " DMMU" post-suspect
   post-ro-va to post-adr  h# 80 to post-asi
   ['] (stx) h# 6c " Write protection not trapped" post-expect-trap
   h# 30 h# 58 spacex@ pagesize 1- invert and  post-ro-va " DMMU Tag Access" post-compare
   dmmu-sfsr-clear
;

\ ---------------------------------------------------------------------
\ The RIO ("Cheerio"): its EBus and Ethernet functions' configuration
\ headers (device 1 on pci@1,1's bus).
0 value cheerio-bus
: post-cheerio-bus ( -- )
   0 to cheerio-bus
   " /pci@1f,0/pci@1,1" find-package if
      " bus-range" rot get-package-property 0= if  decode-int to cheerio-bus 2drop  then
   then
;
: post-cheerio ( fn id class msg$ -- )
   " Cheerio" post-suspect  post-cheerio-bus
   2>r >r >r  cheerio-bus 1 rot				( bus 1 fn ) ( r: msg$ class id )
   3dup 0 config-l@ r> 2r@ post-compare
   8 config-l@ 8 rshift r> 2r> post-compare
;
: post-cheerio-ebus ( -- )  0 h# 1000.108e h# 6.8000 " EBus Config Space" post-cheerio  ;
: post-cheerio-enet ( -- )  1 h# 1001.108e h# 2.0000 " Ethernet Config Space" post-cheerio  ;

\ A PIO read no PCI device answers: data_access_error (32), the Sabre's
\ PCI status then Received Master Abort (bit 13, cleared by writing 1).
: pbm-status@ ( -- w )  4 sabre-config-l@ d# 16 rshift  ;
: pbm-status-clear ( -- )
   h# 2000.0000 4 sabre-config-pa + h# 1d spacel@ h# ffff and or
   4 sabre-config-pa + h# 1d spacel!
;
: post-master-abort ( -- )
   " Sabre" post-suspect
   cpu-afsr-clear  pci-nothing to post-adr  bypass-asi to post-asi
   ['] (ldl) h# 32 " PIO read master abort not trapped" post-expect-trap
   pbm-status@ h# 2000 and  h# 2000 " PCI Received Master Abort" post-compare
   pbm-status-clear  cpu-afsr-clear
;

\ ---------------------------------------------------------------------
\ The run.

: .post-cpu ( -- )
   push-decimal
   ." CPU: UltraSPARC-IIi (Clock Frequency: "
   cpu-clock-frequency d# 1000000 / (.d) type
   ." MHz, Ecache Size: " ecache-size d# 1024 / (.d) type ." KB)" cr
   pop-base
;

: .2d ( n -- )  push-decimal  <# u# u# u#> type  pop-base  ;
: post-clock$ ( -- path$ )  " /pci@1f,0/pci@1,1/ebus@1/eeprom@14,0"  ;
: .post-time ( -- )
   post-clock$ open-dev ?dup 0= if  exit  then  >r
   " get-time" r@ $call-method			( sec min hour day month year )
   r> close-dev
   ." Time Stamp [hour:min:sec] "
   >r >r >r  .2d ascii : emit  .2d ascii : emit  .2d
   ."   [month/date year] "  r> r> .2d ascii / emit  .2d space
   r> push-decimal (.d) type pop-base cr
;

: post-groups ( -- )
   " NVRAM" post-group
      ['] post-battery " NVRAM Battery Detect" post-test
      ['] post-nv-addr " NVRAM Scratch Addr" post-test
      ['] post-nv-data " NVRAM Scratch Data" post-test
   " DMMU TLB Tags" post-group
      ['] post-dtlb-tags " DMMU TLB Tag Access" post-test
   " DMMU TLB RAM" post-group
      ['] post-dtlb-data " DMMU TLB RAM Access" post-test
   " Probe Ecache" post-group
      ['] post-probe-ec " Probe Ecache" post-step
   " Ecache Tests" post-group
      ['] post-ec-ram-addr " Ecache RAM Addr" post-test
      ['] post-ec-tag-addr " Ecache Tag Addr" post-test
      ['] post-ec-ram " Ecache RAM" post-max-test
      ['] post-ec-tag " Ecache Tag" post-max-test
   " All CPU Basic Tests" post-group
      ['] post-v9 " V9 Instruction" post-test
      ['] post-tick-test " CPU Tick and Tick Compare Reg" post-max-test
      ['] post-soft-trap " CPU Soft Trap" post-test
      ['] post-softint-test " CPU Softint Reg and Int" post-test
   " All Basic MMU Tests" post-group
      ['] post-pctx " DMMU Primary Context Reg" post-test
      ['] post-sctx " DMMU Secondary Context Reg" post-test
      ['] post-dtsb " DMMU TSB Reg" post-test
      ['] post-dtag " DMMU Tag Access Reg" post-test
      ['] post-vawp " DMMU VA Watchpoint Reg" post-max-test
      ['] post-pawp " DMMU PA Watchpoint Reg" post-max-test
      ['] post-itsb " IMMU TSB Reg" post-test
      ['] post-itag " IMMU Tag Access Reg" post-test
   " All Basic Cache Tests" post-group
      ['] post-dcache-ram " Dcache RAM" post-test
      ['] post-dcache-tag " Dcache Tag" post-max-test
      ['] post-icache-ram " Icache RAM" post-test
      ['] post-icache-tag " Icache Tag" post-max-test
      ['] post-icache-next " Icache Next" post-max-test
      ['] post-icache-predecode " Icache Predecode" post-max-test
   " Memory Probe" post-group
      ['] post-banks " Probe Memory" post-step
   " Memory Init" post-group
      ['] post-ec-access " Ecache Access" post-test
   " All FPU Basic Tests" post-group
      ['] post-fpu-regs " FPU Regs" post-test
      ['] post-fpu-move " FPU Move Regs" post-test
      ['] post-fsr-test " FPU State Reg" post-max-test
      ['] post-fpu-func " FPU Functional" post-max-test
      ['] post-fpu-trap " FPU Trap" post-max-test
   ['] post-upa " UPA Data Bus Line" post-group-test
   " Memory Tests" post-group
      ['] post-mem-addr-ec " Memory Addr with Ecache" post-max-test
      ['] post-mem-addr-ecc " ECC Memory Addr" post-test
      ['] post-blk-addr " Block Memory Addr" post-max-test
      ['] post-blk " Block Memory" post-max-test
      ['] post-ecc-blk " ECC Blk Memory" post-max-test
   " All Basic Sabre MMU Tests" post-group
      ['] post-pio-decoder " PIO Decoder and BCT" post-max-test
      ['] post-byte-enable " PCI Byte Enable" post-max-test
      ['] post-imap " Interrupt Map (short) Reg" post-test
      ['] post-iclr " Interrupt Set/Clr Reg" post-test
      ['] post-iommu-regs " Sabre IOMMU Regs" post-test
      ['] post-iommu-ram " Sabre IOMMU RAM Address" post-test
      ['] post-iommu-cam " Sabre IOMMU CAM Address" post-test
      ['] post-iommu-compare " IOMMU TLB Compare" post-max-test
      ['] post-iommu-flush " IOMMU TLB Flush" post-max-test
      ['] post-pbm-config " PBMA PCI Config Space Regs" post-test
      ['] post-pbm-ctl " PBMA Control/Status Reg" post-test
      ['] post-pbm-diag " PBMA Diag Reg" post-test
      ['] post-sabre-io " Sabre IO Regs" post-test
   post-pages
   " All Advanced CPU Tests" post-group
      ['] post-dmmu-hit-miss " DMMU Hit/Miss" post-max-test
      ['] post-immu-hit-miss " IMMU Hit/Miss" post-max-test
      ['] post-little " DMMU Little Endian" post-max-test
      ['] post-iu-asi " IU ASI Access" post-test
      ['] post-fpu-asi " FPU ASI Access" post-test
      ['] post-ec-thrash " Ecache Thrash" post-max-test
   " All CPU Error Reporting Tests" post-group
      ['] post-data-access " CPU Data Access Trap" post-test
      ['] post-align " CPU Addr Align Trap" post-test
      ['] post-priv " DMMU Access Priv Page" post-test
      ['] post-protect " DMMU Write Protected Page" post-test
   post-pages-free
   " All Basic Cheerio Tests" post-group
      ['] post-cheerio-ebus " Cheerio Ebus PCI Config Space" post-test
      ['] post-cheerio-enet " Cheerio Ethernet PCI Config Space" post-test
   " All Sabre IOMMU Error Reporting Tests" post-group
      ['] post-master-abort " PIO Read, Master Abort" post-test
;

\ At a hardware power-on, with diag-switch? true and diag-level not off.
: post-wanted? ( -- flag )
   diag-switch? diag-level 0<> and  power-on-reset? and
;

headers
: post-ok? ( -- flag )  post-status 0=  ;

: system-tests ( -- )
   post-wanted? 0= if  exit  then
   true to post-ran?  0 to post-status  0 0 post-suspect
   diag-level h# 40 = to post-max?
   ecache-size to post-ec-size  post-find-scratch
   cr ." Executing Power On SelfTest" cr cr
   ." Ultra 5/10 POST, OpenBoot " obp-release ". cr cr
   .post-cpu cr
   post-groups
   cr cr ." Status of this POST run:" tab
   post-failed? if  ." FAIL"  else  ." PASS"  0 0 post-suspect  then  cr
   ." manfacturing mode=" mfg-mode 0= if  ." OFF"  else  ." ON"  then  cr
   .post-time  cr
   ." Power On Selftest Completed" cr
;

: show-post-results ( -- )
   post-ran? 0= if  ." Power On Selftest not run on last reset" cr exit  then
   ." Status 0=Pass, Non-Zero=Fail (%o0): " push-hex post-status . pop-base cr
   ." Message String               (%o1): " post-suspect$ /post-suspect$ type space cr
;
headerless
