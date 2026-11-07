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
\ is pressed.
: watch-net ( -- )
   map-resources
   my-args parse-devargs 2drop
   promiscuous to mac-mode
   bringup-link if
      ." Looking for Ethernet packets." cr
      ." '.' is a good packet.  'X' is a bad packet." cr
      ." Type any key to stop." cr
      begin
         h# 800 alloc-mem dup h# 800 receive if  ." ."  then  h# 800 free-mem
         key?
      until  key drop cr
   then
   net-off unmap-resources
;

: selftest ( -- 0 | error )
   map-resources  net-on 0=  net-off unmap-resources
;

: reset ( -- )  net-off  ;

headers
