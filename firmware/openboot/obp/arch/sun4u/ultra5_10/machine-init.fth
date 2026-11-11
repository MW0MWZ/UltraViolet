\ Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
\ OpenBoot release it is part of (see LICENSE).
id: @(#)machine-init.fth 1.0 26/11/07
purpose: Resetting and switching off the Ultra 5/10; the root node's identity
copyright: Copyright 2026 Andy Taylor

\ What arch/sun4v/machine-init.fth does through the hypervisor: reset-all
\ by the Sabre's reset control (a software power-on reset), power-off by
\ the EBus power register, and the root node's names and clock.

headerless
h# 1fe.0000.f020 constant reset-control-pa
h# 1ff.f172.4000 constant power-pa		\ EBus BAR1 + 72.4000
h# 4000.0000 constant soft-por

\ The Ultra 10 says so first ("Resetting ... ", then two line ends),
\ from the ok prompt and on a reboot from the operating system alike.
chain: (reset-all
   ." Resetting ... " cr cr
   soft-por reset-control-pa bypass-asi spacex!
   begin again
;
' (reset-all is reset-all

headers
: power-off ( -- )
   1 power-pa bypass-asi spacec!
   begin again
;

headerless
\ The processor's clock, as measured at start-up (below); the UPA's,
\ a quarter of it on the SME1430 (333 MHz and up) and a third on the
\ SME1040 (UltraSPARC-IIi manual 1.3.4 and its addendum); and the
\ machine's names, the ones an Ultra 5/10 has.
d# 440.000.000 value cpu-clock-frequency

\ The clock: TICK counted against the APB's tick register, which counts
\ the primary PCI bus's clock (APB manual 4.1.3.1), 66 MHz on the Ultra
\ 5/10 (the Sabre's clock-frequency), over 10 ms; to the nearest MHz.
h# 1fe.0100.08b0 constant apb-tick-pa		\ APB function 0, register b0
d# 66.000.000 constant pci-hz

: apb-tick@ ( -- n )  apb-tick-pa bypass-asi spacel@ lbflip  ;

: measure-cpu-clock ( -- hz )
   apb-tick@ tick@					( a0 t0 )
   begin  apb-tick@ 2 pick - h# ffff.ffff and  pci-hz d# 100 / u>=  until
   apb-tick@ tick@					( a0 t0 a1 t1 )
   rot - -rot swap - h# ffff.ffff and			( dt da )
   >r pci-hz * r> /					( hz )
   d# 500.000 + d# 1.000.000 / d# 1.000.000 *		( hz' )
;

\ The system board's part number (the root's model)
: get-board-part# ( -- adr,len )  " 375-0066"  ;
: upa-clock-frequency ( -- hz )
   cpu-clock-frequency  dup d# 320.000.000 >  if  4  else  3  then  /
;

: banner-name$ ( -- adr,len )
   " Sun Ultra 5/10 UPA/PCI (UltraSPARC-IIi " "temp pack >r
   cpu-clock-frequency d# 1.000.000 / (.d) r@ $cat
   " MHz)" r@ $cat  r> count
;

: make-root-props ( -- )
   root-device
      upa-clock-frequency encode-int " clock-frequency" property
      " SUNW,Ultra-5_10" encode-string " name" property
      " SUNW," "temp pack >r  get-board-part# r@ $cat
      r> count encode-string " model" property
      banner-name$ encode-string " banner-name" property
      \ (Energy Star's second version: Solaris's power management)
      0 0 encode-bytes " energystar-v2" property
   device-end
;

stand-init: Measuring the processor clock
   measure-cpu-clock dup to cpu-clock-frequency  to system-tick-speed
;

stand-init: Build root-properties
   make-root-props
;
