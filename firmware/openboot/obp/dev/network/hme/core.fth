\ Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
\ OpenBoot release it is part of (see LICENSE).
id: @(#)core.fth 1.0 26/11/07
purpose: The Happy Meal: reset, rings, the link, sending and receiving
copyright: Copyright 2026 Andy Taylor

\ The DMA block (64 KB): the transmit ring (16 descriptors) at 0, the
\ receive ring (32) at 800 (rings are 2 KB aligned), the transmit buffer at
\ 1000, the 32 receive buffers from 2000. A descriptor is a flags word
\ (OWN 80000000: the device's; for transmit, start and end of frame
\ 40000000 and 20000000 and the length; for receive, the buffer's size
\ above bit 16, the frame's length there once received) and an address.

headerless

d# 16 constant #tx
d# 32 constant #rx
h# 600 constant /buf

h# 8000.0000 constant own
h# 4000.0000 constant tx-sop
h# 2000.0000 constant tx-eop

0 instance value tx-next
0 instance value rx-next

: tx-ring ( -- va )  cpu-dma-base  ;
: rx-ring ( -- va )  cpu-dma-base h# 800 +  ;
: tx-buf ( -- va )  cpu-dma-base h# 1000 +  ;
: rx-buf ( n -- va )  /buf *  cpu-dma-base h# 2000 + +  ;
: txd ( n -- adr )  8 * tx-ring +  ;
: rxd ( n -- adr )  8 * rx-ring +  ;

: give-rxd ( n -- )  own /buf d# 16 lshift or  swap rxd le-l!  ;

: init-rings ( -- )
   #tx 0 do  0 i txd le-l!  0 i txd 4 + le-l!  loop
   #rx 0 do  i rx-buf cpu>io  i rxd 4 + le-l!  i give-rxd  loop
   0 to tx-next  0 to rx-next
;

\ The station address: the system's, from the IDPROM.
6 instance buffer: mac-buf
: system-mac ( -- adr len )
   " /" find-package if
      " idprom" rot get-package-property 0= if	( xdr,len )
         drop 2+ mac-buf 6 move  mac-buf 6 exit
      then
   then
   mac-buf 6 erase  mac-buf 6
;

: set-station-address ( -- )
   system-mac drop					( adr )
   dup c@ 8 lshift over 1+ c@ or bmac-madd0 reg!
   dup 2+ c@ 8 lshift over 3 + c@ or bmac-madd1 reg!
   dup 4 + c@ 8 lshift swap 5 + c@ or bmac-madd2 reg!
;

: timed-out? ( when -- flag )  get-msecs - 0<  ;

\ A global reset of both DMA channels; done when the bits read 0.
: reset-hme ( -- ok? )
   3 seb-reset reg!
   get-msecs d# 100 +  begin
      seb-reset reg@ 3 and 0= if  drop true exit  then
      dup timed-out?
   until  drop false
;

: init-mac ( -- )
   0 bmac-tx-reset reg!  0 bmac-rx-reset reg!
   set-station-address
   0 bmac-hash3 reg!  0 bmac-hash3 4 + reg!
   0 bmac-hash3 8 + reg!  0 bmac-hash3 h# c + reg!
   h# 5f2 bmac-tx-max reg!  h# 5f2 bmac-rx-max reg!	\ 1518 + 4
   1 bmac-xif reg!					\ Output drivers on
;

: start-dma ( -- )
   -1 seb-imask reg!  h# ffff mif-imask reg!		\ Polled: no interrupts
   0 seb-cfg reg!
   tx-ring cpu>io etx-ring reg!  0 etx-rsize reg!	\ 16 descriptors
   rx-ring cpu>io erx-ring reg!
   h# 3ff etx-cfg reg!					\ On
   1 erx-cfg reg!					\ On: 32, no offset
;

: enable-mac ( -- )
   full-duplex-link? if  h# 201  else  1  then  bmac-tx-cfg reg!
   mac-mode promiscuous = if  h# 41  else  h# 201  then  bmac-rx-cfg reg!
;

: net-off ( -- )
   reg-base if
      0 bmac-tx-cfg reg!  0 bmac-rx-cfg reg!
      0 etx-cfg reg!  0 erx-cfg reg!
      reset-hme drop
   then
;

\ The link: the internal transceiver reset, then autonegotiation (or the
\ speed and duplex the device arguments force), waited for.
: phy-reset ( -- ok? )
   phycr.reset phy-cr!
   get-msecs d# 500 +  begin
      phy-cr@ phycr.reset and 0= if  drop true exit  then
      dup timed-out?
   until  drop false
;

: wait-link ( mask -- up? )
   get-msecs d# 5000 +  begin			( mask when )
      over phy-sr@ and  2 pick = if  2drop true exit  then
      dup timed-out?
   until  2drop false
;

: an-result ( -- speed duplex )
   phy-anlpar@ phy-anar@ and				( common )
   dup anlpar.100fdx and if  drop 100Mbps full-duplex exit  then
   dup anlpar.100hdx and if  drop 100Mbps half-duplex exit  then
   dup anlpar.10fdx  and if  drop 10Mbps  full-duplex exit  then
   drop 10Mbps half-duplex
;

: abilities ( -- anar )
   h# 1e0							( 10/100, half/full )
   user-speed 10Mbps = if  h# 60 and  then
   user-speed 100Mbps = if  h# 180 and  then
   user-duplex half-duplex = if  h# a0 and  then
   user-duplex full-duplex = if  h# 140 and  then
   anar.selector-field or
;

: forced-link ( -- up? )
   0  user-speed 100Mbps = if  phycr.speed-100 or  then
   user-duplex full-duplex = if  phycr.duplex or  then	( cr )
   dup phy-cr!
   physr.link-up wait-link				( cr up? )
   swap  dup phycr.speed-100 and if  100Mbps  else  10Mbps  then
   swap phycr.duplex and if  full-duplex  else  half-duplex  then
   set-chosen-speed&duplex
;

: setup-transceiver ( -- up? )
   phy-reset 0= if  false exit  then
   user-speed auto-speed =  user-duplex auto-duplex = and if
      abilities phy-anar!
      phycr.an-enable phycr.an-restart or phy-cr!
      physr.an-complete physr.link-up or wait-link	( up? )
      dup if  an-result set-chosen-speed&duplex  then
   else
      forced-link
   then
;

\ With the tpe-link-test? option false, the link is taken to be up
\ whatever the transceiver says (for hubs without link pulses).
: link-test? ( -- flag )
   " /options" find-package if
      " tpe-link-test?" rot get-package-property if  true  then
   else  true  then
;

: net-on ( -- link-up? )
   reset-hme drop  init-mac  init-rings  start-dma
   setup-transceiver  link-test? 0= or  dup if  enable-mac  then
;

\ Sending: one frame at a time, from the transmit buffer; done when the
\ device gives the descriptor back.
: transmit ( adr len -- #sent )
   d# 1518 min  tuck tx-buf swap move			( len )
   tx-buf cpu>io  tx-next txd 4 + le-l!
   dup d# 60 max  own or tx-sop or tx-eop or  tx-next txd le-l!
   1 etx-pending reg!
   get-msecs d# 2000 +  begin
      tx-next txd le-l@ own and 0= if
         drop  tx-next 1+ #tx mod to tx-next  exit
      then
      dup timed-out?
   until  2drop 0
   tx-next 1+ #tx mod to tx-next
;

\ Receiving: the next descriptor, once the device has given it back.
: receive ( adr len -- #rcvd )
   rx-next rxd le-l@  dup own and if  3drop 0 exit  then	( adr len flags )
   d# 16 rshift h# 3fff and  min				( adr len' )
   rx-next rx-buf -rot  dup >r  move  r>			( #rcvd )
   rx-next give-rxd
   rx-next 1+ #rx mod to rx-next
;
