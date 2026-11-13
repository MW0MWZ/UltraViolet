\ Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
\ OpenBoot release it is part of (see LICENSE).
id: @(#)rootnexus.fth 1.1 26/11/11
purpose: The root node of the Ultra 5/10
copyright: Copyright 2026 Andy Taylor

\ arch/sun4v/rootnexus.fth's root node, for sun4u: its children are on the
\ UPA, addressed by physical address (two cells: phys.hi, phys.lo), with
\ unit addresses as sun4u OpenBoot writes them ("1f,0": port ID, offset).

headerless
h# 1e constant upa-slave-port		\ (the UPA methods below)
h# 1fe.0000.f000 constant ffb-config-pa
variable upa-slave-xt	\ (kept: nothing here interrupts through it)

\ The root's DMA methods are the Sabre's: all of an Ultra 10's DMA goes
\ through its IOMMU. The PCI node is opened once, when first needed.
0 value iommu-ih
: iommu-method ( ?? method$ -- ?? )
   iommu-ih 0=  if
      " /pci@1f,0" open-dev  dup 0= abort" Can't open the PCI node"
      to iommu-ih
   then
   iommu-ih $call-method
;
headers

\ The root's properties are made here in the order an Ultra 10's are (its
\ .properties lists them newest first), so they list in the same order:
\ those whose values come later (machine-init.fth, probeall.fth, the IDPROM,
\ uniproc.fth) are made empty here and set in their place then.
root-device
   " upa" " device_type" string-property
   0 0 encode-bytes " banner-name" property
   0 " clock-frequency" integer-property
   0 0 encode-bytes " name" property
   0 0 encode-bytes " model" property
   2 " #size-cells" integer-property
   breakpoint-trap# h# 7f and " breakpoint-trap"  integer-property
   2 " #address-cells" integer-property
   1 " #interrupt-cells" integer-property
   0 0 encode-bytes " interrupt-map-mask" property
   0 0 encode-bytes " interrupt-map" property
   0 0 encode-bytes " idprom" property
   0 0 encode-bytes " reset-reason" property
   \ (Energy Star's second version: Solaris's power management)
   0 0 encode-bytes " energystar-v2" property

   : open  ( -- true )  true  ;
   : close  ( -- )  ;

   : map-in  ( pa.lo pa.hi size -- virtual )
      >r lxjoin  0 r> memmap				( va )
   ;
   : map-out  ( virtual size -- )
      swap cif-release
   ;

   \ A UPA port's unit address is its port ID and the offset in its 8 GB
   \ (pci@1f,0 at 1fe.0000.0000, SUNW,ffb@1e,0 at 1fc.0000.0000, the
   \ processor at 1c0.0000.0000).
   : encode-unit  ( pa.lo pa.hi -- adr,len )
      push-hex
      dup 1 rshift h# 1f and -rot			( portid pa.lo pa.hi )
      1 and d# 32 lshift or				( portid offset )
      <# u#s drop  ascii , hold  u#s u#>
      pop-base
   ;
   \ A space as text: a number, kept as phys.hi when it has any of bits
   \ 8:6 (a UPA physical address's), else taken as a port ID.
   : decode-space ( adr len -- phys.hi )
      push-hex $number pop-base  abort" Bad number syntax"	( n )
      dup h# 1c0 and 0=  if  1 lshift h# 1c0 or  then
   ;

   : dma-alloc ( size -- vaddr )  " dma-alloc" iommu-method  ;
   : dma-free ( vaddr size -- )  " dma-free" iommu-method  ;
   : dma-map-in ( vaddr size cache? -- devaddr )  " dma-map-in" iommu-method  ;
   : dma-map-out ( vaddr devaddr size -- )  " dma-map-out" iommu-method  ;

   \ A unit address with no port ID (@,0) is an error, as an Ultra 10's
   \ (a path's search then ends at the root, finddev.fth).
   : decode-unit   ( adr len -- pa.lo pa.hi )
      ascii , left-parse-string  dup 0=  abort" Bad unit address"
      2swap parse-int  -rot parse-int			( offset portid )
      1 lshift h# 1c0 or  over d# 32 rshift or		( offset pa.hi )
      swap h# ffff.ffff and swap			( pa.lo pa.hi )
   ;

   \ The UPA methods a UPA card's FCode calls (the Creator3D's: its port
   \ ID, its request queue size, and the routine that makes it interrupt),
   \ as an Ultra 10 has them. Its one UPA slave port is the graphics slot,
   \ port 1e; the processor is the only master, and the IIi has no caches
   \ or queues to set for one (the queue size is the IIi's FFB_Config
   \ register: SPRQS, bits 27:24, IIi manual 18.1).

   : upa-get-portid ( adr space -- portid )  nip 1 rshift h# 1f and  ;

   \ As the Ultra 10 writes it: the slave request queue size in SPRQS, the
   \ second and third arguments at bits 18 and 16 and bit 15 set (reserved
   \ bits on the IIi, which ignores them); the last three not used.
   : upa-set-queue-sizes ( preq a b c d e portid -- )
      upa-slave-port <> abort" Invalid portid"		( preq a b c d e )
      3drop  d# 16 lshift  swap d# 18 lshift or  swap d# 24 lshift or
      h# 8000 or  ffb-config-pa  bypass-asi spacex!
   ;
   : upa-get-queue-sizes ( portid -- )
      true abort" Invalid method for slave device"
   ;
   : upa-set-cache-parameters ( -- )
      true abort" No CIM to be set in this implementation"
   ;
   : upa-slave-interrupt ( xt portid -- 0 | xt true )
      upa-slave-port =  if  upa-slave-xt ! 0  else  true  then
   ;
   : upa-master-interrupt ( xt portid -- xt true )  drop true  ;
device-end \ root device

\ The probing lines in diagnostic mode (diagmsg.fth), for the PCI bus
\ module's prober (FCode: it finds these as methods of the built-in
\ drivers' package, where the root node's methods stay an Ultra 10's).
\ A line for a device on a bus, closed with its names or "Nothing there";
\ and a probe made with the release's own (4.x) messages off.
" /packages/SUNW,builtin-drivers" find-device
   : probe-line-open ( dev$ bus-phandle -- )
      diag-probe?  if
         ??cr ." Probing "  phandle>devname type  ."  at Device "  type 2 spaces
         open-probe-line
      else
         3drop
      then
   ;
   : probe-line-close ( -- )  diag-probe?  if  close-probe-line  then  ;
   : probe-quietly ( ?? xt -- ?? )  quietly  ;
device-end
