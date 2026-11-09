\ Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
\ OpenBoot release it is part of (see LICENSE).
id: @(#)diagmsg.fth 1.0 26/11/09
purpose: The Ultra 5/10's probing messages in diagnostic mode
copyright: Copyright 2026 Andy Taylor

\ In diagnostic mode an Ultra 10 shows, as it probes, a line for each
\ place probed ("Probing UPA Slot at 1e,0 ", "Probing /pci@1f,0/pci@1,1 at
\ Device 1  ") and on it the name of each node made there, as the name is
\ set: its "name" property as encoded (its 0 byte too), and a space; or
\ "Nothing there". probeall.fth and the PCI bus module's prober print the
\ lines; this is the name's part, shown as the name is set, for the
\ nodes made while a line is open.

headerless

false value diag-probe?		\ diagnostic mode, as probing began
false value probe-names?	\ a probing line open: names shown
0 value #probe-names		\ names shown on it

\ The release's own (4.x) diagnostic messages kept quiet while xt runs.
: quietly ( ?? xt -- ?? )
   ['] diagnostic-mode? behavior >r  ['] false to diagnostic-mode?
   catch  r> to diagnostic-mode?  throw
;

\ A line opened, and closed: "Nothing there" if no node was named.
: open-probe-line ( -- )  0 to #probe-names  true to probe-names?  ;
: close-probe-line ( -- )
   false to probe-names?
   #probe-names 0=  if  ." Nothing there"  then  cr
;

also magic-properties definitions
headers
overload: name ( xdr,len name$ -- xdr,len name$ )
   probe-names?  if
      2over type space  #probe-names 1+ to #probe-names
   then
   name
;
previous definitions
headerless
