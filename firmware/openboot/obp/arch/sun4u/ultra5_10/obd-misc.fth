\ Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
\ OpenBoot release it is part of (see LICENSE).
id: @(#)obd-misc.fth 1.0 26/11/14
purpose: obdiag's tests of the audio codec, the IDE controller and the Rage
copyright: Copyright 2026 Andy Taylor

headerless

\ --- Audio (audio_test), in the SUNW,CS4231 node ---------------------------
\
\ The CS4231 (its index and data registers at 0 and 4, in the EBus's
\ second window at 20.0000), its playback and capture DMA channels (EBus
\ DMA at 70.2000 and 70.4000) and the auxiliary register at 72.2000
\ (non-zero holds the codec in reset; written as a plain 32-bit word, an
\ Ultra 10's obdiag writing it 1 then 0). The codec found and its
\ index and data registers checked; then, with the cables for it, the
\ line and the headphone outputs looped to the line and microphone inputs;
\ then its registers walked.

0 value codec   0 value play-dma   0 value cap-dma   0 value aux
0 value abuf-va   0 value abuf-io	\ Four 8 KB DVMA buffers: the played
h# 8000 constant /abufs		\ wave at 0, the captured at 2000
: play-buf ( -- va )  abuf-va  ;
: cap-buf ( -- va )  abuf-va h# 2000 +  ;

: au-open ( -- )
   h# 200000 h# 10 ebus-map to codec
   h# 702000 h# 10 ebus-map to play-dma
   h# 704000 h# 10 ebus-map to cap-dma
   h# 722000 4 ebus-map to aux
;
: au-close ( -- )
   aux ?dup if  4 ebus-unmap  0 to aux  then
   cap-dma ?dup if  h# 10 ebus-unmap  0 to cap-dma  then
   play-dma ?dup if  h# 10 ebus-unmap  0 to play-dma  then
   codec ?dup if  h# 10 ebus-unmap  0 to codec  then
;

: iar! ( b -- )  codec rb!  ;
: iar@ ( -- b )  codec rb@  ;
: idr! ( b -- )  codec 4 + rb!  ;
: idr@ ( -- b )  codec 4 + rb@  ;
: ind! ( value reg -- )  iar! idr!  ;
: ind@ ( reg -- value )  iar! idr@  ;
: aux! ( l -- )  aux rl!  ;

\ The codec's reset: held 10 ms, then 300 ms, then until its index
\ register no longer reads 80 (initialising; no limit)
: codec-reset ( -- )
   h# ffff.ffff aux!  d# 10 ms  0 aux!  d# 300 ms
   begin  iar@ h# 80 <>  until
;
\ (at the test's end, and before each external loopback)
: dma-codec-reset ( -- )
   play-dma dma-reset  cap-dma dma-reset
   1 aux!  0 aux!  d# 300 ms
;

\ A codec there: its timer (mode 2) gives its interrupt
: probe-codec ( -- found? )
   h# c ind@ h# 40 or h# c ind!
   0 d# 24 ind!  0 d# 21 ind!  h# f d# 20 ind!  h# 40 d# 16 ind!
   1 ms
   d# 24 ind@ h# 40 and 0<>
   h# ffff.ffff aux!  d# 512 ms  0 aux!  d# 300 ms
;
: codec-test ( -- )
   " codec_test" subtest
   probe-codec 0= if
      ." Fail on probing Audio module." cr
      obd-mfg h# 41 = if  " Fail on probing Audio module." fail-msg  then
      exit
   then
   codec-reset
   0 h# c ind!  ." Codec_ID='" h# c ind@ (u.) type ." '" cr
   h# 40 h# c ind!  ." Version_ID='" d# 25 ind@ (u.) type ." '" cr
   " IAR Error" msg
   0 h# c ind!
   h# 21 0 do
      i h# 10 = if  h# 40 h# c ind!  then
      i iar!  iar@ i ?fail-compare
   loop
   0 iar!  iar@ 0 ?fail-compare
   " IDR Error" msg
   h# 80 h# 40 h# 20 8 4 2 1
   7 0 do  dup idr!  idr@ swap ?fail-compare  loop
   codec-reset
;

\ The indirect registers walked (8 and 9 with MCE), 300 ms for each
\ value; no message of its own
create ind-masks
   h# ef c, h# ef c, h# 9f c, h# 9f c, h# 9f c, h# 9f c, h# bf c, h# bf c,
   h# 7e c, h# 1c c, h# ca c, h# 00 c, h# 00 c, h# fd c, h# ff c, h# ff c,
: ind-reg-test ( -- )
   " ind_reg_test" subtest
   codec-reset
   h# 10 0 do
      i h# c <> if
         i  i 8 = i 9 = or if  h# 40 or  then  iar!
         8 0 do
            ind-masks j + c@  1 i lshift and		( value )
            dup idr!  d# 300 ms
            j swap idr@ ?fail-at
         loop
         0 idr!
      then
   loop
   codec-reset
;

\ The external loopbacks: 48 cycles of a 68-sample wave (a quarter wave of
\ 18 values, rising, falling, then the same mirrored about 80), played on
\ both channels (8-bit unsigned stereo at 48 kHz) and captured at once;
\ each channel's capture then looked through for a cycle of the wave, at
\ the level the outputs give.
create quarter-wave
   h# 80 c, h# 85 c, h# 8b c, h# 90 c, h# 96 c, h# 9b c, h# a0 c, h# a4 c, h# a9 c,
   h# ad c, h# b1 c, h# b4 c, h# b7 c, h# ba c, h# bc c, h# be c, h# bf c, h# c0 c,
d# 68 constant /cycle
d# 48 /cycle * 2* constant /played		\ 1980
h# 2000 constant /captured
: quarter ( i -- b )  quarter-wave + c@  ;
\ The wave's sample k (0-67): 85..c0, bf..80, 7b..40, 41..80
: half-wave ( k -- b )
   dup d# 17 < if  1+ quarter  else  d# 33 swap - quarter  then
;
: wave ( k -- b )
   dup d# 34 >= if  d# 34 - half-wave  h# 100 swap -  else  half-wave  then
;
: fill-wave ( -- )
   d# 48 0 do
      /cycle 0 do
         i wave  j /cycle * i + 2* play-buf +	( b adr )
         2dup c!  1+ c!
      loop
   loop
;

: au-set-up ( input -- )
   dma-codec-reset
   \ (MCE and TRD with the mode, the formats and the interface
   \ configuration only, as an Ultra 10's obdiag sets them)
   h# 40 h# 6c ind!
   h# c0 d# 26 ind!
   h# 80 2 ind!  h# 80 3 ind!  h# 80 4 ind!
   h# 80 5 ind!  h# 80 d# 18 ind!  h# 80 d# 19 ind!
   3 d# 17 ind!
   0 6 ind!  0 7 ind!
   h# 1c 8 h# 60 or ind!  h# 1c d# 28 h# 60 or ind!
   8 9 h# 60 or ind!
   h# 20 iar!
   h# 1000 0 do  d# 11 h# 20 or ind@ h# 20 and if  leave  then  loop
   d# 16 ms
   h# 1000 0 do  d# 11 h# 20 or ind@ h# 20 and 0= if  leave  then  loop
   d# 768 ms
   \ (the inputs and the enables with TRD clear)
   dup 0 ind!  1 ind!
   3 9 ind!
;
: tc? ( base -- flag )  dma-csr@ h# 4000 and 0<>  ;
: au-dma ( -- )
   \ (each address, then its count, as an Ultra 10's obdiag loads them)
   abuf-io play-dma dma-acr!  /played play-dma dma-bcr!
   abuf-io h# 2000 + cap-dma dma-acr!  /captured cap-dma dma-bcr!
   h# 0189.2200 play-dma dma-csr!  h# 0189.2300 cap-dma dma-csr!
   " Audio DMA transfer timed out." msg
   h# 1.0000 0 do
      play-dma tc?  cap-dma tc?  and if  d# 256 ms  unloop exit  then
      1 ms
   loop
   fail
;

\ A channel's capture (every second byte from the first, left or right):
\ from sample 110, up to 40 cycles tried for one whose minimum, maximum
\ and next minimum lie 34 and 68 samples apart (a sample either way); its
\ 68 samples then compared with the played wave at the output's level
\ (84 per cent for the line, 63 for the headphones), within a quarter of
\ the wave's height, a sample earlier or later as well.
0 value au-chan   0 value au-level
: sample ( n -- b )  2* au-chan + cap-buf + c@  ;
/captured 2/ constant #samples
: min-at ( from -- n )
   dup  dup /cycle + #samples min  rot ?do		( n )
      i sample over sample < if  drop i  then
   loop
;
: max-at ( from -- n )
   dup  dup /cycle + #samples min  rot ?do
      i sample over sample > if  drop i  then
   loop
;
: near? ( n target -- flag )  - abs 1 <=  ;
\ (the wave's minimum is its sample 50)
: expected ( k -- b )
   d# 50 + /cycle mod wave  h# 80 -  au-level * d# 100 /  h# 80 +
;
: cycle-ok? ( start -- flag )
   true  /cycle 0 do					( start ok? )
      over i + sample  i expected - abs		( start ok? diff )
      h# 40 au-level * d# 400 / 1+ > if  drop false leave  then
   loop  nip
;
: compare-cycle ( start -- ok? )
   dup cycle-ok? if  drop true exit  then
   dup 1+ cycle-ok? if  drop true exit  then
   1- cycle-ok?
;
\ The cycle's start (a minimum), if one is found
: find-cycle ( -- start true | false )
   h# 110  d# 40 0 do					( from )
      dup min-at  dup max-at  dup min-at		( from m1 mx m2 )
      2 pick d# 34 + 2 pick swap near?
      3 pick /cycle + 2 pick swap near?  and if	( from m1 mx m2 )
         2drop nip  dup /cycle + #samples < if  true unloop exit  then
         drop false unloop exit
      then
      3drop /cycle +
      dup /cycle 2* + #samples >= if  leave  then
   loop  drop false
;
: .chan ( -- )  au-chan if  ." Right"  else  ." Left"  then  ;
\ (With no cycle found the comparison is still made, from sample 110: an
\ Ultra 10's obdiag then prints both lines for a silent input.)
: .out-of-limit ( -- )  ." Values are out of limit.  " .chan ."  Channel Test Failed" cr  ;
: check-chan ( chan -- ok? )
   to au-chan
   find-cycle 0= if
      ." Did not find Symetric wave to check, " .chan ."  Channel Test Failed" cr
      h# 110 compare-cycle 0= if  .out-of-limit  then
      false exit
   then
   compare-cycle 0= if  .out-of-limit  false exit  then  true
;
: ext-audio ( input level -- )
   to au-level
   fill-wave
   cap-buf /captured erase
   au-set-up  au-dma
   cap-buf abuf-io h# 2000 + /captured dvma-sync
   0 check-chan  1 check-chan  and 0= if
      " Can not find Symmetric Wave." fail-msg
   then
;
: line-test ( -- )
   ." Starting External Audio Line-out/Line-in Loopback Test" cr
   6 d# 84 ext-audio
;
: mic-test ( -- )
   ." Starting External Audio Headphone/Microphone Loopback Test" cr
   h# 86 d# 63 ext-audio
;

\ The order, as an Ultra 10's: the codec; the external loopbacks (which
\ bft modes turn off, and only mfg-mode sys-ext or the tests enabled run);
\ the registers
: (audio-subs) ( -- )
   codec-test
   obd-mfg h# 20 and if  false to ext-lpbk?  then
   ext-lpbk? obd-mfg 1 = or if
      " external_lpbk" subtest
      ext-lpbk? obd-mfg h# 41 = or if
         line-test mic-test
      else
         " External Audio Test not run: Please set the mfg-mode to sys-ext." fail-msg
      then
   then
   obd-mfg h# 21 = if  true to ext-lpbk?  then
   ind-reg-test
;
: au-bufs ( -- )  /abufs dvma-alloc to abuf-io to abuf-va  ;
\ The DMA channels and codec reset and the buffers freed at the end
\ (whatever the end: an Ultra 10's skips this after a register failure)
: au-done ( -- )
   abuf-va if
      dma-codec-reset
      abuf-va abuf-io /abufs dvma-free  0 to abuf-va
   then
;
: (audio-test) ( -- )  au-bufs  ['] (audio-subs) catch  au-done  throw  ;
: audio-test1 ( -- )  ['] (audio-test) ['] au-open ['] au-close run-with  ;
: audio-test ( -- )  audio-path ['] audio-test1 with-node  ;

\ --- EIDE (ide_test), in the ide node --------------------------------------
\
\ The CMD646 (PCI 1095,0646): its two channels' command and control
\ blocks (BARs 10 and 14, 18 and 1c), each channel reset; a device asked
\ to identify itself at the primary and at the secondary master.

0 value ide-cmd0   0 value ide-ctl0   0 value ide-cmd1   0 value ide-ctl1
0 value ch-cmd   0 value ch-ctl
create ide-id  d# 512 allot

: ide-bar ( bar len -- va )  >r  0 0 rot my-space or h# 0100.0000 or  r> " map-in" $call-parent  ;
\ The secondary channel is there if its BAR is assigned
: second-channel? ( -- flag )
   " assigned-addresses" get-my-property if  false exit  then	( adr len )
   false -rot  begin  dup 0>  while			( flag adr len )
      over l@ h# ff and h# 18 = if  rot drop true -rot  then
      d# 20 /string
   repeat  2drop
;
: select-channel ( n -- )
   if  ide-cmd1 ide-ctl1  else  ide-cmd0 ide-ctl0  then  to ch-ctl to ch-cmd
;
: ide-status ( -- b )  ch-cmd 7 + rb@  ;
: bsy-clear? ( -- flag )  ide-status h# 80 and 0=  ;
\ A channel's reset: SRST for a millisecond (interrupts off), then up to
\ 5 seconds for BSY to clear
: soft-reset ( -- )
   6 ch-ctl 2+ rb!  1 ms  2 ch-ctl 2+ rb!
   ['] bsy-clear? d# 5000 poll drop
;
: ide-open ( -- )
   h# 10 8 ide-bar to ide-cmd0  h# 14 4 ide-bar to ide-ctl0
   second-channel? if  h# 18 8 ide-bar to ide-cmd1  h# 1c 4 ide-bar to ide-ctl1  then
   4 cfg-w@ 1 or 4 cfg-w!
   0 select-channel soft-reset
   ide-cmd1 if  1 select-channel soft-reset  then
;
: ide-close ( -- )
   ide-ctl1 ?dup if  4 " map-out" $call-parent  0 to ide-ctl1  then
   ide-cmd1 ?dup if  8 " map-out" $call-parent  0 to ide-cmd1  then
   ide-ctl0 ?dup if  4 " map-out" $call-parent  0 to ide-ctl0  then
   ide-cmd0 ?dup if  8 " map-out" $call-parent  0 to ide-cmd0  then
;

: probe-cmd-device ( -- )
   " probe-cmd-device" subtest
   0 cfg-l@ h# 0646.1095 <> if
      " CMD IDE controller not detected EIDE, unknown" fail-msg
   then
;

\ An identify command (ec, or a1 for a packet device) to the master: its
\ 256 words read if the device gives them; else whether it answered with
\ an error
: identify ( cmd -- ok? err? )
   h# a0 ch-cmd 6 + rb!  ch-cmd 7 + rb!
   ch-ctl 2+ rb@ drop
   ['] bsy-clear? d# 2000 poll 0= if  false false exit  then
   ide-status  dup 8 and if
      drop
      d# 256 0 do  ch-cmd le-rw@  ide-id i 2* + w!  loop
      true false
   else
      1 and 0<>  false swap
   then
;
\ (The first byte of the model number)
: model-byte ( -- b )  ide-id d# 54 + c@  ;

\ (An identify answered with an error: the channel reset, then the packet
\ device's identify; failing too, the position's device fails.)
: undetected ( position -- )
   0= if  " Hard disk undetected!"  else  " CD-ROM drive undetected!"  then  fail-msg
;
: hd-cd-position ( position -- )
   dup 2 = select-channel				( position )
   h# ec identify					( position ok? err? )
   over 0= over and if
      2drop  soft-reset  h# a1 identify drop		( position ok? )
      0= if  dup undetected  then  true		( position true )
   else
      drop						( position ok? )
   then
   if
      ."     "  model-byte h# ff = if  dup undetected  then  drop
   else
      0= if  ." Primary ATA hard disk not detected"  else  ." CD-ROM drive undetected"  then  cr
   then
   \ (Two line ends each, as an Ultra 10's test gives)
   cr cr
;
: hd-and-cd-check ( -- )
   " hd-and-cd-check" subtest
   0 hd-cd-position
   ide-cmd1 if  2 hd-cd-position  then
;
: (ide-test) ( -- )  probe-cmd-device hd-and-cd-check  ;
: ide-test1 ( -- )  ['] (ide-test) ['] ide-open ['] ide-close run-with  ;
: ide-test ( -- )  ide-path ['] ide-test1 with-node  ;

\ --- Video (video_test), in the Rage's node --------------------------------
\
\ The onboard Rage: its IDs; its frame buffer (the first 1.25 MB of its
\ memory BAR) written with ones and zeros; two scratch registers walked
\ (register block 0 at ff.fc00 of the BAR, the big-endian aperture's).
\ The screen is left black.

0 value vid-ih
0 value rage
h# 100.0000 constant /rage
h# 14.0000 constant /rage-fb
: rage-reg ( n -- adr )  4 * h# ff.fc00 + rage +  ;

: chip-id-check ( -- )
   " mach64-chip-id-vendor-id-check" subtest
   0 cfg-l@  case
      h# 4754.1002 of  ." ATI Rage II GT on Darwin Motherboard" cr  endof
      h# 4750.1002 of  ." Mach64Pro on Darwin+ motherboard" cr  endof
      cr  " Neither ATI Rage II GT nor Pro is detected, device unknown" fail-msg
   endcase
;
: fb-pattern ( l -- )
   /rage-fb 0 do  dup rage i + rl!  4 +loop
   /rage-fb 0 do
      rage i + rl@ over <> if
         4 0 do
            rage j + i + rb@  over h# ff and <> if
               ." An error occurred at" rage j + i + .  " Frame buffer test" fail-msg
            then
         loop
      then
   4 +loop  drop
;
: frame-buffer-test ( -- )
   " video-frame-buffer-test" subtest
   h# ffff.ffff fb-pattern  0 fb-pattern
;
: walk-one-test ( -- )
   " mach64-walk-one-test" subtest
   " Mach64 walk one test failed with pattern " msg
   h# 20 rage-reg rl@				( saved )
   d# 32 0 do
      1 i lshift  dup h# 20 rage-reg rl!  h# 20 rage-reg rl@ <> if  fail  then
   loop
   h# 20 rage-reg rl!
;
: walk-zero-test ( -- )
   " mach64-walk-zero-test" subtest
   " Mach64 walk 0 test failed " msg
   0  d# 32 0 do
      dup h# 21 rage-reg rl!  dup h# 21 rage-reg rl@ <> if  fail  then
      1 rshift h# 8000.0000 or
   loop  drop
;
: vid-open ( -- )
   m64-path open-dev  dup to vid-ih
   0= if  " failed to open video device" fail-msg  then
;
: rage-map ( -- )  0 0 my-space h# 0200.0010 or /rage " map-in" $call-parent to rage  ;
: vid-close ( -- )
   rage ?dup if  /rage " map-out" $call-parent  0 to rage  then
   vid-ih ?dup if  close-dev  0 to vid-ih  then
;
: (video-test) ( -- )
   vid-open  chip-id-check  rage-map
   frame-buffer-test  walk-one-test  walk-zero-test
;
: video-test1 ( -- )  ['] (video-test) catch  vid-close  throw  ;
: video-test ( -- )  m64-path ['] video-test1 with-node  ;

\ With the Rage as the console, the test is not run (an Ultra 10's obdiag
\ says so from the ide node).
: .video-console ( -- )
   ." Please use console output other than monitor such as ttya/ttyb to run this test" cr
   ." It will change the screen you are using" cr
   ." Test not started" cr
;
headers
