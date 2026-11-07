\ Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
\ OpenBoot release it is part of (see LICENSE).
id: @(#)diagprint.fth 1.0 26/11/07
purpose: The reset code's console output: ttya, the SAB82532's channel A
copyright: Copyright 2026 Andy Taylor

\ The words arch/sun4v/diagprint.fth gives the reset code (puts, puthex,
\ print", print-reg and their diag- forms), writing to ttya directly: the
\ SAB82532's transmit-immediate register, through the physical bypass ASI
\ (so they work with the MMU off or on). diag- output is only with
\ VERBOSE-RESET defined.
\
\ They trash %o0, %o1, %o2, %o3, %o4, %o5 and %o6 (puts) or %o4 (puthex).

hex
headerless
h# 1ff.f140.0000 constant ttya-pa
h# 20 constant ttya-star	\ status
h# 35 constant ttya-tic		\ transmit immediate
h# 08 constant ttya-tec		\ TIC executing

\ %o0 = char; %o1 = channel PA; %o5 = scratch
label putc
   begin
      %o1  ttya-star	%o5	add
      %o5  %g0  bypass-asi	%o5	lduba
      %o5  ttya-tec	%g0	andcc
   0= until
      nop
   %o1  ttya-tic	%o5	add
   retl
   %o0  %o5  %g0  bypass-asi	stba
end-code

\ The string after the call: a count byte, the characters, then code from
\ the next word boundary.
\ %o6 = Count
\ %o3 = String address
\ %o2 = Scratch
label puts
   %o7			%o3	move	\ Return Address
   %o3 8		%o3	add	\ Addr. of the first char.
   %o3 %g0		%o6	ldub	\ Count
   ttya-pa  %o2		%o1	setx
   begin
      %o6  %g0		%g0	subcc
   0>  while
      %o3  1		%o3	add	\ (delay)
      %o3  %g0		%o0	ldub
      %o7		%o4	move
      putc		call
      nop
      %o4		%o7	move
   repeat
      %o6  1		%o6	sub
   %o3  4		%o3	add
   %o3  3		%o3	andn
   %o3  %g0		%g0	jmpl
   nop
end-code

\ %o0 = number, printed in hex, with a dot every 16 bits, then a space.
\ %o4 = Return Address
\ %o6 = Shift count
\ %o3 = number
\ %o2 = Scratch
label puthex
   %o7			%o4	move	\ Return adr.
   ttya-pa  %o2		%o1	setx
   h# 3c		%o6	set	\ Shift count
   %o0			%o3	move	\ Number
   begin
      %o3  %o6		%o2	srlx
      %o2  h# 0f	%o2	and
      %o2  h# 0a	%g0	subcc
      %o2  ascii 0	%o0	add
      <  if annul
	 %o2  ascii a h# 0a -  %o0	add
      then
      putc		call
      nop
      %o6  h# 0f  %g0  andcc
      0=  if %o6  %g0  %g0  subcc
	 0<>  if
	    ascii .	%o0	move
	    putc	call
	    nop
	 then
      then
      %o6  4		%o6	subcc
   <  until
      nop
   h# 20		%o0	move
   putc			call
   nop
   %o4	8		%g0	jmpl
   nop
end-code

headers
transient
also assembler definitions
: $print ( str$ -- )
   puts call nop  ", align
;
: print" ( -- ) \ string"
   puts call nop  ," align
;
: print-cr ( -- )  " "r"n" $print  ;
: print-reg ( reg -- )
   puthex call %o0 move
;
[ifdef] VERBOSE-RESET
: $diag-print ( str$ -- )  $print  ;
: diag-print" ( -- ) \ string"
   puts call nop  ," align
;
: diag-print-cr ( -- )  print-cr  ;
: diag-print-reg ( reg -- )  print-reg  ;
[else]
: $diag-print ( str$ -- )  2drop  ;
: diag-print" ( -- ) \ string"
   here  ,"  dp !		\ parsed as print" does, then given back
;
: diag-print-cr ( -- )  ;
: diag-print-reg ( reg -- )  drop  ;
[then]
resident
previous definitions
headers
