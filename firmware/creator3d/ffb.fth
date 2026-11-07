\ SPDX-License-Identifier: MIT
\ Copyright (C) 2026 Andy Taylor
\
\ FCode for the Sun Creator3D (FFB2+, SUNW,ffb) in a UPA slot: the device
\ node and its properties, the monitor's EDID over DDC, the card's set-up in
\ each of its video modes (the DAC's clock, timing and window lookup, and
\ the FBC), and the console as IEEE 1275's display device interface, drawn
\ with the FBC's fill, scroll and font operations, with the card's own
\ colour logo.
\
\ Written from the open documentation of the card: the X.Org sunffb
\ driver's register definitions (ffb_regs.h, ffb_dac.h), the NetBSD and
\ OpenBSD drivers' use of the FBC, Linux's ffb.c, ffbconfig(1M)'s list of
\ video modes, and the card as it is seen working: its properties, and the
\ registers it is left with in each mode. It holds none of Sun's FCode.

\ The card's PROM gives a byte every 4 addresses: start4 says so.
start4
hex

headers

\ --- The card's spaces (offsets in the UPA space) ---------------------------

200000 constant /page          \ (the size mapped of each register space)
200000 constant exp-offset     \ expansion: the board strapping, monitor sense
400000 constant dac-offset     \ the RAMDAC, reached indirectly
600000 constant fbc-offset     \ the FBC

\ --- Register access ------------------------------------------------------

-1 value exp
-1 value dac
-1 value fbc

: map  ( offset -- virt )  my-address + my-space /page " map-in" $call-parent  ;
: unmap  ( virt -- )  /page " map-out" $call-parent  ;
: map-card  ( -- )
   exp-offset map to exp  dac-offset map to dac  fbc-offset map to fbc
;
: unmap-card  ( -- )
   exp unmap  dac unmap  fbc unmap  -1 to exp  -1 to dac  -1 to fbc
;

\ The frame buffer's 8-bit view (4 MiB), mapped at the first open and kept,
\ as the node's "address", as the card keeps it.
-1 value fb8
: map-fb8  ( -- )
   fb8 -1 = if  1000000 my-address + my-space 400000 " map-in" $call-parent  to fb8  then
   fb8 encode-int " address" property
;

\ The DAC's registers are reached through an address and a data word.
: dac!  ( value reg -- )  dac rl!  dac 4 + rl!  ;
: dac@  ( reg -- value )  dac rl!  dac 4 + rl@  ;
: cursor!  ( value reg -- )  dac 8 + rl!  dac c + rl!  ;

: fbc!  ( value reg -- )  fbc + rl!  ;
: fbc@  ( reg -- value )  fbc + rl@  ;

\ The FBC's FIFO: wait for room for n writes; wait until it is idle.
900 constant ucsr
: fifo-wait  ( n -- )  begin  ucsr fbc@ fff and 8 -  over >=  until  drop  ;
: fbc-idle  ( -- )  begin  ucsr fbc@ 03000000 and 0=  until  ;

\ --- The card's identity, read from it ----------------------------------------

8000 constant dac-did
1001 constant dac-uctrl


\ --- Video modes --------------------------------------------------------------

\ The modes the card offers, each set up as the card itself sets it up. Open
\ takes one as its argument ("screen:r1280x1024x76", as output-device names
\ it); ffbconfig(1M) lists them. A row a mode: width, height, refresh rate,
\ the name's suffix (s stereo, i), monitor_mode (1 stereo, 2 the high
\ resolutions), the pixel PLL, the timing generator's control, the DAC
\ control, the timing generator's 14 registers (VBNP to EINEQV), then the
\ FBC's frame buffer configuration (FBCFG0: the 3D-RAM's refresh, the
\ screen's extent, and the frame buffer's organisation: standard, high
\ resolution or stereo).
d# 23 constant /mode
create modes
\    width       height     rate   suffix mm ppll   tgen  dacctrl  VBNP .. EINEQV   FBCFG0
   d#  640 , d#  480 , d#  60 ,  0 , 0 , 54c3 , 2b ,   0 , d# 34 , d# 514 , d# 1 , d# 524 , d# 351 , d# 71 , d# 391 , d# 47 , d# 399 , d# 387 , d# 67 , d# 0 , d# 0 , d# 0 , f32003ef ,
   d#  640 , d#  480 , d#  60 , 69 , 0 , 5dd0 , 63 ,  80 , d# 37 , d# 517 , d# 5 , d# 524 , d# 165 , d# 59 , d# 184 , d# 28 , d# 194 , d# 180 , d# 55 , d# 13 , d# 11 , d# 518 , ce2003e7 ,
   d#  768 , d#  575 , d#  50 , 69 , 0 , 5b34 , 63 ,  80 , d# 43 , d# 618 , d# 4 , d# 624 , d# 200 , d# 76 , d# 226 , d# 34 , d# 233 , d# 222 , d# 72 , d# 16 , d# 9 , d# 619 , ca2804a7 ,
   d#  800 , d#  600 , d#  75 ,  0 , 0 , 4cc2 , 2b , 100 , d# 23 , d# 623 , d# 2 , d# 624 , d# 487 , d# 119 , d# 519 , d# 39 , d# 527 , d# 515 , d# 115 , d# 0 , d# 0 , d# 0 , fca8002b ,
   d#  960 , d#  680 , d# 108 , 73 , 1 , 42a5 , 33 ,  80 , d# 37 , d# 717 , d# 3 , d# 720 , d# 607 , d# 127 , d# 607 , d# 31 , d# 639 , d# 603 , d# 123 , d# 0 , d# 0 , d# 0 , b0300013 ,
   d#  960 , d#  680 , d# 112 , 73 , 1 , 432d , 33 ,  80 , d# 39 , d# 719 , d# 7 , d# 721 , d# 599 , d# 131 , d# 611 , d# 23 , d# 623 , d# 607 , d# 127 , d# 0 , d# 0 , d# 0 , adb00013 ,
   d# 1024 , d#  768 , d#  60 ,  0 , 0 , 4ab0 , 2b ,   0 , d# 34 , d# 802 , d# 5 , d# 805 , d# 603 , d# 147 , d# 659 , d# 67 , d# 671 , d# 655 , d# 143 , d# 0 , d# 0 , d# 0 , c634062f ,
   d# 1024 , d#  768 , d#  70 ,  0 , 0 , 4bce , 2b ,   0 , d# 34 , d# 802 , d# 5 , d# 805 , d# 595 , d# 139 , d# 651 , d# 67 , d# 663 , d# 647 , d# 135 , d# 0 , d# 0 , d# 0 , c0b4002b ,
   d# 1024 , d#  768 , d#  75 ,  0 , 0 , 4b46 , 2b , 100 , d# 30 , d# 798 , d# 2 , d# 799 , d# 607 , d# 135 , d# 647 , d# 47 , d# 655 , d# 643 , d# 131 , d# 0 , d# 0 , d# 0 , d234002b ,
   d# 1024 , d#  768 , d#  77 ,  0 , 0 , 4b4b , 33 ,  80 , d# 34 , d# 802 , d# 3 , d# 804 , d# 615 , d# 151 , d# 663 , d# 31 , d# 679 , d# 659 , d# 147 , d# 0 , d# 0 , d# 0 , c7b4002b ,
   d# 1024 , d#  800 , d#  84 ,  0 , 0 , 4ac6 , 33 ,  80 , d# 34 , d# 834 , d# 3 , d# 836 , d# 637 , d# 139 , d# 651 , d# 63 , d# 659 , d# 647 , d# 135 , d# 0 , d# 0 , d# 0 , ad34002b ,
   d# 1152 , d#  900 , d#  66 ,  0 , 0 , 4ac6 , 33 ,  80 , d# 34 , d# 934 , d# 3 , d# 936 , d# 699 , d# 167 , d# 743 , d# 63 , d# 763 , d# 739 , d# 163 , d# 0 , d# 0 , d# 0 , c83c002b ,
   d# 1152 , d#  900 , d#  76 ,  0 , 0 , 42a8 , 33 ,  80 , d# 40 , d# 940 , d# 7 , d# 942 , d# 687 , d# 159 , d# 735 , d# 63 , d# 751 , d# 731 , d# 155 , d# 0 , d# 0 , d# 0 , c93c002b ,
   d# 1280 , d#  800 , d#  76 ,  0 , 0 , 432d , 33 ,  80 , d# 42 , d# 842 , d# 6 , d# 845 , d# 727 , d# 134 , d# 774 , d# 56 , d# 784 , d# 770 , d# 130 , d# 0 , d# 0 , d# 0 , f740002b ,
   d# 1280 , d# 1024 , d#  60 ,  0 , 0 , 42a8 , 2b , 100 , d# 40 , d# 1064 , d# 2 , d# 1065 , d# 787 , d# 179 , d# 819 , d# 55 , d# 843 , d# 815 , d# 175 , d# 0 , d# 0 , d# 0 , fd4003ef ,
   d# 1280 , d# 1024 , d#  67 ,  0 , 0 , 4334 , 33 ,  80 , d# 40 , d# 1064 , d# 7 , d# 1066 , d# 767 , d# 167 , d# 807 , d# 55 , d# 815 , d# 803 , d# 163 , d# 0 , d# 0 , d# 0 , c9c0002b ,
   d# 1280 , d# 1024 , d#  75 ,  0 , 0 , 42b2 , 2b , 100 , d# 40 , d# 1064 , d# 2 , d# 1065 , d# 771 , d# 195 , d# 835 , d# 71 , d# 843 , d# 831 , d# 191 , d# 0 , d# 0 , d# 0 , fd40002b ,
   d# 1280 , d# 1024 , d#  76 ,  0 , 0 , 42b2 , 33 ,  80 , d# 39 , d# 1063 , d# 7 , d# 1065 , d# 799 , d# 175 , d# 815 , d# 31 , d# 831 , d# 811 , d# 171 , d# 0 , d# 0 , d# 0 , adc0002b ,
   d# 1280 , d# 1024 , d#  85 ,  0 , 0 , 4346 , 2b , 100 , d# 46 , d# 1070 , d# 2 , d# 1071 , d# 783 , d# 191 , d# 831 , d# 79 , d# 863 , d# 827 , d# 187 , d# 0 , d# 0 , d# 0 , f8c0002b ,
   d# 1440 , d#  900 , d#  76 ,  0 , 2 , 42b2 , 33 ,  80 , d# 41 , d# 941 , d# 2 , d# 943 , d# 429 , d# 99 , d# 459 , d# 39 , d# 469 , d# 455 , d# 95 , d# 0 , d# 0 , d# 0 , ef24003b ,
   d# 1600 , d# 1000 , d#  66 ,  0 , 2 , 42b2 , 33 ,  80 , d# 36 , d# 1036 , d# 4 , d# 1038 , d# 457 , d# 81 , d# 481 , d# 33 , d# 491 , d# 477 , d# 77 , d# 0 , d# 0 , d# 0 , eaa8003b ,
   d# 1600 , d# 1000 , d#  76 ,  0 , 2 , 42bf , 33 ,  80 , d# 48 , d# 1048 , d# 2 , d# 1050 , d# 477 , d# 123 , d# 523 , d# 53 , d# 531 , d# 519 , d# 119 , d# 0 , d# 0 , d# 0 , f528003b ,
   d# 1600 , d# 1200 , d#  75 ,  0 , 2 , 423c , 2b , 100 , d# 48 , d# 1248 , d# 2 , d# 1249 , d# 491 , d# 123 , d# 523 , d# 47 , d# 539 , d# 519 , d# 119 , d# 0 , d# 0 , d# 0 , fc28003b ,
   d# 1600 , d# 1280 , d#  76 ,  0 , 2 , 42d0 , 33 ,  80 , d# 57 , d# 1337 , d# 7 , d# 1339 , d# 475 , d# 123 , d# 523 , d# 53 , d# 529 , d# 519 , d# 119 , d# 0 , d# 0 , d# 0 , c728003b ,
   d# 1920 , d# 1080 , d#  72 ,  0 , 2 , 42d0 , 33 ,  80 , d# 88 , d# 1168 , d# 2 , d# 1171 , d# 585 , d# 147 , d# 627 , d# 53 , d# 639 , d# 623 , d# 143 , d# 0 , d# 0 , d# 0 , ca30003b ,
   d# 1920 , d# 1200 , d#  70 ,  0 , 2 , 4241 , 33 ,  80 , d# 38 , d# 1238 , d# 2 , d# 1239 , d# 571 , d# 142 , d# 622 , d# 56 , d# 628 , d# 618 , d# 138 , d# 0 , d# 0 , d# 0 , de30003b ,
here modes - /mode /n * / constant #modes

0 value mode                                    \ the mode in use: its row
: mode@  ( field -- n )  /n * mode + @  ;
: width  ( -- n )  0 mode@  ;
: height  ( -- n )  1 mode@  ;
: high-res?  ( -- flag )  4 mode@ 2 =  ;

\ A mode's name: width x height x rate, and its suffix if it has one
\ ("1280x1024x76", "960x680x112s").
d# 16 constant /name
create name-buf /name allot
0 value name-len
: +name  ( adr len -- )
   tuck  name-buf name-len + swap move  name-len + to name-len
;
: mode-name  ( -- adr len )
   0 to name-len
   base @ >r  d# 10 base !
   0 mode@ (u.) +name  " x" +name  1 mode@ (u.) +name  " x" +name  2 mode@ (u.) +name
   r> base !
   3 mode@ ?dup if  name-buf name-len + c!  name-len 1+ to name-len  then
   name-buf name-len
;

\ The mode a name begins with: of those whose names it begins with, the
\ longest ("640x480x60ix" is 640x480x60i; "1280x1024x76junk" 1280x1024x76).
0 value best
0 value best-len
: find-mode  ( adr len -- row | 0 )
   0 to best  0 to best-len
   #modes 0 ?do
      modes i /mode * /n * +  to mode
      2dup mode-name                            ( adr len adr nadr nlen )
      rot over >= if                            ( adr len adr nadr nlen )
         dup >r comp 0= r> swap                 ( adr len nlen match? )
         over best-len > and if  to best-len  mode to best  else  drop  then
      else  drop 2drop  then
   loop
   2drop best
;

\ The mode a monitor's three sense lines name, read in the expansion space
\ (7, nothing sensed, for a monitor without them).
: sense-name  ( -- adr len )
   exp 10 + rl@ d# 24 rshift 7 and
   case
      0 of  " 1024x768x77"  endof
      2 of  " 1280x1024x76"  endof
      4 of  " 1280x1024x67"  endof
      5 of  " 1024x768x60"  endof
      6 of  " 1152x900x76"  endof
      >r  " 1152x900x66"  r>
   endcase
;

\ --- The monitor's EDID, over DDC ---------------------------------------------

\ The DAC's monitor port: SCL (bit 0) and SDA (bit 1), driven through one
\ register and read back through another; open drain, a 1 letting the line
\ go. Each change is held for a few DAC reads.
8001 constant mpdata
8002 constant mpsense
3 value lines
: lines!  ( n -- )  dup to lines  mpdata dac!  4 0 do  mpsense dac@ drop  loop  ;
: scl  ( flag -- )  if  lines 1 or  else  lines 1 invert and  then  lines!  ;
: sda  ( flag -- )  if  lines 2 or  else  lines 2 invert and  then  lines!  ;
: sda?  ( -- flag )  mpsense dac@ 2 and 0<>  ;
: i2c-start  ( -- )  true sda  true scl  false sda  false scl  ;
: i2c-stop  ( -- )  false sda  true scl  true sda  ;
: bit-out  ( flag -- )  sda  true scl  false scl  ;
: bit-in  ( -- flag )  true sda  true scl  sda?  false scl  ;
\ A byte out, and whether it was acknowledged; a byte in, acknowledged
\ unless it is the last.
: byte-out  ( byte -- ack? )
   8 0 do  dup 80 and bit-out  2*  loop  drop  bit-in 0=
;
: byte-in  ( last? -- byte )
   0  8 0 do  2*  bit-in 1 and or  loop  swap bit-out
;
\ The bus freed (nine clocks with SDA let go, then a stop).
: i2c-free  ( -- )  true sda  9 0 do  false scl  true scl  loop  i2c-stop  ;

\ The EDID's first block, from the memory at I2C address 50: its offset
\ set to 0, then 128 bytes read. Three tries for one that checks: the
\ header, and the bytes summing to 0. Kept as read (zero if nothing
\ answered) as the node's edid_data; no_edid_blks counts the block and its
\ extensions, 0 if none checks.
d# 128 constant /edid
create edid /edid allot
: edid-read  ( -- answered? )
   i2c-free
   i2c-start  a0 byte-out dup if  drop 0 byte-out  then  i2c-stop
   dup if
      i2c-start  a1 byte-out and
      dup if  /edid 0 do  i /edid 1- =  byte-in  edid i + c!  loop  then
      i2c-stop
   then
;
: edid-ok?  ( -- flag )
   0  /edid 0 do  edid i + c@ +  loop  ff and 0=
   edid c@ 0= and  edid 7 + c@ 0= and
   7 1 do  edid i + c@ ff = and  loop
;
: fetch-edid  ( -- )
   edid /edid erase
   3 0 do  edid-read if  edid-ok? if  leave  then  then  loop
;
: edid-properties  ( -- )
   edid-ok? if  edid 7e + c@ 1+  else  0  then  encode-int " no_edid_blks" property
   edid /edid encode-bytes " edid_data" property
;

\ The card's mode of a width, height and rate (not stereo or interlaced).
0 value want-w
0 value want-h
0 value want-r
: mode-of  ( w h r -- row | 0 )
   to want-r  to want-h  to want-w
   0  #modes 0 do
      modes i /mode * /n * +  to mode
      0 mode@ want-w =  1 mode@ want-h = and  2 mode@ want-r = and  3 mode@ 0= and
      if  drop mode  leave  then
   loop
;
\ The modes the card takes from a monitor's standard timings (not
\ 1024x768x77, 1600x1280x76 or those 16:10).
create std-modes
   d#  640 , d#  480 , d# 60 ,   d#  800 , d#  600 , d# 75 ,
   d# 1024 , d#  768 , d# 60 ,   d# 1024 , d#  768 , d# 70 ,   d# 1024 , d#  768 , d# 75 ,
   d# 1280 , d# 1024 , d# 60 ,   d# 1280 , d# 1024 , d# 67 ,   d# 1280 , d# 1024 , d# 75 ,
   d# 1280 , d# 1024 , d# 76 ,   d# 1280 , d# 1024 , d# 85 ,
   d# 1600 , d# 1200 , d# 75 ,   d# 1920 , d# 1080 , d# 72 ,
here std-modes - 3 /n * / constant #std-modes
: std-mode?  ( w h r -- flag )
   false  #std-modes 0 do
      std-modes i 3 * /n * +  >r
      3 pick r@ @ =  3 pick r@ na1+ @ = and  2 pick r> 2 na+ @ = and  or
   loop
   nip nip nip
;
\ The i'th standard timing's mode: width (bytes / 8 - 31), height from
\ the aspect ratio (16:10, 4:3, 5:4, 16:9), rate (60 and up).
: std-mode  ( i -- row | 0 )
   2* d# 38 + edid +  dup c@ swap 1+ c@             ( b1 b2 )
   over 1 = over 1 = and if  2drop 0 exit  then
   swap d# 31 + 8 *                                 ( b2 w )
   over 6 rshift
   case
      0 of  dup a * d# 16 /  endof
      1 of  dup 3 * 4 /  endof
      2 of  dup 4 * 5 /  endof
      3 of  dup 9 * d# 16 /  endof
   endcase                                          ( b2 w h )
   rot 3f and d# 60 +                               ( w h r )
   3dup std-mode? if  mode-of  else  3drop 0  then
;
\ The established timings the card takes, the first of those the monitor
\ has: (byte, bit) and the mode.
create est-modes
   24 , 01 , d# 1280 , d# 1024 , d# 75 ,   24 , 02 , d# 1024 , d# 768 , d# 75 ,
   24 , 04 , d# 1024 , d#  768 , d# 70 ,   24 , 08 , d# 1024 , d# 768 , d# 60 ,
   24 , 40 , d#  800 , d#  600 , d# 75 ,   23 , 20 , d#  640 , d# 480 , d# 60 ,
here est-modes - 5 /n * / constant #est-modes
: est-mode  ( -- row | 0 )
   0  #est-modes 0 do
      est-modes i 5 * /n * +                       ( 0 entry )
      dup @ edid + c@  over na1+ @ and if
         nip  dup 2 na+ @  over 3 na+ @  rot 4 na+ @  mode-of  leave
      then
      drop
   loop
;
\ The monitor's mode by its EDID: the first standard timing the card
\ takes, else the first established one.
: edid-mode  ( -- row | 0 )
   edid-ok? 0= if  0 exit  then
   0  8 0 do  i std-mode ?dup if  nip leave  then  loop
   ?dup 0= if  est-mode  then
;

\ With no mode named (or none that is one), the monitor's: by its EDID,
\ else its sense code.
: default-mode  ( -- row )  edid-mode ?dup 0= if  sense-name find-mode  then  ;
\ The mode open names ("screen:r1280x1024x76"), or the monitor's.
: select-mode  ( -- )
   my-args  dup if  over c@ ascii r =  else  false  then
   if  1- swap 1+ swap find-mode  else  2drop 0  then
   ?dup 0= if  default-mode  then  to mode
;

\ The node's description of the mode.
: mode-properties  ( -- )
   width encode-int " width" property
   height encode-int " height" property
   2 mode@ encode-int " v_freq" property
   4 mode@ encode-int " monitor_mode" property
;

\ --- The DAC: clock, timing, window lookup ------------------------------------

\ The pixel clock is 13.5 MHz x M / D / 2^PFD. The timing generator counts
\ lines vertically, and horizontally pixels by two (by four in the high
\ resolutions, which take the pixels 8/2:1 rather than 4/2:1). The high
\ resolutions are single-buffered: the Combined window mode, with an
\ overlay; the others Separate_8, double-buffered, with an overlay.
: set-timing  ( -- )
   0 6000 dac!                                  \ the timing generator off
   5 mode@ 0 dac!                               \ the pixel PLL
   high-res? if  3 8  else  2 2c  then  1001 dac!  1000 dac!
   7 mode@ 5001 dac!                            \ sync
   d# 14 0 do  8 i + mode@  6001 i + dac!  loop
   6 mode@ 6000 dac!                            \ on: video, timing, master
;

\ The FBC's frame buffer configuration, as the card sets it for the mode
\ (the interlaced modes' FBCFG2 differing).
: set-fbcfg  ( -- )
   d# 22 mode@ 10270 fbc!
   00133600 10274 fbc!
   3 mode@ ascii i = if  02403520  else  02402520  then  10278 fbc!
   00000800 1027c fbc!
;

\ Window ID 0 true colour from buffer A, its overlay not shown; in the high
\ resolutions every window ID. Written to the shadow lookups, then
\ transferred. The cursor off, out of sight.
4000 constant wlut-true
: set-windows  ( -- )
   d# 64 0 do  high-res? i 0= or if  wlut-true  else  0  then  3200 i + dac!  loop
   4 0 do  i if  0  else  high-res? if  500  else  100  then  then  3100 i + dac!  loop
   high-res? if  e0 ff 1f  else  0 0 33f  then  3153 dac!  3152 dac!  3151 dac!
   2 3150 dac!                                  \ transfer the window lookup
   3 100 cursor!                                \ the cursor's planes off
   8fc08fc0 104 cursor!
;

\ Palette 0 a grey ramp, but for the console's white (0) and black (ff).
: set-palette  ( -- )
   100 0 do  i 10101 *  2000 i + dac!  loop
   ffffff 2000 dac!  0 20ff dac!
;

\ --- The FBC ------------------------------------------------------------------

\ Pixel processor: constant colour, window ID from the WID register, opaque
\ background, no clipping, blending, patterns or depth cue.
00069a07 constant ppc-plain
083 constant rop-new
085 constant rop-old
08a constant rop-invert
\ Frame buffer control: buffer A written and read, RGB through the plane
\ mask; the X planes (window ID) written or not; buffers A and B.
2000707f constant fbc-x-off
600070bf constant fbc-ab-x-on

: rectangle  ( x y w h -- )
   4 fifo-wait  >r >r  60 fbc!  64 fbc!  r> r>  70 fbc!  74 fbc!   \ (y, x, h, w: w starts it)
;
: drawop  ( rop op -- )  3 fifo-wait  ppc-plain 200 fbc!  300 fbc!  258 fbc!  ;
: colours  ( fg bg -- )  2 fifo-wait  20c fbc!  208 fbc!  ;

: init-fbc  ( -- )
   7 fifo-wait
   0 204 fbc!                                   \ window ID 0
   ppc-plain 200 fbc!
   fbc-ab-x-on 254 fbc!
   rop-new 258 fbc!
   8 300 fbc!                                   \ rectangles
   ffffffff 290 fbc!                            \ every plane
   10000 31c fbc!                               \ font rows a line apart
   ffffff 0 colours
   0 0 width height rectangle                   \ both buffers white, window ID 0
   fbc-idle
   \ The view clip, compare, X and window ID plane masks and stencil as the
   \ card leaves them.
   8 fifo-wait
   03ff07ff 224 fbc!  80808080 25c fbc!  ff 294 fbc!  ff 35c fbc!
   f0000000 338 fbc!  33300000 33c fbc!
   high-res? if  33300000  else  33380000  then  2cc fbc!
   high-res? 0= if  101 2d8 fbc!  then
   1 fifo-wait  fbc-x-off 254 fbc!
;

\ --- The console ------------------------------------------------------------

\ Black on white, or white on black for an inverse screen; swapped again
\ for inverse text.
: back  ( -- colour )  inverse-screen? if 0 else ffffff then  ;
: fore  ( -- colour )  back ffffff xor  ;
: text-colours  ( -- fg bg )
   fore back  inverse? if swap then
;

: cell-x  ( column -- x )  char-width * window-left +  ;
: line-y  ( line -- y )  char-height * window-top +  ;

: fill-rect  ( x y w h colour -- )
   0 colours  rop-new 8 drawop  rectangle
;
: invert-rect  ( x y w h -- )  rop-invert 8 drawop  rectangle  ;

\ Draws a glyph's rows through the FBC's font registers: each row's bits,
\ first bit at the top, the FBC stepping a line down after each.
: glyph-row  ( adr -- bits )
   0  fontbytes 0 do  8 lshift  over i + c@ or  loop  nip
   d# 32 fontbytes 8 * - lshift
;
: ffb-draw-character  ( char -- )
   >font
   text-colours colours
   4 fifo-wait
   ppc-plain 200 fbc!  rop-new 258 fbc!
   column# cell-x  line# line-y d# 16 lshift or  314 fbc!
   char-width 318 fbc!
   char-height 0 do
      1 fifo-wait  dup glyph-row 320 fbc!  fontbytes +
   loop
   drop
;

: ffb-toggle-cursor  ( -- )
   column# cell-x line# line-y char-width char-height invert-rect  fbc-idle
;
: ffb-erase-screen  ( -- )  0 0 width height back fill-rect  fbc-idle  ;
: ffb-invert-screen  ( -- )  0 0 width height invert-rect  fbc-idle  ;
: ffb-blink-screen  ( -- )  ffb-invert-screen d# 20 ms ffb-invert-screen  ;
: ffb-reset-screen  ( -- )  ;

\ Moves a block of the text area: the FBC's vertical scroll, from (sx, sy)
\ to (dx, dy).
: scroll  ( sx sy dx dy w h -- )
   rop-old dup 8 lshift or  b drawop
   6 fifo-wait  >r >r >r >r  60 fbc!  64 fbc!  r> r>  68 fbc!  6c fbc!  r> r>  70 fbc!  74 fbc!
   fbc-idle
;
: text-width  ( -- w )  #columns char-width *  ;

\ Lines from the cursor's down: n deleted (those below moved up, blank lines
\ at the bottom), or n inserted (those below moved down).
: ffb-delete-lines  ( n -- )
   #lines line# - min  dup 0> if
      >r
      window-left  line# r@ + line-y  window-left  line# line-y
      text-width  #lines line# - r@ - char-height *  scroll
      window-left  #lines r@ - line-y  text-width  r> char-height *  back fill-rect
      fbc-idle
   else  drop  then
;
: ffb-insert-lines  ( n -- )
   #lines line# - min  dup 0> if
      >r
      window-left  line# line-y  window-left  line# r@ + line-y
      text-width  #lines line# - r@ - char-height *  scroll
      window-left  line# line-y  text-width  r> char-height *  back fill-rect
      fbc-idle
   else  drop  then
;

\ Characters from the cursor's right, on its line.
: ffb-delete-characters  ( n -- )
   #columns column# - min  dup 0> if
      >r
      column# r@ + cell-x  line# line-y  column# cell-x  line# line-y
      #columns column# - r@ - char-width *  char-height  scroll
      #columns r@ - cell-x  line# line-y  r> char-width *  char-height  back fill-rect
      fbc-idle
   else  drop  then
;
: ffb-insert-characters  ( n -- )
   #columns column# - min  dup 0> if
      >r
      column# cell-x  line# line-y  column# r@ + cell-x  line# line-y
      #columns column# - r@ - char-width *  char-height  scroll
      column# cell-x  line# line-y  r> char-width *  char-height  back fill-rect
      fbc-idle
   else  drop  then
;

\ OpenBoot's logo, a bitmap a bit a pixel, drawn at the text's left on a line:
\ 32 columns at a time through the font registers.
0 value logo-y
0 value logo-adr
0 value logo-bytes
0 value logo-height
: logo-word  ( row column32 -- bits )
   4 *  swap logo-bytes *  +  logo-adr +
   0  4 0 do  8 lshift  over i + c@ or  loop  nip
;
: mono-logo  ( line# adr width height -- )
   to logo-height  8 / to logo-bytes  to logo-adr  line-y to logo-y
   fore back colours
   3 fifo-wait  ppc-plain 200 fbc!  rop-new 258 fbc!  d# 32 318 fbc!
   logo-bytes 3 + 4 / 0 ?do
      1 fifo-wait  logo-y d# 16 lshift  window-left i d# 32 * +  or  314 fbc!
      logo-height 0 ?do
         i j logo-word  1 fifo-wait  320 fbc!
      loop
   loop
   fbc-idle
;

\ The card's own logo, in colour: a Sun raster file in its PROM after the
\ FCode, at PROM byte 4000 (24 bits a pixel, R, G and B, no colour map),
\ drawn at the text's left through the frame buffer's 32-bit view; or,
\ without it, OpenBoot's.
4000 constant logo-at
-1 value prom
: prom-c@  ( n -- byte )  4 * prom + rb@  ;
: prom-l@  ( n -- l )  0  4 0 do  8 lshift  over i + prom-c@ or  loop  nip  ;
: ras@  ( field -- n )  4 * logo-at + prom-l@  ;
: colour-logo?  ( -- flag )
   0 ras@ 59a66a95 =  3 ras@ d# 24 = and  5 ras@ 3 = and  6 ras@ 0= and
;
-1 value logo-fb
: draw-colour-logo  ( y -- )
   2 ras@ 2000 *  >r                            ( y )  ( r: bytes )
   2000 * 2000000 + my-address + my-space r@ " map-in" $call-parent  to logo-fb
   logo-at 20 +                                 ( n )
   2 ras@ 0 ?do
      1 ras@ 0 ?do
         dup prom-c@  over 1+ prom-c@ 8 lshift or  over 2 + prom-c@ d# 16 lshift or
         logo-fb j 2000 * +  window-left i + 4 * +  rl!
         3 +
      loop
   loop
   drop
   logo-fb r> " map-out" $call-parent
;
: ffb-draw-logo  ( line# adr width height -- )
   my-address my-space 40000 " map-in" $call-parent to prom
   colour-logo? if  2drop drop  line-y draw-colour-logo  else  mono-logo  then
   prom 40000 " map-out" $call-parent
;

\ --- The node -----------------------------------------------------------------

\ Its properties, made in the order the card makes them. The mode's are the
\ monitor's until an open names another.
" SUNW,ffb" device-name
" display" device-type
map-card
fetch-edid  edid-properties
default-mode to mode  mode-properties
\ The UPA port: the slot's address is 1c0.0000.0000 + port << 33.
my-space 1c0 - 2/ encode-int " upa-portid" property
5 encode-int " interrupts" property
0 0 encode-bytes " upa-interrupt-slave" property
\ The card's identity, read from it.
exp rl@ d# 24 rshift ff and encode-int " board_type" property
" SUNW,501-4788" model
" UltraViolet ffb.fth 0.4" encode-string " fcode_version" property
dac-uctrl dac@ 8 rshift f and encode-int " ramdac_rev" property
800 fbc@ encode-int " fbc_reg_id" property
unmap-card
\ The card's spaces, as a real card names them: the PROM, DAC, FBC, then
\ the frame buffer's views.
: reg+  ( adr len offset size -- adr' len' )
   >r my-address + my-space encode-phys encode+
   0 encode-int encode+  r> encode-int encode+
;
0 0 encode-bytes
       0     400 reg+   400000  200000 reg+   600000  200000 reg+
 1000000  400000 reg+  1400000  400000 reg+  1800000  400000 reg+
 1c00000  400000 reg+  2000000 1000000 reg+  3000000 1000000 reg+
 4000000  400000 reg+  4400000  400000 reg+  4800000  400000 reg+
 4c00000  400000 reg+  5000000 1000000 reg+  6000000 2000000 reg+
 9000000  800000 reg+  9800000  800000 reg+  a000000 1000000 reg+
 b000000  800000 reg+  b800000  800000 reg+  c000000  400000 reg+
 c800000  800000 reg+  d000000  800000 reg+  d800000  800000 reg+
" reg" property

\ --- Install and remove ---------------------------------------------------

: ffb-install  ( -- )
   map-card
   fetch-edid  edid-properties
   select-mode mode-properties
   my-self encode-int " ihandle" property
   map-fb8
   " ISO8859-1" encode-string " character-set" property
   set-timing  set-fbcfg  set-windows  set-palette  init-fbc
   \ The text area: fb8-install sizes it from the screen-#columns and
   \ screen-#rows options and centres it; the drawing is all ours. (It
   \ puts the left edge on a multiple of 8, for its own 8-bit drawing; the
   \ card centres it exactly, which differs at 640 and 800 wide.)
   default-font set-font
   width height  width char-width /  height char-height /  fb8-install
   width #columns char-width * - 2/ to window-left
   ['] ffb-draw-character to draw-character
   ['] ffb-reset-screen to reset-screen
   ['] ffb-toggle-cursor to toggle-cursor
   ['] ffb-erase-screen to erase-screen
   ['] ffb-blink-screen to blink-screen
   ['] ffb-invert-screen to invert-screen
   ['] ffb-insert-characters to insert-characters
   ['] ffb-delete-characters to delete-characters
   ['] ffb-insert-lines to insert-lines
   ['] ffb-delete-lines to delete-lines
   ['] ffb-draw-logo to draw-logo
;

: ffb-remove  ( -- )  unmap-card  ;
: ffb-selftest  ( -- 0 )  0  ;

' ffb-install is-install
' ffb-remove is-remove
' ffb-selftest is-selftest

fcode-end
