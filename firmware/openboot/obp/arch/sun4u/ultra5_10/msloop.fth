\ Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
\ OpenBoot release it is part of (see LICENSE).
id: @(#)msloop.fth 1.0 26/11/07
purpose: Delays of milliseconds, and the millisecond clock
copyright: Copyright 2026 Andy Taylor

\ arch/sun4s/msloop.fth's words timed by the processor's TICK register
\ (which counts at its clock) in place of sun4v's STICK. system-tick-speed
\ is the processor's clock, as start-up measures it.

headers
d# 440.000.000 value system-tick-speed

code (tick-wait) ( ticks -- )
   sc1				rdtick
   sc1  1		sc1	sllx
   sc1  1		sc1	srlx
   sc1  tos		tos	add		\ End
   h# 20 .align					\ Align on a Cache Line
   begin
      tos  sc1		%g0	subcc
      sc1			rdtick
      sc1  1		sc1	sllx
   0<=  until
      sc1  1		sc1	srlx		\ (delay)
   sp			tos	pop
c;

: (ms ( ms -- )
   system-tick-speed d# 1000 / * (tick-wait)
;

h# 8000000.0000000 1- constant tick-mask
: getmsecs ( -- value )
   tick@ tick-mask and  system-tick-speed d# 1000 / /
;

' getmsecs is get-msecs
' (ms is ms
headers
