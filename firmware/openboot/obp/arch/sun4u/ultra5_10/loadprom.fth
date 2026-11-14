\ Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
\ OpenBoot release it is part of (see LICENSE).
id: @(#)loadprom.fth 1.1 26/11/11
purpose: Load file for the Sun Ultra 5 and Ultra 10 (UltraSPARC IIi)
copyright: Copyright 2026 Andy Taylor

\ The OpenBoot image for the Ultra 5 and 10, built as the release's sun4v
\ platforms are, with the machine-dependent parts the Ultra 5/10's own.

transient
[define] assembler?
fload debug.fth
resident

fload ${BP}/os/unix/simforth/findnext.fth

fload ${BP}/fm/lib/loadcomm.fth		\ Generic CPU-independent Forth tools
fload ${BP}/os/sun/sparc/loadmach.fth	\ CPU and OS-specific extensions
fload ${BP}/os/sun/sparc/loadfw.fth	\ Platform-independent Open Firmware

\ ===========================================================================
\ Up to this point, nothing machine-dependent is loaded.

fload ${BP}/arch/sun4u/ultra5_10/sysinfo.fth

d# 256 1meg * constant 256meg
d# 128 1meg * constant 128meg
d#  64 1meg * constant  64meg
d#  16 1meg * constant  16meg
d#   8 1meg * constant   8meg
d#   4 1meg * constant   4meg
d#   2 1meg * constant   2meg
h# 10.0000 constant ROMsize		\ The flash PROM: 1 MiB

alias obmem 0
alias obio 0

fload ${BP}/cpu/sparc/ultra/loadultra.fth

fload ${BP}/arch/sun4u/ultra5_10/uart.fth
fload ${BP}/arch/sun4u/ultra5_10/virtaddrs.fth

fload ${BP}/os/bootprom/loadlist.fth	\ S Virtual, physical memory allocators
fload ${BP}/os/bootprom/availmem.fth
fload ${BP}/arch/sun4u/ultra5_10/ramforth.fth	\ S Ramforth
fload ${BP}/os/bootprom/allocmor.fth
fload ${BP}/os/bootprom/msgbuf.fth
fload ${BP}/arch/sun4u/ultra5_10/msgemit.fth	\ ttya's early text kept

fload ${BP}/arch/sun4s/reentry-table.fth
fload ${BP}/arch/sun4u/cpustruct.fth		\ S Allocate cpu structs

fload ${BP}/cpu/sparc/ultra/savecpu.fth
fload ${BP}/arch/sun4u/asmmacros.fth

fload ${BP}/arch/sun4u/catchexc.fth		\ S pssave and rssave
fload ${BP}/arch/sun4u/mmumiss.fth
fload ${BP}/arch/sun4u/ultra5_10/mapdi.fth	\ Mapping for drop-in drivers
fload ${BP}/os/bootprom/dropin.fth		\ Drop-in driver support
fload ${BP}/pkg/decompressor/dropin.fth

fload ${BP}/arch/sun4u/trans.fth
fload ${BP}/arch/sun4u/traptable.fth
fload ${BP}/arch/sun4u/fieldberr.fth		\ Bus error handler for probing
fload ${BP}/os/bootprom/pdump.fth		\ physical dump

fload ${BP}/os/stand/sysuart.fth
fload ${BP}/cpu/sparc/ultra/tlbasm.fth
fload ${BP}/arch/sun4u/ultra5_10/fastfill.fth

fload ${BP}/arch/sun4u/arcbpsup.fth		\ S Arch-dependent breakpoint support
fload ${BP}/arch/sun4u/reenter.fth		\ S Getting back to Forth from Unix
\ /openprom: an Ultra 10 has decode-complete there, made before model and
\ version as its are.
" /openprom" find-device
   0 0 " decode-complete" property
device-end
fload ${BP}/arch/sun/model.fth			\ /openprom model and version props.
fload ${BP}/arch/sun4u/ultra5_10/msloop.fth	\ Delays of n milliseconds

fload ${BP}/arch/sun4u/ultra5_10/machine-init.fth
fload ${BP}/arch/sun4u/forthint.fth		\ S alarm trap handler

fload ${BP}/arch/sun4u/ultra5_10/uniproc.fth
fload ${BP}/arch/sun4u/slavecpu.fth

fload ${BP}/arch/sun4u/ultra5_10/boot.fth	\ SI startup code

fload ${BP}/arch/sun4u/startcpu.fth
fload ${BP}/arch/sun4u/switchcpu.fth
fload ${BP}/arch/sun4u/quark/le-access.fth	\ little endian access code.

fload ${BP}/dev/builtin.fth

stand-init: Loading Support Packages
   diagnostic-mode? if
      ." Loading Support Packages: "
   then
   " support-pkg" do-drop-in
   diagnostic-mode? if  cr  then
;

stand-init: Loading Builtin Devices
   " onboard-devices" ['] builtin-drivers-package find-method if
      execute
   then
;

fload ${BP}/arch/sun4u/ultra5_10/loadconfig.fth
fload ${BP}/arch/sun4u/ultra5_10/diagmsg.fth	\ the probing messages
fload ${BP}/arch/sun/idprom.fth			\ IDPROM layout
fload ${BP}/arch/sun4u/ultra5_10/nvram.fth	\ NVRAM and IDPROM packages
fload ${BP}/arch/sun4u/ultra5_10/devalias.fth	\ Device aliases
fload ${BP}/arch/sun4u/ultra5_10/cpu.fth	\ The processor's node

' diag-switch? is diagnostic-mode?

\ The firmware's messages at the release's "min" verbosity, always: an Ultra 10
\ has no verbosity variable.
stand-init: Firmware verbosity
   VRBS-MIN to fw-verbosity
;

fload ${BP}/arch/sun4u/ultra5_10/memprobe.fth	\ S memory sizer
fload ${BP}/os/bootprom/scrubmem.fth
fload ${BP}/arch/sun4u/ultra5_10/memscrub.fth	\ its display, as an Ultra 10's

fload ${BP}/arch/sun/keystore.fth		\ Security keystore

headers
: bzero-region  ( va len -- ) 2drop  ;
: btouch-region ( va len -- ) 2drop  ;

fload ${BP}/arch/sun4j/nvram-personality.fth

\ (The release's platforms keep the flash PROM mapped from here on; an
\ Ultra 10 maps it only while drop-ins are read, mapdi.fth.)

stand-init: cpu-devices-
   " cpu-devices-" do-drop-in
;

fload ${BP}/pkg/fcode/obsfcode.fth
fload ${BP}/arch/sun4u/retained.fth		\ S retained memory allocator
fload ${BP}/arch/sun4u/ultra5_10/rootnexus.fth
fload ${BP}/dev/pci/knownprops.fth

stand-init: cpu-devices+
   " cpu-devices+" do-drop-in
;

0 value xir-reset?			\ XXXX

fload ${BP}/cpu/sparc/init-c9.fth	\ S Init. C Stack Pointer
fload ${BP}/arch/sun4u/starthacks.fth	\ XXXX
fload ${BP}/arch/sun4u/unixmap.fth	\ MMU initialization
fload ${BP}/arch/sun4u/reboot.fth	\ S Reboot Info
fload ${BP}/arch/sun4u/power-off.fth	\ S Power-off client service
fload ${BP}/arch/sun4u/consredir.fth

\ Packages
fload ${BP}/pkg/loadpkg.fth
fload ${BP}/pkg/dropins/loadpkg.fth
\ The drop-ins package reads drop-ins through the flash PROM's device
" /packages/dropins" find-device
   " /pci@1f,0/pci@1,1/ebus@1/flashprom@10,0:" encode-string " source" property
device-end
fload ${BP}/pkg/selftest/selftest.fth

\ /chosen's properties in the order an Ultra 10 makes them (its .properties
\ lists them newest first): stdout-#lines, bootpath, bootargs, memory, mmu,
\ stdin, stdout. The release makes them as its files load (console.fth,
\ clntmem.fth, bootparm.fth); those after stdout-#lines are made again here.
" /chosen" find-device
   " stdin" delete-property  " stdout" delete-property
   " memory" delete-property  " mmu" delete-property
   " bootpath" delete-property  " bootargs" delete-property
device-end
' path-buf	" bootpath"	chosen-string
' args-buf	" bootargs"	chosen-string
' memory-node	" memory"	chosen-variable
' mmu-node	" mmu"		chosen-variable
' stdin		" stdin"	chosen-variable
' stdout	" stdout"	chosen-variable

fload ${BP}/arch/sun4u/ultra5_10/probeall.fth
fload ${BP}/dev/scsi/probescsi.fth      \ probe-scsi
fload ${BP}/pkg/inet/watchnet.fth	\ watch-net
fload ${BP}/dev/ide/probe.fth		\ probe-ide
fload ${BP}/arch/sun4u/ultra5_10/diag.fth	\ .speed, probe-pci, ...

fload builtin.fth

fload ${BP}/arch/sun4u/ultra5_10/post.fth	\ system-tests, show-post-results
fload ${BP}/arch/sun4u/ultra5_10/obdiag.fth	\ obdiag: its state and reports,
fload ${BP}/arch/sun4u/ultra5_10/obd-pci.fth	\ its tests,
fload ${BP}/arch/sun4u/ultra5_10/obd-ebus.fth
fload ${BP}/arch/sun4u/ultra5_10/obd-misc.fth
fload ${BP}/arch/sun4u/ultra5_10/obdmenu.fth	\ its menus and script

true value system-test-ok?

fload ${BP}/arch/sun4u/startup.fth \ S misc. startup stuff
' .post-failure is pre-boot-hook		\ (post.fth): a failed POST's cause
' (.memory-speed) is .memory-speed	\ (memprobe.fth): the DRAMs' speed

\ After an error at the ok prompt an Ultra 10 empties the data stack, but
\ not after an undefined word's ("xyzzy ?"); the release's interpreter
\ (fm/kernel/ansio.fth) keeps it after any.
headerless
: .error-clear ( error# -- )  dup d# -13 <> >r  .error  r>  if  clear  then  ;
patch .error-clear .error (interact)


headers
fload ${BP}/arch/sun4u/ultra5_10/console-tty.fth

headerless
fload ${BP}/arch/sun4u/ultra5_10/cleanup.fth
