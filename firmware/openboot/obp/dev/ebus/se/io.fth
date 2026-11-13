\ Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
\ OpenBoot release it is part of (see LICENSE).
id: @(#)io.fth 1.0 26/11/07
purpose: The SAB82532 (ESCC2) in asynchronous mode, polled
copyright: Copyright 2026 Andy Taylor

\ The words the release's tty driver (dev/serial/ttydriver.fth) uses, as
\ dev/serial/su16550/io.fth gives them for a 16550, for the ESCC2 (Siemens
\ SAB 82532 User's Manual, chapter 10.2: asynchronous mode). Channel A's
\ registers are at 00, channel B's at 40. Received characters come one at
\ a time through the receive FIFO (threshold one byte, released with RMC);
\ characters go out through the transmit-immediate register (TIC).

headers
d# 90   constant ubufsize
ubufsize buffer: ubuf
0 	value  getptr
0 	value  putptr
0 	value  endptr

variable ttylock

: initubuf  ( -- )
   ubuf  is  getptr
   ubuf  is  putptr
   ubuf ubufsize + is endptr
;

: unext  ( ptr -- ptr' )  1+ dup endptr >=  if  drop ubuf  then  ;
: bput	( key -- )  putptr c!  putptr unext is putptr  ;
: bputclr  ( -- )  getptr is putptr  ;
: bget	( -- key )  getptr c@  getptr unext is getptr  ;
: ubuf-empty? 	( -- flag )  getptr putptr =  ;
\ Full with one place free: characters wait in the receive FIFO (and on
\ the line) until there is room, not overwriting those not yet read.
: ubuf-full?  ( -- flag )  putptr unext getptr =  ;

headerless
: usea  ( -- )  uartbase to uart  ;
: useb  ( -- )  uartbase h# 40 + to uart  ;

: uart!  ( c offset -- )  uart + rb!  ;
: uart@  ( offset -- c )  uart + rb@  ;

h# 20 constant star			\ Status (read), command (write)
h# 35 constant tic			\ Transmit immediate
h# 3b constant isr1

h# 20 constant star-rfne		\ Receive FIFO not empty
h# 08 constant star-tec			\ TIC executing
h# 04 constant star-cec			\ Command executing
h# 80 constant cmdr-rmc			\ Receive message complete
h# 41 constant cmdr-reset		\ Receiver and transmitter reset
h# 80 constant isr1-brk			\ Break

: star@  ( -- b )  star uart@  ;
: cmdr!  ( b -- )  begin  star@ star-cec and 0=  until  star uart!  ;

\ ISR1's bits clear as it is read: those seen are kept until used.
variable isr1-shadow
: isr1@  ( -- b )  isr1 uart@ isr1-shadow c@ or dup isr1-shadow c!  ;

\ The channel's mode: asynchronous, from the 29.4912 MHz clock, NRZ,
\ push-pull, every interrupt masked (their statuses still visible), the
\ receive FIFO's threshold one byte; 9600 baud, 8N1 until set otherwise.
: inituart  ( -- )
   initubuf
   h# c3 h# 2c uart!		\ CCR0: power up, NRZ, asynchronous
   h# c0 h# 3f uart!		\ CCR4: master clock divide, enhanced baud rate
   cmdr-reset cmdr!
   h# 1f h# 2d uart!		\ CCR1: push-pull, clock mode 7
   h# af h# 34 uart!		\ BGR: 9600 baud
   h# 38 h# 2e uart!		\ CCR2: baud rate generator, TxD out
   h# 2c h# 22 uart!		\ MODE: receiver on, RTS
   0     h# 2b uart!
   h# ff h# 3a uart!		\ IMR0, IMR1: masked
   h# ff h# 3b uart!
   h# 80 h# 39 uart!		\ IPC: masked statuses visible
   0     h# 27 uart!		\ DAFO: 8 bits, no parity, 1 stop bit
   h# 40 h# 28 uart!		\ RFC: threshold 1 byte
   cmdr-reset cmdr!
   0 isr1-shadow c!
;

: ubreak?  ( -- flag )  isr1@ isr1-brk and  ;

: uemit?  ( -- flag )  star@ star-tec and 0=  ;
: uemit  ( char -- )  begin  uemit?  until  tic uart!  ;

: (ukey?)  ( -- flag )  star@ star-rfne and 0<>  ;
: (ukey)   ( -- key )   begin  (ukey?)  until  0 uart@  cmdr-rmc cmdr!  ;

\ Wait for the last character to go
: uwait  ( -- )  begin  uemit?  until  ;

: uwrite  ( adr len -- #written )
   ttylock on
   tuck  bounds  ?do  i c@  uemit  loop		( len )
   uwait
   ttylock off
;

: uread  ( -- )
   ttylock on
   begin  ubuf-full? 0=  (ukey?) and  while  (ukey) bput  repeat
   ttylock off
;

: ukey?  ( -- flag )  uread  ubuf-empty? 0=  ;
: ukey   ( -- char )  begin  ukey?  until  bget  ;

: clear-break  ( -- )
   ukey? drop bputclr
   0 isr1-shadow c!
;

\ The line's settings (DAFO: character length, parity, stop bits; the baud
\ rate generator's divisor). The baud rate generator divides 1.8432 MHz
\ (the clock as the channel uses it) by k = (N + 1) x 2^M, N in BGR's low
\ six bits and M above them (enhanced mode); 9600 baud is N 47, M 2.
0 value dafo

: set-baud  ( baud -- )
   d# 1843200 swap /  0 swap			( m k )
   begin  dup d# 64 >  while  2/ swap 1+ swap  repeat	( m k' )
   1- swap 6 lshift or				( br )
   dup h# ff and h# 34 uart!			\ BGR
   8 rshift 6 lshift h# 38 or h# 2e uart!	\ CCR2: BR9-8
;

: set-databits  ( #bits -- )
   case
      5 of  3  endof
      6 of  2  endof
      7 of  1  endof
      ( 8 ) 0 swap
   endcase  to dafo
;

: set-parity  ( parity -- )
   case
      p.space of  h# 04  endof		\ PARE, PAR 00
      p.odd   of  h# 0c  endof		\ PARE, PAR 01
      p.even  of  h# 14  endof		\ PARE, PAR 10
      p.mark  of  h# 1c  endof		\ PARE, PAR 11
      ( none ) 0 swap
   endcase  dafo or to dafo
;

: set-stopbits  ( #stp -- )  1- if  dafo h# 20 or to dafo  then  ;

\ RTS and DTR: MODE's RTS bit (the ESCC2 has no DTR of its own)
: set-dtr-rts  ( on? -- )  if  h# 2c  else  h# 28  then  h# 22 uart!  ;

: set-handshake  ( hs -- )  drop  ;

headers

: config-serial  ( hs stp prty dbits baud dtr-rts-on? mode -- )
   rs-mode-select	( hs stp prty dbits baud dtr-rts-on? )
   uwait
   set-dtr-rts		( hs stp prty dbits baud )
   set-baud		( hs stp prty dbits )
   set-databits		( hs stp prty )
   set-parity		( hs stp )
   set-stopbits		( hs )
   set-handshake	(  )
   dafo h# 27 uart!	\ DAFO
   cmdr-reset cmdr!
;

headerless
