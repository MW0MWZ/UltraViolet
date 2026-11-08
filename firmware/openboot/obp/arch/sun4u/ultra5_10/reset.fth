\ Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
\ OpenBoot release it is part of (see LICENSE).
id: @(#)reset.fth 1.0 26/11/07
purpose: The Ultra 5/10's reset code
copyright: Copyright 2026 Andy Taylor

\ The code at the start of the flash PROM, built as arch/sun4v/niagara's
\ reset.fth is (into reset.bin, the PROM's first drop-in, "OBP"). Where a
\ sun4v machine's hypervisor has set the machine up, on the Ultra 5/10 this
\ does it: the UltraSPARC IIi resets into RED state with its MMU and caches
\ off, at the PROM's start (1ff.f000.0000) plus the reset's offset.
\
\ Power-on reset:
\   1. The processor's state; the memory controller and the PCI bus numbers
\      and EBus window (as the machine's own OpenBoot sets them up at
\      first); ttya, for messages.
\   2. Bank 0 sized; OpenBoot's 8 MiB at its top (membase).
\   3. This code (with the decompressor) copied to physical 0, as it is
\      linked, and run there.
\   4. OpenBoot's Forth image (the "bootprom" drop-in) decompressed to
\      membase + 2 MiB, where ROMbase maps.
\   5. Locked TLB entries: 0 to 0 (this code), monvirtbase to membase (4 MiB,
\      the image), RAMbase to membase + 2 MiB - 64 KiB (64 KiB); the MMUs
\      and caches on; the image entered at its power-on entry (ROMbase +
\      30) with %i0 = membase, %i1 = 8 MiB, %i2 = the reset (0 power-on,
\      1 watchdog, 2 XIR, 3 SIR, 4 RED state; kept in %y until then).
\ The other resets (watchdog, XIR, SIR, RED) say which they were and start
\ again as from power-on.

\ [define] VERBOSE-RESET

resident headerless
h# 4f.42.4d.44  constant  dropin-magic

fload ${BP}/arch/sun4s/reset/common.fth
fload ${BP}/arch/sun/auto-field.fth

: save-binary
   new-file
   obj-base  obj-size ofd @  fputs
   ofd @ fclose
;

h# 1ff.f000.0000 constant prom-pa		\ the flash PROM
h# 1fe.0000.f010 constant mc-control0-pa	\ the memory controller
h# 1fe.0000.f018 constant mc-control1-pa
h# 1ff.f172.6000 constant auxio-pa		\ (read for delays)
h# 1ff.f140.0000 constant reset-ttya-pa
h# 1fe.0100.0000 constant pci-config-pa	\ PCI configuration space

\ The memory controller's first settings, as the Ultra 5/10's OpenBoot
\ writes them: SDRAM timing; refresh on, 11-bit columns.
h# 0c4a.ab14 constant mc-control1-value
h# 8000.153d constant mc-control0-value

h# 10.0000  constant compressed-copy		\ the image read here from the PROM
d# 256 1meg * constant /bank-window
d# 16 1meg *  constant smallest-bank

begin-obj

label reset-vector
   resident-assembler
   nop nop nop nop
   nop nop nop nop
   0 F: always brif annul nop			\ 20: power-on reset
   nop nop nop nop nop nop
   1 F: always brif annul nop			\ 40: watchdog reset
   nop nop nop nop nop nop
   2 F: always brif annul nop			\ 60: XIR
   nop nop nop nop nop nop
   3 F: always brif annul nop			\ 80: SIR
   nop nop nop nop nop nop
   4 F: always brif annul nop			\ a0: RED state
   nop nop nop nop nop nop
end-code

label sccs-string  sccs-id cscount ", align  end-code
label boot-name$ " bootprom" bounds do i c@ c, loop 0 c, end-code
label obp-name$  " OBP" bounds do i c@ c, loop 0 c, end-code

fload ${BP}/arch/sun4u/ultra5_10/diagprint.fth
fload ${BP}/cpu/sparc/ultra/tlbasm.fth
fload ${BP}/pkg/dropins/sparc/find-dropin.fth
fload ${BP}/arch/sun/reset-dropin.fth

\ A delay: %o5 reads of the auxio register (as the machine's OpenBoot
\ waits for the memory controller). %o4 trashed.
label io-delay
   auxio-pa  %o4		%o3	setx
   begin
      %o3  %g0  bypass-asi	%o4	lda
      %o5  1		%o5	subcc
   0= until
      nop
   retl
   nop
end-code

\ The PCI side, as the Ultra 5/10's own OpenBoot sets it up first: the bus
\ numbers of the Sabre (0, and all below it) and of the APB's two buses
\ (function 1, bus A: 1; function 0, bus B: 2 and up), and the EBus's
\ second window (ttya and the EBus's other devices) at PCI F100.0000, its
\ first (the flash PROM) being at F000.0000 from reset. Configuration
\ space is little-endian. %o0, %o1, %o2 trashed.
label init-reset-pci
   pci-config-pa  %o2	%o1	setx
                   %o1 h#  40 %o2 add   %g0  %o2  %g0  bypass-asi  stba
   %g0 h# ff %o0 add   %o1 h#  41 %o2 add   %o0  %o2  %g0  bypass-asi  stba
                   %o1 h# 818 %o2 add   %g0  %o2  %g0  bypass-asi  stba
   %g0 h#  2 %o0 add   %o1 h# 819 %o2 add   %o0  %o2  %g0  bypass-asi  stba
   %g0 h# ff %o0 add   %o1 h# 81a %o2 add   %o0  %o2  %g0  bypass-asi  stba
                   %o1 h# 918 %o2 add   %g0  %o2  %g0  bypass-asi  stba
   %g0 h#  1 %o0 add   %o1 h# 919 %o2 add   %o0  %o2  %g0  bypass-asi  stba
                   %o1 h# 91a %o2 add   %o0  %o2  %g0  bypass-asi  stba
   h# 1.0814		%o2	set		\ bus 1, device 1: BAR1
   %o1  %o2		%o2	add
   %g0 h# f1 %o0 add   %o0  %o2  %g0  bypass-asi  sta
   retl
   nop
end-code

\ ttya: 9600 baud, 8N1, as the image's uart.fth sets it up.
\ %o0, %o1, %o2 trashed.
label init-reset-ttya
   reset-ttya-pa  %o2	%o1	setx
   %g0 h# c3 %o0 add   %o1 h# 2c %o2 add   %o0  %o2  %g0  bypass-asi  stba
   %g0 h# c0 %o0 add   %o1 h# 3f %o2 add   %o0  %o2  %g0  bypass-asi  stba
   %g0 h# 41 %o0 add   %o1 h# 20 %o2 add   %o0  %o2  %g0  bypass-asi  stba
   %g0 h# 1f %o0 add   %o1 h# 2d %o2 add   %o0  %o2  %g0  bypass-asi  stba
   %g0 h# af %o0 add   %o1 h# 34 %o2 add   %o0  %o2  %g0  bypass-asi  stba
   %g0 h# 38 %o0 add   %o1 h# 2e %o2 add   %o0  %o2  %g0  bypass-asi  stba
   %g0 h# 2c %o0 add   %o1 h# 22 %o2 add   %o0  %o2  %g0  bypass-asi  stba
   %o1 h# 2b %o2 add   %g0  %o2  %g0  bypass-asi  stba
   %g0 h# ff %o0 add   %o1 h# 3a %o2 add   %o0  %o2  %g0  bypass-asi  stba
                    %o1 h# 3b %o2 add   %o0  %o2  %g0  bypass-asi  stba
   %g0 h# 80 %o0 add   %o1 h# 39 %o2 add   %o0  %o2  %g0  bypass-asi  stba
   %o1 h# 27 %o2 add   %g0  %o2  %g0  bypass-asi  stba
   %g0 h# 40 %o0 add   %o1 h# 28 %o2 add   %o0  %o2  %g0  bypass-asi  stba
   %g0 h# 41 %o0 add   %o1 h# 20 %o2 add   %o0  %o2  %g0  bypass-asi  stba
   retl
   nop
end-code

\ Clear %o1 bytes of physical memory from %o0 (both 64-byte multiples).
label clear-phys
   %g0  memory-asi		wrasi
   begin
      %o1  h# 40	%o1	subcc
      %o0  %o1		%o2	add
      %g0  %o2  h#  0  %asi  stxa
      %g0  %o2  h#  8  %asi  stxa
      %g0  %o2  h# 10  %asi  stxa
      %g0  %o2  h# 18  %asi  stxa
      %g0  %o2  h# 20  %asi  stxa
      %g0  %o2  h# 28  %asi  stxa
      %g0  %o2  h# 30  %asi  stxa
   0= until
      %g0  %o2  h# 38  %asi  stxa
   retl
   nop
end-code

\ The processor's state after a reset: out of RED state, TL 0, the normal
\ globals, interrupts off, the register windows clean; the MMUs and caches
\ off. (Changing the window changes %o7: the return address is kept in
\ %g1.)
label init-processor
   %g0  h# f			wrpil
   %g0  h# 14			wrpstate	\ PEF, PRIV
   %o7  %g0		%g1	add
   %g0  0			wrtl
   %g0  7			wrcleanwin
   %g0  0			wrotherwin
   %g0  0			wrwstate
   %g0  0			wrcanrestore
   %g0  6			wrcansave
   %g0  0			wrcwp
   %g0  %g0  %g0  h# 45		stxa		\ LSU control: all off
   #Sync			membar
   %g1  8		%g0	jmpl
   nop
end-code

\ The rest of power-on reset, run from the copy at physical 0 (%l0 =
\ membase).
label continue-low
   \ Running at physical 0: the image decompressed to membase + 2 MiB.
   boot-name$ obj-base -  %o1	%o0	setx	\ the name, here
   prom-pa  %o2		%o1	setx		\ the PROM
   2meg			%o2	set
   %l0  %o2		%o2	add		\ to membase + 2 MiB
   compressed-copy	%o3	set		\ read from a copy
   find&copy-dropin		call
   %g0	%g0		%o4	add
   %o0  %g0		%g0	subcc
   0= if  nop
      begin again  nop			\ no image: stay here
   then

   \ The TLB entries, then the MMUs and caches on.
   %g0			%o0	move		\ 0 -> 0, 4 MiB
   %g0			%o1	move
   %g0  3		%o2	add
   setup-itlb-entry		call
   %g0  0		%o3	add
   %g0			%o0	move
   %g0			%o1	move
   %g0  3		%o2	add
   setup-dtlb-entry		call
   %g0  0		%o3	add
   monvirtbase		%o0	set		\ monvirtbase -> membase, 4 MiB
   %l0			%o1	move
   %g0  3		%o2	add
   setup-itlb-entry		call
   %g0  0		%o3	add
   monvirtbase		%o0	set
   %l0			%o1	move
   %g0  3		%o2	add
   setup-dtlb-entry		call
   %g0  0		%o3	add
   RAMbase		%o0	set		\ RAMbase -> membase + 2 MiB - 64 KiB
   2meg RAMsize-start -	%o1	set
   %l0  %o1		%o1	add
   %g0  1		%o2	add
   setup-dtlb-entry		call
   %g0  0		%o3	add

   %g0  h# f		%o0	add		\ IC, DC, IM, DM
   %o0  %g0  %g0  h# 45		stxa
   #Sync			membar

   \ Into the image: %i0 = membase, %i1 = its size, %i2 = the reset.
   %l0			%i0	move
   8meg			%i1	set
   %i2				rdy
   ROMbase		%o0	set
   %o0 h# 30		%g0	jmpl
   nop
   begin again nop
end-code


label power-on-reset
   init-processor		call
   nop
   init-reset-pci		call		\ (auxio, for the delays, is on the EBus)
   nop

   \ The memory controller.
   mc-control1-pa  %o1	%o2	setx
   mc-control1-value  %o1	%o0	setx
   %o0  %o2  %g0  bypass-asi	stxa
   d# 1000		%o5	set
   io-delay			call
   nop
   mc-control0-pa  %o1	%o2	setx
   mc-control0-value  %o1	%o0	setx
   %o0  %o2  %g0  bypass-asi	stxa
   d# 1000		%o5	set
   io-delay			call
   nop

   init-reset-ttya		call
   nop
   diag-print" "r"nUltra 5/10 OpenBoot reset"

   \ Bank 0's size (%l1): the smallest power of two from 16 MiB at which
   \ a marker written lands on its base.
   %g0  %g0  %g0  memory-asi	stxa		\ base: 0
   smallest-bank	%l1	set
   /bank-window		%l2	set
   begin
      %l1  %g0  %l1  memory-asi	stxa		\ marker at %l1
      %g0  %g0  memory-asi  %l3	ldxa		\ the base
      %l3  %g0		%g0	subcc
      0<> if
         nop
         %l1		%l2	move		\ it repeats: the size
      then
      %l1  1		%l1	sllx
      %l1  %l2		%g0	subcc
   >= until
      nop
   %l2			%l1	move		\ %l1 = bank 0's size
   diag-print" "r"nBank 0: "
   %l1				diag-print-reg

   \ OpenBoot's 8 MiB, at its top: cleared (the memory's ECC), then
   \ physical 0 to 2 MiB (this code, the decompressor's data, the copy).
   8meg			%l0	set
   %l1  %l0		%l0	sub		\ %l0 = membase
   %l0			%o0	move
   8meg			%o1	set
   clear-phys			call
   nop
   %g0			%o0	move
   2meg			%o1	set
   clear-phys			call
   nop

   \ This code to physical 0, as linked, and on from there.
   obp-name$ obj-base -  %o1	%o0	setx
   prom-pa  %o2		%o1	setx
   %o1  %o0		%o0	add		\ the name, in the PROM
   %g0  h# 20		%o2	add		\ to 20 (its data's offset)
   %g0  %g0		%o3	add
   find&copy-dropin		call
   %g0	%g0		%o4	add

   continue-low obj-base -  %o1	%o0	setx	\ on, at physical 0
   %o0  %g0		%g0	jmpl
   nop
end-code
\ The other resets: which it was, then on as from power-on.
label other-reset
   init-processor		call
   nop
   init-reset-pci		call
   nop
   init-reset-ttya		call
   nop
   %g0  0			wrtl
   power-on-reset	always	brif
   nop
end-code

\ Where the reset vectors branch to.
label reset-entries
h# 20 .align
0 L:  %g0 0 wry  power-on-reset always brif annul nop
1 L:  %g0 1 wry  init-reset-pci call nop  init-reset-ttya call nop  print" "r"nWatchdog Reset"r"n"  other-reset always brif annul nop
2 L:  %g0 2 wry  init-reset-pci call nop  init-reset-ttya call nop  print" "r"nExternally Initiated Reset"r"n"  other-reset always brif annul nop
3 L:  %g0 3 wry  init-reset-pci call nop  init-reset-ttya call nop  print" "r"nSoftware Initiated Reset"r"n"  other-reset always brif annul nop
4 L:  %g0 4 wry  init-reset-pci call nop  init-reset-ttya call nop  print" "r"nRED State Exception"r"n"  other-reset always brif annul nop
end-code

end-obj

" reset-vector"		" reset" 	$export-procedure

fload ${BP}/arch/sun/reset-cleanup.fth
