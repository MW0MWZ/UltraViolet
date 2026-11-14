\ Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
\ OpenBoot release it is part of (see LICENSE).
id: @(#)obdmenu.fth 1.0 26/11/14
purpose: obdiag's menus, its script, and the obdiag and run-obd-scripts commands
copyright: Copyright 2026 Andy Taylor

\ What an Ultra 10 shows: the first obdiag (or run-obd-scripts) of a
\ session "loads" the diagnostics (its lines, the device nodes named, and
\ mfg-mode, diag-level and diag-switch? taken for the rest of the
\ session); then by mfg-mode: off nothing more, menu-int and menu-ext the
\ sub-test menu, the others the main menu. A menu waits for a number
\ (decimal, up to five characters, the line edited as at the ok prompt),
\ runs that test (or all, or toggles a setting), and asks again; the data
\ stack is emptied after each answer, as an Ultra 10's obdiag empties it
\ (what was there before obdiag included).

headerless

\ --- Running a test ---------------------------------------------------------

\ A test: its TEST= name and its word. The first failure ends it (its
\ block printed); a failure does not end "All Above" or a script.
: run-test ( name$ xt -- )
   0 to test-errors
   -rot to /test$ to test$
   script-debug? if  cr ." TEST='" test$ /test$ type ." '" cr  then
   catch  dup obd-failed = if  drop 0  then  throw
;

\ --- The entries ------------------------------------------------------------
\
\ An entry: its menu title, TEST= name, the node its test runs in, and the
\ test's word.

: m0 " PCI/Cheerio" " all_pci/cheerio_test" ebus-path ['] cheerio-test ;
: m1 " EBUS DMA/TCR Registers" " all_dma/ebus_test" ebus-path ['] dma-test ;
: m2 " Ethernet" " ethernet_test" net-path ['] eth-test ;
: m3 " Keyboard" " keyboard_test" mouse-path ['] kbd-test ;
: m4 " Mouse" " mouse_test" mouse-path ['] mouse-test ;
: m5 " Floppy" " floppy_test" fd-path ['] floppy-test ;
: m6 " Parallel Port" " parallel_port_test" ecpp-path ['] parallel-test ;
: m7 " Serial Port A" " uarta_test" se-path ['] uarta-test ;
: m8 " Serial Port B" " uartb_test" se-path ['] uartb-test ;
: m9 " NVRAM" " nvram_test" eeprom-path ['] nvram-test ;
: m10 " Audio" " audio_test" audio-path ['] audio-test ;
: m11 " EIDE" " ide_test" ide-path ['] ide-test ;
\ The video entry, with the Rage as the console, says so instead
: m12
   rage-console? if
      " Video(console should be tty)" " video_test" m64-path ['] .video-console
   else
      " Video" " video_test" m64-path ['] video-test
   then
;
create main-entries
   ' m0 token,  ' m1 token,  ' m2 token,  ' m3 token,  ' m4 token,
   ' m5 token,  ' m6 token,  ' m7 token,  ' m8 token,  ' m9 token,
   ' m10 token, ' m11 token, ' m12 token,
d# 13 constant #main

\ The sub-test menu's (as an Ultra 10's, without its video entry, which
\ it has but numbers past "All Above")
: s0 " Cheerio/V_id" " cheerio_test/v_id" ebus-path ['] vendor-id-test ;
: s1 " Cheerio/Dev_id" " cheerio_test/dev_id" ebus-path ['] device-id-test ;
: s2 " Cheerio/mixmode_rd" " cheerio_test/mixmode_rd" ebus-path ['] mixmode-read ;
: s3 " Cheerio/e2_class" " cheerio_test/e2_class" ebus-path ['] e2-class-test ;
: s4 " Cheerio/status_reg_walk1" " cheerio_test/status_reg" ebus-path ['] status-reg-walk1 ;
: s5 " Cheerio/line_size_walk1" " cheerio_test/line_size" ebus-path ['] line-size-walk1 ;
: s6 " Cheerio/latency_walk1" " cheerio_test/latency" ebus-path ['] latency-walk1 ;
: s7 " Cheerio/line_walk1" " cheerio_test/line" ebus-path ['] line-walk1 ;
: s8 " Cheerio/pin_test" " cheerio_test/pin_test" ebus-path ['] pin-test ;
: s9 " EBUS DMA Reg Test" " ebus/dma_reg" ebus-path ['] dma-reg-only ;
: s10 " EBUS DMA Func Test" " ebus/dma_func" ebus-path ['] dma-func-only ;
: s11 " Enet_hme_reg" " enet_hme_reg" net-path ['] eth-reg-only ;
: s12 " Enet_mac_lpbk" " enet_mac" net-path ['] eth-mac-only ;
: s13 " Enet_10mb_xcvr" " enet_10mb_xcvr" net-path ['] eth-10-only ;
: s14 " Enet_100mb_phy" " enet_100mb_phy" net-path ['] eth-100-only ;
: s15 " Enet_100mb_twister" " enet_100mb_twister" net-path ['] eth-tw-only ;
: s16 " Floppy" " floppy_test" fd-path ['] floppy-test ;
: s17 " PPort Loopback" " pport_lpbk_test" ecpp-path ['] pp-lpbk-only ;
: s18 " PPort DVMA RD" " pport_dvmard_test" ecpp-path ['] pp-dvmard-only ;
: s19 " Serial Port A" " uarta_test" se-path ['] uarta-test ;
: s20 " Serial Port B" " uartb_test" se-path ['] uartb-test ;
: s21 " NVRAM" " nvram_test" eeprom-path ['] nvram-test ;
: s22 " Audio" " audio_test" audio-path ['] audio-test ;
: s23 " EIDE" " ide_test" ide-path ['] ide-test ;
create sub-entries
   ' s0 token,  ' s1 token,  ' s2 token,  ' s3 token,  ' s4 token,  ' s5 token,
   ' s6 token,  ' s7 token,  ' s8 token,  ' s9 token,  ' s10 token, ' s11 token,
   ' s12 token, ' s13 token, ' s14 token, ' s15 token, ' s16 token, ' s17 token,
   ' s18 token, ' s19 token, ' s20 token, ' s21 token, ' s22 token, ' s23 token,
d# 24 constant #sub

\ (The Cheerio sub-tests run with the EBus node opened as for the whole
\ test: the latency timer set.)
: sub-xt ( n xt -- xt' )
   swap 9 < if  to cheerio-sub  ['] cheerio-one  then
;

\ The menu in force: its entries and their count, its title
0 value entries   0 value #entries   false value sub-menu?
: menu-entry ( n -- title$ name$ path$ xt )  entries swap ta+ token@ execute  ;

\ (The test's node under the EBus: the latency timer set first, as the
\ EBus's open in an Ultra 10's obdiag sets it.)
: ebus-child? ( path$ -- flag )
   dup ebus-path nip > 0= if  2drop false exit  then
   drop ebus-path tuck $=
;
\ An entry run, TEST= named
0 value entry-xt
: run-entry-at ( n -- )
   dup >r menu-entry  to entry-xt			( title$ name$ path$ ) ( r: n )
   ebus-child? if  ebus-latency  then		( title$ name$ )
   2swap 2drop					( name$ )
   sub-menu? if  r@ entry-xt sub-xt to entry-xt  then  r> drop
   entry-xt run-test
;

\ --- The menus --------------------------------------------------------------

: #all ( -- n )  #entries  ;
: #quit ( -- n )  #entries 1+  ;
: #redisplay ( -- n )  #entries 2+  ;

\ An entry's line: its number in decimal in three columns, then the title;
\ " <Test Missing>" after it if its node is not there
: .entry-line ( n title$ -- )
   rot push-decimal 3 .r pop-base  ."  ..... "  type
;
: .menu ( -- )
   d# 10 spaces
   sub-menu? if  ." OBDiag SubMenu"  else  ." OBDiag Menu"  then  cr cr
   #entries 0 do
      i  i menu-entry drop  locate-device if  true  else  drop false  then	( n title$ name$ missing? )
      >r 2drop  .entry-line
      r> if  ."  <Test Missing>"  then  cr
   loop
   #all       " All Above" .entry-line cr
   #quit      " Quit" .entry-line cr
   #redisplay " Display this Menu" .entry-line cr
   #redisplay 1+ " Toggle script-debug" .entry-line cr
   #redisplay 2+ " Enable External Loopback Tests" .entry-line cr
   #redisplay 3 + " Disable External Loopback Tests" .entry-line cr
   cr
;
: .prompt ( -- )
   ." Enter (0-" #all .d ." tests, " #quit .d ." -Quit, " #redisplay .d ." -Menu) ===> "
;

\ The answer: up to five characters, a decimal number (with a sign) or not
5 buffer: sel-buf
0 value /sel
: sel$ ( -- adr len )  sel-buf /sel  ;
: sel-number ( adr len -- n true | false )
   dup 0= if  2drop false exit  then
   over c@ ascii - = dup >r  if  1 /string  then	( adr' len' ) ( r: neg? )
   dup 0= if  2drop r> drop false exit  then
   0 -rot  bounds ?do					( n )
      i c@ ascii 0 -  dup 9 u> if  2drop unloop r> drop false exit  then
      swap d# 10 * +
   loop
   r> if  negate  then  true
;
\ A wrong answer: as typed (or the number), a bell, and the complaint
: .not-valid ( -- )
   7 emit  ."  is not a valid selection " cr
;

false value obd-quit?
: do-selection ( n -- )
   dup #all <  if  run-entry-at exit  then
   dup #all =  if  drop  #entries 0 do  i run-entry-at  loop  exit  then
   dup #quit = if  drop true to obd-quit?  exit  then
   dup #redisplay = if  drop .menu exit  then
   #redisplay - case
      1 of  script-debug? 0= to script-debug?  endof
      2 of  true to ext-lpbk?  endof
      3 of  false to ext-lpbk?  endof
   endcase
;
: in-range? ( n -- flag )  0 #redisplay 4 + within  ;

: menu ( entries #entries sub? -- )
   to sub-menu?  to #entries  to entries
   false to obd-quit?
   h# 0c emit  .menu
   begin
      1 to obd-passes
      .prompt
      sel-buf 5 accept to /sel
      sel$ sel-number if
         dup in-range? if  do-selection  else  .d .not-valid  then
      else
         sel$ type space .not-valid
      then
      clear
   obd-quit? until
;

\ --- The load ---------------------------------------------------------------

: load-node ( path$ -- )
   ." loading code into: " 2dup type cr
   locate-device if  abort  then  drop
;
: obd-load ( -- )
   obd-loaded? if  exit  then
   ." stdin: " stdin @ . cr
   ." stdout: " stdout @ . cr
   ebus-path load-node  eeprom-path load-node  ecpp-path load-node
   mouse-path load-node  se-path load-node  net-path load-node
   fd-path load-node  audio-path load-node  ide-path load-node
   " /pci@1f,0/pci@1,1/ide@3/disk" load-node
   " /pci@1f,0/pci@1,1/ide@3/cdrom" load-node
   m64-path load-node
   ." Debugging enabled" cr
   mfg-mode to obd-mfg  diag-level to obd-diag-level  diag-switch? to obd-diag-switch
   true to obd-loaded?
;

\ --- The script -------------------------------------------------------------

: script-test ( name$ xt -- )  run-test  ;
: script-run ( -- )
   " all_pci/cheerio_test" ['] cheerio-test script-test
   " all_dma/ebus_test" ['] dma-test script-test
   " ethernet_test" ['] eth-test script-test
   ebus-latency " parallel_port_test" ['] parallel-test script-test
   ebus-latency " uartb_test" ['] uartb-test script-test
   obd-mfg h# 40 and if  ebus-latency " floppy_test" ['] floppy-test script-test  then
   ebus-latency " nvram_test" ['] nvram-test script-test
   ebus-latency " audio_test" ['] audio-test script-test
   obd-mfg h# 40 and if
      " ide_test" ['] ide-test script-test
      rage-console? 0= if  " video_test" ['] video-test script-test  then
   then
;
: .script-end ( -- )
   cr ." ###OBDIAG_MFG_START###" cr
   ." MESSAGE='TESTS FINISHED'" cr
   obd-errors if  ." STATUS='FAILED'"  else  ." STATUS='PASSED'"  then  cr
   ." TTF='" .ttf ." '" cr
   ." SPEED='" .mhz ."  MHz'" cr
   obd-errors if
      ." ERRORS=" obd-errors . cr
   else
      ." ERRORS='0'" cr
      ." PASSES='" obd-passes .d ." '" cr
   then
   ." ###OBDIAG_MFG_STOP###" cr
   ." ###OBDIAG_STOP###" cr
;

headers

\ The menu, by mfg-mode (as the first obdiag of the session found it)
: obdiag ( -- )
   obd-load
   obd-mfg 0= if  exit  then
   obd-mfg h# 61 and if  true to ext-lpbk?  then
   false to obd-script?  0 to obd-errors
   get-msecs to obd-t0
   obd-mfg h# 10 and if
      sub-entries #sub true menu
   else
      main-entries #main false menu
   then
;

\ The manufacturing script (bft and sys modes): its tests twice, each
\ failure's block ended by a stop line, then the summary
: run-obd-scripts ( -- )
   obd-load
   obd-mfg h# 21 and if  true to ext-lpbk?  then
   obd-mfg h# 60 and 0= if  exit  then
   ." ###OBDIAG_START###" cr
   get-msecs to obd-t0  0 to obd-passes  0 to obd-errors
   true to script-debug?  true to obd-script?
   2 0 do  script-run  obd-passes 1+ to obd-passes  loop
   .script-end
   false to obd-script?
;

headerless
\ At a start-up after a power-on selftest that passed (post.fth), with
\ mfg-mode not off: the script.
: obd-startup ( -- )  mfg-mode if  run-obd-scripts  then  ;
' obd-startup is obd-scripts
headers
