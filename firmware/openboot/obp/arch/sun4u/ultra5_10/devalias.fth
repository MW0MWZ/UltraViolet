\ Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
\ OpenBoot release it is part of (see LICENSE).
id: @(#)devalias.fth 1.0 26/11/07
purpose: The Ultra 5/10's device aliases
copyright: Copyright 2026 Andy Taylor

\ The aliases an Ultra 5/10 has (as arch/sun4v/devalias.fth
\ makes sun4v's from the machine description). "screen" is the Creator3D
\ if there is one, else the Rage Pro: it is set once the devices are
\ probed (probeall.fth).

headerless
: ebus-alias ( name$ path$ -- )
   " /pci@1f,0/pci@1,1/ebus@1/" "temp pack >r  r@ $cat  r> count  $devalias
;
: ide-alias ( name$ path$ -- )
   " /pci@1f,0/pci@1,1/ide@3" "temp pack >r  r@ $cat  r> count  $devalias
;

\ In the order an Ultra 10 makes them (its devalias lists them newest first).
stand-init: Add devaliases
   " mouse"	" su@14,3062f8"			ebus-alias
   " keyboard"	" su@14,3083f8"			ebus-alias
   " keyboard!"	" su@14,3083f8:forcemode"	ebus-alias
   " ttya"	" se:a"				ebus-alias
   " ttyb"	" se:b"				ebus-alias
   " floppy"	" fdthree"			ebus-alias
   " ide"	" "				ide-alias
   " disk0"	" /disk@0,0"			ide-alias
   " disk1"	" /disk@1,0"			ide-alias
   " disk2"	" /disk@2,0"			ide-alias
   " disk3"	" /disk@3,0"			ide-alias
   " disk"	" /disk@0,0"			ide-alias
   " cdrom"	" /cdrom@2,0:f"			ide-alias
   " net"	" /pci@1f,0/pci@1,1/network@1,1" $devalias
;
headers
