\ Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
\ OpenBoot release it is part of (see LICENSE).
id: @(#)post.fth 2.0 26/11/10
purpose: The power-on selftest's result
copyright: Copyright 2026 Andy Taylor

\ The power-on selftest (POST) is a program of its own in the flash PROM's
\ second half (firmware/openboot/post), as an Ultra 10 has it: the reset
\ code runs it at a hardware power-on with diag-switch? true and
\ diag-level not off, before OpenBoot, and a software power-on reset
\ follows. Here is what OpenBoot does with its result, which the reset
\ code passes on (boot.fth: post-ret0 its status, post-ret1 the address of
\ its message in the PROM, post-ret2 the board descriptor, post-retv
\ "post-run"): show-post-results; after a failure, the part suspected
\ once the memory is set up, and no booting by itself.

headerless

h# 706f.7374.2d72.756e constant post-run-magic
: post-ran? ( -- flag )  post-retv @ post-run-magic =  ;
: post-status ( -- n )  post-ran?  if  post-ret0 @  else  0  then  ;

\ Its message: up to 50 characters from the PROM, as an Ultra 10 shows it
\ (anything not printable as [xx]).
: .post-message ( -- )
   0  begin					( i )
      dup h# 50 <  over post-ret1 @ + bypass-asi spacec@ 0<> and
   while					( i )
      dup post-ret1 @ + bypass-asi spacec@	( i c )
      dup printable?  if  emit  else  ascii [ emit  push-hex 2 .r pop-base  ascii ] emit  then
      1+
   repeat  drop
;

headers
: post-ok? ( -- flag )  post-status 0=  ;

\ After a failed run, once the memory is set up: the part suspected
\ (pre-boot-hook, loadprom.fth).
: .post-failure ( -- )
   post-ok? 0=  if
      ." Power On Self Test Failed.  Cause: " .post-message space cr
   then
;

\ (startup's: the POST has run before OpenBoot.)
: system-tests ( -- )  ;

: show-post-results ( -- )
   post-ran? 0= if  ." Power On Selftest not run on last reset" cr exit  then
   push-hex
   ." Status 0=Pass, Non-Zero=Fail (%o0): " post-status . cr
   ." Message String               (%o1): " .post-message space cr
   ." Board Descriptor             (%o2): " post-ret2 @ . cr
   pop-base
;
headerless
