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

\ Pending soft interrupts cleared (the tick alarm's among them).
: reset-interrupts ( -- )  -1 clear-softint!  ;

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
headerless
