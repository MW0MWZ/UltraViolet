\ Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
\ OpenBoot release it is part of (see LICENSE).
id: @(#)methods.fth 1.0 26/11/07
purpose: The Sabre's methods, and the probing of the APB's buses
copyright: Copyright 2026 Andy Taylor

\ As obp/dev/sun4v-devices/vpci/methods.fth and bus-ops.fth, for the
\ Sabre's own address spaces.

headerless

\ The APB (IIi manual 19, APB manual 3.3): bus A (function 1, the Ultra
\ 5/10's own devices) and bus B (function 0, the PCI slots) each take whole
\ 512 MB segments of memory space and 2 MB segments of I/O space, and
\ segment 6 of memory space (C000.0000) is neither's: it is the DVMA space.
\ The buses are numbered and given their spaces as the Ultra 5/10 firmware
\ does: bus A first (bus 1, memory segment 7, I/O segments 6 and 7), then
\ bus B (bus 2 and up, memory segments 0 to 5, I/O segments 0 to 5). The
\ top of segment 7 is the EBus's (its fixed windows at F000.0000, the flash
\ PROM, and F100.0000), so bus A's other devices are given E000.0000 to
\ EFFF.FFFF.

h# e000.0000 constant bus-a-mem		h# 1000.0000 constant /bus-a-mem
h#  c0.0000 constant bus-a-io		h#   40.0000 constant /bus-a-io
h# 0000.0000 constant bus-b-mem		h# c000.0000 constant /bus-b-mem
h#   00.0000 constant bus-b-io		h#   c0.0000 constant /bus-b-io

1 constant apb-dev#

\ While each bus's bridge is probed, the memory and I/O lists hold only that
\ bus's segments, so that the bridge (which asks for all there is) gets
\ just them.
: clear-lists ( -- )
   pci-memlist @ free-list  pci-memlist off
   pci-io-list @ free-list  pci-io-list off
;

: apb-lists ( mem /mem io /io -- )
   clear-lists  pci-io-list free-memrange  pci-memlist free-memrange
;

: probe-apb ( -- )
   " " " 1"  apb-dev# dev#>cfg			( arg$ reg$ phys.hi )
   dup pci-function-present? if
      bus-a-mem /bus-a-mem bus-a-io /bus-a-io apb-lists
      1 probe-function
      bus-b-mem /bus-b-mem bus-b-io /bus-b-io apb-lists
      0 probe-function
   then
   3drop 2drop
   clear-lists					\ Nothing left on bus 0
;

: sabre-prober ( probe$ -- )  2drop probe-apb  ;

\ The release's prober, but with an Ultra 10's messages in diagnostic
\ mode (the release's own 4.x ones kept quiet): for each device on the bus
\ (a Simba's: it is the prober-xt the bridges below use, and runs as them),
\ "Probing <bus> at Device <n>  ", the names of the nodes made for it as
\ they are set, or "Nothing there" (OpenBoot's diagmsg.fth, by the root's
\ methods). An Ultra 10 says nothing for the Sabre's own bus, the two Simbas.
: root-method ( method$ -- xt true | false )
   " /" find-package  if  find-method  else  2drop false  then
;
: quietly ( ?? xt -- ?? )
   " probe-quietly" root-method  if  execute  else  execute  then
;
: pci-prober31 ( probe-list$adr,len -- )
   pci-depth 9 > if
      ." Maximum PCI probe depth " pci-depth . ." exceeded;"
      ." No further PCI bridge devices can be probed" cr
      2drop exit
   then
   pci-depth dup >r 1+ to pci-depth		( $adr,len ) ( r: old-depth )
   begin  dup  while				( $adr,len )
      ascii , left-parse-string			( rem$ dev#$ )
      " "  2swap				( rem$ arg$ dev#$ )
      2dup my-self ihandle>phandle		( rem$ arg$ dev#$ dev#$ phandle )
      " probe-line-open" root-method  if  execute  else  3drop  then
      ['] pci-probe-self quietly		( rem$ )
      " probe-line-close" root-method  if  execute  then
   repeat					( null$ )
   2drop
   r> to pci-depth
;

external

: decode-unit ( adr len -- phys.lo phys.mid phys.hi )
   pci-decode-unit lwsplit drop  my-pci-bus  wljoin
;

: encode-unit  ( phys.lo phys.mid phys.hi -- adr len ) pci-encode-unit ;

: map-in ( p.low p.mid p.hi len -- vaddr ) pci-map-in ;
: map-out ( va len -- ) (map-out) ;

: allocate-bus# ( n mem-aln mem-sz io-aln io-sz -- . . . . . . . )
                      ( . . . . . -- mem-l io-l dma-l mem-h io-h dma-h bus# )
   pci-allocate-bus#
;

: assign-int-line ( int phys.hi -- false )		2drop false  ;
: prober ( adr len -- )					['] sabre-prober quietly  ;
: prober-xt ( -- adr )					['] pci-prober31  ;

variable my-memlist  my-memlist off
variable my-io-list  my-io-list off

: master-probe  ( adr len -- )
   bar-struct-addr my-memlist @ my-io-list @		( reg mem io )
   set-pointers						( -- )
   setup-swapped-fcodes
   pci-master-probe
   make-available-property				( -- )
   get-pointers						( reg mem io )
   my-io-list ! my-memlist ! drop			( -- )
   restore-fcodes
;

: open true ;
: close ;

headerless
