\ Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
\ OpenBoot release it is part of (see LICENSE).
id: @(#)obdiag.fth 1.0 26/11/14
purpose: obdiag, the on-board I/O diagnostics: their state and reports
copyright: Copyright 2026 Andy Taylor

\ An Ultra 10's obdiag: tests of the on-board I/O (the RIO's PCI header and
\ EBus DMA, the Ethernet, the SuperIO's keyboard and mouse UARTs, floppy
\ controller and parallel port, the SAB82532's serial ports, the NVRAM, the
\ CS4231, the CMD646 and the Rage), run from a numbered menu (obdmenu.fth)
\ or as a manufacturing script, reporting as an Ultra 10's does. The tests
\ are in obd-pci.fth, obd-ebus.fth and obd-misc.fth.
\
\ The tests run in an instance of the device's node made without its
\ driver's open (create-dev-instance), so that the drivers stay as they
\ are: an Ultra 10's obdiag instead carries its own open and close methods
\ (and a whole IDE driver) and puts them in the nodes, which only changes
\ what a later open of those nodes does. Each test maps, and gives back,
\ what it uses.
\
\ (Its identification: this file's id line, and the OpenBoot version
\ /openprom gives; an Ultra 10's obdiag carries a version line of its own
\ that it never prints.)

headerless

\ --- The session's state ----------------------------------------------------

false value obd-loaded?		\ The first obdiag (or run-obd-scripts) has run
0 value obd-mfg			\ mfg-mode, diag-level and diag-switch? as
0 value obd-diag-level		\ they were then: an Ultra 10's obdiag takes
0 value obd-diag-switch		\ them once, at its load, for the session
false value script-debug?	\ TEST=, SUBTEST= and BAUDRATE= lines (off at first)
false value ext-lpbk?		\ The external loopback tests enabled
false value obd-script?		\ Running a manufacturing script
0 value obd-t0			\ The milliseconds at the menu's (or script's) start
0 value obd-passes		\ PASSES (1 in the menu; the script's runs done)
0 value obd-errors		\ The script's failures so far (0 in the menu)
0 value test-errors		\ The test's failures (it ends at the first)

\ The test's name, its latest sub-test's and its message, as the failure
\ block gives them. The sub-test and the message stay from one test to the
\ next, as an Ultra 10's do: a test that fails before it names its own
\ shows the last one set.
0 value test$	0 value /test$
0 value subtest$	0 value /subtest$
0 value msg$	0 value /msg$

: msg ( adr len -- )  to /msg$ to msg$  ;

\ (mfg-mode's bits: 01 external, 10 the sub-test menu, 20 bft, 40 sys)
: mfg-ext? ( -- flag )  obd-mfg 1 and 0<>  ;
: ext-tests? ( -- flag )  mfg-ext? ext-lpbk? and  ;

\ --- Output -----------------------------------------------------------------

: u.h ( u -- )  push-hex u. pop-base  ;
: (u.d) ( u -- adr len )  push-decimal (u.) pop-base  ;

: subtest ( adr len -- )
   2dup to /subtest$ to subtest$
   script-debug? if  ." SUBTEST='" type ." '" cr  else  2drop  then
;

\ TTF: whole seconds since the menu (or script) began, in decimal
: .ttf ( -- )  get-msecs obd-t0 - d# 1000 / .d  ;

\ SPEED: the processor's clock-frequency in MHz, three digits, a point and
\ two (0 with no processor node of either name)
: obd-clock ( -- hz )
   " clock-frequency" " /SUNW,UltraSPARC-IIi" node-int if  exit  then
   " clock-frequency" " /SUNW,UltraSPARC" node-int 0= if  0  then
;
: .mhz ( -- )
   obd-clock d# 10.000 /  push-decimal
   <# u# u# ascii . hold u# u# u# u#> type
   pop-base
;

\ The failure block, as an Ultra 10's: a blank line, the markers and fields,
\ ERRORS in the present base, TTF and PASSES in decimal, then the message,
\ which the caller ends.
: .fail-head ( -- )
   cr ." ###OBDIAG_MFG_START###" cr
   ." TEST='" test$ /test$ type ." '" cr
   ." STATUS='FAILED'" cr
   ." SUBTEST='" subtest$ /subtest$ type ." '" cr
   ." ERRORS='" obd-errors 1+ . ." '" cr
   ." TTF='" .ttf ." '" cr
   ." SPEED='" .mhz ."  MHz'" cr
   ." PASSES='" obd-passes .d ." '" cr
   ." MESSAGE='Error: " msg$ /msg$ type
;

\ The end of a failure: in a script its stop line and the count; the test
\ ended (the first failure ends a test, as an Ultra 10's stop count of 1 does).
create obd-failed
: fail-end ( -- )
   obd-script? if
      ." ###OBDIAG_MFG_STOP###" cr
      obd-errors 1+ to obd-errors
   then
   test-errors 1+ to test-errors
   obd-failed throw
;

\ The kinds of failure (each with the message in force)
: fail ( -- )  .fail-head ." '" cr  fail-end  ;
: fail-msg ( adr len -- )  msg fail  ;
\ A value read that differs from the one expected, at an address
: fail-at ( adr exp obs -- )
   .fail-head  ." ; Addr=" rot u.h ." Exp=" swap u.h ." Obs=" u.h ." '" cr
   fail-end
;
: ?fail-at ( adr exp obs -- )  2dup <> if  fail-at  then  3drop  ;
\ A data line stuck
: fail-bit ( bit# -- )
   .fail-head  ." ; Bit " (u.d) type ."  Stuck Low or Open'" cr  fail-end
;
\ Read and expected values, each in two columns (the audio's registers)
: fail-compare ( actual expected -- )
   .fail-head  ." ; Actual=" swap 2 u.r ."   Expected=" 2 u.r ." '" cr  fail-end
;
: ?fail-compare ( actual expected -- )
   2dup <> if  fail-compare  then  2drop
;

\ --- Time -------------------------------------------------------------------

: obd-timed-out? ( when -- flag )  get-msecs - 0<  ;

\ Until xt ( -- flag ) gives true, or ms have passed: true if it did.
: poll ( xt ms -- ok? )
   get-msecs +  begin				( xt when )
      over execute if  2drop true exit  then
      dup obd-timed-out?
   until  2drop false
;

\ --- The device's instance -------------------------------------------------

h# 14 constant ebus-space	\ The EBus's second window (BAR 14)

\ A node's instance with its parents open, its own open not run; 0 if
\ there is no such node
: obd-instance ( path$ -- ihandle | 0 )
   locate-device if  0  else  " " rot create-dev-instance  then
;

\ xt run in an instance of the node, which is then destroyed (whatever
\ the test's end). With no such node, nothing.
: with-node ( path$ xt -- )
   -rot obd-instance ?dup 0= if  drop exit  then	( xt ih )
   dup >r ['] call-package catch			( 0 | x x code )
   dup if  nip nip  then				( code )
   r> destroy-dev-instance  throw
;

\ In an instance of an EBus device: its registers mapped through the EBus
\ node (offset in the second window, size)
: ebus-map ( offset size -- va )  ebus-space swap " map-in" $call-parent  ;
: ebus-unmap ( va size -- )  " map-out" $call-parent  ;

\ DVMA memory: allocated, and mapped for the device (consistent)
: dvma-alloc ( size -- va devadr )
   dup " dma-alloc" $call-parent  tuck swap false " dma-map-in" $call-parent
;
: dvma-free ( va devadr size -- )
   3dup " dma-map-out" $call-parent  nip " dma-free" $call-parent
;
: dvma-sync ( va devadr size -- )  " dma-sync" $call-parent  ;

\ A PCI device's configuration space, in its instance
: cfg-b@ ( offset -- b )  my-space + " config-b@" $call-parent  ;
: cfg-w@ ( offset -- w )  my-space + " config-w@" $call-parent  ;
: cfg-l@ ( offset -- l )  my-space + " config-l@" $call-parent  ;
: cfg-b! ( b offset -- )  my-space + " config-b!" $call-parent  ;
: cfg-w! ( w offset -- )  my-space + " config-w!" $call-parent  ;
: cfg-l! ( l offset -- )  my-space + " config-l!" $call-parent  ;

\ The EBus DMA channels' registers: little-endian words (CSR at 0, address
\ at 4, byte count at 8)
: dma-csr@ ( base -- l )  le-rl@  ;
: dma-csr! ( l base -- )  le-rl!  ;
: dma-acr! ( l base -- )  4 + le-rl!  ;
: dma-bcr! ( l base -- )  8 + le-rl!  ;
\ A channel's reset: RESET for 100 ms, then 0
: dma-reset ( base -- )  h# 80 over dma-csr!  d# 100 ms  0 swap dma-csr!  ;

\ --- The device paths, the console ----------------------------------------

: ebus-path ( -- $ )  " /pci@1f,0/pci@1,1/ebus@1"  ;
: eeprom-path ( -- $ )  " /pci@1f,0/pci@1,1/ebus@1/eeprom@14,0"  ;
: ecpp-path ( -- $ )  " /pci@1f,0/pci@1,1/ebus@1/ecpp@14,3043bc"  ;
: mouse-path ( -- $ )  " /pci@1f,0/pci@1,1/ebus@1/su@14,3062f8"  ;
: kbd-path ( -- $ )  " /pci@1f,0/pci@1,1/ebus@1/su@14,3083f8"  ;
: se-path ( -- $ )  " /pci@1f,0/pci@1,1/ebus@1/se@14,400000"  ;
: net-path ( -- $ )  " /pci@1f,0/pci@1,1/network@1,1"  ;
: fd-path ( -- $ )  " /pci@1f,0/pci@1,1/ebus@1/fdthree@14,3023f0"  ;
: audio-path ( -- $ )  " /pci@1f,0/pci@1,1/ebus@1/SUNW,CS4231@14,200000"  ;
: ide-path ( -- $ )  " /pci@1f,0/pci@1,1/ide@3"  ;
: m64-path ( -- $ )  " /pci@1f,0/pci@1,1/SUNW,m64B@2"  ;

\ Whether a console instance is (an instance of) the node
: console-on? ( ihandle path$ -- flag )
   rot ?dup 0= if  2drop false exit  then		( path$ ih )
   ihandle>phandle -rot locate-device if  drop false exit  then
   =
;
\ The onboard Rage is the console: the video test is then not run
: rage-console? ( -- flag )  stdout @ m64-path console-on?  ;
\ The keyboard (its UART) is the console's input
: kbd-console? ( -- flag )  stdin @ kbd-path console-on?  ;

\ The EBus node's latency timer, which an Ultra 10's obdiag sets (28) each
\ time it opens the EBus node: before each test of an EBus device.
: ebus-latency ( -- )
   ebus-path obd-instance ?dup if		( ih )
      h# 28 h# 0d ['] cfg-b! 3 pick call-package
      destroy-dev-instance
   then
;
headers
