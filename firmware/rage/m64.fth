\ SPDX-License-Identifier: MIT
\ Copyright (C) 2026 Andy Taylor
\
\ FCode for the ATI Rage Pro (mach64 GT-C, pci1002,4750) on the Sun Ultra 5
\ and Ultra 10's motherboard, and on Sun's PGX24 card (SUNW,m64B): the
\ device node and its properties, the chip's start from cold (bus, memory
\ and clock synthesiser), the size of its memory, the monitor's EDID over
\ DDC, the chip's set-up in each of its video modes, and the console as
\ IEEE 1275's display device interface, drawn in its 8-bit frame buffer.
\
\ Written from the open documentation of the chip: the register
\ definitions of X.Org's mach64 driver and Linux's atyfb, and the chip as
\ it is seen working: its properties, the order in which it is brought up,
\ and the registers it is left with in each mode. It holds none of Sun's
\ or ATI's FCode.

\ A PCI expansion ROM image: the header names the chip and its class
\ (a VGA-less display controller, 030000).
tokenizer[ h# 1002 h# 4750 h# 030000 ]tokenizer pci-header

fcode-version2
hex

headers

\ --- The chip's spaces ----------------------------------------------------------

\ The 16 MiB aperture (BAR 0, PCI register 10): the frame buffer's
\ little-endian view in its lower half, with the registers in its last
\ 2 KiB (block 1, then block 0); and the 4 KiB register space (BAR 2, PCI
\ register 18). The lower half is mapped, the registers through it.
800000 constant /aperture
7ffc00 constant regs-offset
02000010 constant bar0
02000018 constant bar2

-1 value aperture
-1 value regs

: config-w@  ( reg -- w )  my-space + " config-w@" $call-parent  ;
: config-w!  ( w reg -- )  my-space + " config-w!" $call-parent  ;
\ The chip answers memory cycles: memory space on in its command register.
: memory-on  ( -- )  4 config-w@ 2 or 4 config-w!  ;
: memory-off  ( -- )  4 config-w@ 2 invert and 4 config-w!  ;

: map-chip  ( -- )
   memory-on
   0 0 my-space bar0 or /aperture " map-in" $call-parent  to aperture
   aperture regs-offset + to regs
;
: unmap-chip  ( -- )
   aperture /aperture " map-out" $call-parent  -1 to aperture  -1 to regs
;

\ --- Register access ------------------------------------------------------------

\ The registers are little-endian, as PCI is; the parent's mapping of PCI
\ space stores a value in PCI's byte order, so rl! and rw! take the
\ registers' values as they are. Block 0's registers by offset; block 1's
\ below it.
: reg@  ( offset -- l )  regs + rl@  ;
: reg!  ( l offset -- )  regs + rl!  ;
: regw!  ( w offset -- )  regs + rw!  ;
: regb@  ( offset -- b )  regs + rb@  ;
: regb!  ( b offset -- )  regs + rb!  ;

00 constant crtc-h-total-disp
04 constant crtc-h-sync
08 constant crtc-v-total-disp
0c constant crtc-v-sync
14 constant crtc-off-pitch
18 constant crtc-int-cntl
1c constant crtc-gen-cntl
20 constant dsp-config
24 constant dsp-on-off
34 constant mem-addr-config
40 constant ovr-clr
44 constant ovr-wid-left-right
48 constant ovr-wid-top-bottom
78 constant gp-io
90 constant clock-cntl
a0 constant bus-cntl
ac constant ext-mem-cntl
b0 constant mem-cntl
c0 constant dac-regs
c4 constant dac-cntl
d0 constant gen-test-cntl
e0 constant config-chip-id
e4 constant config-stat0
1b4 constant src-cntl
1fc constant scale-3d-cntl
-fc constant setup-cntl                         \ (block 1)

\ The clock synthesiser's registers, through CLOCK_CNTL: the register's
\ number (and the write enable), then its value.
: pll!  ( b n -- )  2 lshift 2 or  clock-cntl 1+ regb!  clock-cntl 2+ regb!  ;
: pll@  ( n -- b )  2 lshift  clock-cntl 1+ regb!  clock-cntl 2+ regb@  ;

02 constant pll-ref-div
06 constant vclk-post-div
07 constant vclk0-fb-div
0b constant pll-ext-cntl

\ The CRTC: its top byte turns it on (3) or leaves it off with the
\ display's extended mode selected (1).
: crtc-off  ( -- )  1 crtc-gen-cntl 3 + regb!  ;
: crtc-on  ( -- )  3 crtc-gen-cntl 3 + regb!  ;

\ --- From cold --------------------------------------------------------------------

\ The memory's start-up commands, through EXT_MEM_CNTL's low byte (the
\ SDRAM's precharge, mode set and refresh), as the chip is brought up.
: memory-start  ( -- )
   ext-mem-cntl regb@ drop
   3 ext-mem-cntl regb!  b ext-mem-cntl regb!  f ext-mem-cntl regb!
   3 ext-mem-cntl regb!  1 ext-mem-cntl regb!
;

\ The chip from reset: its bus interface, memory controller, DAC and
\ clock synthesiser (a 14.318 MHz crystal, reference divider 33; the memory
\ clock), then its memory started and its drawing engines idle.
: chip-start  ( -- )
   7b33a040 bus-cntl reg!
   0 crtc-int-cntl regw!
   01000240 crtc-gen-cntl reg!
   0 gen-test-cntl reg!
   00130c01 ext-mem-cntl reg!
   00000202 mem-addr-config reg!
   00651a73 mem-cntl reg!
   001d config-stat0 regw!
   80010102 dac-cntl reg!                         \ the DAC 8 bits a colour
   0 clock-cntl regb!
   ad 00 pll!  d5 01 pll!  cc 16 pll!  e8 07 pll!  03 06 pll!
   21 pll-ref-div pll!  03 05 pll!  e4 04 pll!  ab 15 pll!  01 pll-ext-cntl pll!
   10 17 pll!  44 03 pll!
   a6 0c pll!  e6 0c pll!  a6 0c pll!             \ the DLL reset
   memory-start
   c0000000 scale-3d-cntl reg!  0 setup-cntl reg!  0 scale-3d-cntl reg!
   10 gen-test-cntl regb!  0 gen-test-cntl regb!  \ the engine reset
   ff dac-regs 2+ regb!                           \ the pixel mask
   0 src-cntl reg!
   0f config-chip-id 2+ regb!
   crtc-off
;

\ --- Memory ---------------------------------------------------------------------

\ MEM_CNTL's size field, as X.Org's driver names it for this chip: 2, 4, 6
\ or 8 MiB. The memory is sized with the largest set: a byte written a
\ MiB apart, until one does not read back or reaches the first.
: mem-size!  ( mib -- )
   case  2 of 3 endof  4 of 7 endof  6 of 9 endof  >r b r>  endcase
   70 or mem-cntl regb!  memory-start
;
: vram@  ( offset -- b )  aperture + rb@  ;
: vram!  ( b offset -- )  aperture + rb!  ;
d# 8 constant max-mib
0 value vram-mib
: size-memory  ( -- )
   max-mib mem-size!
   a5 0 vram!
   max-mib  max-mib 1 do
      i  i 100000 * vram!
      i 100000 * vram@ i <>  0 vram@ a5 <> or  if  drop i leave  then
   loop
   to vram-mib
   vram-mib 2 max  dup 1 and +  max-mib min  dup to vram-mib  mem-size!
;

\ --- Video modes ------------------------------------------------------------------

\ The modes the chip offers, each set up as the chip itself sets it up. A
\ row a mode: width, height, refresh rate; CRTC_GEN_CNTL's composite sync
\ (10 for the modes of Sun's monitors); the pixel clock's post divider (1,
\ 2, 4, 8, 3, 5, 6, 12 as 0-7) and feedback divider; the CRTC's horizontal
\ and vertical totals and syncs; the display FIFO's DSP_CONFIG and
\ DSP_ON_OFF. Every mode is 8 bits a pixel.
d# 12 constant /mode
create modes
\    width       height     rate   csync post fb  h_total_disp h_sync  v_total_disp v_sync  dsp_config dsp_on_off
   d#  640 , d#  480 , d# 60 ,  0 , 6 , ae ,   4f0063 ,   2c0153 ,  1df020c ,   2201e9 ,   5a07dc ,   600784 ,
   d#  800 , d#  600 , d# 75 ,  0 , 2 , e4 ,   630083 ,    a0167 ,  2570270 ,    30258 ,   5a0400 ,   4203e2 ,
   d# 1024 , d#  768 , d# 60 ,  0 , 4 , e0 ,   7f00a7 ,   310284 ,  2ff0325 ,   260302 ,   4a061a ,   7405d4 ,
   d# 1024 , d#  768 , d# 70 ,  0 , 1 , ad ,   7f00a5 ,   310184 ,  2ff0325 ,   260302 ,   4a0545 ,   6c04dc ,
   d# 1024 , d#  768 , d# 75 ,  0 , 1 , b6 ,   7f00a3 ,    c0183 ,  2ff031f ,    30300 ,   4a0502 ,   6c04dc ,
   d# 1024 , d#  768 , d# 77 , 10 , 1 , c2 ,   7f00a9 ,    80285 ,  2ff0324 ,    40301 ,   4a04b3 ,   680460 ,
   d# 1024 , d#  800 , d# 84 , 10 , 1 , da ,   7f00a4 ,   100283 ,  31f0344 ,    40321 ,   4a042e ,   6403e4 ,
   d# 1152 , d#  900 , d# 66 , 10 , 1 , da ,   8f00be ,   100296 ,  38303a8 ,   240385 ,   4a042e ,   6403e4 ,
   d# 1152 , d#  900 , d# 76 , 10 , 0 , 7c ,   8f00bb ,   100295 ,  38303ae ,   280385 ,   3a0759 ,   c006d0 ,
   d# 1280 , d#  800 , d# 76 , 10 , 0 , 75 ,   9f00c3 ,    e02a3 ,  31f034d ,   270322 ,   3a07ca ,   c006d0 ,
   d# 1280 , d# 1024 , d# 60 ,  0 , 0 , 7c ,   9f00d2 ,    e02a7 ,  3ff0429 ,   230400 ,   3a0759 ,   c006d0 ,
   d# 1280 , d# 1024 , d# 67 , 10 , 0 , 87 ,   9f00cb ,    e01a3 ,  3ff042a ,    80401 ,   3a06c0 ,   b805d8 ,
   d# 1280 , d# 1024 , d# 75 ,  0 , 0 , 9c ,   9f00d2 ,   1201a3 ,  3ff0429 ,    30400 ,   3a05d7 ,   b804e0 ,
   d# 1280 , d# 1024 , d# 76 , 10 , 0 , 9c ,   9f00cf ,    801a5 ,  3ff0429 ,    80401 ,   3a05d7 ,   b804e0 ,
   d# 1280 , d# 1024 , d# 85 ,  0 , 0 , b6 ,   9f00d7 ,   1401a7 ,  3ff042f ,    30400 ,   3a0502 ,   b804e0 ,
   d# 1440 , d#  900 , d# 76 , 10 , 0 , 9c ,   b300ea ,   1402ba ,  38303af ,   230385 ,   3a05d7 ,   b804e0 ,
   d# 1600 , d# 1000 , d# 66 , 10 , 0 , 9c ,   c700f5 ,   1101ce ,  3e7040e ,   2503e9 ,   3a05d7 ,   b804e0 ,
   d# 1600 , d# 1000 , d# 76 , 10 , 0 , c4 ,   c70109 ,   1b02cb ,  3e7041a ,    303e9 ,   3a04a6 ,   b803e8 ,
   d# 1600 , d# 1200 , d# 60 ,  0 , 0 , bb ,   c7010d ,   1801d1 ,  4af04e1 ,    304b0 ,   3a04de ,   b803e8 ,
here modes - /mode /n * / constant #modes

0 value mode                                    \ the mode in use: its row
: mode@  ( field -- n )  /n * mode + @  ;
: width  ( -- n )  0 mode@  ;
: height  ( -- n )  1 mode@  ;

\ A mode's name: width x height x rate ("1280x1024x76").
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
   name-buf name-len
;

\ The mode a name begins with: of those whose names it begins with, the
\ longest ("1280x1024x76junk" is 1280x1024x76).
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

\ --- The monitor's EDID, over DDC ---------------------------------------------

\ The monitor's DDC lines on GP_IO pins 13 (SCL) and 12 (SDA): a pin set
\ as an output drives its data bit, 0, and pulls its line low; as an input
\ it lets the line go, and its data bit reads the line. Each change is
\ held for a few reads.
gp-io 1+ constant gp-data                       \ data bits 8-15
gp-io 3 + constant gp-dir                       \ direction bits 24-31
20 constant scl-pin
10 constant sda-pin
0 value low                                     \ the pins pulled low
: low!  ( pins -- )
   dup to low
   gp-dir regb@  scl-pin sda-pin or invert and  or  gp-dir regb!
   8 0 do  gp-data regb@ drop  loop
;
: line  ( flag pin -- )  swap if  invert low and  else  low or  then  low!  ;
: scl  ( flag -- )  scl-pin line  ;
: sda  ( flag -- )  sda-pin line  ;
: sda?  ( -- flag )  gp-data regb@ sda-pin and 0<>  ;
: i2c-init  ( -- )  gp-data regb@  scl-pin sda-pin or invert and  gp-data regb!  0 low!  ;
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
\ header, and the bytes summing to 0.
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
0 value edid-answered?		\ the monitor answered at I2C address 50
: fetch-edid  ( -- )
   i2c-init
   edid /edid erase  false to edid-answered?
   3 0 do  edid-read if  true to edid-answered?  edid-ok? if  leave  then  then  loop
;
\ The node's edid: the block, when one checks; and aty,flags as the card's
\ own FCode gives it: 2, with 1 if the monitor answered and 8 if its EDID
\ checks.
: edid-property  ( -- )
   edid-ok? if  edid /edid encode-bytes " edid" property  then
;
: flags-property  ( -- )
   2  edid-answered? if  1 or  then  edid-ok? if  8 or  then
   encode-int " aty,flags" property
;

\ The chip's mode of a width, height and rate.
0 value want-w
0 value want-h
0 value want-r
: mode-of  ( w h r -- row | 0 )
   to want-r  to want-h  to want-w
   0  #modes 0 do
      modes i /mode * /n * +  to mode
      0 mode@ want-w =  1 mode@ want-h = and  2 mode@ want-r = and
      if  drop mode  leave  then
   loop
;
\ The modes the chip takes from a monitor's standard timings (none
\ above 1280 x 1024, or 16:10).
create std-modes
   d#  640 , d#  480 , d# 60 ,   d#  800 , d#  600 , d# 75 ,
   d# 1024 , d#  768 , d# 60 ,   d# 1024 , d#  768 , d# 70 ,   d# 1024 , d#  768 , d# 75 ,
   d# 1024 , d#  768 , d# 77 ,
   d# 1280 , d# 1024 , d# 60 ,   d# 1280 , d# 1024 , d# 67 ,   d# 1280 , d# 1024 , d# 75 ,
   d# 1280 , d# 1024 , d# 76 ,   d# 1280 , d# 1024 , d# 85 ,
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
\ The established timings the chip takes, the first of those the monitor
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
\ The monitor's mode by its EDID: the first standard timing the chip
\ takes, else the first established one.
: edid-mode  ( -- row | 0 )
   edid-ok? 0= if  0 exit  then
   0  8 0 do  i std-mode ?dup if  nip leave  then  loop
   ?dup 0= if  est-mode  then
;

\ With no mode named (or none that is one), the monitor's by its EDID,
\ else 1152 x 900 at 66 Hz.
: default-mode  ( -- row )  edid-mode ?dup 0= if  " 1152x900x66" find-mode  then  ;
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
   8 encode-int " depth" property
   width encode-int " linebytes" property
;

\ --- Setting a mode -------------------------------------------------------------

\ The pixel clock: VCLK0, 14.318 MHz x 2 x the feedback divider / 33 /
\ the post divider (whose top bit is in PLL_EXT_CNTL), chosen with the
\ CRTC off; then the CRTC's timing, 8 bits a pixel, with the display
\ off until it is all set.
: set-clock  ( -- )
   5 mode@ vclk0-fb-div pll!
   pll-ext-cntl pll@  10 invert and  4 mode@ 4 and if  10 or  then  pll-ext-cntl pll!
   4 mode@ 3 and vclk-post-div pll!
   21 pll-ref-div pll!
;
: set-mode  ( -- )
   crtc-off
   set-clock
   0240 crtc-gen-cntl regw!
   a mode@ dsp-config reg!  b mode@ dsp-on-off reg!
   9 mode@ crtc-v-sync reg!  8 mode@ crtc-v-total-disp reg!
   7 mode@ crtc-h-sync reg!  6 mode@ crtc-h-total-disp reg!
   40 3 mode@ or crtc-gen-cntl regb!
   0 ovr-clr reg!  0 ovr-wid-left-right reg!  0 ovr-wid-top-bottom reg!
   width 8 / d# 22 lshift crtc-off-pitch reg!   \ the pitch in 8 pixels, from 0
;
: display-on  ( -- )  3 mode@ crtc-gen-cntl regb!  crtc-on  ;

\ The palette: a grey ramp, but for the console's white (0 and fe) and
\ black (ff), and OpenBoot's colours 1-7.
: colour!  ( rgb index -- )
   dac-regs regb!
   dup d# 16 rshift ff and dac-regs 1+ regb!
   dup 8 rshift ff and dac-regs 1+ regb!
   ff and dac-regs 1+ regb!
;
create obp-colours
   ffffff , 6441b4 , ff0000 , ffff00 , 00ff00 , 00ffff , 0000ff , ff00ff ,
: set-palette  ( -- )
   ff dac-regs 2+ regb!
   100 0 do  i 10101 *  i colour!  loop
   8 0 do  obp-colours i na+ @  i colour!  loop
   ffffff fe colour!  0 ff colour!
;

\ --- The logo ---------------------------------------------------------------------

\ The chip's own logo: a Sun raster file of 100 x 100 pixels, 8 bits a
\ pixel with a colour map of its own (RT_STANDARD, RMT_EQUAL_RGB), kept in
\ the FCode. It is drawn at the text's left on its line, its colours put
\ in the palette from entry 10 (the grey ramp's, unused by the console).
fload logo.fth
: ras@  ( field -- n )
   4 * logo-ras +  0  4 0 do  8 lshift  over i + c@ or  loop  nip
;
: colour-logo?  ( -- flag )
   0 ras@ 59a66a95 =  3 ras@ 8 = and  5 ras@ 1 = and  6 ras@ 1 = and
   7 ras@ 3 /  d# 64 <= and
;
10 constant logo-base
: logo-map  ( -- adr )  logo-ras 20 +  ;
: logo-pixels  ( -- adr )  logo-map 7 ras@ +  ;
: logo-colours  ( -- )
   7 ras@ 3 /  dup 0 ?do                       ( n )
      logo-map i + c@ d# 16 lshift
      logo-map i + 2 pick + c@ 8 lshift or
      logo-map i + 2 pick 2* + c@ or
      i logo-base + colour!
   loop
   drop
;
\ (A row of the raster is padded to an even number of bytes.)
: logo-stride  ( -- n )  1 ras@ 1+ -2 and  ;
: m64-draw-logo  ( line# adr width height -- )
   colour-logo? 0= if  2drop 2drop exit  then
   2drop drop  char-height * window-top +       ( y )
   logo-colours
   2 ras@ 0 ?do
      dup i + width * window-left +  frame-buffer-adr +    ( y fb-row )
      logo-pixels i logo-stride * +                        ( y fb-row src )
      1 ras@ 0 ?do  dup i + c@ logo-base +  2 pick i + rb!  loop
      2drop
   loop
   drop
;

\ --- The node -------------------------------------------------------------------

\ Probe: the chip brought up from cold, its memory sized, the monitor's
\ EDID read and its mode set, the CRTC left off till the screen is opened.
\ The properties are made in the order an Ultra 10's PROM makes the onboard
\ Rage's (its .properties lists them newest first); edid's place among
\ them (only with a monitor that answers) taken as first.
map-chip
chip-start
size-memory
fetch-edid
default-mode to mode  set-mode
edid-property
0 encode-int " aty,status" property
flags-property
vram-mib encode-int " fb-memory" property
mode-properties
" display" device-type
" ISO8859-1" encode-string " character-set" property
unmap-chip
memory-off

\ Its spaces: configuration, the aperture, the register space.
my-address my-space encode-phys  0 encode-int encode+  0 encode-int encode+
0 0 my-space bar0 or encode-phys encode+  0 encode-int encode+  01000000 encode-int encode+
0 0 my-space bar2 or encode-phys encode+  0 encode-int encode+  00001000 encode-int encode+
" reg" property
" UltraViolet m64.fth 0.2" encode-string " pgx_version" property
" SUNW,m64B" device-name
" ATY,GT-C" model
" 113-41901-104" encode-string " aty,rom#" property
" 109-41900-00" encode-string " aty,card#" property
" 1.60" encode-string " aty,fcode" property

\ --- Install and remove ---------------------------------------------------------

\ Back from a client (X leaves the chip in its own mode, depth, palette
\ and pixel mask): the console's mode set again if the CRTC is not in it,
\ the DAC 8 bits a colour, the palette, the display on.
: mode-kept?  ( -- flag )
   crtc-h-total-disp reg@ 6 mode@ =
   crtc-v-total-disp reg@ 8 mode@ =  and
   crtc-off-pitch reg@  width 8 / d# 22 lshift =  and
   crtc-gen-cntl reg@ 700 and 200 =  and
;
: m64-reset-screen  ( -- )
   mode-kept? 0= if  set-mode  then
   dac-cntl reg@ 100 or dac-cntl reg!
   set-palette  display-on
;

\ The frame buffer, kept mapped from the first open as the node's
\ "address".
: m64-install  ( -- )
   aperture -1 = if  map-chip  then
   aperture encode-int " address" property
   fetch-edid  edid-property  flags-property
   select-mode  mode-properties
   set-mode  set-palette  display-on
   aperture to frame-buffer-adr
   default-font set-font
   width height  width char-width /  height char-height /  fb8-install
   ['] m64-draw-logo to draw-logo
   ['] m64-reset-screen to reset-screen
;

: m64-remove  ( -- )  ;
\ --- The selftest -----------------------------------------------------------------

\ The registers (the two scratch registers), the RAMDAC (every palette
\ entry, written and read back) and the frame buffer (every word of the
\ memory, put back as it was), each passed or failed. Unless the chip is
\ the console's output, it says first that its display is not installed.
80 constant scratch-reg0
84 constant scratch-reg1
: console?  ( -- flag )
   " /chosen" find-package if
      " stdout" rot get-package-property 0= if
         decode-int nip nip  ?dup if  ihandle>phandle my-self ihandle>phandle =  exit  then
      then
   then  false
;
: .result  ( ok? -- ok? )  dup if  ."  passed Ok"  else  ."  failed"  then  cr  ;
: reg-ok?  ( pattern reg -- ok? )  2dup reg!  reg@ =  ;
: reg-test  ( -- ok? )
   scratch-reg0 reg@  scratch-reg1 reg@  true		( s0 s1 ok? )
   55555555 scratch-reg0 reg-ok? and  2aaaaaaa scratch-reg0 reg-ok? and
   2aaaaaaa scratch-reg1 reg-ok? and  55555555 scratch-reg1 reg-ok? and
   0 scratch-reg0 reg-ok? and  0 scratch-reg1 reg-ok? and
   >r  scratch-reg1 reg!  scratch-reg0 reg!  r>
;
: dac-pattern  ( index phase -- b )  over 3 * + 2a xor  swap 2/ xor  3f and  ;
: dac-test  ( -- ok? )
   0 dac-regs regb!
   100 0 do  3 0 do  j i dac-pattern dac-regs 1+ regb!  loop  loop
   0 dac-regs 3 + regb!  true
   100 0 do  3 0 do  dac-regs 1+ regb@  j i dac-pattern =  and  loop  loop
   set-palette
;
: fb-test  ( -- ok? )
   true  vram-mib 100000 * regs-offset min  0 do	( ok? )
      aperture i + dup rl@  swap			( ok? old adr )
      i over rl!  dup rl@ i =			( ok? old adr ok1? )
      i invert 7fffffff and  2 pick rl!		( ok? old adr ok1? )
      over rl@  i invert 7fffffff and =  and	( ok? old adr ok2? )
      >r rl!  r> and				( ok? )
   4 +loop
;
: m64-selftest  ( -- 0 | 1 )
   aperture -1 = dup >r  if  map-chip  then
   console? 0= if  cr ." Display not installed" cr cr  then
   ." Test hardware registers - "  reg-test .result
   ." Test RamDAC - "  dac-test .result  and
   ." Test Frame buffer - "  fb-test .result  and
   r> if  unmap-chip  then
   0= 1 and
;

' m64-install is-install
' m64-remove is-remove
' m64-selftest is-selftest

fcode-end
pci-header-end
