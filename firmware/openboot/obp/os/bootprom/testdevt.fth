\ ========== Copyright Header Begin ==========================================
\ 
\ Hypervisor Software File: testdevt.fth
\ 
\ Copyright (c) 2006 Sun Microsystems, Inc. All Rights Reserved.
\ 
\  - Do no alter or remove copyright notices
\ 
\  - Redistribution and use of this software in source and binary forms, with 
\    or without modification, are permitted provided that the following 
\    conditions are met: 
\ 
\  - Redistribution of source code must retain the above copyright notice, 
\    this list of conditions and the following disclaimer.
\ 
\  - Redistribution in binary form must reproduce the above copyright notice,
\    this list of conditions and the following disclaimer in the
\    documentation and/or other materials provided with the distribution. 
\ 
\    Neither the name of Sun Microsystems, Inc. or the names of contributors 
\ may be used to endorse or promote products derived from this software 
\ without specific prior written permission. 
\ 
\     This software is provided "AS IS," without a warranty of any kind. 
\ ALL EXPRESS OR IMPLIED CONDITIONS, REPRESENTATIONS AND WARRANTIES, 
\ INCLUDING ANY IMPLIED WARRANTY OF MERCHANTABILITY, FITNESS FOR A 
\ PARTICULAR PURPOSE OR NON-INFRINGEMENT, ARE HEREBY EXCLUDED. SUN 
\ MICROSYSTEMS, INC. ("SUN") AND ITS LICENSORS SHALL NOT BE LIABLE FOR 
\ ANY DAMAGES SUFFERED BY LICENSEE AS A RESULT OF USING, MODIFYING OR 
\ DISTRIBUTING THIS SOFTWARE OR ITS DERIVATIVES. IN NO EVENT WILL SUN 
\ OR ITS LICENSORS BE LIABLE FOR ANY LOST REVENUE, PROFIT OR DATA, OR 
\ FOR DIRECT, INDIRECT, SPECIAL, CONSEQUENTIAL, INCIDENTAL OR PUNITIVE 
\ DAMAGES, HOWEVER CAUSED AND REGARDLESS OF THE THEORY OF LIABILITY, 
\ ARISING OUT OF THE USE OF OR INABILITY TO USE THIS SOFTWARE, EVEN IF 
\ SUN HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGES.
\ 
\ You acknowledge that this software is not designed, licensed or
\ intended for use in the design, construction, operation or maintenance of
\ any nuclear facility. 
\ 
\ ========== Copyright Header End ============================================
\ Adapted by Andy Taylor, 2026: on the Ultra 5/10 (Ultra5_10) .properties
\ shows values as an Ultra 10's does, and its pager stops only between
\ properties.
id: @(#)testdevt.fth 2.43 05/02/03
purpose:
copyright: Copyright 2005 Sun Microsystems, Inc.  All Rights Reserved
copyright: Use is subject to license terms.

headerless

also hidden
: chdump ( addr len -- )  push-hex ['] c@ to dc@ d.2 pop-base ;
previous

: char?  ( byte -- flag )
   dup bl h# 7e between           ( byte printable?)
   over carret = rot linefeed =   ( printable? cr? nl?)
   or or			  ( printable?)
;

\ Algorithm:
\
\ This is a reasonable heuristic to test composite encoded strings.
\
\ A printable string is a sequence of bytes that contains all 
\ printable chars; a composite string is a sequence of non-empty 
\ printable strings separated by a null byte. Legalistically, 
\ a null byte is the terminator of a printable string,
\ but in existing practice, some string properties (e.g., in the
\ /options node) are encoded without a null byte at the end.
\ To maintain compatibility, we will consider the last string
\ valid either way - with or without a terminating null byte.
\ While two null bytes in a row might be interpreted as an empty
\ string, we will not consider that valid in a composite string.
\
\ Any sequence of printable bytes will be decoded as string(s) 
\ and printed even if it was originally encoded as an integer byte 
\ stream. There is no foolproof decode solution available until we 
\ change the implementation of properties in OBP to encode a
\ property type as well as the property data. 
\
\ Implementation:
\
\ Initial setting of "previous byte non-null?" flag will affect 
\ how a leading null-byte is treated. We want a leading null-byte 
\ to become non-valid; it will be, if we initialize this flag to 
\ false, by virtue of running afoul of the "two consecutive nulls" 
\ rule. However, we want to make a special case of a property that
\ consists of only a single null-byte: we want to allow that as an 
\ empty string, and that can be achieved by initializing this flag 
\ to true if the length is 1 (a single-byte non-null will be 
\ subjected to the char? test). Initial setting of composite? flag 
\ will only be applicable to an empty string, which we want to fail. 
\ Otherwise, the initial composite? flag  will be discarded upon
\ entering the ?do loop.

: text?  ( adr len -- composite? )
   dup 1 = false		        ( adr len prev-non-null? false ) 
   2swap bounds  ?do 			( prev-non-null? composite? )
      drop i c@ dup  if	 		( prev-non-null? byte|null )
	 char?				( prev-non-null? printable? )
	 \  Update prev-non-null? It should be true,
	 \  but if byte is not printable, it doesn't matter.
	 nip dup			( prev-non-null? composite? )
      else
 	 \  Null-byte seen.		( prev-non-null? false )
	 \  Update prev-non-null? It is now false.
	 \  If existing prev-non-null? was also false,
	 \  then this is not a valid composite.
	 swap				( false prev-non-null? )
      then				( non-null? composite? )
      dup 0= ?leave			( non-null? composite? )
   loop  nip			        ( composite? )
;
  
: .node-name ( -- ) "temp 0 (append-name+unit) type ;

: .nodeid  ( -- )  current-device .h  .node-name  cr  ;

: 8.x  ( n -- )
   push-hex
   <#  u# u# u# u#  u# u# u# u#  u#>  type space
   pop-base 
;

: to-display-column  ( -- )  d# 25 to-column  ; 

\ Displays the property value "adr,len" as a list of integer values,
\ showing '#ints/line' on each line.

: .ints  ( adr len #ints/line  -- exited? )
   >r begin  dup 0>  while	( adr len )
[ifndef] Ultra5_10
      \ (The Ultra 5/10's pager stops only between properties, as an
      \ Ultra 10's: .properties's own exit?.)
      exit?  if  r> 3drop true  exit  then 	
[then]
      to-display-column		( adr len )
      r@  0  do			( adr len  )  
         decode-int 8.x		( adr'len' ) 
         dup 0=  ?leave		( adr'len' ) 
      loop cr			( adr'len' )
   repeat			( adr'len' )
   r> 3drop false		( exited? )
;

\ Display the property value "adr,len" as a list of strings,
\ showing one string on each line; "adr,len" must pass the 
\ printability test first (use text?).

: show-strings  ( adr len -- exited? )
   begin  dup  while  
      exit?  if  2drop true exit  then
      decode-string  to-display-column type cr  
   repeat  2drop false 
;

: my-#size-cells  ( -- #size-cells )
   " #size-cells" get-property  if  1  else  get-encoded-int  then
;

: parent-#size-cells  ( -- #size-cells )
   \ Root node has no parent, therefore the size of its parent's address
   \ space is meaningless
   root-device?  if  0  exit  then
   current-device >r pop-device my-#size-cells r> push-device
;

: size+  ( #cells -- #cells+#size-cells )  parent-#size-cells +  ;

[ifndef] Ultra5_10
headers
vocabulary known-int-properties
also known-int-properties definitions

: available        ( -- n )  '#adr-cells @ size+  ;
: reg              ( -- n )  '#adr-cells @ size+  ;
: existing         ( -- n )  '#adr-cells @ size+  ;
: ranges           ( -- n )  '#adr-cells @  #adr-cells + my-#size-cells +  ;

alias address    1 ( -- n )    
alias interrupts 1 ( -- n )   
alias intr       2 ( -- n )  
alias clock-frequency 1 ( -- n )    

previous definitions

headerless
: display  ( anf prop-addr,len -- exited? )
   rot  name>string			     ( adr,len  name,len )

   ['] known-int-properties (search-wordlist)  if
       execute .ints  exit
   then                                      ( adr,len )

   \ Test for unprintable characters; allow composite strings.
   2dup text?  if  show-strings exit  then   ( adr,len )

   dup /l =  if  1 .ints exit  then          ( adr,len )

   dup -rot				     ( len adr,len ) 
   to-display-column  h# 10 min chdump       ( len )	      
   h# 10 >  if ." ..."  then                 ( )
   false				     ( exited? )  
;

[else]
\ The Ultra 5/10's display: the integer properties known by name (these
\ and the PCI ones of knownprops.fth), a row of cells to a line; any
\ other value of two cells as two integers, one to a line; a single
\ string of printable bytes (with or without a final null) as text,
\ however short; anything else as hex bytes, at most 16 and nothing
\ after them. Composite strings are bytes. A known property's rows end
\ with its value, a short last row and all.

: my-cells  ( default name$ -- n )  get-property 0=  if  get-encoded-int nip  then  ;

headers
vocabulary known-int-properties
also known-int-properties definitions

: available        ( -- n )  '#adr-cells @ size+  ;
: reg              ( -- n )  '#adr-cells @ size+  ;
: existing         ( -- n )  '#adr-cells @ size+  ;
: ranges           ( -- n )  '#adr-cells @  #adr-cells + my-#size-cells +  ;
: interrupt-map-mask  ( -- n )  #adr-cells  1 " #interrupt-cells" my-cells +  ;
\ A unit's address and interrupt, the interrupt parent's phandle and its
\ interrupt (no parent address, as these maps are built).
: interrupt-map  ( adr len -- adr len n )
   interrupt-map-mask >r                         ( adr len )  ( r: ai )
   dup r@ 1+ /l * <  if  r> 1+ exit  then        ( adr len )
   over r@ la+ /l decode-int nip nip             ( adr len phandle )
   current-device >r  push-device
   1 " #interrupt-cells" my-cells                ( adr len pi )
   r> push-device  r> + 1+                       ( adr len n )
;
: translations     ( -- n )  6  ;

alias address    1 ( -- n )
alias interrupts 1 ( -- n )
alias intr       2 ( -- n )
alias clock-frequency 1 ( -- n )
alias #address-cells 1 ( -- n )
alias #size-cells 1 ( -- n )
alias #interrupt-cells 1 ( -- n )
alias upa-portid 1 ( -- n )
alias scsi-initiator-id 1 ( -- n )

previous definitions

headerless
\ Printable bytes, the last of them perhaps a null.
: string?  ( adr len -- flag )
   dup  if  2dup + 1- c@ 0=  if  1-  then  then  ( adr len' )
   true -rot  bounds  ?do  i c@ char? and  loop
;
: display  ( anf prop-addr,len -- exited? )
   rot  name>string			     ( adr,len  name,len )

   ['] known-int-properties (search-wordlist)  if
       execute .ints  exit
   then                                      ( adr,len )

   2dup string?  if                          ( adr,len )
      to-display-column
      dup  if  2dup + 1- c@ 0=  if  1-  then  then
      type false exit
   then                                      ( adr,len )

   dup 2 /l * =  if  1 .ints exit  then      ( adr,len )

   to-display-column  h# 10 min chdump  false  ( exited? )
;
[then]

: .not-devtree ( -- )
   ." Not at a device tree node. Use 'dev <device-pathname>'."
;
: (.property)  ( anf xt -- exited? )  dup .name >r r@ get r> decode display  ;
headers
: .properties  ( -- )
   device-context?  if
      0  current-properties                         ( alf voc-acf )
      begin                                         ( alf voc-acf )
         ??cr exit?  if  2drop exit  then           ( )
         another-word?  while                       ( alf' voc-acf anf )
         dup name> (.property) if  2drop exit  then ( )
      repeat                                        ( alf' voc-acf )
   else                                             ( )
      .not-devtree                                  ( )
   then                                             ( )
;
: ls  ( -- )
   device-context?  if
      'child token@                   ( first-node )
      begin  non-null?  while         ( node )
	 push-device                  ( )
	 .nodeid                      ( )
	 'peer token@                 ( node' )
	 pop-device
      repeat                          ( )
   else
      .not-devtree
   then
;

: pwd  ( -- )
   device-context?  if
      pwd$ type
   else
      .not-devtree
   then
   cr
;
headerless
: shownode  ( -- false )  exit?  if  true  else  pwd false  then  ;
: optional-arg-or-/$ ( -- adr len )
   parse-word dup 0=  if  2drop " /"  then  ( adr len )
;
headers
: $show-devs ( path$ -- )
   also
   find-device
   ['] shownode  ['] (search-preorder) catch 2drop
   previous definitions
;
: show-devs  ( ["path"] -- )  optional-arg-or-/$ $show-devs  ;

: dev  ( -- )
   optional-arg-or-/$            ( adr,len )
   ?expand-alias                 ( adr,len )
   2dup " .." $=  if             ( adr,len )
      2drop device-context?  if  (  )
	 pop-device              (  )
      else                       (  )
	 .not-devtree            (  )
      then                       (  )
   else                          ( adr,len )
      find-device                (  )
   then                          (  )
;

: show-props  ( -- )
   also
   optional-arg-or-/$           ( adr len )
   find-device  .properties  device-end
   previous definitions
;
headerless
: show-aliases  ( -- )
   also  " /aliases" find-device  .properties  (  )
   previous definitions                        (  )
;
: show-alias  ( adr len -- )
   2dup " name" $= 0=  if     ( adr,len )
      ['] aliases $vfind  if  ( xt )
	 dup >name swap  (.property) drop exit
      then                    ( adr,len )
   then                       ( adr,len )
   type ."  : no such alias"  (  )
;
headers
: devalias  \ name string  ( -- )
   parse-word  parse-word
   dup  if                        ( name$ path$ )
      $devalias  (  )
   else                           ( name$ path$ )
      2drop dup  if               ( name$ )
	 show-alias               (  )
      else                        ( name$ )
	 2drop show-aliases       (  )
      then                        (  )
   then                           (  )
;
