\ Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
\ OpenBoot release it is part of (see LICENSE).
id: @(#)memprobe.fth 1.0 26/11/07
purpose: Sizing the Ultra 5/10's memory: the /memory node
copyright: Copyright 2026 Andy Taylor

\ What arch/sun4v/memprobe.fth builds from the hypervisor's description:
\ the memory node's reg, and the physical memory free for clients. The
\ Ultra 5/10's memory controller decodes four banks, each in a 256 MiB
\ window from physical address 0 (a bank smaller than its window repeats
\ in it). Each window is tried: a bank is there if a pattern written reads
\ back, and its size is where a marker written further up first lands on
\ its base. OpenBoot's own 8 MiB (at hi-memory-base) stays reserved.

headerless
4		constant #banks
d# 256 1meg *	constant /bank-window

: mem-x@ ( pa -- x )  memory-asi spacex@  ;
: mem-x! ( x pa -- )  memory-asi spacex!  ;

h# 5a5a.a5a5.3c3c.c3c3 constant bank-pattern
: bank-present? ( base -- flag )
   dup >r  mem-x@				( saved )
   bank-pattern r@ mem-x!  r@ mem-x@ bank-pattern =	( saved flag )
   bank-pattern invert r@ mem-x!  r@ mem-x@ bank-pattern invert = and
   swap r> mem-x!				( flag )
;
\ The size: the smallest power of two from 16 MiB at which a marker
\ written lands on the base (or the whole window).
: bank-size ( base -- size )
   0 over mem-x!				( base )
   /bank-window  d# 16 1meg * begin		( base size try )
      2dup >  while				( base size try )
      dup 3 pick + >r  dup r> mem-x!		( base size try )
      2 pick mem-x@ 0<> if  nip dup  then	( base size' try )
      2*					( base size try' )
   repeat  drop nip				( size )
;

: (free-range) ( xdr,len adr size  -- xdr,len' )
   >r xlsplit swap >r encode-int encode+  r> encode-int encode+
   r> xlsplit swap >r encode-int encode+  r> encode-int encode+
;
" /memory" find-device

\ A bank's memory freed for clients, but for OpenBoot's 8 MiB (its image,
\ and the memory it has from the start, hi-memory-base on: see ramforth).
: obp-base ( -- pa )  hi-memory-base @ pageshift lshift  8meg round-down  ;
: free-pa-range ( adr len -- )
   ?dup if  obmem swap phys-adr,len>pages physavail free-memrange
   else  drop  then
;
: free-bank ( adr size -- )
   over + >r					( adr ) ( r: end )
   obp-base over r@ within if			( adr )
      obp-base over - free-pa-range		(  )
      obp-base 8meg +  r> over - free-pa-range	(  )
   else						( adr )
      r> over - free-pa-range			(  )
   then
;

headers
: probe-memory ( -- )
   0 0 encode-bytes					( xdr,len )
   #banks 0 do						( xdr,len )
      i /bank-window *  dup bank-present?  if		( xdr,len base )
         dup bank-size					( xdr,len base size )
         2dup + physmax 2dup @ u> if ! else 2drop then	( xdr,len base size )
         2dup free-bank					( xdr,len base size )
         (free-range)					( xdr',len' )
      else						( xdr,len base )
         drop						( xdr,len )
      then						( xdr,len )
   loop							( xdr,len )
   " reg" property
;
device-end

stand-init: Build memory nodes
   " probe-memory" memory-node @ $call-method
;
