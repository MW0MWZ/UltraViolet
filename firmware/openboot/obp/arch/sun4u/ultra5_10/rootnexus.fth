\ Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
\ OpenBoot release it is part of (see LICENSE).
id: @(#)rootnexus.fth 1.0 26/11/07
purpose: The root node of the Ultra 5/10
copyright: Copyright 2026 Andy Taylor

\ arch/sun4v/rootnexus.fth's root node, for sun4u: its children are on the
\ UPA, addressed by physical address (two cells: phys.hi, phys.lo), with
\ unit addresses as sun4u OpenBoot writes them ("1f,0": port ID, offset).

root-device
   2 " #size-cells" integer-property
   2 " #address-cells" integer-property
   " upa" " device_type" string-property
   breakpoint-trap# h# 7f and " breakpoint-trap"  integer-property

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
   : decode-unit   ( adr len -- pa.lo pa.hi )
      ascii , left-parse-string  2swap parse-int  -rot parse-int  ( offset portid )
      1 lshift h# 1c0 or  over d# 32 rshift or		( offset pa.hi )
      swap h# ffff.ffff and swap			( pa.lo pa.hi )
   ;

   \ The probing lines in diagnostic mode (diagmsg.fth), for the
   \ PCI bus module's prober (FCode: it finds these as the root's methods).
   \ A line for a device on a bus, closed with its names or "Nothing there";
   \ and a probe made with the release's own (4.x) messages off.
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
device-end \ root device

\ /openprom: an Ultra 10 has decode-complete there.
" /openprom" find-device
   0 0 " decode-complete" property
device-end
