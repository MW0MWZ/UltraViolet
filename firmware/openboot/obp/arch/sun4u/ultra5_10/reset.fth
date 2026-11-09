\ Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
\ OpenBoot release it is part of (see LICENSE).
id: @(#)reset.fth 1.1 26/11/09
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
\      and EBus window (as an Ultra 10 sets them up at
\      first); ttya, for messages.
\   2. The memory probed as an Ultra 10 probes it: the DIMMs found and sized,
\      the column mode chosen (11-bit if it gives the most memory), and
\      OpenBoot's 1 MiB placed at the top of the highest pair side used
\      (membase).
\   3. This code (with the decompressor) copied to physical 0, as it is
\      linked, and run there.
\   4. OpenBoot's Forth image (the "bootprom" drop-in) decompressed to
\      ROMbase (monvirtbase), mapped to membase + 512 KiB.
\   5. Locked TLB entries: 0 to 0 (this code), monvirtbase to membase +
\      512 KiB (512 KiB, the image), RAMbase to membase + 448 KiB (64 KiB);
\      the MMUs and caches on; the image entered at its power-on entry
\      (ROMbase + 30) with %i0 = membase, %i1 = 1 MiB, %i2 = the reset
\      (0 power-on, 1 watchdog, 2 XIR, 3 SIR, 4 RED state; kept in %y
\      until then).
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

\ The memory controller's first settings, as an Ultra 10 has them: SDRAM
\ timing (0626.128a on a processor of mask version below 8.0); refresh on, pairs 0 and 2 present, 10-bit
\ columns (the power-on mode) until the probe below has chosen.
h# 0c4a.ab14 constant mc-control1-value
h# 0626.128a constant mc-control1-value-old
h# 8000.053d constant mc-control0-value

h# 1ff.f100.0000 constant nvram-pa		\ the M48T59 (EBus 0)
h# 1ff.f130.0000 constant superio-pa		\ the Super I/O (EBus 30.0000)
h# 1ff.f130.83f8 constant kbd-uart-pa		\ the keyboard's UART
h# 1ff.f171.0000 constant ebus-710-pa		\ (EBus 71.0000)
h# 1fe.0100.08b0 constant apb-tick-pa		\ the APB's tick register
h# 1fe.0000.f020 constant reset-control-pa
h# 1fe.0000.0000 constant upa-port-id-pa
h# 1ff.f008.0000 constant post-pa		\ the POST, in the PROM's second half
h# 736f.6674.2d72.7374 constant soft-rst-magic	\ "soft-rst", NVRAM 0x10
h# 706f.7374.2d72.756e constant post-run-magic	\ "post-run", for OpenBoot

h# 10.0000  constant compressed-copy		\ the image read here from the PROM
d# 16 1meg *  constant 16meg
d# 32 1meg *  constant 32meg
d# 64 1meg *  constant 64meg

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
   5 F: always brif annul nop			\ c0: the POST's return
   nop nop nop nop nop nop
end-code

label sccs-string  sccs-id cscount ", align  end-code
label boot-name$ " bootprom" bounds do i c@ c, loop 0 c, end-code
label obp-name$  " OBP" bounds do i c@ c, loop 0 c, end-code

fload ${BP}/arch/sun4u/ultra5_10/diagprint.fth
fload ${BP}/cpu/sparc/ultra/tlbasm.fth
fload ${BP}/pkg/dropins/sparc/find-dropin.fth
fload ${BP}/arch/sun/reset-dropin.fth

\ A delay: %o5 reads of the auxio register (as an Ultra 10
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

\ The PCI side, as an Ultra 10 sets it up first: the bus
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

\ A SAB82532 channel (%o1) at 9600 baud, 8N1, as an Ultra 10's reset code
\ sets both up (its table of register values, a command waiting for the
\ last to finish). %o0, %o2 trashed.
label init-sab-channel
   %g0 h# c3 %o0 add   %o1 h# 2c %o2 add   %o0  %o2  %g0  bypass-asi  stba
   %g0 h# c0 %o0 add   %o1 h# 3f %o2 add   %o0  %o2  %g0  bypass-asi  stba
   %o1 h# 20 %o2 add
   begin
      %o2  %g0  bypass-asi  %o0  lduba
      %o0  4		%g0	andcc
   0= until  nop
   %g0 h# 41 %o0 add   %o0  %o2  %g0  bypass-asi  stba
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
   %o1 h# 20 %o2 add
   begin
      %o2  %g0  bypass-asi  %o0  lduba
      %o0  4		%g0	andcc
   0= until  nop
   %g0 h# 41 %o0 add   %o0  %o2  %g0  bypass-asi  stba
   retl
   nop
end-code

\ ttya and ttyb. %o0, %o1, %o2, %g5 trashed.
label init-reset-ttya
   %o7			%g5	move
   reset-ttya-pa  %o2	%o1	setx
   init-sab-channel		call
   nop
   reset-ttya-pa h# 40 +  %o2  %o1  setx
   init-sab-channel		call
   nop
   %g5  8		%g0	jmpl
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

\ The string after the call (as puts), shown only with diag-switch? true
\ (NVRAM byte 1), as an Ultra 10's reset code shows its messages. %o0-%o6
\ trashed.
label dputs
   %o7			%o3	move
   %o3  8		%o3	add
   %o3  %g0		%o6	ldub		\ the count
   ttya-pa  %o2		%o1	setx
   nvram-pa 1 +  %o2	%o5	setx
   %o5  %g0  bypass-asi	%o2	lduba		\ %o2 = diag-switch?
   begin
      %o6  %g0		%g0	subcc
   0>  while
      %o3  1		%o3	add
      %o3  %g0		%o0	ldub
      %o2  %g0		%g0	subcc
      0<> if  nop
         %o7		%o4	move
         putc		call
         nop
         %o4		%o7	move
      then
   repeat
      %o6  1		%o6	sub
   %o3  4		%o3	add
   %o3  3		%o3	andn
   %o3  %g0		%g0	jmpl
   nop
end-code

\ %o0 in hex, an Ultra 10's way: 16 digits, a dot every four, nothing after.
\ %o0-%o6 trashed.
label puthex16
   %o7			%o4	move
   ttya-pa  %o2		%o1	setx
   h# 3c		%o6	set
   %o0			%o3	move
   begin
      %o3  %o6		%o2	srlx
      %o2  h# 0f	%o2	and
      %o2  h# 0a	%g0	subcc
      %o2  ascii 0	%o0	add
      <  if annul
         %o2  ascii a h# 0a -  %o0	add
      then
      putc		call
      nop
      %o6  h# 0f  %g0  andcc
      0=  if %o6  %g0  %g0  subcc
         0<>  if
            ascii .	%o0	move
            putc	call
            nop
         then
      then
      %o6  4		%o6	subcc
   <  until
      nop
   %o4	8		%g0	jmpl
   nop
end-code
\ The same, with diag-switch? true only.
label dputhex16
   nvram-pa 1 +  %o2	%o1	setx
   %o1  %g0  bypass-asi	%o2	lduba
   %o2  %g0		%g0	subcc
   0<> if  nop
      puthex16  always brif
      nop
   then
   retl
   nop
end-code

transient also assembler definitions
\ dprint" text": the text shown with diag-switch? true (dputs).
: dprint" ( -- ) \ string"
   dputs call nop  ," align
;
resident previous definitions

\ #power-cycles (NVRAM 8, 32 bits) counted, as an Ultra 10's reset code counts a
\ hardware power-on. %o0-%o2 trashed.
label count-power-cycle
   nvram-pa 8 +  %o2	%o1	setx
   %o1  %g0  bypass-asi	%o0	lda
   %o0  1		%o0	add
   retl
   %o0  %o1  %g0  bypass-asi	sta
end-code

\ The Super I/O's configuration index register at %o0: %o1 = -1 if it
\ reads 88 and then 00 (as after a reset), else 0.
label superio-index?
   %o0  %g0  bypass-asi	%o1	lduba
   %o1  h# 88		%g0	subcc
   0<> if
      %g0  %g0		%o1	add
      retl
      nop
   then
   %o0  %g0  bypass-asi	%o1	lduba
   %o1  %g0		%g0	subcc
   0<> if
      %g0  %g0		%o1	add
      retl
      nop
   then
   retl
   %g0  1		%o1	sub
end-code

\ The Super I/O (PC87317): its configuration registers found at one of the
\ three places an Ultra 10 looks (398, 15C, 2E) and set as an Ultra 10 sets them (each
\ value written twice). %o0-%o3, %g5 trashed.
label superio-setup
   %o7			%g5	move
   superio-pa h# 398 +  %o2  %o0  setx
   superio-index?		call
   nop
   %o1  %g0		%g0	subcc
   0= if  nop
      superio-pa h# 15c +  %o2  %o0  setx
      superio-index?		call
      nop
      %o1  %g0		%g0	subcc
      0= if  nop
         superio-pa h# 2e +  %o2  %o0  setx
         superio-index?		call
         nop
         %o1  %g0		%g0	subcc
         0= if  nop
            %g5  8	%g0	jmpl		\ none
            nop
         then
      then
   then
   %o0			%o1	move
   %o1  1		%o3	add
   %g0  0  %o2  add   %o2  %o1  %g0  bypass-asi  stba
   %g0 h# f %o2 add   %o2  %o3  %g0  bypass-asi  stba   %o2  %o3  %g0  bypass-asi  stba
   %g0  1  %o2  add   %o2  %o1  %g0  bypass-asi  stba
   %g0 h# 11 %o2 add  %o2  %o3  %g0  bypass-asi  stba   %o2  %o3  %g0  bypass-asi  stba
   %g0  2  %o2  add   %o2  %o1  %g0  bypass-asi  stba
   %g0 h# 80 %o2 add  %o2  %o3  %g0  bypass-asi  stba   %o2  %o3  %g0  bypass-asi  stba
   %g0  3  %o2  add   %o2  %o1  %g0  bypass-asi  stba
   %g0  0  %o2  add   %o2  %o3  %g0  bypass-asi  stba   %o2  %o3  %g0  bypass-asi  stba
   %g0  4  %o2  add   %o2  %o1  %g0  bypass-asi  stba
   %g0  0  %o2  add   %o2  %o3  %g0  bypass-asi  stba   %o2  %o3  %g0  bypass-asi  stba
   %g0  6  %o2  add   %o2  %o1  %g0  bypass-asi  stba
   %g0  0  %o2  add   %o2  %o3  %g0  bypass-asi  stba   %o2  %o3  %g0  bypass-asi  stba
   %g0  7  %o2  add   %o2  %o1  %g0  bypass-asi  stba
   %g0  0  %o2  add   %o2  %o3  %g0  bypass-asi  stba   %o2  %o3  %g0  bypass-asi  stba
   %g0 h# 41 %o2 add  %o2  %o1  %g0  bypass-asi  stba
   #Sync			membar
   %g0 h# b0 %o2 add  %o2  %o3  %g0  bypass-asi  stba   %o2  %o3  %g0  bypass-asi  stba
   %g5  8		%g0	jmpl
   nop
end-code

\ The keyboard's UART (%o0): 1200 baud, 8N1, FIFOs reset, as an Ultra 10's.
\ %o3, %o4 trashed.
label kbd-uart-init
   %o0 1 %o3 add  %g0 %o4 move          %o4 %o3 %g0 bypass-asi stba
   %o0 4 %o3 add                        %o4 %o3 %g0 bypass-asi stba
   %o0 2 %o3 add                        %o4 %o3 %g0 bypass-asi stba
                  %g0 6 %o4 add         %o4 %o3 %g0 bypass-asi stba
                  %g0 1 %o4 add         %o4 %o3 %g0 bypass-asi stba
   %o0 3 %o3 add  %g0 h# 83 %o4 add     %o4 %o3 %g0 bypass-asi stba
                  %g0 h# 60 %o4 add     %o4 %o0 %g0 bypass-asi stba
   %o0 1 %o3 add  %g0 %o4 move          %o4 %o3 %g0 bypass-asi stba
   %o0 3 %o3 add  %g0 3 %o4 add         %o4 %o3 %g0 bypass-asi stba
   %o0 4 %o3 add  %g0 %o4 move          %o4 %o3 %g0 bypass-asi stba
   retl
   nop
end-code
\ %o0 sent to the keyboard (UART %o1): when the transmitter is empty, and
\ until it has gone. %o2, %o3 trashed.
label kbd-send
   %o1  5		%o3	add
   begin
      %o3  %g0  bypass-asi  %o2  lduba
      %o2  h# 20	%g0	andcc
   0<> until  nop
   %o0  %o1  %g0  bypass-asi	stba
   begin
      %o3  %g0  bypass-asi  %o2  lduba
      %o2  h# 40	%g0	andcc
   0<> until  nop
   retl
   nop
end-code
\ A byte from the keyboard (%o0), or -1 when none comes within the APB's
\ tick count 3F9.41F8 (as an Ultra 10 waits). %o1-%o4 trashed.
label kbd-recv
   kbd-uart-pa  %o2	%o0	setx
   apb-tick-pa  %o2	%o1	setx
   h# 3f9.41f8		%o3	set
   %g0  %o1  %g0  h# 1d	stxa		\ the count from 0 (little-endian)
   begin
      %o0  5		%o2	add
      %o2  %g0  bypass-asi  %o2  lduba
      %o2  1		%g0	andcc
      0<> if  nop
         %o0  %g0  bypass-asi  %o0  lduba
         retl
         nop
      then
      %o1  %g0  h# 1d	%o4	ldxa
      %o4  %o3		%g0	subcc
   >= until  nop
   retl
   %g0  1		%o0	sub
end-code
\ "Probing keyboard": a reset sent, then what comes back: %o0 bit 0 if
\ L1 (Stop, 01) was held, bit 1 if D (4F) was. %o0-%o5, %g4, %g5 trashed.
label kbd-probe
   %o7			%g4	move
   kbd-uart-pa  %o2	%o0	setx
   kbd-uart-init		call
   nop
   begin
      kbd-recv			call		\ (anything waiting)
      nop
      %o0  %g0		%g0	subcc
   0< until  nop
   kbd-uart-pa  %o2	%o1	setx
   kbd-send			call
   %g0  1		%o0	add		\ reset
   kbd-recv			call
   nop
   %g0  %g0		%g5	add
   %o0  h# ff		%g0	subcc
   0= if  nop
      kbd-recv			call		\ (the reset's response)
      nop
   then
   begin
      kbd-recv			call
      nop
      %o0  1		%g0	subcc
      0= if  nop
         %g5  1		%g5	or
      then
      %o0  h# 4f		%g0	subcc
      0= if  nop
         %g5  2		%g5	or
      then
      %o0  %g0		%g0	subcc
   0< until  nop
   %g5			%o0	move
   %g4  8		%g0	jmpl
   nop
end-code

\ The E-cache's size (%o0), found through its diagnostic data access as
\ an Ultra 10 finds it. %o1 trashed.
label ecache-size
   %g0  1		%o1	add
   %o1  d# 39		%o1	sllx
   %g0  %o1  %g0  h# 76	stxa
   2meg			%o0	set
   %o0  %o1  %o0  h# 76	stxa
   %o0  1		%o0	srl
   %o0  %o1  %o0  h# 76	stxa
   %o0  1		%o0	srl
   %o0  %o1  %o0  h# 76	stxa
   %o0  1		%o0	srl
   %o0  %o1  %o0  h# 76	stxa
   #Sync			membar
   %g0  %o1  h# 7e	%o0	ldxa
   retl
   nop
end-code
\ Every E-cache tag (of %o0 bytes) invalid. %o0, %o1 trashed.
label ecache-clear-tags
   h# 3.0000		%o1	set
   %o1  %g0  %g0  h# 4e	stxa
   #Sync			membar
   %g0  2		%o1	add
   %o1  d# 39		%o1	sllx
   begin
      %o0  h# 40	%o0	subcc
      %g0  %o1  %o0  h# 76	stxa
      #Sync			membar
   0= until  nop
   retl
   nop
end-code
\ Every I- and D-TLB entry cleared. %o0, %o1 trashed.
label clear-tlbs
   h# 200		%o0	set
   %g0  h# 30		%o1	add
   %g0  %g0  %o1  h# 58	stxa
   #Sync			membar
   %g0  %g0  %o1  h# 50	stxa
   #Sync			membar
   begin
      %o0  8		%o0	subcc
      %g0  %o0  %g0  h# 5d	stxa
      #Sync			membar
      %g0  %o0  %g0  h# 55	stxa
      #Sync			membar
   0= until  nop
   retl
   nop
end-code

\ The processor's state after a reset: out of RED state, TL 0, the normal
\ globals, interrupts off, the register windows clean; the MMUs and caches
\ off. (Changing the window changes %o7: the return address is kept in
\ %g1.)
label init-processor
   %g0  8		%o0	add		\ D-MMU contexts 0
   %g0  %o0  %g0  h# 58	stxa
   #Sync			membar
   %g0  h# 10		%o0	add
   %g0  %o0  %g0  h# 58	stxa
   #Sync			membar
   %g0  h# f			wrpil
   %g0  h# 14			wrpstate	\ PEF, PRIV
   %o7  %g0		%g1	add
   %g0  0			wrtba
   %g0  0  %l6			wrasr		\ SOFTINT 0
   \ Trap levels 5 to 1: TPC, TNPC and TT all ones.
   %g0 5 wrtl  %g0 -1 wrtpc  %g0 -1 wrtnpc  %g0 -1 wrtt
   %g0 4 wrtl  %g0 -1 wrtpc  %g0 -1 wrtnpc  %g0 -1 wrtt
   %g0 3 wrtl  %g0 -1 wrtpc  %g0 -1 wrtnpc  %g0 -1 wrtt
   %g0 2 wrtl  %g0 -1 wrtpc  %g0 -1 wrtnpc  %g0 -1 wrtt
   %g0 1 wrtl  %g0 -1 wrtpc  %g0 -1 wrtnpc  %g0 -1 wrtt
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
   \ Running at physical 0 (the same virtual), the MMUs on: the image
   \ decompressed to ROMbase (membase + 512 KiB).
   dprint" "r"nPC = "
   %o0				rdpc
   dputhex16			call
   nop
   dprint" "r"nDecompressing into Memory "
   boot-name$ obj-base -  %o1	%o0	setx	\ the name, here
   prom-pa  %o2		%o1	setx		\ the PROM
   ROMbase		%o2	set
   compressed-copy	%o3	set		\ read from a copy
   find&copy-dropin		call
   %g0	%g0		%o4	add
   %o0  %g0		%g0	subcc
   0= if  nop
      begin again  nop			\ no image: stay here
   then
   %o0			%l7	move
   dprint" Done"
   dprint" "r"nSize = "
   dputhex16			call
   %l7			%o0	move

   %g0  h# f		%o0	add		\ IC, DC, IM, DM
   %o0  %g0  %g0  h# 45		stxa
   #Sync			membar

   \ Into the image: %i0 = membase, %i1 = its size, %i2 = the reset; the
   \ POST's result, if it ran: %i3 its status, %i4 the address of its
   \ message (in the PROM), %l0 the board descriptor, %i5 = post-run-magic.
   %l0			%i0	move
   /obp-memory		%i1	set
   %i2				rdy
   %g0  h# 15			wrpstate
   %g2			%i3	move
   %g3			%i4	move
   %g4			%l0	move
   %g5			%i5	move
   %g0  h# 14			wrpstate
   ROMbase		%o0	set
   %o0 h# 30		%g0	jmpl
   nop
   begin again nop
end-code

\ Reached at the PROM's own address (mapped to itself), %l0 = membase: the
\ MMUs on, this code copied to physical 0 (mapped to itself) as linked,
\ and on from there.
label mmus-on
   dprint" "r"nMMUs "
   %g0  h# c		%o0	add		\ IM, DM
   %o0  %g0  %g0  h# 45		stxa
   #Sync			membar
   dprint" ON"r"nCopy "
   obp-name$ obj-base -  %o1	%o0	setx
   prom-pa  %o2		%o1	setx
   %o1  %o0		%o0	add		\ the name, in the PROM
   %g0  h# 20		%o2	add		\ to 20 (its data's offset)
   %g0  %g0		%o3	add
   find&copy-dropin		call
   %g0	%g0		%o4	add
   dprint" Done"
   dprint" "r"nPC = "
   %o0				rdpc
   dputhex16			call
   nop
   continue-low obj-base -  %o1	%o0	setx
   %o0  %g0		%g0	jmpl
   nop
end-code


\ The memory probe, as an Ultra 10 does it: with 10-bit
\ columns, each pair (0 at 0, 2 at 256 MiB) and each of its two DIMMs (+0,
\ +8) is looked for and sized; with 11-bit columns, a marker 16 KiB up
\ (column bit 10, PA[14]) shows whether the DIMM's DRAMs have 11-bit
\ columns (it does not land on +0: the DIMM then holds twice what 10-bit
\ columns reach). The routines leave the memory as they found it; every
\ write is read back through the E-cache (direct-mapped, 2 MiB) as an Ultra 10's
\ are, so that a write that wraps round lands where it would in the DRAMs.

\ Mem_Control0's column mode changed: refresh off, then on with the mode
\ toggled; a wait. %o0-%o5, %g5 trashed.
label mc0-toggle-col
   %o7			%g5	move
   mc-control0-pa  %o1	%o2	setx
   %o2  %g0  bypass-asi	%o0	ldxa
   h# 8000.0000		%o1	set
   %o0  %o1		%o0	andn
   %o0  %o2  %g0  bypass-asi	stxa
   h# 8000.1000		%o1	set
   %o0  %o1		%o0	xor
   %o0  %o2  %g0  bypass-asi	stxa
   d# 1000		%o5	set
   io-delay			call
   nop
   %g5  8		%g0	jmpl
   nop
end-code

\ %o0 = PA: patterns written at +0, +8 MiB and +16 MiB; %o0 = -1 if the
\ first reads back, else 0. %o1-%o5, %g1-%g4 trashed.
label dimm-present?
   8meg			%o4	set
   16meg		%o5	set
   %o0  %g0  memory-asi	%g1	ldxa
   %o0  %o4  memory-asi	%g2	ldxa
   %o0  %o5  memory-asi	%g3	ldxa
   h# aaaa.aaaa.5555.5555  %o2  %o1  setx
   %o1  -1		%o2	xor
   h# a5a5.a5a5.a5a5.a5a5  %o3  %g4  setx
   %o1  %o0  %g0  memory-asi	stxa
   %o2  %o0  %o4  memory-asi	stxa
   %g4  %o0  %o5  memory-asi	stxa
   %o0  %g0  memory-asi	%o2	ldxa
   %g0  %g0		%o3	add
   %o1  %o2		%g0	subcc
   0= if  nop
      %g0  1		%o3	sub
   then
   %g1  %o0  %g0  memory-asi	stxa
   %g2  %o0  %o4  memory-asi	stxa
   %g3  %o0  %o5  memory-asi	stxa
   %o0  %g0  memory-asi	%g0	ldxa
   retl
   %o3			%o0	move
end-code

\ %o0 = PA: 64 MiB written at +0, 32 MiB at +64 MiB, 16 MiB at +32 MiB;
\ %o0 = what +0 then holds (a DIMM's size, up to 64 MiB, as 10-bit
\ columns reach it). %o1-%o5, %g1-%g4 trashed.
label dimm-size
   16meg		%g4	set
   32meg		%o4	set
   64meg		%o5	set
   %o0  %g0  memory-asi	%g1	ldxa
   %o0  %o5  memory-asi	%g2	ldxa
   %o0  %o4  memory-asi	%g3	ldxa
   %o5  %o0  %g0  memory-asi	stxa
   %o0  %g0  memory-asi	%g0	ldxa
   %o4  %o0  %o5  memory-asi	stxa
   %o0  %g0  memory-asi	%g0	ldxa
   %o0  %o5  memory-asi	%g0	ldxa
   %g4  %o0  %o4  memory-asi	stxa
   %o0  %g0  memory-asi	%g0	ldxa
   %o0  %o4  memory-asi	%g0	ldxa
   %o0  %g0  memory-asi	%o1	ldxa
   %g1  %o0  %g0  memory-asi	stxa
   %g2  %o0  %o5  memory-asi	stxa
   %g3  %o0  %o4  memory-asi	stxa
   %o0  %g0  memory-asi	%g0	ldxa
   retl
   %o1			%o0	move
end-code

\ %o0 = PA, %o1 = n: 2n written at +0, n at +16 KiB; %o0 = what +0 then
\ holds (n if the second landed on it). %o2-%o5, %g1-%g3 trashed.
label col-alias
   %o1			%o2	move
   %o1  1		%o3	sllx
   2meg			%o4	set
   h# 4000		%o5	set
   %o0  %o5		%o5	add
   %o0  %g0  memory-asi	%g1	ldxa
   %o5  %g0  memory-asi	%g2	ldxa
   %o3  %o0  %g0  memory-asi	stxa
   %o0  %o4  memory-asi	%g0	ldxa
   %o2  %o5  %g0  memory-asi	stxa
   %o0  %o4  memory-asi	%g0	ldxa
   %o5  %o4  memory-asi	%g0	ldxa
   %o0  %g0  memory-asi	%g3	ldxa
   %g1  %o0  %g0  memory-asi	stxa
   %g2  %o5  %g0  memory-asi	stxa
   %o0  %g0  memory-asi	%g0	ldxa
   retl
   %g3			%o0	move
end-code

\ One DIMM, at %o0: %o0 = its size (doubled if its DRAMs have 11-bit
\ columns: what +0 holds after col-alias with 11-bit columns), %o1 = 1 if
\ they have, else 0. %o0-%o5, %g1-%g5, %i4, %i5, %l7 trashed.
label probe-dimm
   %o7			%l7	move
   %o0			%i4	move
   dimm-size			call
   nop
   %o0			%i5	move		\ the size
   mc0-toggle-col		call		\ 11-bit columns
   nop
   %i4			%o0	move
   col-alias			call
   %i5			%o1	move
   %o0			%i4	move
   mc0-toggle-col		call		\ 10-bit again
   nop
   %i4			%o0	move
   %i4  %i5		%g0	subcc
   0<> if
      %g0  %g0		%o1	add
      %g0  1		%o1	add
   then
   %l7  8		%g0	jmpl
   nop
end-code

label power-on-reset
   init-processor		call
   nop
   \ What the POST's return left in %i2-%i5 (window 0, kept through the
   \ reset): kept in the alternate globals, which nothing here uses, for
   \ OpenBoot (continue-low); dropped below unless this is the software
   \ power-on reset after it.
   %g0  h# 15			wrpstate	\ PEF, PRIV, AG
   %i2			%g2	move
   %i3			%g3	move
   %i4			%g4	move
   %i5			%g5	move
   %g0  h# 14			wrpstate
   init-reset-pci		call		\ (auxio, for the delays, is on the EBus)
   nop
   ebus-710-pa  %o2	%o1	setx		\ (all ones, as an Ultra 10)
   %g0  1		%o0	sub
   %o0  %o1  %g0  h# 1d	sta
   %o1  4		%o1	add
   %o0  %o1  %g0  h# 1d	sta
   %o1  4		%o1	add
   %o0  %o1  %g0  h# 1d	sta
   superio-setup		call
   nop
   init-reset-ttya		call
   nop

   \ The kind of start, as an Ultra 10 tells it (shown with diag-switch? true):
   \ "soft-rst" in NVRAM 0x10 (left by the POST's return), cleared; else
   \ the reset control's status bits. %g6 = -1 software power-on, 1 a
   \ power-on (hardware, button, or no status bits), else 0. A hardware
   \ power-on counts in #power-cycles.
   %g0  %g0		%g6	add
   nvram-pa h# 10 +  %o2  %o1  setx
   %o1  %g0  bypass-asi	%o3	ldxa
   soft-rst-magic  %o2	%o0	setx
   %o0  %o3		%g0	subcc
   0= if  nop
      dprint" Software Power ON"r"n"
      %g0  1		%g6	sub
   then
   nvram-pa h# 10 +  %o2  %o1  setx
   %g0  %o1  %g0  bypass-asi	stxa
   %g6  %g0		%g0	subcc
   0= if  nop
      %g0  h# 15		wrpstate	\ no POST result
      %g0			%g5	move
      %g0  h# 14		wrpstate
      reset-control-pa  %o2  %o1  setx
      %o1  %g0  bypass-asi	%g1	ldxa
      %g1  %g0		%g0	subcc
      0= if  nop
         dprint" RST CTL=0 Assuming Hardware Power ON"r"n"
         count-power-cycle	call
         nop
         %g0  1		%g6	add
      else  nop
         h# 8000.0000	%o0	set
         %g1  %o0	%g0	andcc
         0<> if  nop
            dprint" Hardware Power ON"r"n"
            count-power-cycle	call
            nop
            %g0  1	%g6	add
         then
         h# 2000.0000	%o0	set
         %g1  %o0	%g0	andcc
         0<> if  nop
            dprint" Software XIR"r"n"
         then
         h# 1000.0000	%o0	set
         %g1  %o0	%g0	andcc
         0<> if  nop
            dprint" Button Power ON"r"n"
            %g0  1	%g6	add
         then
         h# 0800.0000	%o0	set
         %g1  %o0	%g0	andcc
         0<> if  nop
            dprint" Button XIR"r"n"
         then
      then
   then
   auxio-pa  %o2	%o1	setx		\ (all ones, as an Ultra 10)
   %g0  1		%o0	sub
   %o0  %o1  %g0  bypass-asi	sta

   \ The memory controller.
   %o3				rdver
   %o3  d# 28		%o3	srlx
   %o3  h# f		%o3	and
   mc-control1-value	%o0	set
   %o3  8		%g0	subcc
   < if  nop
      mc-control1-value-old  %o0	set
   then
   mc-control1-pa  %o1	%o2	setx
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


   \ E-cache error reporting off, the AFSR cleared, the UPA configuration's
   \ E-cache field from its size (an Ultra 10: 7 less the steps from 256 KiB; a
   \ size it doesn't know stops at 0 here, where an Ultra 10's would loop).
   %g0  %g0  %g0  h# 4b	stxa
   %g0  1		%o0	sub
   %o0  %g0  %g0  h# 4c	stxa
   ecache-size			call
   nop
   h# 4.0000		%g4	set
   %g0  7		%g3	add
   begin
      %g3  1		%g3	sub
      %g4  %o0		%g0	subcc
      0<> if  nop
         %g3  %g0	%g0	subcc
      then
   0= until
      %g4  1		%g4	sllx
   %g3  d# 33		%g3	sllx
   %g3  %g0  %g0  h# 4a	stxa
   #Sync			membar

   dputs			call		\ the PROM's version
   nop
   sccs-id cscount ", align

   \ A power-on: the E-cache's tags invalid, the keyboard probed (L1 held
   \ skips the POST, L1-D sets diag-switch? unless security-mode is set),
   \ and with diag-switch? (and diag-level not off) the POST: in the
   \ PROM's second half (1ff.f008.0000, "POST" at +10, entered at +20 with
   \ %o0 = 1, diag-level (40 at most) << 8, mfg-mode << 16), as an Ultra 10's.
   \ %l5 = -1 to run it, -2 forced by L1-D.
   %g6  %g0		%g0	subcc
   > if  nop
      ecache-size		call
      nop
      ecache-clear-tags		call
      nop
      nvram-pa 1 +  %o2	%o1	setx
      %o1  %g0  bypass-asi  %o0  lduba
      %g0  %g0		%l5	add
      %o0  %g0		%g0	subcc
      0<> if  nop
         %g0  1		%l5	sub
      then
      dprint" "r"nProbing keyboard "
      kbd-probe			call
      nop
      %o0			%l6	move
      dprint" Done"
      %l6  1		%g0	andcc
      0<> if  nop
         %l6  2		%g0	andcc
         0<> if  nop
            nvram-pa h# 50 +  %o2  %o1  setx
            %o1  %g0  bypass-asi  %o0  lduba
            %o0  %g0	%g0	subcc
            0= if  nop
               print" "r"n"r"n"
               print" Setting diag-switch? because of L1-D keyboard command."
               print" "r"n"
               nvram-pa 1 +  %o2  %o1  setx
               %g0  h# ff	%o0	add
               %o0  %o1  %g0  bypass-asi  stba
               %g0  2	%l5	sub
            then
         else  nop
            dprint" "r"n"r"n"
            dprint" Skipping POST because of L1 keyboard command."r"n"
            %g0  %g0	%l5	add
         then
      then
      %l5  %g0		%g0	subcc
      0<> if  nop
         nvram-pa 2 +  %o2  %o1  setx
         %o1  %g0  bypass-asi  %g2  lduba	\ diag-level
         %l5  2		%g0	addcc
         0<> if  nop
            %g2  %g0	%g0	subcc
            0= if  nop
               %g0  %g0	%l5	add		\ off: no POST
            then
            %g2  h# 40	%g0	andcc
            0<> if  nop
               %g0  h# 40	%g2	add
            then
         then
         %g2  8		%g2	sllx
         %g0  1		%g1	add
         %g1  %g2		%g1	or
         nvram-pa 3 +  %o2  %o1  setx
         %o1  %g0  bypass-asi  %g2  lduba	\ mfg-mode
         %g2  d# 16		%g2	sllx
         %g1  %g2		%g1	or
         %l5  %g0		%g0	subcc
         0<> if  nop
            print" "r"n%o0 = "
            puthex16		call
            %g1			%o0	move
            print" "r"n"
            print" "r"nExecuting Power On SelfTest"r"n"r"n"
            reset-control-pa  %o2  %o0  setx	\ (an Ultra 10 writes it to the
            %o0  %g0  bypass-asi  %o1  ldxa	\ UPA port ID, read-only)
            upa-port-id-pa  %o2  %o0  setx
            %o1  %o0  %g0  bypass-asi  stxa
            post-pa  %o2	%g7	setx
            %g7  h# 10		%o0	add
            %o0  %g0  bypass-asi  %o0  lda
            h# 504f.5354	%o1	set		\ "POST"
            %o0  %o1		%g0	subcc
            0= if  nop
               %g7  0			wrtba
               %g1		%o0	move
               %g7  h# 20	%g0	jmpl
               %g0  %g0		%g7	add
            then
            print" There is no POST in this PROM."r"n"
         then
      then
   then

   dprint" "r"nClearing E$ Tags "
   ecache-size			call
   nop
   ecache-clear-tags		call
   nop
   dprint"  Done"
   dprint" "r"nClearing I/D TLBs "
   clear-tlbs			call
   nop
   dprint" Done"
   dprint" "r"nProbing Memory "

   \ The memory, as an Ultra 10 probes it (the routines above). For each pair, a
   \ 9-bit field in %l2 (pair 0's above pair 2's): bits 0-6 a DIMM's size
   \ in 8 MiB units (the smaller of the two, both sides), bit 7 both
   \ DIMMs with 11-bit columns, bit 8 both with a second side (+512 MiB;
   \ An Ultra 10 looks for the second DIMM's at +512 MiB + 16, which is the first
   \ DIMM's: followed). Of two DIMMs whose columns differ, the 11-bit one
   \ is taken at its 10-bit size.
   %g0  %g0		%l2	add		\ the fields
   %g0  %g0		%l3	add		\ any DIMMs
   %g0  %g0		%l4	add		\ the pair's base
   %g0  2		%l5	add
   begin
      %l2  9		%l2	sllx
      dimm-present?		call
      %l4			%o0	move
      %o0  %g0		%g0	subcc
      0<> if  nop
         dimm-present?		call
         %l4  8		%o0	add
         %o0  %g0		%g0	subcc
         0<> if  nop
            probe-dimm		call		\ the first DIMM
            %l4			%o0	move
            %o0			%l6	move
            %o1			%l1	move
            h# 2000.0000	%o1	set
            dimm-present?	call
            %l4  %o1		%o0	add
            %o0  %g0		%g0	subcc
            0<> if
               %g0  %g0		%i2	add
               %l6  1		%l6	sllx
               %g0  1		%i2	add
            then
            probe-dimm		call		\ the second
            %l4  8		%o0	add
            %o0			%i0	move
            %o1			%i1	move
            h# 2000.0010	%o1	set
            dimm-present?	call
            %l4  %o1		%o0	add
            %o0  %g0		%g0	subcc
            0<> if
               %g0  %g0		%i3	add
               %i0  1		%i0	sllx
               %g0  1		%i3	add
            then
            %i1  %l1		%g0	subcc	\ columns differing
            0<> if  nop
               %i1  %g0		%g0	subcc
               0<> if  nop
                  %i0  1	%i0	srlx
               else  nop
                  %l6  1	%l6	srlx
               then
               %g0  %g0		%i1	add
            then
            %i1  %g0		%g0	subcc
            0<> if  nop
               %l2  h# 80	%l2	or
            then
            %i0  %l6		%g0	subcc	\ the smaller
            <= if  nop
               %i0		%l6	move
            then
            %l6  d# 23		%o0	srlx
            %l2  %o0		%l2	or
            %i2  %i3		%g0	andcc
            0<> if  nop
               %l2  h# 100	%l2	or
            then
            %g0  1		%l3	add
         then
      then
      h# 1000.0000	%o0	set
      %l4  %o0		%l4	add
      %l5  1		%l5	subcc
   0= until
      nop
   %l3  %g0		%g0	subcc
   0= if  nop
      print" "r"nNo memory found"r"n"		\ (an Ultra 10's case unknown)
      begin again  nop
   then

   \ The column mode: 11-bit if the 11-bit DIMMs (%i4, 8 MiB units) give at
   \ least what 10-bit columns would give all of them (%i5, and half of
   \ %i4). %l7 = 80 for 11-bit, 0.
   %g0  %g0		%i4	add
   %g0  %g0		%i5	add
   %l2			%o0	move
   %g0  2		%o3	add
   begin
      %o0  h# 1ff		%o1	and
      %o1  %g0		%g0	subcc
      0<> if  nop
         %o1  h# 7f	%o2	and
         %o1  h# 80	%g0	andcc
         0<> if  nop
            %i4  %o2	%i4	add
         else  nop
            %i5  %o2	%i5	add
         then
      then
      %o0  9		%o0	srlx
      %o3  1		%o3	subcc
   0= until
      nop
   %i4  1		%o2	srlx
   %i5  %o2		%o2	add
   %g0  %g0		%l7	add
   %i4  %o2		%g0	subcc
   >= if  nop
      %g0  h# 80		%l7	add
   then

   \ OpenBoot's memory: of the pairs' sides the mode uses (10-bit DIMMs not
   \ in 11-bit mode), the highest-addressed (from 768 MiB down: a pair's
   \ second side only if both its DIMMs have one), as an Ultra 10 chooses: %i0 its
   \ base, %i1 its size.
   %g0  %g0		%i0	add
   %g0  %g0		%o3	add
   h# 3000.0000		%o0	set
   begin
      %o0  d# 28		%o1	srlx
      %o1  1		%o1	and
      %g0  9		%o2	add
      %o1  %g0		%g0	subcc
      0<> if  nop
         %g0  %g0	%o2	add		\ pair 2's field, else pair 0's
      then
      %l2  %o2		%o3	srlx
      %o3  h# 1ff		%o3	and
      %o3  %g0		%g0	subcc
      0<> if  nop
         %g0  1		%o4	sub		\ usable
         %l7  %g0		%g0	subcc
         0<> if  nop
            %o3  h# 80	%g0	andcc
            0= if  nop
               %g0  %g0	%o4	add
            then
         then
         %o4  %g0		%g0	subcc
         0<> if  nop
            h# 2000.0000	%o5	set
            %o3  h# 100	%g0	andcc
            0<> if  nop
               %o0  %o5	%g0	andcc
               0<> if  nop
                  %o0	%i0	move
                  %g0  %g0	%o0	add
               then
            else  nop
               %o0  %o5	%g0	andcc
               0= if  nop
                  %o0	%i0	move
                  %g0  %g0	%o0	add
               then
            then
         then
      then
      h# 1000.0000	%o5	set
      %o0  %o5		%o0	subcc
   0< until
      nop
   %o3  h# 7f		%i1	and
   %i1  d# 23		%i1	sllx
   %o3  h# 100		%g0	andcc
   0<> if  nop
      %i1  1		%i1	srlx
   then
   %l7  %g0		%g0	subcc
   0= if  nop
      %o3  h# 80	%g0	andcc
      0<> if  nop
         %i1  1	%i1	srlx
      then
   then
   %i1  1		%i1	sllx		\ two DIMMs
   %l7  %g0		%g0	subcc
   0<> if  nop
      mc0-toggle-col		call		\ 11-bit columns
      nop
   then
   %i0  %i1		%l1	add		\ %l1 = its top

   \ OpenBoot's 1 MiB, at its top: cleared (the memory's ECC), then
   \ physical 0 to 2 MiB (this code, the decompressor's data, the copy).
   /obp-memory		%l0	set
   %l1  %l0		%l0	sub		\ %l0 = membase
   %l0			%o0	move
   /obp-memory		%o1	set
   clear-phys			call
   nop
   %g0			%o0	move
   2meg			%o1	set
   clear-phys			call
   nop

   dprint" Done"
   dprint" "r"nMEM BASE = "
   dputhex16			call
   %i0			%o0	move
   dprint" "r"nMEM SIZE = "
   dputhex16			call
   %i1			%o0	move
   mc-control0-pa  %o1	%o2	setx
   %o2  %g0  bypass-asi	%o0	ldxa
   h# 1000		%o1	set
   %o0  %o1		%g0	andcc
   0<> if  nop
      dprint" "r"n11-Column Mode Enabled "
   then

   \ The TLB entries: the PROM to itself, 0 to 0 (4 MiB: this code, once
   \ copied), monvirtbase to membase + 512 KiB (512 KiB, the image),
   \ RAMbase to membase + 448 KiB (64 KiB); then on at the PROM's own
   \ address.
   prom-pa  %o1		%o0	setx
   prom-pa  %o2		%o1	setx
   %g0  3		%o2	add
   setup-itlb-entry		call
   %g0  0		%o3	add
   prom-pa  %o1		%o0	setx
   prom-pa  %o2		%o1	setx
   %g0  3		%o2	add
   setup-dtlb-entry		call
   %g0  0		%o3	add
   %g0			%o0	move
   %g0			%o1	move
   %g0  3		%o2	add
   setup-itlb-entry		call
   %g0  0		%o3	add
   %g0			%o0	move
   %g0			%o1	move
   %g0  3		%o2	add
   setup-dtlb-entry		call
   %g0  0		%o3	add
   monvirtbase		%o0	set
   /obp-image		%o1	set
   %l0  %o1		%o1	add
   %g0  2		%o2	add
   setup-itlb-entry		call
   %g0  0		%o3	add
   monvirtbase		%o0	set
   /obp-image		%o1	set
   %l0  %o1		%o1	add
   %g0  2		%o2	add
   setup-dtlb-entry		call
   %g0  0		%o3	add
   RAMbase		%o0	set
   /obp-image RAMsize-start -	%o1	set
   %l0  %o1		%o1	add
   %g0  1		%o2	add
   setup-dtlb-entry		call
   %g0  0		%o3	add
   prom-pa mmus-on obj-base - +  %o1  %o0  setx
   %o0  %g0		%g0	jmpl
   nop
end-code
\ The POST's return (its +C0 entry here): %o0 its status, %o1 the address
\ of its message, %o2 the board descriptor; kept in window 0's %i2-%i4,
\ with post-run-magic in %i5, and shown; then "soft-rst" left in the NVRAM
\ (0x10) and a software power-on reset, as an Ultra 10 does.
label post-return
   %g0  h# 14			wrpstate
   %o0			%g2	move
   %o1			%g3	move
   %o2			%g4	move
   %g0  0			wrcwp
   %g2			%i2	move
   %g3			%i3	move
   %g4			%i4	move
   post-run-magic  %o1	%i5	setx
   print" "r"n"r"nPower On Selftest Completed"
   print" "r"n    Status  = "
   puthex16			call
   %i2			%o0	move
   print"  "
   puthex16			call
   %i3			%o0	move
   print"  "
   puthex16			call
   %i4			%o0	move
   print" "r"n"r"n"
   nvram-pa h# 10 +  %o2  %o1  setx
   soft-rst-magic  %o2	%o0	setx
   %o0  %o1  %g0  bypass-asi	stxa
   reset-control-pa  %o2  %o1  setx
   %g0  %o1  %g0  bypass-asi	stxa
   %g0  1		%o0	sub
   %o0  %o1  %g0  bypass-asi	stxa
   begin again  nop
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
5 L:  post-return always brif annul nop
end-code

end-obj

" reset-vector"		" reset" 	$export-procedure

fload ${BP}/arch/sun/reset-cleanup.fth
