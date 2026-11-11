\ Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
\ OpenBoot release it is part of (see LICENSE).
id: @(#)pkg.fth 1.0 26/11/07
purpose: The Happy Meal's methods
copyright: Copyright 2026 Andy Taylor

\ As the release's ophir driver's (dev/network/ophir/pkg.fth): the device
\ opened with the network boot's arguments (and the link's, parsed by
\ devargs.fth), the boot through obp-tftp.

headerless
0 instance value obp-tftp

: init-obp-tftp ( tftp-args$ -- okay? )
   " obp-tftp" find-package if
      open-package
   else
      ." Can't open OBP standard TFTP package" cr  2drop 0
   then
   dup to obp-tftp
;

: map-resources ( -- )  reg-base 0= if  map-regs map-buffers  then  ;
: unmap-resources ( -- )  reg-base if  unmap-buffers unmap-regs  then  ;

: bringup-link ( -- up? )
   net-on dup 0= if
      " Link down: check the network cable" diagnostic-mode? if type cr else 2drop then
   then
;

external

: mac-address ( -- adr len )  system-mac  ;

: close ( -- )
   obp-tftp ?dup if  close-package  0 to obp-tftp  then
   net-off unmap-resources
;

: open ( -- flag )
   map-resources
   my-args parse-devargs
   init-obp-tftp 0= if  close false exit  then
   bringup-link 0= if  close false exit  then
   publish-properties
   mac-address encode-bytes " mac-address" property
   true
;

: read ( adr len -- #rcvd | -2 )  receive ?dup 0= if  -2  then  ;
: write ( adr len -- #sent )  transmit  ;
: load ( adr -- size )  " load" obp-tftp $call-method  ;

\ watch-net: frames counted as they come (promiscuous), until a key
\ is pressed. A bad packet is one the MAC counts as an error and drops
\ (too long, misaligned, or with a bad CRC: its three receive error
\ counters, set back to zero as they are read), or one that overflowed its buffer.
headerless
: rx-errors ( -- n )
   0  bmac-crc-errs 4 + bmac-len-errs do  i reg@ h# ff and +  0 i reg!  4 +loop
;
external
: watch-net ( -- )
   map-resources
   my-args parse-devargs 2drop
   promiscuous to mac-mode
   bringup-link if
      rx-errors drop
      ." Looking for Ethernet packets." cr
      ." '.' is a good packet.  'X' is a bad packet." cr
      ." Type any key to stop." cr
      h# 800 alloc-mem					( buf )
      begin
         dup h# 800 receive if  rx-bad? if  ." X"  else  ." ."  then  then
         rx-errors 0 ?do  ." X"  loop
         key?
      until  key drop cr				( buf )
      h# 800 free-mem
   then
   net-off unmap-resources
;

\ The selftest: a frame to the station's own address through the MAC's
\ internal loopback (the output drivers off), compared when it comes
\ back; then the transceiver, reset and answering with its ID.
d# 64 constant /test-frame
: test-pattern ( adr -- )
   system-mac drop  over 6 move  system-mac drop  over 6 + 6 move	( adr )
   /test-frame d# 12 do  i over i + c!  loop  drop
;
: loopback-test ( -- ok? )
   reset-hme drop  init-mac  init-rings  start-dma
   2 bmac-xif reg!					\ Loopback, drivers off
   h# 201 bmac-tx-cfg reg!  1 bmac-rx-cfg reg!		\ Full duplex; own frames
   /test-frame alloc-mem  dup test-pattern		( frame )
   dup /test-frame transmit /test-frame = if		( frame )
      /test-frame alloc-mem  false			( frame buf ok? )
      get-msecs d# 200 +  begin			( frame buf ok? when )
         2 pick /test-frame receive ?dup if		( frame buf ok? when #rcvd )
            nip nip  /test-frame >= if		( frame buf )
               2dup /test-frame comp 0=		( frame buf ok? )
            else  false  then			( frame buf ok? )
            get-msecs				( frame buf ok? when )
         then
         dup timed-out?  2 pick or		( frame buf ok? when done? )
      until  drop					( frame buf ok? )
      swap /test-frame free-mem			( frame ok? )
   else  false  then				( frame ok? )
   swap /test-frame free-mem			( ok? )
   net-off
;
: transceiver-test ( -- ok? )
   phy-reset dup if  drop phy-id1@ dup 0<> swap h# ffff <> and  then
;

: selftest ( -- 0 | error )
   map-resources
   loopback-test  ." Internal loopback test -- "
   dup if  ." succeeded."  else  ." failed."  then  cr	( ok1? )
   transceiver-test  ." Transceiver check  -- "
   dup if  ." passed"  else  ." failed"  then  cr		( ok1? ok2? )
   and 0=  net-off unmap-resources
;

: reset ( -- )  net-off  ;

headers
