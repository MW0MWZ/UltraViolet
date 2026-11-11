\ Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
\ OpenBoot release it is part of (see LICENSE).
id: @(#)memscrub.fth 1.1 26/11/11
purpose: The memory's check bits set, shown as an Ultra 10 shows it
copyright: Copyright 2026 Andy Taylor

\ os/bootprom/scrubmem.fth's scrub-memory (the free memory cleared, so its
\ ECC check bits are valid), a megabyte at a time, with an Ultra 10's
\ display for each free range: in diagnostic mode "Initializing" and its
\ size ("%4d megs"), "of memory at addr" and its address (17 hex digits
\ wide), then the megabytes left counting down (each " %4d", rubbed out by
\ five backspaces); otherwise "Initializing Memory " and a turning bar
\ (one of \-/| for the megabytes left, then a backspace); each range's
\ line then cleared (a carriage return, 70 spaces, a carriage return).
\ With silent-mode? true and not in diagnostic mode, the Ultra 10 shows
\ none of it (the memory is cleared all the same).

" /memory" find-device
headerless

: (backspaces) ( n -- )  0 ?do  control H emit  loop  ;
: scrub-quiet? ( -- flag )  silent-mode? diagnostic-mode? 0= and  ;
: .scrub-start ( pa len -- )
   scrub-quiet?  if  2drop exit  then
   push-decimal
   diagnostic-mode?  if
      ." Initializing"  1meg round-up 1meg / 5 .r  ."  megs of memory at addr "
      push-hex d# 17 u.r pop-base
   else
      2drop ." Initializing Memory "
   then
   pop-base
;
: .scrub-left ( megs-left -- )
   scrub-quiet?  if  drop exit  then
   diagnostic-mode?  if
      push-decimal 5 .r pop-base  5 (backspaces)
   else
      3 and " \-/|" drop + c@ emit  1 (backspaces)
   then
;
\ (A range need not be whole megabytes: its last piece is what is left.)
: scrub-node ( node-adr -- false )
   node-range  pages>phys-adr,len		( pa.lo pa.hi len )
   2 pick over .scrub-start			( pa.lo pa.hi len )
   dup 1meg round-up 1meg /  0 ?do		( pa.lo pa.hi len )
      2 pick i 1meg * +  2 pick			( pa.lo pa.hi len pa pa.hi )
      2 pick i 1meg * - 1meg min  scrub-4meg	( pa.lo pa.hi len )
      dup 1meg round-up 1meg / i - 1- .scrub-left
   loop  3drop
   scrub-quiet? 0=  if  (cr d# 70 spaces (cr  then  false
;

headers
\ (As the release's, with scrub-node above.)
: scrub-memory  ( -- )
   memory-clean? @  0=  if
      physavail ['] scrub-node find-node 2drop
      first-phys-avail  drop  0  4meg  map-pages
      memory-clean? on
      ['] clear-memory is clear-mem
   then
;

device-end
