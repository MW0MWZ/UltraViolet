\ Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
\ OpenBoot release it is part of (see LICENSE).
id: @(#)memprobe.fth 1.2 26/11/11
purpose: Sizing the Ultra 5/10's memory: the /memory node
copyright: Copyright 2026 Andy Taylor

\ What arch/sun4v/memprobe.fth builds from the hypervisor's description:
\ the memory node's reg, and the physical memory free for clients; sized as an
\ Ultra 10 sizes it. The board has two pairs of DIMM
\ sockets, the memory controller's pairs 0 and 2, at 0 and 256 MiB (pair n
\ at n x 128 MiB), each DIMM's second side (stack) 512 MiB above; the two
\ DIMMs of a pair are the two halves of each 16-byte line (+0 and +8).
\
\ The reset code chose the column mode (reset.fth): 11-bit if it gives the
\ most memory. For each pair and each of its DIMMs, as an Ultra 10 does:
\  - present: three patterns written at +0, +8 MiB and +16 MiB, the first
\    read back;
\  - its size: markers written at +0, +128, +64 and +32 MiB, the one left
\    at +0 being the size (where the rest landed on it): up to 128 MiB a
\    side; twice that with a second side present;
\  - in 11-bit mode its columns: a DIMM whose DRAMs have 10-bit columns
\    ignores column bit 10 (PA[14]), so a marker 16 KiB up lands on +0:
\    then its size is halved. In 10-bit mode a DIMM sized at 128 MiB (an
\    11-bit DIMM, half of it seen) is halved;
\  - the pair's size: twice the smaller DIMM's (per side), both sides if
\    both DIMMs have two.
\ In 11-bit mode a pair with 10-bit DIMMs ("Unused 10 bit Column DIMMS -
\ See User's Manual!" in diagnostic mode) is not used; a pair not used is
\ switched off in Mem_Control0 (its DIMMPairPresent bit) - except, as on
\ an Ultra 10, in 11-bit mode on an UltraSPARC-IIi of version 12.10.
\
\ The Ultra 10's quirks followed: a DIMM's second side is only looked for, its size
\ taken as the first side's; the reg property lists pair 0 (both sides),
\ then pair 2. Memory reads
\ after the writes are through the E-cache (2 MiB, direct-mapped), so
\ every write is read back and a line 2 MiB on read to push it out, as
\ an Ultra 10's are.

headerless
d# 32 1meg *	constant 32meg
h# 20.0000	constant /ecache-flush	\ (the E-cache's size)
h# 1fe.0000.f010 constant mc0-pa	\ Mem_Control0

: mem-x@ ( pa -- x )  memory-asi spacex@  ;
: mem-x! ( x pa -- )  tuck memory-asi spacex!  mem-x@ drop  ;
: mc0@ ( -- x )  mc0-pa bypass-asi spacex@  ;
: mc0! ( x -- )  mc0-pa bypass-asi spacex!  ;
: col11? ( -- flag )  mc0@ h# 1000 and 0<>  ;
: flush@ ( pa -- )  /ecache-flush + mem-x@ drop  ;

h# aaaa.aaaa.5555.5555 constant pattern1
h# 5555.5555.aaaa.aaaa constant pattern2
h# a5a5.a5a5.a5a5.a5a5 constant pattern3

\ A DIMM at pa: the patterns read back. (The memory put back as found.)
: dimm-present? ( pa -- flag )
   dup mem-x@ >r  dup 8meg + mem-x@ >r  dup 16meg + mem-x@ >r	( pa )
   >r  pattern1 r@ mem-x!  pattern2 r@ 8meg + mem-x!		( )
   pattern3 r@ 16meg + mem-x!  r@ mem-x@ pattern1 =		( flag )
   r> 2r> rot							( flag x8 x16 pa )
   tuck 16meg + mem-x!  tuck 8meg + mem-x!  r> swap mem-x!	( flag )
;
\ Its size (one side): 0 if absent.
: dimm-side ( pa -- size )
   dup dimm-present? 0=  if  drop 0 exit  then		( pa )
   dup mem-x@ >r  dup 128meg + mem-x@ >r		( pa )
   dup 64meg + mem-x@ >r  dup 32meg + mem-x@ >r	( pa )
   128meg over mem-x!  dup flush@			( pa )
   64meg over 128meg + mem-x!  dup flush@  dup 128meg + flush@
   32meg over 64meg + mem-x!  dup flush@  dup 64meg + flush@
   16meg over 32meg + mem-x!  dup flush@  dup 32meg + flush@
   dup mem-x@  swap					( size pa )
   r> over 32meg + mem-x!  r> over 64meg + mem-x!	( size pa )
   r> over 128meg + mem-x!  r> swap mem-x!		( size )
;
\ A marker off bytes up landing on pa.
: col-alias? ( pa off -- flag )
   over mem-x@ >r  2dup + mem-x@ >r			( pa off )
   pattern1 2 pick mem-x!  over flush@			( pa off )
   over + pattern2 over mem-x!  dup flush@		( pa pa+off )
   over flush@  over mem-x@ pattern1 <>		( pa pa+off flag )
   r> rot mem-x!  r> rot mem-x!				( flag )
;
: dimm-11bit? ( pa -- flag )  d# 14 1 swap lshift col-alias? 0=  ;

\ A DIMM: its size, its column width (in 11-bit mode), and whether it has
\ a second side (at stack-pa).
: dimm ( pa stack-pa -- stacked? 11bit? size )
   over dimm-side  swap dimm-present?  if  2*  true  else  false  then
   swap rot						( stacked? size pa )
   col11?  if						( stacked? size pa )
      dimm-11bit?  swap over 0=  if  2/  then		( stacked? 11bit? size )
   else							( stacked? size pa )
      drop  false swap  dup 128meg =  if  2/  then	( stacked? false size )
   then
;

\ What each pair came to (for the report in diagnostic mode): its DIMMs'
\ sizes as shown, whether they were unused 10-bit DIMMs, its size.
2 constant #pairs
#pairs /n* buffer: pair-dimm0
#pairs /n* buffer: pair-dimm1
#pairs /n* buffer: pair-unused
#pairs /n* buffer: pair-sizes
: pair! ( n index adr -- )  swap na+ !  ;

\ Pair n (0 or 2): its base and size, and whether it has two sides.
: probe-pair ( n -- false | stacked? base size true )
   >r							( ) ( r: n )
   r@ d# 27 lshift  dup h# 2000.0000 or  dimm		( s0 c0 z0 )
   dup r@ 2/ pair-dimm0 pair!				( s0 c0 z0 )
   r@ d# 27 lshift 8 +  dup h# 2000.0000 or  dimm	( s0 c0 z0 s1 c1 z1 )
   dup r@ 2/ pair-dimm1 pair!				( s0 c0 z0 s1 c1 z1 )
   0 r@ 2/ pair-unused pair!
   col11?  if						( s0 c0 z0 s1 c1 z1 )
      swap 4 roll and 0=  if				( s0 z0 s1 z1 )
         dup 0<>  3 pick 0<> and  if
            true r@ 2/ pair-unused pair!
            drop 0  rot drop 0 -rot			( s0 0 s1 0 )
         then
      then
   else							( s0 c0 z0 s1 c1 z1 )
      swap drop  3 roll drop				( s0 z0 s1 z1 )
   then
   over  if  2/  then					( s0 z0 s1 z1' )
   2swap over  if  2/  then				( s1 z1' s0 z0' )
   rot min  -rot and  if  2*  true  else  false  then	( z stacked? )
   swap 2*  dup r@ 2/ pair-sizes pair!			( stacked? size )
   ?dup  if						( stacked? size )
      r> d# 27 lshift  swap  true			( stacked? base size true )
   else							( stacked? )
      drop							( )
      ver@ d# 24 rshift h# ff.ffff and  h# 1210 =  col11? and  0=  if
         mc0@  1 r@ 8 + lshift invert and  mc0!		\ switched off
      then
      r> drop  false
   then
;

: (free-range) ( xdr,len adr size  -- xdr,len' )
   >r xlsplit swap >r encode-int encode+  r> encode-int encode+
   r> xlsplit swap >r encode-int encode+  r> encode-int encode+
;
" /memory" find-device

\ A pair's memory freed for clients, but for OpenBoot's 1 MiB at the top
\ (its image, and the memory it has from the start, hi-memory-base on: see
\ ramforth), as an Ultra 10 keeps it.
: obp-base ( -- pa )  hi-memory-base @ pageshift lshift  /obp-memory round-down  ;
: free-pa-range ( adr len -- )
   ?dup if  obmem swap phys-adr,len>pages physavail free-memrange
   else  drop  then
;
: free-bank ( adr size -- )
   over + >r					( adr ) ( r: end )
   obp-base over r@ within if			( adr )
      obp-base over - free-pa-range		(  )
      obp-base /obp-memory +  r> over - free-pa-range	(  )
   else						( adr )
      r> over - free-pa-range			(  )
   then
;
: add-bank ( xdr,len adr size -- xdr,len' )
   2dup + physmax 2dup @ u> if ! else 2drop then
   2dup free-bank  (free-range)
;

\ OpenBoot's 1 MiB is taken from the top as it starts (its dictionary
\ grows a page at a time from there); a compressed drop-in's buffers
\ (pkg/decompressor/decompress.fth), claimed and released while its driver
\ runs and grows the dictionary, are taken from the bottom, so that
\ releasing them leaves no hole between used pages.
0 value low-want	\ Pages wanted
0 value low-page	\ The lowest that fits, or -1
: obp-pages ( -- first-page end-page )
   obp-base bytes>pages  dup /obp-memory bytes>pages +
;
: lower-fit? ( node -- false )
   node-range over +				( page# end )
   obp-pages nip umin				( page# end' )
   swap obp-pages drop max			( end' start )
   tuck low-want + u>=  if			( start )
      low-page umin to low-page
   else
      drop
   then  false
;
: (obp-low-claim) ( size -- pa.lo pa.hi true | false )
   mmu-pagesize round-up  dup bytes>pages to low-want	( size )
   -1 to low-page
   physavail ['] lower-fit? find-node 2drop		( size )
   low-page -1 =  if  drop false exit  then		( size )
   low-page pages>bytes 0 rot  3dup claim-physical  drop  true
;
also decompressor  ' (obp-low-claim) to decomp-phys-claim  previous

headers
: ecc-off ( -- )  mc0@ h# 1000.0000 invert and mc0!  ;
: ecc-on ( -- )  mc0@ h# 1000.0000 or mc0!  ;

: probe-memory ( -- )
   ecc-off
   0 0 encode-bytes					( xdr,len )
   #pairs 0 do						( xdr,len )
      i 2* probe-pair  if				( xdr,len stacked? base size )
         rot  if					( xdr,len base size )
            2/  2dup 2>r  add-bank			( xdr,len' )
            2r> swap h# 2000.0000 or  swap		( xdr,len' base' size )
         then						( xdr,len base size )
         add-bank					( xdr,len' )
      then						( xdr,len )
   loop							( xdr,len )
   " reg" property
   ecc-on
;
device-end

\ The memory controller tuned to the processor's clock (as measured,
\ machine-init.fth) and the pairs in use, as an Ultra 10 tunes it: the
\ refresh interval (Mem_Control0<7:0>, in 32 clocks) for a refresh every
\ 15.6 microseconds a pair, the clock in Hz / 2048000 / pairs (the IIi
\ manual's formula); the timings (Mem_Control1) for 50 ns DRAMs, or for 60
\ ns ones with both pairs in use, from a table by the clock in MHz (the
\ Ultra 10's values, some the manual's table 18-17, and its frequency
\ bands). Refresh is turned off (Mem_Control0 written 0) first.
: pairs-in-use ( -- n )  0  #pairs 0 do  pair-sizes i na+ @ 0<> -  loop  ;
create mc1-50ns
   d# 270 , h# 0626.168a ,   d# 379 , h# 0645.9acb ,   d# 400 , h# 4c48.22d3 ,
   d# 440 , h# 4c68.2792 ,   d# 450 , h# 4e69.a792 ,   -1 ,     h# 4c4e.bf9d ,
create mc1-60ns
   d# 270 , h# 0626.168a ,   d# 333 , h# 0645.9acb ,   d# 379 , h# 0c4a.ab14 ,
   d# 400 , h# 4b4b.2794 ,   d# 440 , h# 524f.39da ,   d# 450 , h# 544e.b9da ,
   -1 ,     h# 556e.35dd ,
: mc1-value ( mhz table -- x )
   begin  2dup @ u>  while  2 na+  repeat  nip na1+ @
;
h# 1fe.0000.f018 constant mc1-pa
: mc1! ( x -- )  mc1-pa bypass-asi spacex!  ;
: tune-memory-controller ( -- )
   pairs-in-use 1 max					( pairs )
   cpu-clock-frequency d# 2048000 / over / h# ff and	( pairs refresh )
   mc0@ h# ff invert and or				( pairs mc0 )
   0 mc0!  mc0!						( pairs )
   cpu-clock-frequency d# 1000000 /			( pairs mhz )
   swap 2 =  if  mc1-60ns  else  mc1-50ns  then  mc1-value mc1!
;

\ The DRAMs' speed the timings are for, in the banner as an Ultra 10 has
\ it: "256 MB (50 ns) memory installed" (sysconfig.fth's .memory-speed,
\ set in loadprom.fth).
: dram-ns ( -- ns )  pairs-in-use 2 =  if  d# 60  else  d# 50  then  ;
headers
: (.memory-speed) ( -- )  ."  (" dram-ns (.d) type ."  ns)"  ;
headerless

stand-init: Build memory nodes
   " probe-memory" memory-node @ $call-method
   tune-memory-controller
;
