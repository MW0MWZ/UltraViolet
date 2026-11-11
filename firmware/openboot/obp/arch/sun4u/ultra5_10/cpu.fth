\ Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
\ OpenBoot release it is part of (see LICENSE).
id: @(#)cpu.fth 1.1 26/11/11
purpose: The processor: its clock, and its node
copyright: Copyright 2026 Andy Taylor

\ As arch/sun4v/cpu.fth makes sun4v's processor nodes from the machine
\ description, the UltraSPARC IIi's, from the processor itself: its
\ version register, its module ID, the E-cache's size and the clock (as
\ machine-init.fth measures it).

headerless

\ The E-cache's size, as the reset code found it (its data array, through
\ the diagnostic ASIs, before the caches were on) and set the UPA
\ configuration's ELIM field for it (bits 35:33; IIi manual 18.5): 110
\ (or 111) 256 KB, 101 512 KB, 100 1 MB, 011 2 MB. Read back from there,
\ so the E-cache isn't touched while it is in use.
: ecache-size ( -- n )
   0 h# 4a spacex@ d# 33 rshift 7 and  6 min  3 max	( elim )
   6 swap -  d# 256 1024 *  swap lshift			( n )
;

: mid ( -- n )  0 h# 4a spacex@ d# 17 rshift h# 1f and  ;

: en+ ( xdr,len n -- xdr',len' )  encode-int encode+  ;

: make-cpu-props ( -- )
   " SUNW,UltraSPARC-IIi" device-name
   h# 1c0 mid 1 lshift or encode-int 0 en+ 0 en+ 8 en+ " reg" property
   " cpu" device-type				\ (with reg made: its hook reads it)
   mid " upa-portid" integer-property
   cpu-clock-frequency " clock-frequency" integer-property
   h# 4000 " icache-size" integer-property
   h# 20 " icache-line-size" integer-property
   2 " icache-associativity" integer-property
   d# 64 " #itlb-entries" integer-property
   h# 4000 " dcache-size" integer-property
   h# 20 " dcache-line-size" integer-property
   1 " dcache-associativity" integer-property
   d# 64 " #dtlb-entries" integer-property
   ecache-size " ecache-size" integer-property
   h# 40 " ecache-line-size" integer-property
   1 " ecache-associativity" integer-property
   9 " sparc-version" integer-property
   ver@ d# 48 rshift h# ffff and " manufacturer#" integer-property
   ver@ d# 32 rshift h# ffff and " implementation#" integer-property
   ver@ d# 24 rshift h# ff and " mask#" integer-property
;

: make-cpu-node ( -- )
   0 0 " 0,0" " /" begin-package
      make-cpu-props
   end-package
;
\ (Quietly: an Ultra 10 says nothing of it in diagnostic mode.)
stand-init: Making cpu node
   ['] make-cpu-node quietly
;
headers
