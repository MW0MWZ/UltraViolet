id: @(#)make-device.fth 1.7 05/10/12
purpose: PCI bus package
copyright: Copyright 1994 FirmWorks  All Rights Reserved
copyright: Copyright 2005 Sun Microsystems, Inc.  All Rights Reserved
copyright: Use is subject to license terms.

\ Adapted by Andy Taylor, 2026: on the Ultra 5/10 (Ultra5_10) a device is
\ probed as an Ultra 10's own OpenBoot probes it (see final-assign).


\ ---------------------------------------------------------------------
\ All routines loaded from here assume we are executing in the child
\ node of a pci bus device.
\ any references to my-space are only valid for such children.
\ and all routines executing in this context will use the parent-X words.
\
\ Routines that are used in both contexts will leave phys.hi on the
\ data stack and won't do $call-<anything>  the routine not-relocatable?
\ is an example of such a routine.
\

: parent-l!  ( l phys.hi -- )   my-space + $config-l! $call-parent  ;
: parent-b@  ( phys.hi -- b )   my-space + $config-b@ $call-parent  ;
: parent-b!  ( b phys.hi -- )   my-space + $config-b! $call-parent  ;
: parent-w@  ( phys.hi -- b )   my-space + $config-w@ $call-parent  ;
: parent-w!  ( w phys.hi -- )   my-space + $config-w! $call-parent  ;
: parent-l@  ( phys.hi -- l )   my-space + $config-l@ $call-parent  ;
: parent-l!  ( l phys.hi -- )   my-space + $config-l! $call-parent  ;

: not-relocatable?  ( phys.hi -- flag )  h# 80 ss#>cfg and  0<>  ;

: header-type ( -- type ) h# 0e parent-b@ h# 7f and ;

\ True if the header type indicates that the function is a PCI-PCI bridge
: bridge?  ( -- flag )   
   header-type h# 1 = h# 0a parent-w@ h# 0609 =  or 
;

\ True if the header type indicates that the funtion is a PCI-Cardbus
: card-bus? ( -- flag )  header-type  h# 2 =  ;

: expansion-rom  ( -- offset )
   header-type h# 1 =  if  h# 38  else  h# 30   then
;

: subsystem-base ( -- offset )
   card-bus? if  h# 40  else  h# 2c  then
;

: base-register-bounds  ( -- high low )
   bridge?  if  h# 18  else
      card-bus?	if  h# 14  else  h# 28  then
   then
   h# 10
;

: io? ( phys.hi -- flag ) cfg>ss# 1 = ;

false value pci-express?	\ Set by each call to populate-device-node 

fload ${BP}/dev/pci/probe-reg.fth
fload ${BP}/dev/pci/device-props.fth
fload ${BP}/dev/pci/fcode-rom.fth

[ifdef] Ultra5_10
\ Once a device is probed, as an Ultra 10's own OpenBoot leaves it:
\ - one whose "reg" has fixed (non-relocatable) addresses (the EBus) keeps
\   them, with memory, bus master, parity and SERR# on (0x146);
\ - any other has its BARs cleared, then each relocatable space of its
\   "reg" (as its FCode left it, at the size it gives) placed lowest-first in
\   the bus's space, aligned to that size's power of 2, and its command
\   register cleared: its driver turns it on as it opens. Its expansion ROM
\   BAR keeps the address it was read at, turned off.
\ "assigned-addresses" are those placements.
: reg-fixed? ( -- flag )
   " reg" get-my-property  if  false exit  then	( xdr,len )
   false -rot  begin  dup 0>  while		( flag xdr,len )
      decode-int >r  4 0 do  decode-int drop  loop	( flag xdr,len ) ( r: phys.hi )
      rot r> not-relocatable? or -rot		( flag' xdr,len )
   repeat  2drop				( flag )
;
: place ( phys.hi size -- phys.hi pa )
   over cfg>ss# 3 and 1 =  dup >r  if  pci-io-list p2-io  else  pci-memlist p2-mem  then
   to al-floor >r				( phys.hi size ) ( r: io? list )
   dup make-power-of-2 over r> allocate-low	( phys.hi size pa false | phys.hi size true )
   if  ." Unable to assign resources for device " my-space .x cr  abort  then
   tuck +  r> if  to p2-io  else  to p2-mem  then	( phys.hi pa )
;
: final-assign ( -- )
   reg-fixed?  if  h# 146 4 parent-w!  exit  then
   h# 28 h# 10 do  0 i parent-l!  4 +loop
   " reg" get-my-property  if  0 4 parent-w!  exit  then	( xdr,len )
   0 0 encode-bytes 2swap			( out$ reg$ )
   begin  dup 0>  while				( out$ reg$ )
      decode-int >r  decode-int drop  decode-int drop	( out$ reg$ ) ( r: phys.hi )
      decode-int drop  decode-int >r		( out$ reg$ ) ( r: phys.hi size )
      r> r> swap				( out$ reg$ phys.hi size )
      over cfg>ss# 3 and  if			( out$ reg$ phys.hi size )
         tuck place				( out$ reg$ size phys.hi pa )
         2dup swap h# ff and parent-l!		( out$ reg$ size phys.hi pa )
         over cfg>ss# 3 and 3 =  if  0 2 pick h# ff and 4 + parent-l!  then
         rot >r  >r >r  2swap			( reg$ out$ ) ( r: size pa phys.hi )
         r> h# 80 ss#>cfg or encode-int encode+	( reg$ out$ ) ( r: size pa )
         0 encode-int encode+  r> encode-int encode+	( reg$ out$ ) ( r: size )
         0 encode-int encode+  r> encode-int encode+	( reg$ out$ )
         2swap					( out$ reg$ )
      else					( out$ reg$ phys.hi size )
         2drop
      then
   repeat  2drop				( out$ )
   dup  if  " assigned-addresses" property  else  2drop  then
   0 4 parent-w!
;
[then]

: load-fcode  ( adr len -- )
   >r >r r@  1 byte-load  r> r> free-mem
;

[ifdef] Ultra5_10
\ An Ultra 10 makes a PCI-PCI bridge's and the EBus's own properties first
\ and the configuration header's after them (a bridge's driver makes those
\ itself where they come among its own, dev/pci-bridge/simba/simba.fth;
\ made again here, in place); any other device's, the header's first.
: ebus? ( -- flag )  vid,did h# 1000 = swap h# 108e = and  ;
: header-last? ( -- flag )  bridge? ebus? or  ;
[then]

: populate-device-node  ( -- )
   pcie-capability-regs 0<> to pci-express?
[ifdef] Ultra5_10
   header-last? 0=  if  pci-make-function-properties  then
[else]
   pci-make-function-properties
[then]
[ifdef] Ultra5_10
   \ Each device's temporary addresses from its bus's base; a bridge's own
   \ BARs (an APB has none) not probed.
   bus-mem-base to p1-mem  bus-io-base to p1-io
   bridge?  if  bar-struct-addr /bar-data erase  else  assign-addresses  then
[else]
   assign-addresses				( )
[then]
   find-fcode? if				( adr len )
      load-fcode true				( )
   else						( )
      no-builtin-fcode?  if
         make-std-fcode-properties false	( false )
      else					( )
         true					( true )
      then					( flag )
   then						( flag )
[ifdef] Ultra5_10
   drop  header-last?  if  pci-make-function-properties  then
   bridge? 0=  if  final-assign  then
[else]
   make-assigned-property			( )
   
   device-enabled? 0= bridge? 0= and if
      \ Disables all card response
      0 4 parent-w!
   then
[then]
;
