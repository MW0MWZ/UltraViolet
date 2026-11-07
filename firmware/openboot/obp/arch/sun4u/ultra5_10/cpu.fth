\ Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
\ OpenBoot release it is part of (see LICENSE).
id: @(#)cpu.fth 1.0 26/11/07
purpose: The processor: its clock, and its node
copyright: Copyright 2026 Andy Taylor

\ As arch/sun4v/cpu.fth makes sun4v's processor nodes from the machine
\ description, the UltraSPARC IIi's, from the processor itself: its
\ version register, its module ID, the E-cache's size and the clock (as
\ machine-init.fth measures it).

headerless

\ The E-cache's size: its data array, seen through the diagnostic ASIs
\ (76 to write, 7e to read, VA bit 39 selecting the data), repeats every
\ size bytes (IIi manual, 256 KB to 2 MB). Two doublewords are borrowed,
\ and given back, with interrupts held off.
h# 80.0000.0000 constant ec-data
: ec@ ( index -- x )  ec-data or h# 7e spacex@  ;
: ec! ( x index -- )  ec-data or h# 76 spacex!  ;

: ecache-size ( -- n )
   lock[
   0 ec@ >r						( ) ( r: x0 )
   d# 256 1024 *  begin  dup 2meg <  while		( size )
      dup ec@ >r					( size ) ( r: x0 xs )
      0 0 ec!  dup dup ec!  0 ec@ 0<>			( size wrapped? )
      r> 2 pick ec!					( size wrapped? )
      if  r> 0 ec!  ]unlock  exit  then			( size )
      2*						( size' )
   repeat						( 2meg )
   r> 0 ec!
   ]unlock
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

stand-init: Making cpu node
   0 0 " 0,0" " /" begin-package
      make-cpu-props
   end-package
;
headers
