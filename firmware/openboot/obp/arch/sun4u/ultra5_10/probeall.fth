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
\ node, the card making the node's name and properties (as an Ultra 5/10
\ probes the UPA). An empty slot answers with a bus error.
h# 1fc.0000.0000 constant upa-gfx-pa
h# 10.0000 constant /upa-gfx-prom

: fcode-start? ( byte -- flag )  dup h# f0 h# f3 between  swap h# fd =  or  ;

: probe-upa-graphics ( -- )
   diag-probe?  if  ??cr ." Probing UPA Slot at 1e,0 "  open-probe-line  then
   upa-gfx-pa 0 /upa-gfx-prom memmap			( va )
   dup cpeek if						( va byte )
      fcode-start? if					( va )
         0 0 " 1e,0" " /" begin-package
            dup 4 ['] byte-load quietly
         end-package
      then						( va )
   then							( va )
   /upa-gfx-prom 2dup mmu-unmap mmu-release		( )
   diag-probe?  if  close-probe-line  then
;

\ The UPA graphics' interrupt goes to the Sabre (its graphics mapping
\ register, interrupt number 33), as the root's interrupt map says.
\ (rootnexus.fth made the three in an Ultra 10's order; set in place here.)
: make-root-interrupt-map ( -- )
   " /pci@1f,0" find-package 0= if
      root-device
         " interrupt-map" delete-property  " interrupt-map-mask" delete-property
         " #interrupt-cells" delete-property
      device-end  exit
   then							( sabre )
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
   " root-devices" ['] builtin-drivers-package find-method if  quietly  then
   make-root-interrupt-map
;

\ An Ultra 10's report in diagnostic mode, before the probing: the Sabre's
\ reset control's bits (as first read: uniproc.fth), the processor module,
\ and each pair of DIMM sockets (banks #0 and #2: the memory controller's
\ pairs 0 and 2), its two DIMMs' sizes and its total, in megabytes, as
\ memprobe.fth found them; 10-bit DIMMs in 11-bit mode shown unused.
: .reset-control ( -- )
   reset-status@ d# 27 rshift				( bits )
   ." Reset Control: BXIR:" dup 1 and .d  1 rshift
   ." BPOR:" dup 1 and .d  1 rshift
   ." SXIR:" dup 1 and .d  1 rshift
   ." SPOR:" dup 1 and .d  1 rshift
   ." POR:" 1 and .d  cr
;
\ The module named by the E-cache's latency, as an Ultra 10 names it: 2-2-2
\ SRAMs at e000 or more. On an Ultra 10 ecache-latency is a value of fd5 that nothing
\ changes (none of its code refers to it but 222-module?), so the module
\ is 2-2 unless it is set by hand; the same here.
h# fd5 value ecache-latency
: 222-module? ( -- flag )  ecache-latency h# e000 >=  ;
: .module ( -- )
   222-module?  if  ." UltraSPARC-IIi 2-2-2 module"  else  ." UltraSPARC-IIi 2-2 module"  then  cr
;
: .mb3 ( bytes -- )  1meg /  3 .r space  ;
: .memory-bank ( pair -- )
   push-decimal
   ." Probing Memory Bank #"  dup 2* .d			( pair )
   dup pair-dimm0 swap na+ @ .mb3  ." + "			( pair )
   dup pair-dimm1 swap na+ @ .mb3  ." : "			( pair )
   dup pair-unused swap na+ @  if
      ." Unused 10 bit Column DIMMS - See User's Manual!"
   then							( pair )
   pair-sizes swap na+ @ .mb3  ." Megabytes" cr
   pop-base
;
stand-init: Diagnostic report
   diagnostic-mode?  if
      .reset-control  .module  2 0 do  i .memory-bank  loop
   then
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

\ The RIO's EBus, named by its IDs while the PCI bus is probed, becomes
\ "ebus" and gets its devices afterwards, as an Ultra 10 makes them
\ (nothing of it shown in diagnostic mode).
: (probe-ebus) ( -- )
   " /pci@1f,0/pci@1,1/pci108e,1000@1"  2dup find-package  if	( path$ phandle )
      drop  select-dev  " probe-ebus" my-self $call-method  unselect-dev
   else
      2drop
   then
;
: probe-ebus ( -- )  ['] (probe-ebus) quietly  ;

headers
: probe-io ( -- )
   probe-upa-graphics					\ (an Ultra 10's order)
   " 1"  " /pci@1f,0" pci-probe
   probe-ebus
;

: probe-all ( -- )
   ['] diagnostic-mode? behavior 			( xt )
   max+mode? if  ['] true to diagnostic-mode?  then	( xt )
   diagnostic-mode? to diag-probe?			( xt )
   probe-io						( xt )
   false to diag-probe?					( xt )
   to diagnostic-mode?					( )
   set-screen-alias
;
headerless
