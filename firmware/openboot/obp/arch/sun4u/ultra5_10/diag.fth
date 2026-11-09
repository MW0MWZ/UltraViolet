\ Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
\ OpenBoot release it is part of (see LICENSE).
id: @(#)diag.fth 1.0 26/11/07
purpose: The Ultra 5/10's diagnostic commands
copyright: Copyright 2026 Andy Taylor

\ The Ultra 5/10's own commands that the release has no form
\ of: .speed, eject-floppy, probe-pci and probe-pci-slot (watch-clock and
\ date are the clock's, in its driver; show-post-results is the POST's).

headerless

: prop-int ( name$ phandle -- n true | false )
   get-package-property if  false  else  decode-int nip nip true  then
;
: node-int ( name$ path$ -- n true | false )
   find-package if  prop-int  else  2drop false  then
;
: .2digits ( n -- )  <# u# u# u#> type  ;
: .digits ( n #digits -- )  <# 0 ?do  u#  loop  u#> type  ;
: .mhz.2 ( hz -- )  d# 10.000 /  d# 100 /mod (.d) type  ." ." .2digits  ;
: .bus-mhz ( path$ -- )
   " clock-frequency" 2swap node-int if  d# 1.000.000 / (.d) type  else  ." ?"  then
   ." Mhz"
;

headers

\ The processor's clock, the UPA's and the two PCI buses'.
: .speed ( -- )
   push-decimal
   ." CPU  Speed : " cpu-clock-frequency .mhz.2 ." MHz" cr
   ." UPA  Speed : " upa-clock-frequency .mhz.2 ." MHz" cr
   ." PCI  Bus A : " " /pci@1f,0/pci@1,1" .bus-mhz cr
   ." PCI  Bus B : " " /pci@1f,0/pci@1" .bus-mhz cr
   pop-base
;

: eject-floppy ( -- )  " floppy" $eject  ;

headerless

\ A PCI device: its device and function numbers, name, IDs and class.
\ (push-decimal and push-hex keep the base on the return stack.)
0 value pci-node
: .pci-device ( phandle -- )
   to pci-node
   " reg" pci-node prop-int if				( phys.hi )
      push-decimal
      ."   Device " dup d# 11 rshift h# 1f and (.d) type
      ." , function " 8 rshift 7 and (.d) type
      pop-base
      ." : "  " name" pci-node get-package-property 0= if  decode-string type 2drop  then
      push-hex
      " vendor-id" pci-node prop-int if  ."   (vendor " 4 .digits  then
      " device-id" pci-node prop-int if  ." , device " 4 .digits  then
      " class-code" pci-node prop-int if  ." , class " 6 .digits  then
      ." )"  pop-base cr
   then
;
: .pci-bus ( name$ path$ -- )
   2dup find-package if				( name$ path$ phandle )
      >r  2swap type ."  (" type ." ):" cr  r>	( phandle )
      child begin  ?dup while  dup .pci-device peer  repeat
   else
      2drop 2drop
   then
;

headers

\ The devices on the PCI buses: A, the Ultra 5/10's own; B, the slots'.
: probe-pci ( -- )
   " PCI bus A" " /pci@1f,0/pci@1,1" .pci-bus
   " PCI bus B" " /pci@1f,0/pci@1" .pci-bus
;

\ The device in one of the slots (bus B's device number).
: probe-pci-slot ( -- )  \ slot#
   parse-word dup 0= if  2drop ." Usage: probe-pci-slot slot#" cr exit  then
   push-decimal $number pop-base if  ." Bad slot number" cr exit  then	( n )
   " /pci@1f,0/pci@1" find-package 0= if  drop exit  then		( n phandle )
   false swap child  begin  ?dup while				( n found? ph )
      dup " reg" rot prop-int if					( n found? ph reg )
         d# 11 rshift h# 1f and  3 pick = if  dup .pci-device nip true swap  then
      then
      peer
   repeat								( n found? )
   if  drop  else  push-decimal ." No device in slot " .d cr pop-base  then
;
