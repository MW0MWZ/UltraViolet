\ Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
\ OpenBoot release it is part of (see LICENSE).
id: @(#)kbd.fth 1.1 26/11/11
purpose: The Sun Type 5 keyboard, on the SuperIO's first UART
copyright: Copyright 2026 Andy Taylor

\ The keyboard talks at 1200 baud, 8N1: commands from the host (reset 01,
\ bell on 02 and off 03, click on 0a and off 0b, LEDs 0e, layout 0f) and,
\ from the keyboard, its responses (to reset: ff, its type, the keys
\ down; to layout: fe, the layout) and a key number for each key pressed,
\ 80 | the number when released, 7f when no key is left down. The keys held
\ are kept as the USB keyboard driver's report would have them (a modifier
\ byte and up to six keys, by their USB usage), and its key processing
\ (dev/usb-devices/kbd/usbkeyin.fth, kbdutils.fth) and the release's
\ keyboard tables, which are by USB usage and HID country code, do the rest.

headerless
hex

h# 01 constant kbd-reset
h# 02 constant kbd-bell-on
h# 03 constant kbd-bell-off
h# 0a constant kbd-click-on
h# 0b constant kbd-click-off
h# 0f constant kbd-layout
h# ff constant kbd-reset-response
h# fe constant kbd-layout-response
h# 7f constant kbd-idle

\ Each Type 5 key number's USB usage (0 for none)
create type5>usb
   \  0    1    2    3    4    5    6    7    8    9    a    b    c    d    e    f
   00 c, 78 c, 81 c, 79 c, 80 c, 3a c, 3b c, 43 c, 3c c, 44 c, 3d c, 45 c, 3e c, e6 c, 3f c, 00 c,  \ 00
   40 c, 41 c, 42 c, e2 c, 52 c, 48 c, 46 c, 47 c, 50 c, 76 c, 7a c, 51 c, 4f c, 29 c, 1e c, 1f c,  \ 10
   20 c, 21 c, 22 c, 23 c, 24 c, 25 c, 26 c, 27 c, 2d c, 2e c, 35 c, 2a c, 49 c, 7f c, 54 c, 55 c,  \ 20
   66 c, 77 c, 63 c, 7c c, 4a c, 2b c, 14 c, 1a c, 08 c, 15 c, 17 c, 1c c, 18 c, 0c c, 12 c, 13 c,  \ 30
   2f c, 30 c, 4c c, 65 c, 5f c, 60 c, 61 c, 56 c, 74 c, 7d c, 4d c, 00 c, e0 c, 04 c, 16 c, 07 c,  \ 40
   09 c, 0a c, 0b c, 0d c, 0e c, 0f c, 33 c, 34 c, 31 c, 28 c, 58 c, 5c c, 5d c, 5e c, 62 c, 7e c,  \ 50
   4b c, 7b c, 53 c, e1 c, 1d c, 1b c, 06 c, 19 c, 05 c, 11 c, 10 c, 36 c, 37 c, 38 c, e5 c, 00 c,  \ 60
   59 c, 5a c, 5b c, 00 c, 00 c, 00 c, 75 c, 39 c, e3 c, 2c c, e7 c, 4e c, 64 c, 57 c, 00 c, 00 c,  \ 70

\ The layout (the keyboard's DIP switches): its HID country code, for the
\ keyboard tables (layouts 21 and 22 the USA's; the others by Sun's Type 5
\ layout codes)
0 instance value layout
create layout>country
   \ 21 USA, 22 USA (UNIX), 23 France, 24 Denmark, 25 Germany, 26 Italy,
   \ 27 Netherlands, 28 Norway, 29 Portugal, 2a Spain, 2b Sweden, 2c Swiss
   \ French, 2d Swiss German, 2e UK, 2f Korea, 30 Taiwan, 31 Japan, 32
   \ Canadian French, 33 Hungary, 34 Poland, 35 Czech, 36 Russia
   d# 33 c, d# 33 c, d# 8 c, d# 6 c, d# 9 c, d# 14 c, d# 18 c, d# 19 c,
   d# 22 c, d# 25 c, d# 26 c, d# 27 c, d# 28 c, d# 32 c,
   d# 16 c, d# 30 c, d# 15 c, d# 4 c, d# 12 c, d# 21 c, d# 5 c, d# 23 c,

\ Whether the layout's key 58 is the ISO # ~ key (usage 32) or the ANSI \ |
\ key (usage 31), by layout from 21: the ANSI ones, 21 and 22, Korea,
\ Taiwan and Russia; and Japan, Hungary, Poland and Czech, which give
\ nothing on key 7c (the ISO < > key), their key 58 being their tables' 31
create layout-iso
   0 c, 0 c, 1 c, 1 c, 1 c, 1 c, 1 c, 1 c,  1 c, 1 c, 1 c, 1 c, 1 c, 1 c,
   0 c, 0 c, 0 c, 1 c, 0 c, 0 c, 0 c, 0 c,

: get-kbd-cntry-id ( -- cntry-code )
   layout h# 21 - dup 0 d# 22 within if  layout>country + c@  else  drop d# 33  then
;

: iso-layout? ( -- flag )
   layout h# 21 - dup 0 d# 22 within if  layout-iso + c@ 0<>  else  drop false  then
;

: >usage ( key# -- usage )
   dup h# 58 = iso-layout? and if  drop h# 32 exit  then	\ the # ~ key
   type5>usb + c@
;

\ The keys held, by Type 5 key number
h# 80 instance buffer: keys-down

: clear-keys-down ( -- )  keys-down h# 80 erase  ;

\ The report the USB driver's code reads: the modifiers' bits (E0-E7 are
\ bits 0-7), then the other keys' usages, six at most.
: make-report ( -- )
   keybuff-curr^v /key-info-buff erase
   0  h# 80 1 do					( #keys )
      keys-down i + c@ if				( #keys )
         i >usage ?dup if				( #keys usage )
            dup h# e0 h# e8 within if			( #keys usage )
               h# e0 - 1 swap lshift			( #keys bit )
               keybuff-curr^v >kbd-in-modkeys dup c@ rot or swap c!
            else					( #keys usage )
               over 6 < if				( #keys usage )
                  over keybuff-curr^v >kbd-in-byte1 + c!  1+
               else  drop  then				( #keys' )
            then
         then
      then
   loop  drop
;

\ The UART
: kbd-out ( byte -- )  uemit  ;
: kbd-byte? ( -- byte true | false )
   (ukey?) if  (ukey) true  else  false  then
;

\ Wait up to ms milliseconds for a byte
: kbd-wait ( ms -- byte true | false )
   get-msecs +  begin					( until )
      kbd-byte? if  rot drop true exit  then		( until )
      dup get-msecs - 0<				( until timed-out? )
   until  drop false
;

\ The reset's response within a second: bytes before it passed over (a
\ key's release sent after the reset began, as when reset-all is typed:
\ the keyboard is not reset with the machine).
: kbd-reset-response? ( -- found? )
   get-msecs d# 1000 +  begin				( until )
      d# 10 kbd-wait if
         kbd-reset-response = if  drop true exit  then
      then
      dup get-msecs - 0<
   until  drop false
;

\ Reset: the self-test's response, then the keys down (or idle)
: kbd-reset? ( -- present? )
   begin  d# 20 kbd-wait  while  drop  repeat		\ what was waiting
   kbd-reset kbd-out
   kbd-reset-response? 0= if  false exit  then
   d# 20 kbd-wait if  drop  then			\ the type (4)
   clear-keys-down
   begin  d# 20 kbd-wait  while			( byte )
      dup kbd-idle = if  drop  else  keys-down + 1 swap c!  then
   repeat
   true
;

: kbd-get-layout ( -- )
   kbd-layout kbd-out
   d# 100 kbd-wait if
      kbd-layout-response = if  d# 20 kbd-wait if  to layout  then  then
   then
;

\ The keys' click, on as the keyboard-click? option says (reset turns it
\ off)
: kbd-set-click ( -- )
   " /options" find-package if
      " keyboard-click?" rot get-package-property if  false  then
   else  false  then
   if  kbd-click-on  else  kbd-click-off  then  kbd-out
;

\ A byte from the keyboard: a key down or up, or all up
: kbd-event ( byte -- )
   dup kbd-idle = if  drop clear-keys-down exit  then
   dup h# 80 and if  h# 7f and 0  else  1  then		( key# down? )
   swap keys-down + c!
;

\ The USB driver's polling (kbdutils.fth calls it so): new keys go through
\ the key processing; true if Stop-A was pressed.
: poll-usb ( -- stopA? )
   false  begin  kbd-byte?  while			( changed? byte )
      dup kbd-reset-response = if			( changed? byte )
         \ The keyboard was reset (unplugged and plugged): its type and
         \ the keys down follow
         drop clear-keys-down  d# 20 kbd-wait if  drop  then
      else
         kbd-event					( changed? )
      then  drop true					( changed )
   repeat						( changed? )
   if  make-report eval-key-data  else  false  then
;

: ring-keyboard-bell ( -- )
   kbd-bell-on kbd-out  d# 100 ms  kbd-bell-off kbd-out
;

: alloc-vaddr-buffs ( -- )
   /key-info-buff alloc-mem to keybuff-curr^v
;
: dealloc-vaddr-buffs ( -- )
   keybuff-curr^v /key-info-buff free-mem
;
