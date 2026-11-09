\ Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
\ OpenBoot release it is part of (see LICENSE).
id: @(#)fixed-u10.fth 1.0 26/11/09
purpose: The configuration variables in an Ultra 10's NVRAM layout
copyright: Copyright 2026 Andy Taylor

\ The Ultra 5/10 keeps every configuration variable
\ at a fixed place in the M48T59, and so does this one, byte for byte: so
\ that the settings survive a change of firmware either way. The whole
\ area 0x000-0x1CD7 is the configuration variable manager's "fixed"
\ region (nvram.fth), each variable at an Ultra 10's offset with an Ultra 10's encoding
\ (loadconfig.fth places them). The flags, 4-byte integers and keyword
\ bytes are the release's own fixed types; the strings, byte arrays,
\ one-byte integers, the password and nvramrc are here, as an Ultra 10 keeps them.
\
\ An Ultra 10's checksum: byte 0 is 0x17 XORed with every byte 0x00C-0x1CD7
\ (nvram.fth keeps it as each byte is written).
\
\ An Ultra 10's quirks are followed, as nothing tells which of them anything of
\ Sun's relies on:
\  - a string is cut to its field (field size - 2), and its length kept in
\    one byte: boot-device's and diag-device's fields hold 256, so a value
\    of 256 bytes or more is stored with length 0, empty (the old text left
\    behind the 0);
\  - the password is its length and up to 8 characters, plain, no 0 after;
\  - a byte array (keymap, oem-logo) is a 4-byte length and the bytes, the
\    rest of its field zeroed;
\  - nvramrc's text ends at 0x1CD5; its length is at 0x2BC and again at
\    0x1CD6; set-defaults zeroes only the first, and nvrecover takes the
\    text by the second;
\  - security-mode and system-board-date's length share byte 0x050;
\  - at a bad checksum: the defaults, oem-banner and oem-logo emptied, and
\    diag-switch? true (set-defaults alone does neither).

also nvdevice definitions

unexported-words

\ A stretch of the fixed region written: cached, and written through.
: u10-write ( adr len pos -- )
   fixed-options-open?  if			( adr len pos )
      dup >r  >fixed-buffer swap 2dup 2>r move	( ) ( r: pos )
      2r> r> (fixed-write)			( )
   else						( adr len pos )
      3drop					( )
   then						( )
;
: u10-w! ( n pos -- )  >r  fixed-xfer-buffer tuck unaligned-w!  2 r> u10-write  ;
: u10-w@ ( pos -- n )  >fixed-buffer unaligned-w@  ;

\ Strings: after the position, the most the field keeps (w), then the
\ default (counted). A length past the field reads as empty (as an Ultra 10
\ shows an unwritten field's).
: >u10-max ( apf -- n )  la1+ unaligned-w@  ;
: >u10-default ( apf -- adr len )  la1+ wa1+ count  ;

: u10-string@ ( apf -- adr len )
   fixed-options-open?  if			( apf )
      dup fixed-pos@ >fixed-buffer count		( apf adr len )
      rot >u10-max over <  if  drop 0  then		( adr len' )
      exit
   then
   dup fixed-nodefault?  if  drop " "  else  >u10-default  then
;
\ The length cut to the field, then kept in a byte (256 becomes 0).
: u10-string! ( adr len apf -- )
   >r  r@ >u10-max min  h# ff and		( adr len' )
   dup fixed-xfer-buffer c!			( adr len' )
   dup >r  fixed-xfer-buffer 1+ swap move  r>	( len' )
   0 over fixed-xfer-buffer 1+ + c!		( len' )
   fixed-xfer-buffer swap 2+  r> fixed-pos@ u10-write
;

7 actions
action: u10-string@			;
action: u10-string!			;
action: fixed-adr			;
action: drop				;
action: drop				;
action: fixed-nodefault?		;
action: >u10-default			;

exported-headers transient

: u10-string \ name ( default$ max pos -- )
   fixed-create  w,  ",  align  use-actions
;
: nodefault-u10-string \ name ( max pos -- )
   h# 8000.0000 or fixed-create  w,  0 c,  align  use-actions
;

unexported-words resident

\ The password: its length and up to 8 characters, nothing after.
: u10-password! ( adr len apf -- )
   >r  r@ >u10-max min			( adr len' )
   dup fixed-xfer-buffer c!		( adr len' )
   dup >r  fixed-xfer-buffer 1+ swap move  r>	( len' )
   fixed-xfer-buffer swap 1+  r> fixed-pos@ u10-write
;

7 actions
action: u10-string@			;
action: u10-password!			;
action: fixed-adr			;
action: drop  2drop  0 0		;	\ (shown empty)
action: drop				;
action: drop true			;
action: >u10-default			;

exported-headers transient

: u10-password \ name ( max pos -- )
   h# 8000.0000 or fixed-create  w,  0 c,  align  use-actions
;

unexported-words resident

\ Byte arrays: a 4-byte length, then the bytes; the rest of the field
\ zeroed. After the position: the most kept (w), the default's length (w)
\ and bytes.
: >u10-bytes-default ( apf -- adr len )  la1+ wa1+ dup wa1+ swap unaligned-w@  ;

: u10-bytes@ ( apf -- adr len )
   fixed-options-open?  if			( apf )
      dup fixed-pos@ >fixed-buffer  dup la1+ swap unaligned-l@	( apf adr len )
      rot >u10-max over <  if  drop 0  then		( adr len' )
      exit
   then
   dup fixed-nodefault?  if  drop " "  else  >u10-bytes-default  then
;
: u10-bytes! ( adr len apf -- )
   >r  r@ >u10-max min			( adr len' )
   fixed-xfer-buffer  r@ >u10-max la1+ erase	( adr len' )
   dup fixed-xfer-buffer unaligned-l!	( adr len' )
   fixed-xfer-buffer la1+ swap move	( )
   fixed-xfer-buffer  r@ >u10-max la1+  r> fixed-pos@ u10-write
;

7 actions
action: u10-bytes@			;
action: u10-bytes!			;
action: fixed-adr			;
action: drop				;
action: drop				;
action: fixed-nodefault?		;
action: >u10-bytes-default		;

exported-headers transient

: u10-bytes \ name ( default-adr,len max pos -- )
   fixed-create  w,  dup w,  here over allot swap move  align  use-actions
;
: nodefault-u10-bytes \ name ( max pos -- )
   h# 8000.0000 or fixed-create  w,  0 w,  align  use-actions
;

unexported-words resident

\ A keyword taken quietly: a word that isn't one of the variable's (or more
\ than one word) only fails, and setenv says "Invalid value; previous
\ value retained." alone, as an Ultra 10 does (the release's lookup lists the
\ keywords first).
: u10-voc-encode ( adr len apf -- value )
   >fixed-vocab-acf >r				( adr len ) ( r: voc )
   next-keyword 0=  if  r> drop  -1 throw  then	( rem,len str,len' )
   2swap next-keyword  if			( str,len' rem,len w,len )
      2drop 2drop 2drop  r> drop  -1 throw
   then						( str,len' )
   r> search-wordlist 0=  if  -1 throw  then	( acf )
   value>					( value )
;

\ Keyword variables (diag-level, mfg-mode): the release's fixed keyword
\ variable with the quiet lookup.
7 actions
action: ( apf -- value )		fixed-adr c@  ;
action: ( value apf -- )		set-fixed-vocab-value  ;
action: ( apf -- adr )			fixed-adr  ;
action: ( value apf -- adr,len )	get-fixed-vocab-value  ;
action: ( adr,len apf -- value )	u10-voc-encode  ;
action: ( apf -- value )		fixed-nodefault?  ;
action: ( apf -- value )		>fixed-default c@  ;

exported-headers transient

: u10-keyword \ name ( default$ voc pos -- )
   >r  dup >r  voc-string>value  r> r>		( default voc pos )
   fixed-create  1 c,  swap c,  align  token,  use-actions
;

unexported-words resident

\ security-mode (none, command, full: 0, 1, 2), shown as an Ultra 10 shows it:
\ any other byte (system-board-date's length, which shares it) as none,
\ as the release's security-on? takes it.
: u10-mode-decode ( value apf -- adr len )
   drop  case
      1 of  " command"  endof
      2 of  " full"  endof
      >r  " none"  r>
   endcase
;

7 actions
action: ( apf -- value )		fixed-adr c@  ;
action: ( value apf -- )		set-fixed-vocab-value  ;
action: ( apf -- adr )			fixed-adr  ;
action: ( value apf -- adr,len )	u10-mode-decode  ;
action: ( adr,len apf -- value )	u10-voc-encode  ;
action: ( apf -- value )		fixed-nodefault?  ;
action: ( apf -- value )		>fixed-default c@  ;

exported-headers transient

: u10-security-mode \ name ( pos -- )
   h# 8000.0000 or fixed-create  1 c,  align  ['] security-mode-voc token,
   use-actions
;

unexported-words resident

\ One-byte integers (scsi-initiator-id).
: u10-char@ ( apf -- n )
   fixed-options-open?  if  fixed-pos@ >fixed-buffer c@ exit  then
   la1+ c@
;

7 actions
action: u10-char@			;
action: fixed-byte!			;
action: fixed-adr			;
action: drop (.d)			;
action: encode-number			;
action: fixed-nodefault?		;
action: la1+ c@				;

exported-headers transient

: u10-char \ name ( default pos -- )
   fixed-create  c,  align  use-actions
;

unexported-words resident

\ nvramrc: its text ending at 0x1CD5, its length at 0x2BC and at 0x1CD6;
\ up to 5282 bytes (0x834-0x1CD5).
h# 1cd6 constant nvramrc-end
h# 2bc constant nvramrc-len-pos
d# 5282 constant /nvramrc

: (nvramrc-text) ( len -- adr len )  nvramrc-end over - >fixed-buffer swap  ;
: nvramrc@ ( apf -- adr len )
   drop
   fixed-options-open?  if  nvramrc-len-pos u10-w@ (nvramrc-text) exit  then
   " "
;
\ The text kept by the second length, for nvrecover.
: kept-nvramrc ( -- adr len )
   fixed-options-open?  if
      nvramrc-end u10-w@  dup /nvramrc >  if  drop 0  then  (nvramrc-text)
      exit
   then
   " "
;
\ Empty: only the first length zeroed (as set-defaults leaves it, the text
\ and the second length kept for nvrecover).
: nvramrc! ( adr len apf -- )
   drop  dup 0=  if  nip nvramrc-len-pos u10-w! exit  then
   dup /nvramrc >  abort" Insufficient NVRAM space for new nvramrc file"						( adr len )
   tuck  nvramrc-end over -  u10-write		( len )
   dup nvramrc-end u10-w!  nvramrc-len-pos u10-w!
;

7 actions
action: nvramrc@			;
action: nvramrc!			;
action: fixed-adr			;
action: drop				;
action: drop				;
action: drop false			;
action: drop " "			;

exported-headers transient

: u10-nvramrc \ name ( -- )
   nvramrc-len-pos fixed-create  use-actions
;

unexported-words resident
previous definitions
