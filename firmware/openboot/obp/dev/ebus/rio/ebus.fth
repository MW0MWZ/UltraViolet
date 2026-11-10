\ Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
\ OpenBoot release it is part of (see LICENSE).
id: @(#)ebus.fth 1.0 26/11/07
purpose: The EBus nexus: its address spaces, interrupts and methods
copyright: Copyright 2026 Andy Taylor

\ The RIO's EBus is function 0 of its PCI device; the EBus's devices are in
\ its two windows, BAR0 (offset 10: the flash PROM, 16 MB at PCI F000.0000)
\ and BAR1 (offset 14: the other devices, 8 MB at F100.0000). A child's
\ address is the window's BAR offset and the offset in it ("14,400000").

\ Named, while the PCI bus is probed, as an Ultra 10's probe names it then
\ (by its IDs: no driver for it yet); "ebus", and its devices,
\ once the PCI probe is over (probe-ebus, ebus.tok), as an Ultra 10.
" pci108e,1000" name
my-space constant ebus-space			\ Our PCI function
2 " #address-cells" integer-property
1 " #size-cells" integer-property

: my-cfg-l@ ( offset -- l )  my-space + " config-l@" $call-parent  ;

\ Our registers: configuration space and the two windows (fixed: the
\ flash PROM is where the processor starts)
: bar-reg ( xdr,len bar size -- xdr',len' )
   >r >r						( xdr,len ) ( r: size bar )
   h# 8200.0000 my-space or r@ or en+  0 en+		( xdr,len )
   r> my-cfg-l@ h# f invert and en+  0 en+  r> en+	( xdr,len' )
;
my-space encode-int 0 en+ 0 en+ 0 en+ 0 en+
   h# 10 h# 100.0000 bar-reg
   h# 14 h#  80.0000 bar-reg
" reg" property

\ The windows (BAR 10 and 14), as child spaces of the parent's memory space
: en-window ( xdr,len bar size -- xdr',len' )
   >r >r  r@ en+ 0 en+				( xdr,len ) ( r: size bar )
   h# 8200.0000 my-space or r@ or en+ 0 en+	( xdr,len )
   r> my-cfg-l@ h# f invert and en+  r> en+	( xdr,len' )
;

0 0 encode-bytes
   h# 10 h# 100.0000 en-window
   h# 14 h#  80.0000 en-window
" ranges" property

\ Each device's interrupt is one of the Sabre's interrupt numbers (Ultra
\ 5/10 Service Manual, table C-18).
: (sabre-phandle) ( -- phandle )  " /pci@1f,0" find-package 0= if  0  then  ;
(sabre-phandle) constant sabre-phandle

: int-entry ( xdr,len offset intr ino -- xdr',len' )
   >r >r >r  h# 14 en+  r> en+  r> en+  sabre-phandle en+  r> en+
;

0 0 encode-bytes
   h# 3043bc 1 h# 22 int-entry			\ ecpp
   h# 200000 1 h# 23 int-entry			\ SUNW,CS4231: capture
   h# 200000 2 h# 24 int-entry			\ SUNW,CS4231: playback
   h# 724000 1 h# 25 int-entry			\ power
   h# 3023f0 1 h# 27 int-entry			\ fdthree
   h# 3083f8 1 h# 29 int-entry			\ su: keyboard
   h# 3062f8 1 h# 2a int-entry			\ su: mouse
   h# 400000 1 h# 2b int-entry			\ se
" interrupt-map" property
h# 1f encode-int h# ff.ffff en+ 3 en+ " interrupt-map-mask" property
1 " #interrupt-cells" integer-property

external

: decode-unit ( adr len -- offset space )  parse-2int  ;

: encode-unit ( offset space -- adr len )
   push-hex  swap <# u#s drop  ascii , hold  u#s u#>  pop-base
;

: map-in ( offset space size -- va )
   >r  ebus-space or h# 0200.0000 or		( offset phys.hi ) ( r: size )
   0 swap r>  " map-in" $call-parent		( va )
;

: map-out ( va size -- )  " map-out" $call-parent  ;

: dma-alloc    ( size -- vaddr )                 " dma-alloc"   $call-parent  ;
: dma-free     ( vaddr size -- )                 " dma-free"    $call-parent  ;
: dma-map-in   ( vaddr size cache? -- devaddr )  " dma-map-in"  $call-parent  ;
: dma-map-out  ( vaddr devaddr size -- )         " dma-map-out" $call-parent  ;
: dma-sync     ( virt-addr dev-addr size -- )    " dma-sync"    $call-parent  ;

: open ( -- true )  true  ;
: close ( -- )  ;

headerless

\ The SuperIO (a PC87332, at 30.0000 in the second window): its
\ configuration registers, through an index and a data port (15c, 15d)
\ where a register takes the second of two writes to the data port
\ (PC87332 datasheet, 2.2). It is set up as an Ultra 10
\ sets it up: the parallel port, both UARTs and the floppy controller on
\ (FER), at the PC's usual addresses (FAR), the rest at their defaults.
h# 30.015c constant superio-index

: superio! ( value index -- )
   superio-index h# 14 2 map-in >r		( value index ) ( r: va )
   r@ rb!  dup r@ 1+ rb!  r@ 1+ rb!		( )
   r> 2 map-out
;

: init-superio ( -- )
   h# 0f 0 superio!				\ FER: functions on
   h# 11 1 superio!				\ FAR: addresses
   h# 80 2 superio!				\ PTR
   0 3 superio!  0 4 superio!			\ FCR, PCR
   0 6 superio!  0 7 superio!			\ PMC, TUP
;

init-superio
