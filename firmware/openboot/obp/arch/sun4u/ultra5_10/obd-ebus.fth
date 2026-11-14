\ Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
\ OpenBoot release it is part of (see LICENSE).
id: @(#)obd-ebus.fth 1.0 26/11/14
purpose: obdiag's tests of the EBus devices: SuperIO, serial ports, NVRAM
copyright: Copyright 2026 Andy Taylor

headerless

\ A test run with what it maps given back whatever its end: open, then
\ the test, then close (open and close ( -- )).
: run-with ( test-xt open-xt close-xt -- )
   >r  catch ?dup if  r> execute  throw  then	( test-xt )
   catch  r> execute  throw
;

\ --- Keyboard and Mouse (keyboard_test, mouse_test), in the mouse's node ---
\
\ The SuperIO's two 16550-compatible UARTs (at 1200 baud, 8N1, the FIFOs
\ on) looped back: the keyboard's to itself, internally (its MCR's LOOP)
\ and through a plug; the keyboard's to the mouse's through the loopback
\ connector. The keyboard's UART is taken at the mouse's + 100 (3063f8),
\ as an Ultra 10's obdiag takes it: the SuperIO decodes only the ISA
\ address's low bits, so that this reaches the UART at 3f8, the keyboard's
\ (su@14,3083f8 in the device tree).

0 value su-mouse		\ The mouse's UART
0 value su-kbd			\ The keyboard's
false value su-mapped?

: su-open ( -- )
   " address" get-my-property if
      my-address my-space 8 " map-in" $call-parent  true to su-mapped?
   else
      decode-int nip nip  false to su-mapped?
   then  to su-mouse
   su-mouse h# 100 + to su-kbd
;
: su-close ( -- )
   su-mapped? if  su-mouse 8 " map-out" $call-parent  false to su-mapped?  then
;

: uart-init ( base -- )
   0 over 1+ rb!  0 over 4 + rb!  0 over 2+ rb!  1 over 2+ rb!
   h# 83 over 3 + rb!  h# 60 over rb!  0 over 1+ rb!
   3 over 3 + rb!  0 swap 4 + rb!
;
: uarts-init ( -- )  su-kbd uart-init  su-mouse uart-init  ;

\ A line status bit, up to 20 tries a millisecond apart
: lsr-wait ( base bit -- ok? )
   d# 20 0 do
      over 5 + rb@ over and if  2drop true unloop exit  then
      1 ms
   loop  2drop false
;
\ 128 characters (0-7f) sent by one UART and received by the other (or
\ the same); a wrong one at the receiver's address
: uart-loop ( tx rx -- )
   h# 80 0 do
      " Timeout transmitting a character" msg
      over h# 20 lsr-wait 0= if  fail  then
      i 2 pick rb!
      " Timeout receiving a character" msg
      dup 1 lsr-wait 0= if  fail  then
      dup i over rb@  ?fail-at
   loop  2drop
;

: .no-kbd-loopback ( -- )
   ." 'Loopback Test not run.  External loopback required.'" cr
;

: loop-mode ( on? -- )
   su-kbd 4 + dup rb@ h# 10 invert and  rot if  h# 10 or  then  swap rb!
;
: (kbd-test) ( -- )
   uarts-init
   " internal_loopback" subtest
   true loop-mode  su-kbd su-kbd uart-loop  false loop-mode
   ext-tests? if
      uarts-init
      " external_loopback" subtest
      su-kbd su-kbd uart-loop
   then
;
: kbd-test1 ( -- )
   kbd-console? if  .no-kbd-loopback exit  then
   ['] (kbd-test) ['] su-open ['] su-close run-with
;
: kbd-test ( -- )  mouse-path ['] kbd-test1 with-node  ;

: (mouse-test) ( -- )
   uarts-init
   " mouse_loopback" subtest
   su-kbd su-mouse uart-loop
;
: mouse-test1 ( -- )
   kbd-console? if  .no-kbd-loopback exit  then
   mfg-ext? if
      ['] (mouse-test) ['] su-open ['] su-close run-with
   else
      ext-lpbk? if  ." 'Mouse test requires the mfg-mode set to ext/on'" cr  then
   then
;
: mouse-test ( -- )  mouse-path ['] mouse-test1 with-node  ;

\ --- Floppy (floppy_test), in the floppy node ------------------------------
\
\ The SuperIO's floppy controller (82077-compatible: DOR 2, MSR and DSR
\ 4, FIFO 5), driven as an Ultra 10's obdiag drives it: each command and
\ parameter byte once the controller asks (polled 16 ms apart, without a
\ limit, as there), the drive found, then the controller's set-up, the
\ drive's seeks and signals and the first sector read in the non-DMA mode
\ that the SPECIFY's second byte (33) chooses.

0 value fdc
: dor@ ( -- b )  fdc 2+ rb@  ;
: dor! ( b -- )  fdc 2+ rb!  ;
: fmsr@ ( -- b )  fdc 4 + rb@  ;
: dsr! ( b -- )  fdc 4 + rb!  ;
: ffifo@ ( -- b )  fdc 5 + rb@  ;
: ffifo! ( b -- )  fdc 5 + rb!  ;

: rqm-wait ( -- )  begin  d# 16 ms  fmsr@ h# 80 and  until  ;
: fd-cmd ( b -- )  begin  fmsr@ h# 10 and 0=  until  rqm-wait ffifo!  ;
: fd-param ( b -- )  rqm-wait ffifo!  ;
d# 16 buffer: fd-res
0 value #fd-res
: fd-results ( -- )
   0 to #fd-res  fd-res d# 16 erase
   begin  rqm-wait fmsr@ h# 40 and  while
      ffifo@  #fd-res d# 16 < if  fd-res #fd-res + c!  #fd-res 1+ to #fd-res  else  drop  then
   repeat
;
: fd-st0 ( -- b )  fd-res c@  ;
: fd-st1 ( -- b )  fd-res 1+ c@  ;

\ The drive tried: its DOR select and motor bits, the DOR bit cleared at
\ the set-up, and its number in commands (as an Ultra 10's obdiag has
\ each)
0 value fd-sel   0 value fd-motor   0 value fd-eject   0 value fd-drive
: drive1 ( -- )  1 to fd-sel  h# 30 to fd-motor  h# 80 to fd-eject  2 to fd-drive  ;
: drive0 ( -- )  0 to fd-sel  h# 10 to fd-motor  h# 30 to fd-eject  1 to fd-drive  ;
: fd-select ( -- )  dor@ h# bc and fd-sel or fd-motor or dor!  ;
: fd-deselect ( -- )  dor@ fd-motor invert and dor!  ;

: sense-int ( -- )  8 fd-cmd fd-results  ;
: fd-init ( -- )
   4 dor!  dor@ fd-eject invert and dor!
   h# 80 dsr!  0 dsr!
   dor@ fd-motor and  fmsr@ h# 10 and 0=  and if  dor@ fd-motor invert and dor!  then
   dor@ 4 or dor!
   h# 100 0 do  sense-int  fd-st0 h# c0 and h# c0 = if  leave  then  loop
   begin  sense-int  fd-st0 h# c0 and h# 80 =  until
   3 fd-cmd  h# c2 fd-param  h# 33 fd-param
   h# 13 fd-cmd  0 fd-param  h# 58 fd-param  0 fd-param
;
\ A seek's end: its interrupt, for the drive, with seek end or equipment check
: seek-wait ( -- )
   begin
      sense-int
      fd-st0 3 and fd-drive =  fd-st0 h# 30 and 0<>  and
   until
;
: fd-recal ( -- )
   fd-select
   4 0 do
      7 fd-cmd fd-drive fd-param  seek-wait
      fd-st0 h# 10 and 0= if  leave  then
   loop
   fd-deselect
;
: fd-seek ( cyl -- )
   fd-select  h# f fd-cmd fd-drive fd-param fd-param  seek-wait  d# 15 ms  fd-deselect
;
: drive-status ( head -- st3 )
   fd-select  4 fd-cmd  2 lshift fd-drive or fd-param  fd-results  fd-deselect  fd-st0
;

\ The first sector (cylinder 0, head 0, sector 1, 512 bytes; 18 sectors a
\ track, both heads), its 512 bytes taken from the FIFO as the controller
\ gives them (a second at most for each); if it gives none, the
\ controller reset and ST0 taken as ff. The rest of the track is then left
\ to the controller, which ends the read with an overrun.
d# 512 buffer: fd-sector
: fd-pio ( -- ok? )
   d# 512 0 do
      get-msecs d# 1000 +			( when )
      begin  fmsr@ h# 80 and 0=  while
         dup obd-timed-out? if  drop unloop false exit  then
      repeat  drop
      fmsr@ h# f0 and h# f0 <> if  unloop false exit  then
      ffifo@ fd-sector i + c!
   loop  true
;
: read-first ( -- )
   fd-select
   h# e6 fd-cmd  fd-drive fd-param  0 fd-param  0 fd-param  1 fd-param
   2 fd-param  d# 18 fd-param  h# 1b fd-param  h# ff fd-param
   fd-pio 0= dup if  fd-init  then			( reset? )
   d# 50 ms
   begin  fmsr@ h# f0 and h# f0 =  while  ffifo@ drop  d# 16 ms  repeat
   fd-results
   if  h# ff fd-res c!  0 fd-res 1+ c!  then
   fd-deselect
;

: fd-subtest ( -- )
   fd-init
   fd-st0 h# 80 <> if  " Floppy chip initialization failed." fail-msg  then
   h# e fd-cmd fd-results
   fd-res 4 + c@ h# c2 =  fd-res 5 + c@ h# 33 = and
   fd-res 7 + c@ 0= and  fd-res 8 + c@ h# 58 = and  0= if
      " Floppy chip registers read back incorrectly." fail-msg
   then
   fd-recal
   fd-st0 h# 10 and if
      " Recalibrate failed. floppy missing, improperly connected, or defective." fail-msg
   then
   2 fd-seek
   fd-st0 h# 20 fd-drive or <> if  " Seek to track 2 failed." fail-msg  then
   0 drive-status h# 10 and if  " Track 0 reported when on track 2." fail-msg  then
   0 fd-seek
   fd-st0 h# 20 fd-drive or <> if  " Seek to track 0 failed." fail-msg  then
   0 drive-status
   dup h# 10 and 0= if  " Track 0 not reported." fail-msg  then
   4 and if  " Head select is 1; should be 0." fail-msg  then
   1 drive-status 4 and 0= if  " Head select is 0; should be 1." fail-msg  then
   read-first
   fd-st0 h# c0 and h# 40 <>  fd-st1 h# 10 and 0=  and if
      fd-st0 h# ff = if
         " No diskette, or incorrect format" fail-msg
      else
         " Can't read the first sector" fail-msg
      then
   then
;
\ The drive: the first (select 1) if, set up and recalibrated, it shows no
\ equipment check; else the second, whichever it is
: (floppy-test) ( -- )
   drive1 fd-init fd-recal
   fd-st0 h# 10 and 0= if
      " floppy_id1_read_test"
   else
      drive0 fd-init fd-recal  " floppy_id0_read_test"
   then  subtest
   fd-subtest
;
: fd-open ( -- )  my-address my-space 8 " map-in" $call-parent to fdc  ;
: fd-close ( -- )  fdc ?dup if  8 " map-out" $call-parent  0 to fdc  then  ;
: floppy-test1 ( -- )  ['] (floppy-test) ['] fd-open ['] fd-close run-with  ;
: floppy-test ( -- )  fd-path ['] floppy-test1 with-node  ;

\ --- Parallel Port (parallel_port_test), in the ecpp node ------------------
\
\ The SuperIO's parallel port (its ECP registers: data 0, status 1, control
\ 2, the FIFOs 400, ECR 402), its configuration registers (index 30015c,
\ data 30015d: a register takes the second of two writes) and EBus DMA
\ channel 1. The passive loopback needs the parallel loopback plug.

0 value pp			\ The port
0 value sio			\ The configuration index (data at + 1)
0 value pp-dma			\ DMA channel 1
0 value pbuf-va   0 value pbuf-io	\ Two 8 KB DVMA buffers (send, receive)

: sio! ( value reg -- )  sio rb!  dup sio 1+ rb!  sio 1+ rb!  ;
: sio@ ( reg -- value )  sio rb!  sio 1+ rb@  ;

: pp-open ( -- )
   h# 3043bc h# 500 ebus-map to pp
   h# 30015c 2 ebus-map to sio
   h# 700000 h# 10 ebus-map to pp-dma
   h# 4000 dvma-alloc to pbuf-io to pbuf-va
;
: pp-close ( -- )
   pbuf-va ?dup if  pbuf-io h# 4000 dvma-free  0 to pbuf-va  then
   pp-dma ?dup if  h# 10 ebus-unmap  0 to pp-dma  then
   sio ?dup if  2 ebus-unmap  0 to sio  then
   pp ?dup if  h# 500 ebus-unmap  0 to pp  then
;

: dtr! ( b -- )  pp rb!  ;
: str@ ( -- b )  pp 1+ rb@  ;
: dcr! ( b -- )  pp 2+ rb!  ;
: test-fifo ( -- adr )  pp h# 400 +  ;
: ecr! ( b -- )  pp h# 402 + rb!  ;

\ The configuration registers 0, 1, 2, 3, 4, 6 and 7 kept, and put back
\ in the other order
create sio-regs  0 c, 1 c, 2 c, 3 c, 4 c, 6 c, 7 c,
7 buffer: sio-saved
: save-sio ( -- )  7 0 do  sio-regs i + c@ sio@  sio-saved i + c!  loop  ;
: restore-sio ( -- )
   0 6 do  sio-saved i + c@  sio-regs i + c@ sio!  -1 +loop
;

\ The passive loopback's rows: message, control written, the status bits
\ compared, their value, and the whole status byte expected
\ (A mismatch's message line, as an Ultra 10's: the control register in
\ decimal, the bytes in two columns, and no closing quote.)
0 value row-dcr   0 value row-mask   0 value row-exp   0 value row-unm
: row ( adr len dcr mask exp unmasked -- )
   to row-unm  to row-exp  to row-mask  to row-dcr  msg
   row-dcr dcr!  str@ dup row-mask and		( str masked )
   dup row-exp <> if
      .fail-head  ." ;Wrote to CTR: " row-dcr .d
      ." Obs. unmasked byte: " swap 2 u.r
      ."   Exp. unmasked byte: " row-unm 2 u.r
      ."   Obs. masked byte: " 2 u.r
      ."   Exp. masked byte: " row-exp 2 u.r cr
      fail-end
   then  2drop
;
: sio-passive-lb ( -- )
   " sio_passive_lb" subtest
   h# 0f 0 sio!  0 3 sio!  0 4 sio!  0 6 sio!  0 7 sio!  h# 80 2 sio!
   0 dcr!
   0 dtr!
   str@ h# 40 and if  " One of 8 Data Bits Stuck High" fail-msg  then
   " Parallel Port Bit Error" msg
   8 0 do
      1 i lshift dtr!
      str@ h# 40 and 0= if  i fail-bit  then
   loop
   h# ff dtr!
   " BUSY/STB_L Lines"    1 h# 80 h# 80 h# ef row
   " ERROR_L/AFD_L Lines" 2 h# 08 h# 00 h# 67 row
   " SLCT/INIT_L Lines"   4 h# 10 h# 10 h# 7f row
   " SLIN_L/PE Lines"     8 h# 20 h# 00 h# 4f row
   " RESET STATUS"        0 h# ff h# 6f h# 6f row
;
: pp-lpbk ( -- )  ext-tests? if  sio-passive-lb  then  ;

\ 16 bytes moved by DMA (channel 1, memory to device) into the port's
\ FIFO in its test mode, and read back from the test FIFO
: dma-read ( -- )
   " dma_read" subtest
   pp-dma dma-reset
   d# 17 0 do  test-fifo rb@ drop  loop
   h# 100 0 do  i pbuf-va i + c!  loop
   h# 2000 0 do  h# dead.beef pbuf-va h# 2000 + i + l!  4 +loop
   0 4 sio!  0 6 sio!  0 dcr!  d# 100 ms
   4 dcr!  4 4 sio!  8 6 sio!  h# 80 2 sio!  h# b0 h# 41 sio!
   0 dcr!  h# d8 ecr!
   d# 16 pp-dma dma-bcr!  pbuf-io pp-dma dma-acr!  h# 0001.2200 pp-dma dma-csr!
   d# 100 ms
   " Data Mismatch" msg
   d# 16 0 do  test-fifo  i  test-fifo rb@  ?fail-at  loop
;

\ The configuration kept and put back, and the channel reset, whatever
\ the sub-tests' end
0 value pp-subs
: pp-body ( -- )  save-sio  pp-subs catch  restore-sio  pp-dma dma-reset  throw  ;
: (pp-run) ( -- )  ['] pp-body ['] pp-open ['] pp-close run-with  ;
: pp-run ( xt -- )  to pp-subs  ecpp-path ['] (pp-run) with-node  ;
: (pp-all) ( -- )  pp-lpbk dma-read  ;
: parallel-test ( -- )  ['] (pp-all) pp-run  ;
: pp-lpbk-only ( -- )  ['] pp-lpbk pp-run  ;
: pp-dvmard-only ( -- )  ['] dma-read pp-run  ;

\ --- Serial Port A, B (uarta_test, uartb_test), in the se node -------------
\
\ The SAB82532's channel (B at A + 40) in its asynchronous mode, looped
\ back at each speed: in its test loop (MODE's TLP), then through a plug.

0 value se-base   false value se-mapped?
0 value sab			\ The channel's registers

: se-open ( -- )
   " address" get-my-property if
      my-address my-space h# 80 " map-in" $call-parent  true to se-mapped?
   else
      decode-int nip nip  false to se-mapped?
   then  to se-base
;
: se-close ( -- )
   se-mapped? if  se-base h# 80 " map-out" $call-parent  false to se-mapped?  then
;

: sab@ ( offset -- b )  sab + rb@  ;
: sab! ( b offset -- )  sab + rb!  ;
\ A command, once the last is taken (CEC clear; no limit)
: cec-wait ( -- )  begin  h# 20 sab@ 4 and 0=  until  ;
: sab-cmd ( b -- )  cec-wait  h# 20 sab!  ;

: sab-init ( -- )
   h# c3 h# 2c sab!  h# c0 h# 3f sab!  h# 41 sab-cmd  h# 1f h# 2d sab!
   h# af h# 34 sab!  h# 38 h# 2e sab!  h# 2c h# 22 sab!  0 h# 2b sab!
   h# ff h# 3a sab!  h# ff h# 3b sab!  h# 80 h# 39 sab!  0 h# 27 sab!
   h# 4c h# 28 sab!  h# 41 sab-cmd  h# 40 h# 28 sab!
;

\ The speeds and their 10-bit divisor values (BGR, and CCR2's bits 7:6)
create sab-rates
   d# 1200 , h# 16f ,    d# 1800 , h# 15f ,    d# 2400 , h# 12f ,
   d# 4800 , h# 0ef ,    d# 9600 , h# 0af ,    d# 19200 , h# 06f ,
   d# 38400 , h# 057 ,   d# 57600 , h# 04f ,   d# 76800 , h# 04b ,
   d# 115200 , h# 047 ,  d# 153600 , h# 045 ,  d# 230400 , h# 043 ,
   d# 307200 , h# 042 ,  d# 460800 , h# 041 ,
d# 14 constant #sab-rates

0 value sab-ms			\ The time allowed for each step at the speed
0 value sab-n			\ The speed's divisor

: xfw? ( -- flag )  h# 20 sab@ h# 40 and 0<>  ;
: rpf? ( -- flag )  h# 3a sab@ 1 and 0<>  ;

: set-rate ( rate n -- )
   to sab-n
   d# 120000 over / 1+ d# 10 * to sab-ms  drop
   sab-n h# ff and h# 34 sab!
   sab-n 8 rshift 3 and 6 lshift h# 38 or h# 2e sab!
   h# ef h# 3e sab!  0 h# 3c sab!
   d# 32 0 do  rpf? if  0 sab@ drop  then  loop
;
: sab-loop ( -- )
   h# 80 0 do
      " Timeout transmitting a character" msg
      ['] xfw? sab-ms poll 0= if  fail  then
      i 0 sab!  8 sab-cmd
      " Timeout receiving a character" msg
      ['] rpf? sab-ms poll 0= if  fail  then
      0 sab@  h# 80 sab-cmd  cec-wait		( b )
      sab i rot ?fail-at
   loop
;
: (sab-test) ( -- )
   sab-init
   #sab-rates 0 do
      sab-rates i 2* na+  dup @ swap na1+ @		( rate n )
      script-debug? if  ." BAUDRATE='" over (u.d) type ." '" cr  then
      set-rate
      " internal_loopback" subtest
      h# 22 sab@ 1 or h# 22 sab!  sab-loop
      ext-tests? if
         " external_loopback" subtest
         h# 22 sab@ h# fe and h# 22 sab!  sab-loop
      then
   loop
;
\ Afterwards, whatever the end: set up again, at the last speed tried
: sab-after ( -- )
   sab-init  h# 10 h# 3c sab!
   sab-n h# ff and h# 34 sab!  sab-n 8 rshift 3 and 6 lshift h# 2e sab!
;
: sab-body ( -- )  ['] (sab-test) catch  sab-after  throw  ;

\ Whether a console instance is this node's, with the channel's argument
: chan-args ( ihandle -- args$ | 0 0 )
   ?dup 0= if  0 0 exit  then
   dup ihandle>phandle  se-path locate-device if  2drop 0 0 exit  then	( ih ph se )
   <> if  drop 0 0 exit  then
   my-self >r  to my-self  my-args  r> to my-self
;
: chan-console? ( chan$ -- flag )
   2dup stdin @ chan-args $=  >r  stdout @ chan-args $=  r> or
;

0 value sab-chan		\ 0 A, 1 B
: (se-test) ( -- )
   se-base sab-chan h# 40 * + to sab
   sab-body
;
: se-test1 ( -- )
   sab-chan if  " b"  else  " a"  then  chan-console? if
      script-debug? if
         sab-chan if
            ." 'UART B in use as console - Test not run.'"
         else
            ." 'UART A in use as console - Test not run.'"
         then  cr
      then  exit
   then
   ['] (se-test) ['] se-open ['] se-close run-with
;
: uarta-test ( -- )  0 to sab-chan  se-path ['] se-test1 with-node  ;
: uartb-test ( -- )  1 to sab-chan  se-path ['] se-test1 with-node  ;

\ --- NVRAM (nvram_test), in the eeprom node --------------------------------
\
\ 256 bytes of the M48T59 at 1cd8 (in what the power-on selftest uses as
\ scratch), written and read back, then inverted; left inverted (ff to 0),
\ as an Ultra 10's are. No message of its own.

0 value nv
: nv-open ( -- )  my-address my-space h# 2000 " map-in" $call-parent to nv  ;
: nv-close ( -- )  nv ?dup if  h# 2000 " map-out" $call-parent  0 to nv  then  ;
: nv-byte ( i -- adr )  nv h# 1cd8 + +  ;
: nv-patterns ( xor -- )
   h# 100 0 do  i over xor h# ff and  i nv-byte rb!  loop
   h# 100 0 do  i nv-byte  i 2 pick xor h# ff and  i nv-byte rb@  ?fail-at  loop
   drop
;
: (nvram-test) ( -- )
   " write/read_patterns" subtest  0 nv-patterns
   " write/read_inverted_patterns" subtest  h# ff nv-patterns
;
: nvram-test1 ( -- )  ['] (nvram-test) ['] nv-open ['] nv-close run-with  ;
: nvram-test ( -- )  eeprom-path ['] nvram-test1 with-node  ;
headers
