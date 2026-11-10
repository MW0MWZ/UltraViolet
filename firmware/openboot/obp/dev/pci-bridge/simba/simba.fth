\ Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
\ OpenBoot release it is part of (see LICENSE).
id: @(#)simba.fth 1.0 26/11/07
purpose: The Advanced PCI Bridge (APB, "Simba"): pci@1 and pci@1,1
copyright: Copyright 2026 Andy Taylor

\ Each of the APB's two functions is a PCI-PCI bridge to one of its
\ secondary buses (function 0 bus B, function 1 bus A). The setting up is
\ the release's generic bridge's (obp/dev/pci-bridge/dec21152/pcibridg.fth),
\ but for how the APB decodes addresses: not by base and limit registers
\ but by 512 MB segments of memory space and 2 MB segments of I/O space,
\ each given to a bus by a bit of its address map registers (APB User's
\ Manual 3.3, 4.1.3). The bridge takes all the space its parent offers
\ (the Sabre offers just the segments this bus is to have) and keeps it.

hex
headerless

fload ${BP}/dev/pci-bridge/dec21152/config.fth
fload ${BP}/dev/pci/compatible.fth
fload ${BP}/dev/pci-bridge/pcinode.fth

0 0 my-space  encode-phys  0 encode-int encode+  0 encode-int encode+
" reg" property

make-compatible-property

" SUNW,simba" model
" 0" encode-string " no-probe-list" property
d# 33000000 encode-int " clock-frequency" property
7 encode-int " scsi-initiator-id" property

\ Function 1 is bus A (the Ultra 5/10's own devices), function 0 bus B.
: bus-a? ( -- flag )  my-space 8 rshift 7 and 0<>  ;

\ APB registers (APB manual 4.1.3)
h# c0 constant primary-retry-limit
h# c8 constant dma-afsr
h# d8 constant pio-retry-limit
h# d9 constant pio-latency
h# da constant dma-retry-limit
h# db constant dma-latency
h# dc constant secondary-retry-limit
h# de constant io-map
h# df constant mem-map
h# e0 constant pci-control
h# e8 constant pio-afsr

: my-x! ( x offset -- )  >r xlsplit swap r@ my-l! r> 4 + my-l!  ;

\ The segments from lo up to (not including) hi, as address map register
\ bits
: segments ( lo hi shift -- mask )
   >r  1- r@ rshift 1+  swap r> rshift		( hi# lo# )
   0 -rot ?do  1 i lshift or  loop		( mask )
;

: set-address-maps ( mem-lo io-lo mem-hi io-hi -- )
   rot swap d# 21 segments io-map my-b!		( mem-lo mem-hi )
   d# 29 segments mem-map my-b!			( )
;

: disable-mem,io&bus-mastr ( -- )  4 my-w@ h# fff8 and  4 my-w!  ;

\ I/O, memory and bus master, parity and SERR# reporting
: enable-mem,io&bus-mastr ( -- )  4 my-w@ h# 147 or  4 my-w!  ;

: clear-status-bits ( -- )
   h# ffff h# 1e my-w!  h# ffff 6 my-w!
   -1 pio-afsr my-x!  -1 dma-afsr my-x!
;

\ No limits on retries; latency timers as the Ultra 5/10 firmware sets them
\ (with the primary bus's 66 MHz to the secondaries' 33 MHz).
: init-apb ( -- )
   0 primary-retry-limit my-b!
   0 pio-retry-limit my-b!  0 dma-retry-limit my-b!
   0 secondary-retry-limit my-b!
   h# 28 pio-latency my-b!  h# 50 dma-latency my-b!
   h# 28 h# 1b my-b!				\ Secondary latency timer
   \ The internal arbiter, all four request lines, SERR# forwarded
   h# 10f 1 lxjoin pci-control my-x!
;

: init-primary-bus# ( -- )  my-space d# 16 rshift h# ff and  h# 18 my-b!  ;

: claim-pci-resource ( -- mem-lo io-lo dma-lo mem-hi io-hi dma-hi bus# )
   1 h# 10.0000 0 h# 1000 0 allocate-bus#
;

: init-secondary-bus# ( mem-lo io-lo dma-lo mem-hi io-hi dma-hi bus# -- .... )
				( . . . . . . . -- mem-lo io-lo mem-hi io-hi )
   dup to my-bus#  h# 19 my-b!
   drop rot drop
;

: init-subordinate-bus# ( -- )  h# ff h# 1a my-b!  ;

\ As an Ultra 10's own OpenBoot leaves them: bus A keeps all its space, and
\ its subordinate bus number is brought down to the highest below; bus B
\ hands its unused space back to the Sabre (whose "available" it is), and
\ its subordinate bus number is left as set before the probe (ff).
0 value sub-bus#
: retain-pci-resource ( -- )
   bus-a?  if
      0 0 -1 0 -1 allocate-bus#	( mem-lo io-lo dma-lo mem-hi io-hi dma-hi bus# )
      dup h# 1a my-b!
   else
      0 h# 1000 0 h# 100 0 allocate-bus#	( ... bus# )
   then
   to sub-bus#  2drop 2drop 2drop
;

: disable-children  ( -- )
   my-bus# d# 16 lshift  4 +
   h# 1.0000 bounds  do
      h# 800 0 do  0 j i + config-w!  h# 100 +loop
   h# 800 +loop
;

\ The probe list for the bus: the pcia-probe-list option for bus B (the
\ slots), pcib-probe-list for bus A (the Ultra 5/10's own devices).
: option$ ( name$ -- value$ true | false )
   " /options" find-package if  get-package-property 0=  else  2drop false  then
   if  decode-string 2swap 2drop true  else  false  then
;

: probe-list$ ( -- adr len )
   bus-a?  if  " pcib-probe-list"  else  " pcia-probe-list"  then
   option$ 0= if  " 1,2,3,4"  then
;

: probe-children ( -- )  probe-list$ prober-xt execute  ;

: create-bus-range ( -- )
   my-bus#  encode-int  sub-bus#  encode-int encode+
   " bus-range"  property
;

\ Bus A's devices are on the board (no slots); bus B's are the slots
\ (four on an Ultra 10, devices 1 to 4), named as the machine's own
\ firmware names them.
: slot-names ( -- )
   bus-a?  if  0 encode-int " slot-names" property  exit  then
   h# 1e encode-int
   " pcia slot 1" encode-string encode+  " pcia slot 2" encode-string encode+
   " pcia slot 3" encode-string encode+  " pcia slot 4" encode-string encode+
   " slot-names" property
;

\ Interrupts: each device's INTA# to INTD# is one of the Sabre's interrupt
\ numbers (Ultra 5/10 Service Manual, table C-18): on bus A the Ethernet
\ and EBus (device 1), the graphics (2) and the IDE (3); on bus B the four
\ lines of each slot.
: en+ ( xdr,len n -- xdr',len' )  encode-int encode+  ;

: int-map-entry ( xdr,len dev# pin ino -- xdr',len' )
   >r >r  d# 11 lshift  my-bus# d# 16 lshift or  en+  0 en+ 0 en+
   r> en+  my-parent ihandle>phandle en+  r> en+
;

: make-interrupt-map ( -- )
   0 0 encode-bytes
   bus-a?  if
      1 1 h# 21 int-map-entry  2 1 h# 0f int-map-entry  3 1 h# 20 int-map-entry
   else
      5 1 do  5 1 do  j i  j 1- 4 * i 1- + h# 10 +  int-map-entry  loop  loop
   then
   " interrupt-map" property
   h# fff800 encode-int 0 en+ 0 en+ 7 en+ " interrupt-map-mask" property
   1 encode-int " #interrupt-cells" property
;

\ Bus A carries the EBus, with the console (ttya) and the flash PROM: it is
\ left forwarding, and its devices enabled, while it is set up.
: setup-apb  ( -- )
   bus-a? 0= if  disable-mem,io&bus-mastr  then
   init-primary-bus#
   claim-pci-resource		( mem-lo io-lo dma-lo mem-hi io-hi dma-hi bus# )
   init-secondary-bus#				( mem-lo io-lo mem-hi io-hi )
   init-subordinate-bus#
   make-interrupt-map
   bus-a? 0= if  disable-children  then
   set-address-maps				( )
   init-apb
   \ The latency timer, on bus B's bridge only, as an Ultra 10's own
   \ OpenBoot sets it.
   bus-a? 0=  if  h# 50 h# 0d my-b!  then
   clear-status-bits
   enable-mem,io&bus-mastr
   slot-names
   probe-children
   clear-status-bits
   retain-pci-resource
   create-bus-range
;

setup-apb
