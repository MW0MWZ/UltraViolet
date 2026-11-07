\ ========== Copyright Header Begin ==========================================
\ 
\ Hypervisor Software File: mmumiss.fth
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
\ Adapted for the UltraSPARC IIi (sun4u) by Andy Taylor, 2026: the miss
\ traps read the faulting address and context from the MMU's tag access
\ register and load the TLB through its data-in register (sun4v: the
\ hypervisor's MMU fault area and map call), the CPU structure's address
\ is the patched constant cpu-struct-pa, and the data miss has the two
\ entry points sun4u's trap table uses (TL=1 and TL>1); find-prom-tte drops
\ the address as well when it finds no mapping.
id: @(#)mmumiss.fth 1.1 06/02/16
purpose: 
copyright: Copyright 2006 Sun Microsystems, Inc.  All Rights Reserved
copyright: Use is subject to license terms.

headers

0 >mmu-defer per-cpu-defer: va>tte-data

headerless

: null-va>tte-data ( vadr ctx -- false )  2drop false  ;

stand-init: Initialising the per CPU miss handler
   ['] null-va>tte-data  ['] va>tte-data 3 perform-action
;

: va>tte-installed?  ( -- flag )
   addr va>tte-data
   dup x@ 0<> swap
   token@ ['] null-va>tte-data  <> and
;

: prom-virt? ( vadr -- flag )
   dup monvirtbase RAMtop between  ( vadr flag )
   swap vpt-base u>= or        ( flag )
;
: (set-tte-soft ( tte virt -- tte' )
   prom-virt?  if  >tte-soft  then
;
' (set-tte-soft is set-tte-soft

: find-prom-tte ( virtual -- phys-lo phys-hi )
   dup pgmap@  dup valid-tte?  if  ( va tte )
      tuck tte>size 1- and     ( tte offset )
      s>d rot tte> d+          ( phys-lo phys-hi )
   else                        ( va tte )
      2drop -1 -1              ( -1 -1 )
   then                        ( phys-lo phys-hi )
;

headers
: find-client-tte ( vadr context# -- tte vadr' true -or- false )
   dup 0=  if			   ( vaddr context# )
      over  vpt-base u>=  if  2drop false  exit  then
   then				   ( vaddr context# )

   va>tte-installed?  if           ( vadr context# )
      2dup  va>tte-data  if        ( vadr context# tte )
	 rot swap dup tte>size     ( context# vadr tte size )
	 rot swap round-down       ( context# tte vadr )
	 \ Merge VA and CTX#
	 rot or                    ( tte vadr" )
      else                         ( vadr context# )
	 2drop  false  exit
      then                         ( tte vadr )
   else                            ( vadr context# )
      \ We can't handle contexts other than 0
      if  drop false  exit  then   ( vadr )
      dup pgmap@ tuck valid-tte?  0=  if      ( tte vadr )
	 2drop false  exit
      then                         ( tte vadr )
   then  true                      ( tte vadr true )
;

headerless
: (>physical) ( virt -- phys-lo phys-hi )
   dup prom-virt?  if  find-prom-tte exit  then
   dup 0  find-client-tte  if ( virt tte virt'  )
      drop tuck  tte>size     ( tte virt size )
      1-  and                 ( tte offset )
      s>d  rot  tte> d+       ( phys-lo phys-hi )
   else                       ( virt )
      drop -1 -1              ( -1 -1 )
   then                       ( phys-lo phys-hi )
;
' (>physical) is >physical

headers
: map? ( vadr -- )
   dup prom-virt?  if             ( vadr )
      dup pgmap@                  ( vadr tte )
   else                           ( vadr )
      dup 0  find-client-tte  if  ( vadr tte vadr' )
	 drop                     ( vadr tte )
      else                        ( vadr )
	 false                    ( vadr inv-tte )
      then                        ( vadr tte )
   then  (.map)                   (  )
;

headerless
: resolve-immu-miss ( vadr -- ok? )
   va>va,ctx  find-client-tte  if  ( tte vadr )
      itlb-tar-dir!  true          ( ok )
   else                            (  )
      false                        ( flag )
   then                            ( ok? )
;

: resolve-dmmu-miss ( vadr -- ok? )
   va>va,ctx  find-client-tte  if  ( tte vadr )
      dtlb-tar-dir!  true          ( flag )
   else                            (  )
      false                        ( flag )
   then                            ( ok? )
;

code dmmu-miss-return  ( ok? -- )
   tos				%l1	move
   %l5				cpu-struct-pa	\ CPU struct PA
   0 >dmmu-miss-state		%l0	set
   %l0  %l5			%g5	add	\ CPU save area
   %l1				%g0	cmp
   restore-cpu-state		0<>	brif
   %g0				%g4	move
   %g0  2			%g4	add	\ restore and come here
   restore-cpu-state		always	brif
   %g7					rdpc
   %g0  pstate-ag		wrpstate	\ alternate globals
   0 >dmmu-miss-state		%g1	set
   small-forth-save-state	always	brif
					nop
c;

code immu-miss-return  ( ok? -- )
   tos				%l1	move
   %l5				cpu-struct-pa	\ CPU struct PA
   0 >immu-miss-state		%l0	set
   %l0  %l5			%g5	add	\ CPU save area
   %l1				%g0	cmp
   restore-cpu-state		0<>	brif
   %g0				%g4	move
   %g0  2			%g4	add	\ restore and come here
   restore-cpu-state		always	brif
   %g7					rdpc
   %g0  pstate-ag		wrpstate	\ alternate globals
   0 >immu-miss-state		%g1	set
   small-forth-save-state	always	brif
					nop
c;

headers
defer dmmu-miss-enter-hook ( adr -- adr )
defer dmmu-miss-exit-hook  ( ok? -- ok? )
defer immu-miss-enter-hook ( adr -- adr )
defer immu-miss-exit-hook  ( ok? -- ok? )
headerless

' noop to dmmu-miss-enter-hook
' noop to dmmu-miss-exit-hook
' noop to immu-miss-enter-hook
' noop to immu-miss-exit-hook

: immu-miss-handler  ( vadr -- )
   immu-miss-enter-hook  ( vadr )
   resolve-immu-miss     ( ok? )
   immu-miss-exit-hook   ( ok? )
   immu-miss-return      (  )
;

: dmmu-miss-handler  ( vadr -- )
   dmmu-miss-enter-hook  ( vadr )
   resolve-dmmu-miss     ( ok? )
   dmmu-miss-exit-hook   ( ok? )
   dmmu-miss-return      (  )
;

label dmmu-miss-start ( -- vadr )
   %g1				cpu-struct-pa	\ CPU struct PA
   0 >dmmu-miss-state		%g5	set
   %g5  %g1			%g5	add		\ DMMU stacks
   %g0				%g6	move		\ Small Save
   save-cpu-state		always	brif
   %g7					rdpc

   \ We are now in a normal TL=0, IE=1, CWP=0 environment
   \ Set up Forth Machine
   0 >dmmu-miss-state		%l0	set
   'body dmmu-miss-handler	%l7	set
   setup-small-forth-engine	always	brif	nop
end-code

label immu-miss-start ( -- vadr )
   %g1				cpu-struct-pa	\ CPU struct PA
   0 >immu-miss-state		%g5	set
   %g5  %g1			%g5	add		\ DMMU stacks
   %g0				%g6	move		\ Small Save
   save-cpu-state		always	brif
   %g7					rdpc

   \ We are now in a normal TL=0, IE=1, CWP=0 environment
   \ Set up Forth Machine
   0 >dmmu-miss-state		%l0	set
   'body immu-miss-handler	%l7	set
   setup-small-forth-engine	always	brif	nop
end-code

headerless

\ The miss traps (at TL=1, in the MMU globals). The tag access register
\ holds the faulting address (63:13) and context (12:0). OpenBoot's own
\ mappings (context 0, a valid entry in its page tables marked as its own)
\ are loaded here; anything else goes to the handlers above.
\
\   %g4 = the fault address, %g1 = its context; %g5 keeps %o7.

\ The fast path's end: the TTE in %g3 loaded, the trap retried.
transient also assembler definitions
: miss-fast-path  ( slow-label data-in-asi -- )
   >r >r
   %g1  %g0			%g0	subcc
   r@ 0<>				brif		\ a client's context
   %o7				%g5	move		\ save %o7 (delay)
   tte-lookup				call
   nop
   \ %g3 = TTE, %g4 = VA
   %g3	%g0			%g0	subcc
   r@ 0>=				brif		\ not mapped
   %g5				%o7	move		\ restore link (delay)
   %g3  d# 63 d# 7 -		%g1	sllx		\ OpenBoot's own (soft bit 7)?
   r> 0>=				brif
   nop
   %g3  %g0  %g0  r>			stxa		\ data in
   #Sync					membar
   retry
;
resident previous definitions

label immu-miss-trap
   %g0  h# 30			%g1	add
   %g1  %g0  h# 50		%g4	ldxa		\ tag access
   %g4  d# 51			%g1	sllx		\ context (12:0)
   %g1  d# 51			%g1	srlx
   %g4  %g1			%g4	sub		\ address
   immu-miss-start  h# 54  miss-fast-path
end-code

label dmmu-miss-trap-TL=1
   %g0  h# 30			%g1	add
   %g1  %g0  h# 58		%g4	ldxa		\ tag access
   %g4  d# 51			%g1	sllx		\ context (12:0)
   %g1  d# 51			%g1	srlx
   %g4  %g1			%g4	sub		\ address
   dmmu-miss-start  h# 5c  miss-fast-path
end-code

\ A miss in a trap handler (a window spill or fill): only OpenBoot's own
\ mappings; anything else is an error, caught by save-state.
label dmmu-miss-trap-TL>1
   %g0  h# 30			%g1	add
   %g1  %g0  h# 58		%g4	ldxa
   %g4  d# 51			%g1	sllx
   %g1  d# 51			%g1	srlx
   %g4  %g1			%g4	sub
   save-state  h# 5c  miss-fast-path
end-code
