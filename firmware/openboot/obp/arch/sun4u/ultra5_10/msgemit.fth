\ Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
\ OpenBoot release it is part of (see LICENSE).
id: @(#)msgemit.fth 1.0 26/11/09
purpose: The early console's text kept for the console
copyright: Copyright 2026 Andy Taylor

\ What OpenBoot writes on ttya before the console is installed is kept in
\ the message buffer (os/bootprom/msgbuf.fth), and shown again on the
\ console once it is installed, unless the console is ttya (startup.fth's
\ show-tty-msgs), as an Ultra 10 does: so that in diagnostic mode its
\ report and probing lines reach the screen. (The buffer exists from its
\ stand-init; what doesn't fit is not kept.)

headerless
: msg-uemit ( char -- )
   msg-buf-ptr @  ?dup  if				( char ptr )
      dup msg-buf-end @ u<  if				( char ptr )
         2dup c!  1+ msg-buf-ptr !			( char )
      else						( char ptr )
         drop						( char )
      then						( char )
   then							( char )
   uart-emit
;
' msg-uemit is uemit
