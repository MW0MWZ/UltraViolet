\ Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
\ OpenBoot release it is part of (see LICENSE).
id: @(#)uniproc.fth 1.0 26/11/07
purpose: The Ultra 5/10's one processor, and its reset and interrupt state
copyright: Copyright 2026 Andy Taylor

\ The words arch/sun4u's multi-processor code calls to reach other
\ processors (sun4v's xcall.fth): the Ultra 5/10 has none, so each fails.
\ And the machine's reset status, interrupts and keyboard words that
\ arch/sun4u's start-up and the banner call.

headerless
: xcall-start-cpu ( arg addr mid -- failed? )  3drop true  ;
: xcall-execute ( acf mid -- failed? )  2drop true  ;
: xcall-idle-cpu ( cpu# -- failed? )  drop true  ;
: xcall-resume-cpu ( cpu# -- failed? )  drop true  ;
: xcall-get-pc ( cpu# -- pc failed? )  drop 0 true  ;

\ The Sabre's reset control: POR (bit 31) set by a power-on reset, SOFT_POR
\ (bit 30) by reset-all. Read once, at its first use, and kept: the start
\ clears the register (below), and the reason is asked for after that
\ (#power-cycles, POST, retained memory).
h# 1fe.0000.f020 constant reset-status-pa
-1 value reset-status
: reset-status@ ( -- n )
   reset-status -1 =  if
      reset-status-pa bypass-asi spacex@ to reset-status
   then  reset-status
;
: power-on-reset? ( -- flag )  reset-status@ h# 8000.0000 and 0<>  ;
\ The root's reset-reason: a power-on, or one the Sabre's reset control
\ made (SOFT_POR: reset-all)
: soft-power-on-reset? ( -- flag )  reset-status@ h# 4000.0000 and 0<>  ;
stand-init: Reset reason
   root-device
      power-on-reset? if  " POR"  else
      soft-power-on-reset? if  " S-POR"  else  " RESET"  then  then
      encode-string " reset-reason" property
   device-end
   \ The reset control's bits cleared, as an Ultra 10 does at each start:
   \ the value read written back (POR, B_POR and B_XIR clear when written
   \ with 1), then 0 (SOFT_POR and SOFT_XIR hold what is written). A
   \ SOFT_POR left set would make the next reset-all's write of it no reset.
   reset-status@ reset-status-pa bypass-asi spacex!
   0 reset-status-pa bypass-asi spacex!
;

\ On to the client (go): OpenBoot's own pending soft interrupt (level 14)
\ cleared; the tick's (TICK_INT) left, for a client whose clock interrupt
\ came while OpenBoot ran to take it (its tick compare is behind the tick
\ by then, so the clock would not interrupt it again).
: reset-interrupts ( -- )  h# 4000 clear-softint!  ;

\ An interrupt vector: a device's interrupt a client left mapped (the
\ keyboard's, when Solaris stops at Stop-A or halts). OpenBoot takes no
\ device interrupts, so it goes on with interrupts off (TSTATE's PSTATE.IE
\ cleared), and the vector stays in the processor for the client to take
\ when it resumes.
h# 060 begin-trap
   %g1			rdtstate
   %g1 h# 200	%g1	andn
   %g1 0		wrtstate
   retry
h# 061 end-trap

headers
defer clear-keyboard  ' noop is clear-keyboard
\ Whether the console's input is the keyboard (it answered when the
\ console was set up).
: keyboard-input? ( -- flag )
   stdin @ ?dup 0= if  false exit  then  ihandle>phandle	( phandle )
   " keyboard" locate-device if  drop false  else  =  then
;
\ The banner's "Keyboard Present" when a keyboard answers, whatever the
\ console is: with input-device set to ttya the Ultra 10's banner still
\ reports a keyboard plugged in. When the keyboard is not the console's
\ input it is opened (reset) to ask it, and closed again.
: keyboard-present? ( -- flag )
   keyboard-input? if  true exit  then
   " keyboard" open-dev ?dup if  close-dev true  else  false  then
;
defer .keyboard-type
: (.keyboard-type) ( -- )
   keyboard-present? if  ." Keyboard Present"  else  ." No Keyboard"  then
;
' (.keyboard-type) is .keyboard-type

\ The console's output a display.
: display-output? ( -- flag )
   stdout @ ?dup 0= if  false exit  then  ihandle>phandle	( phandle )
   " device_type" rot get-package-property if  false exit  then	( adr len )
   decode-string 2swap 2drop  " display" $=
;

\ Each entry to OpenBoot: the keyboard's keys forgotten (see its restore),
\ and the display's console state put back (its restore, the terminal
\ emulator's reset-screen: a client such as X leaves the card its own).
\ (enter-forth's ?reset-video asks the display for a reset-screen method,
\ which the terminal emulator gives it as restore.)
: (clear-keyboard) ( -- )
   display-output? if
      " restore" stdout @ ['] $call-method catch if  3drop  then
   then
   keyboard-input? 0= if  exit  then
   " restore" stdin @ ['] $call-method catch if  3drop  then
;
' (clear-keyboard) is clear-keyboard
headerless
