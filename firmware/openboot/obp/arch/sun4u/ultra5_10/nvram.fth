\ Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
\ OpenBoot release it is part of (see LICENSE).
id: @(#)nvram.fth 1.0 26/11/07
purpose: The NVRAM and IDPROM: the M48T59's 8 KB
copyright: Copyright 2026 Andy Taylor

\ The M48T59 timekeeper (on the EBus, at its second window's start) holds
\ the configuration variables, the IDPROM (its 32 bytes at 1fd8) and the
\ clock (its registers at 1ff8). The configuration variable manager
\ (pkg/confvar) and the IDPROM code (arch/sun/idprom.fth) read them through
\ packages of device types "nvram" and "idprom", as sun4v's (dev/sun4v-
\ devices/legion-nvram) give them; these are here, in /packages, to be
\ there before the device tree is probed. The partitions: the fixed
\ variables, the whole of an Ultra 10's configuration area (0-1cd7,
\ fixed-u10.fth); past it, after what an Ultra 10's power-on selftest keeps there
\ (its scratch test and working data, 1cd8-1e0f, at a power-on in
\ diagnostic mode; zeros on a machine that has never run it), the
\ release's hashed store (c8, for nothing of the Ultra 10's) and the keys (100),
\ in what an Ultra 10 never writes. Each byte written to 00c-1cd7 keeps its
\ checksum (byte 0: 17 XOR those bytes).

headerless
h# 1ff.f100.0000 constant nvram-pa

: nvram-c@ ( offset -- b )  nvram-pa + bypass-asi spacec@  ;
: nvram-c! ( b offset -- )  nvram-pa + bypass-asi spacec!  ;

\ A byte stored, the checksum (byte 0) kept for it.
: u10-c! ( b offset -- )
   dup h# c h# 1cd8 within  if			( b offset )
      2dup nvram-c@ xor  0 nvram-c@ xor  0 nvram-c!	( b offset )
   then  nvram-c!
;

h#    0 constant nvfixed-base	h# 1cd8 constant /nvfixed-region
h# 1e10 constant nvhash-base	h#   c8 constant /nvhash-region
h# 1ed8 constant keystore-base	h#  100 constant /keystore-region
h#    0 constant nvraw-base	h# 1fd8 constant /nvraw-region
h# 1fd8 constant idprom-base	h#   20 constant /idprom-region

" /packages" find-device

\ (Opened by path, as the configuration variable manager opens its NVRAM,
\ a package's parents are opened too.)
headers
: open ( -- true )  true  ;
: close ( -- )  ;
headerless

new-device
   " nvram" device-name
   " nvram" device-type

   headerless
   0 instance value psize		\ Partition size
   0 instance value basepos		\ Absolute offset of this partition
   0 instance value filepos		\ Current position in partition

   : clip-range ( len -- len' )  psize filepos - min  ;
   : bump-pos   ( n -- )         filepos + psize min to filepos  ;
   : pos ( i -- offset )  basepos filepos + +  ;

   headers
   : open ( -- true )
      nvhash-base /nvhash-region
      my-args " fixed" $= if  2drop nvfixed-base /nvfixed-region  then
      my-args " keys"  $= if  2drop keystore-base /keystore-region  then
      my-args " raw"   $= if  2drop nvraw-base /nvraw-region  then
      to psize  to basepos
      0 to filepos
      true
   ;
   : close ( -- )  ;
   : sync  ( -- )  ;

   : seek ( lo hi -- fail? )
      drop  dup 0<  if  psize +  then		( offset )
      dup 0 psize within if  to filepos false  else  drop true  then
   ;

   : read ( adr len -- len' )
      clip-range  tuck 0 ?do  i pos nvram-c@ over i + c!  loop  drop
      dup bump-pos
   ;

   : write ( adr len -- len' )
      clip-range  tuck 0 ?do  dup i + c@ i pos u10-c!  loop  drop
      dup bump-pos
   ;

   : size ( -- n )  psize filepos - ;
finish-device

new-device
   " idprom" device-name
   " idprom" device-type

   headerless
   0 instance value filepos

   headers
   : open ( -- true )  0 to filepos true  ;
   : close ( -- )  ;
   : seek ( lo hi -- fail? )
      drop dup 0 /idprom-region within if  to filepos false  else  drop true  then
   ;
   : read ( adr len -- len' )
      /idprom-region filepos - min
      tuck 0 ?do  filepos i + idprom-base + nvram-c@ over i + c!  loop  drop
      dup filepos + to filepos
   ;
finish-device

device-end

\ The device-type hooks that note these packages take the current
\ instance's, and there is none as they are built here; nor would a
\ package noted now be where it is when OpenBoot runs. So what they noted
\ is undone, and the packages are noted as OpenBoot starts, the NVRAM
\ attached and the IDPROM read then (the release's own steps for these,
\ just before, having found neither).
0 to nvram-package
0 to idprom-phandle

stand-init: Attach the NVRAM and IDPROM
   " /packages/nvram" find-package if  to nvram-package  then
   " /packages/idprom" find-package if  to idprom-phandle  then
   install-nvram-u10
   ['] init-idprom catch drop
;

\ (#power-cycles is counted by the reset code, at a hardware power-on.)

\ After a watchdog reset or a RED state exception OpenBoot boots only
\ with watchdog-reboot? true; otherwise it stops at the ok prompt.
: error-reset? ( -- flag )  reset-kind @  dup 1 =  swap 4 =  or  ;
chain: don't-boot? ( flag -- flag' )
   error-reset? watchdog-reboot? 0= and or
;
