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

\ The Sabre's reset control: POR (bit 31) set by a power-on reset.
h# 1fe.0000.f020 constant reset-status-pa
: power-on-reset? ( -- flag )
   reset-status-pa bypass-asi spacex@ h# 8000.0000 and 0<>
;
\ The root's reset-reason: a power-on, or one the Sabre's reset control
\ made (SOFT_POR, bit 30: reset-all)
: soft-power-on-reset? ( -- flag )
   reset-status-pa bypass-asi spacex@ h# 4000.0000 and 0<>
;
stand-init: Reset reason
   root-device
      power-on-reset? if  " POR"  else
      soft-power-on-reset? if  " S-POR"  else  " RESET"  then  then
      encode-string " reset-reason" property
   device-end
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
\ The banner's "Keyboard Present" when the console's input is the keyboard
\ (it answered when the console was set up), else "No Keyboard".
: keyboard-input? ( -- flag )
   stdin @ ?dup 0= if  false exit  then  ihandle>phandle	( phandle )
   " keyboard" locate-device if  drop false  else  =  then
;
defer .keyboard-type
: (.keyboard-type) ( -- )
   keyboard-input? if  ." Keyboard Present"  else  ." No Keyboard"  then
;
' (.keyboard-type) is .keyboard-type

\ Each entry to OpenBoot: the keyboard's keys forgotten (see its restore).
: (clear-keyboard) ( -- )
   keyboard-input? 0= if  exit  then
   " restore" stdin @ ['] $call-method catch if  3drop  then
;
' (clear-keyboard) is clear-keyboard
headerless
