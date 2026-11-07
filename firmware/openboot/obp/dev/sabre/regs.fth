\ Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
\ OpenBoot release it is part of (see LICENSE).
id: @(#)regs.fth 1.0 26/11/07
purpose: Sabre registers, configuration space and properties
copyright: Copyright 2026 Andy Taylor

\ The register map is the UltraSPARC IIi User's Manual's (chapter 19); the
\ PCI spaces are at fixed physical addresses:
\
\	1fe.0000.0000	the module's registers (64 KB)
\	1fe.0100.0000	PCI configuration space (16 MB)
\	1fe.0200.0000	PCI I/O space (16 MB)
\	1ff.0000.0000	PCI memory space (4 GB)

fload ${BP}/dev/utilities/swapped-access.fth
fload ${BP}/dev/utilities/misc.fth
fload ${BP}/dev/utilities/shifter.fth
fload ${BP}/dev/utilities/cif.fth

h# 2000                 value pagesize
alias mmu-pagesize pagesize
pagesize invert 1+      value page#mask
h# d                    value pageshift
d# 41                   value #pabits	\ Number of physical address bits
1 #pabits lshift 1-     value pa-mask
h# 100000               constant 1meg
variable pci-memlist    pci-memlist off
variable pci-io-list    pci-io-list off
0 value  bar-struct-addr

fload ${BP}/dev/utilities/memlist.fth
fload ${BP}/dev/pci/debug.fth
fload ${BP}/dev/pci/cfgio.fth
fload ${BP}/dev/pci/memstack.fth

\ Where we find the bootprom driver resources.
: builtin-drivers " SUNW,builtin-drivers" ;

\ Memory for the IOMMU's TSB and for DMA buffers: physical memory from the
\ memory node, mapped in OpenBoot's own virtual range (the client's low
\ range is remapped for the OS as OpenBoot starts it).
: memory-package ( -- ihandle )
   " /memory" find-package if  0 0 rot open-package  else  0  then
;
memory-package constant memory-ih

: phys-claim ( size align -- pa )  " claim" memory-ih $call-method drop  ;

\ The module's place on the UPA: port 1f, its registers at 1fe.0000.0000.
my-space constant sabre-pa.hi			\ 1fe
h# 1f constant upa-portid
upa-portid 6 << constant ign			\ Interrupt group number

\ The PCI spaces, as offsets from the module's registers (1fe.0000.0000).
\ (The tokenizer's numbers are 32 bits: wider ones are put together.)
h# 0100.0000 constant config-base
h# 0200.0000 constant io-base
h# 0100.0000 constant io-size
0 1 lxjoin constant mem-base		\ 1ff.0000.0000
0 1 lxjoin constant mem-size

\ DVMA: the 512 MB the target address space register enables, translated by
\ the IOMMU through a TSB of 128K entries (as for a 1 GB space, so that the
\ index of C000.0000 is 0) of 8 KB pages.
\ (FCode's numbers are 32 bits, sign-extended: wider or unsigned ones are
\ put together with lxjoin.)
h# ffff.ffff 0 lxjoin constant low32
h# c000.0000 0 lxjoin constant virtual-dma-base
h# 2000.0000 constant virtual-dma-size
7 constant tsb-size
h# 400 tsb-size << constant #tsb-entries
#tsb-entries 8 * constant /tsb

: (map-in) ( pa len -- va )  >r xlsplit r> " map-in" $call-parent  ;
: (map-out) ( va len -- )  " map-out" $call-parent  ;

: memory-alloc ( size -- va )
   pagesize round-up  dup pagesize phys-claim	( size pa )
   swap (map-in)				( va )
;
\ The root's map-out (the client interface's release) gives back the
\ physical memory as well as the mapping.
: memory-free ( va size -- )  pagesize round-up (map-out)  ;

sabre-pa.hi d# 32 << constant sabre-pa

sabre-pa h# 1.0000 (map-in) value sabre-regs

: sabre-x@ ( offset -- x )  sabre-regs + rx@  ;
: sabre-x! ( x offset -- )  sabre-regs + rx!  ;

\ Registers
h# 0030 constant ue-afsr			\ DMA uncorrectable error
h# 0040 constant ce-afsr			\ DMA correctable error
h# 0200 constant iommu-control
h# 0208 constant iommu-tsb-base
h# 0210 constant iommu-flush
h# 0c00 constant pci-imap			\ 8 slot maps: INO 00-1f
h# 1000 constant obio-imap			\ 17 maps: INO 20-30
h# 1400 constant pci-iclr			\ 32 clear registers
h# 1800 constant obio-iclr			\ 17 clear registers
h# 1c20 constant dma-write-sync
h# 2000 constant pci-csr
h# 2010 constant pio-afsr
h# 2020 constant pci-diag
h# 2028 constant pci-tasr
h# a580 constant iommu-tag-diag			\ 16 entries
h# a600 constant iommu-data-diag		\ 16 entries

\ PCI configuration space, little-endian, one 64 KB window per bus,
\ mapped as it is first used.
h# 100 /n * dup alloc-mem dup rot erase constant cfg-windows

: cfg-window ( bus# -- va )
   dup /n * cfg-windows +  dup @ ?dup if	( bus# adr va )
      nip nip exit				( va )
   then						( bus# adr )
   swap bus#>cfg config-base + sabre-pa +	( adr pa )
   h# 1.0000 (map-in) tuck swap !		( va )
;

: cfg-adr ( phys.hi -- va )
   dup cfg>bus# cfg-window  swap h# ffff and +
;

external

: config-b@ ( phys.hi -- b )  cfg-adr rb@  ;
: config-w@ ( phys.hi -- w )  cfg-adr rw@ wbflip  ;
: config-l@ ( phys.hi -- l )  cfg-adr rl@ lbflip  ;
: config-b! ( b phys.hi -- )  cfg-adr rb!  ;
: config-w! ( w phys.hi -- )  cfg-adr >r wbflip r> rw!  ;
: config-l! ( l phys.hi -- )  cfg-adr >r lbflip r> rl!  ;

headerless

\ The module set up as the IIi manual recommends for a
\ system with an APB (section 19.3.0.1), with arbitration for all four
\ request lines; errors cleared; the DMA target space C000.0000-DFFF.FFFF;
\ interrupts idle and unmapped.
: init-sabre ( -- )
   h# 20.010f h# 14 lxjoin pci-csr sabre-x!		\ MRLM, park, errint, arb
   -1 pio-afsr sabre-x!				\ PIO write errors
   -1 ue-afsr sabre-x!  -1 ce-afsr sabre-x!	\ DMA errors
   h# 40 pci-diag sabre-x!			\ No PIO retry limit
   h# 40 pci-tasr sabre-x!			\ DVMA at C000.0000
   d# 32 0 do  0 i 8 * pci-iclr + sabre-x!  loop
   d# 17 0 do  0 i 8 * obio-iclr + sabre-x!  loop
   8 0 do  0 i 8 * pci-imap + sabre-x!  loop
   d# 17 0 do  0 i 8 * obio-imap + sabre-x!  loop
   \ Bus 0: our bus number and the highest below us
   0 h# 40 config-b!  h# ff h# 41 config-b!
   \ Memory and master, parity and SERR#; the latency timer
   h# 146 4 config-w!  h# 28 h# d config-b!
   h# f900 6 config-w!				\ Clear status
;

\ The properties of a sun4u PCI host bridge (as Solaris's pcipsy and Linux
\ find them on the Ultra 5 and 10).
: en+x ( xdr,len x -- xdr',len' )  xlsplit swap >r en+ r> en+  ;

: make-sabre-properties ( -- )
   " pci" name
   " pci" device-type
   " SUNW,sabre" model
   " pci108e,a000" encode-string " pciclass,0" encode-string encode+
   " compatible" property
   3 " #address-cells" integer-property
   2 " #size-cells" integer-property

   0 0 encode-bytes
      sabre-pa.hi en+ 0 en+ 0 en+ h# 1.0000 en+
      sabre-pa.hi en+ config-base en+ 0 en+ h# 100 en+
   " reg" property

   0 0 encode-bytes
      0 en+ 0 en+ 0 en+  sabre-pa config-base + en+x  h# 100.0000 en+x
      h# 0100.0000 en+ 0 en+ 0 en+  sabre-pa io-base + en+x  io-size en+x
      h# 0200.0000 en+ 0 en+ 0 en+  sabre-pa mem-base + en+x  mem-size en+x
      h# 0300.0000 en+ 0 en+ 0 en+  sabre-pa mem-base + en+x  mem-size en+x
   " ranges" property

   upa-portid " upa-portid" integer-property
   0 0 encode-bytes
      ign h# 30 + en+				\ PCI bus error
      ign h# 2e + en+				\ DMA uncorrectable error
      ign h# 2f + en+				\ DMA correctable error
      ign h# 25 + en+				\ Power button
   " interrupts" property
   d# 66000000 " clock-frequency" integer-property

   " 66mhz-capable"		boolean-property
   " bus-parity-generated"	boolean-property
   " no-streaming-cache"	boolean-property
   " button-interrupt"		boolean-property
   " latency-timer"		boolean-property

   virtual-dma-base encode-int virtual-dma-size en+ " virtual-dma" property
   1 " #virtual-dma-size-cells" integer-property
   1 " #virtual-dma-addr-cells" integer-property

   sabre-regs encode-int  0 cfg-window en+  " address" property
;

\ Map-ins of the PCI spaces (for obp/dev/pci/map.fth)
: bus-map-cfg ( phys.hi len -- va )
   drop cfg-adr
;

: bus-map-io ( phys.lo phys.mid phys.hi len -- va )
   >r 2drop  io-size 1- and  io-base + sabre-pa +  r> (map-in)
;

: bus-map-mem ( phys.lo phys.mid phys.hi len -- va )
   >r 2drop  -1 and32  mem-base + sabre-pa +  r> (map-in)
;

init-sabre
make-sabre-properties

external
0 value my-pci-bus
headerless
