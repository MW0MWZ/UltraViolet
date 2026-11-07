\ Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
\ OpenBoot release it is part of (see LICENSE).
id: @(#)uart.fth 1.0 26/11/07
purpose: The early console: ttya, the SAB82532's channel A
copyright: Copyright 2026 Andy Taylor

\ OpenBoot's console before the device tree is built (ukey, ukey? and
\ uemit, as arch/sun4v/hyperconsole.fth gives them through the hypervisor):
\ the RIO EBus's SAB82532 (ESCC2), its channel A, polled, at 9600 baud, 8
\ data bits, no parity, 1 stop bit, set up as the machine's own OpenBoot
\ sets it up.

headerless
h# 1ff.f140.0000 constant uart-pa	\ EBus BAR1 + 40.0000: channel A

h# 20 constant star	\ status (read), command (write: CMDR)
h# 35 constant tic	\ transmit immediate character
h# 20 constant star-rfne	\ receive FIFO not empty
h# 08 constant star-tec	\ TIC executing
h# 80 constant cmdr-rmc	\ receive message complete: release the FIFO
h# 41 constant cmdr-reset	\ reset the receiver and transmitter

: uart@ ( reg -- byte )  uart-pa + bypass-asi spacec@  ;
: uart! ( byte reg -- )  uart-pa + bypass-asi spacec!  ;

\ The channel's mode: asynchronous, 9600 baud from the 29.4912 MHz clock,
\ 8N1, every interrupt masked, the receive FIFO's threshold one byte.
: init-uart ( -- )
   h# c3 h# 2c uart!		\ CCR0: power up, NRZ, asynchronous
   h# c0 h# 3f uart!		\ CCR4: master clock divide, extended baud rate
   cmdr-reset star uart!
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
   cmdr-reset star uart!
;

: uart-emit ( char -- )
   begin  star uart@ star-tec and 0=  until
   tic uart!
;
: uart-key? ( -- flag )  star uart@ star-rfne and 0<>  ;
: uart-key ( -- char )
   begin  uart-key?  until
   0 uart@  cmdr-rmc star uart!
;

headers
alias ukey  uart-key
alias ukey? uart-key?
alias uemit uart-emit
headerless
