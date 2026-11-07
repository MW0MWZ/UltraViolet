\ Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
\ OpenBoot release it is part of (see LICENSE).
id: @(#)probeall.fth 1.0 26/11/07
purpose: Probing the Ultra 5/10's devices
copyright: Copyright 2026 Andy Taylor

\ As arch/sun4v/niagara/probeall.fth does for its PCI roots: the Ultra
\ 5/10's PCI (the Sabre at /pci@1f,0, and the buses behind its Simba
\ bridges).

headerless
: (do-probe) ( slot$ dev$ fn$ -- )
   execute-device-method 0= if  2drop  then
;
: pci-probe ( slot$ dev$ -- ) " master-probe" (do-probe) ;

\ The UPA's graphics slot (port 1e, at 1fc.0000.0000): a Creator3D (or
\ Elite3D) carries its FCode in a PROM at the start of its space, a byte
\ every four addresses, and the program runs from there in the card's
\ node, the card making the node's name and properties (as OpenBoot 3.x
\ probes the UPA). An empty slot answers with a bus error.
h# 1fc.0000.0000 constant upa-gfx-pa
h# 10.0000 constant /upa-gfx-prom

: fcode-start? ( byte -- flag )  dup h# f0 h# f3 between  swap h# fd =  or  ;

: probe-upa-graphics ( -- )
   upa-gfx-pa 0 /upa-gfx-prom memmap			( va )
   dup cpeek if						( va byte )
      fcode-start? if					( va )
         0 0 " 1e,0" " /" begin-package
            dup 4 byte-load
         end-package
      then						( va )
   then							( va )
   /upa-gfx-prom 2dup mmu-unmap mmu-release		( )
;

\ The UPA graphics' interrupt goes to the Sabre (its graphics mapping
\ register, interrupt number 33), as the root's interrupt map says.
: make-root-interrupt-map ( -- )
   " /pci@1f,0" find-package 0= if  exit  then		( sabre )
   root-device
      h# 3c encode-int 0 encode-int encode+ 0 encode-int encode+
      rot encode-int encode+  h# 8000.0033 encode-int encode+
      " interrupt-map" property
      h# 3e encode-int 0 encode-int encode+ 0 encode-int encode+
      " interrupt-map-mask" property
      1 encode-int " #interrupt-cells" property
   device-end
;

\ The root's devices on the UPA (the PCI bus module, SUNW,sabre), made once
\ the memory is known and OpenBoot's services are up (their drivers claim
\ memory, as arch/sun4v/root-prober.fth's do).
stand-init: Probe root devices
   " root-devices" ['] builtin-drivers-package find-method if  execute  then
   make-root-interrupt-map
;

\ The console's screen: the Creator3D (UPA port 1e), else the Rage Pro.
: set-screen-alias ( -- )
   " /SUNW,ffb@1e,0" 2dup find-package if		( path$ phandle )
      drop						( path$ )
   else							( path$ )
      2drop " /pci@1f,0/pci@1,1/SUNW,m64B@2"		( path$' )
   then							( path$ )
   " screen" 2swap $devalias				( )
;

headers
: probe-io ( -- )
   " 1"  " /pci@1f,0" pci-probe
   probe-upa-graphics
;

: probe-all ( -- )
   ['] diagnostic-mode? behavior 			( xt )
   max+mode? if  ['] true to diagnostic-mode?  then	( xt )
   probe-io						( xt )
   to diagnostic-mode?					( )
   set-screen-alias
;
headerless
