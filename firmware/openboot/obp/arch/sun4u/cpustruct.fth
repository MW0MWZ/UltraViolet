\ ========== Copyright Header Begin ==========================================
\ 
\ Hypervisor Software File: cpustruct.fth
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
\ Adapted for the UltraSPARC IIi (sun4u) by Andy Taylor, 2026: the CPU
\ structure's physical address, which sun4v keeps in a scratchpad
\ register, is patched into the trap code that needs it (cpu-struct-pa);
\ the hypervisor's mondo queue and TSB set-up are left out; a processor
\ node's number is its UPA port ID.
id: @(#)cpustruct.fth 1.1 06/02/22 
purpose: 
copyright: Copyright 2006 Sun Microsystems, Inc.  All Rights Reserved
copyright: Use is subject to license terms.

hex
headers

fload ${BP}/cpu/sparc/mutex.fth

mutex-create prom-lock

label cif-owner  -1 l,  end-code
label release-slaves? 0 ,  end-code

0 value mp-cpu-state

transient

window-registers h# 10 round-up is window-registers
window-registers d# 24 /x * + constant /min-cpu-save
/min-cpu-save #windows 1- d# 16 * /x * + constant /full-cpu-save

h# 800		constant /cpu-state
d# 64 2* /x *	constant /fth-exception-stack
/min-cpu-save /fth-exception-stack + constant /min-exec-state

/full-cpu-save /cpu-state >
abort" CPU STRUCT ISNT LARGE ENOUGH TO HOLD A STATE SAVE"

resident
headers

struct
   /cpu-state			field >cpu-state
   h# 800 round-up
   ps-size			field >data-stack
   h# 800 round-up
   rs-size			field >return-stack
   h# 800 round-up
   /min-exec-state		field >immu-miss-state
   /min-exec-state		field >dmmu-miss-state
   /min-exec-state		field >fpu-int-state
   /min-exec-state		field >intr-state
   /n				field >mmu-defer
   /n				field >debugger-hook
   /n				field >cpu-status
   /n				field >cpu-node
   /n				field >cpu-rp0
   /n				field >cpu-sp0
   /n				field >cpu-rp0-fence
   /n				field >cpu-sp0-fence
   /n				field >stack-fence?
   /n                           field >guarded-pc       \  used by
   /n                           field >guarded-ip       \  guarded-execute
   ua-size			field >user-save
   8 /x*			field >cpu-tsb-ctrl-area
   /n				field >cpu-mondo-qsize
   /n				field >cpu-mondo-queue
   /n				field >cpu-devmondo-ptr
   /n				field >cpu-version-reg	\ sun4u: %ver
   h# 40 round-up
   0				field >cpu-dynamic-data
      \ CPU mondo queue
      \ TSB CTX0 save area
      \ TSB Non-0 save area
   pagesize round-up		\ Round up to pagesize
constant /cpu-struct

\ 
\ WATCHOUT.. these will adjust what the register display words show!
\ and NOT ALL that they show will be valid.
\ 
\ If you shift the cpu-reg-offset
\ be sure to put it back using select-cpu-state
\ 
: select-cpu-state  ( -- ) 0 is cpu-reg-offset ;
: select-immu-state ( -- ) 0 >immu-miss-state is cpu-reg-offset ;
: select-dmmu-state ( -- ) 0 >dmmu-miss-state is cpu-reg-offset ;
: select-intr-state ( -- ) 0 >intr-state is cpu-reg-offset ;

/cpu-struct 0 >cpu-dynamic-data - d# 400 <
abort" CPU dynamic region is getting small"

: >cpu-struct ( n -- addr )	/cpu-struct * mp-cpu-state + ;
: mid-ok? ( n -- ok? )		max-#cpus < ;

\ compute the offsets
struct
   /x		field	>tsb-allocation
   /x		field	>tsb-saved-size
   /x		field	>tsb-buffer-addr
constant /tsb-data

: (cpu-state ( -- adr )		mid@ >cpu-struct  ;
: >cpu-status! ( n -- )		(cpu-state >cpu-status !  ;
: mid-present? ( n -- present? ) 
   dup mid@ = if 
      drop true 
   else 
      >cpu-struct >cpu-status @ 
   then 
;

headerless

' (cpu-state is cpu-state

headerless

4 actions
action:  ( apf -- ??? )		aligned l@ (cpu-state + token@ execute  ;
action:  ( xt apf -- )		aligned l@ (cpu-state + token!  ;
action:  ( apf -- addr )	aligned l@ (cpu-state + ;
action:  ( xt apf -- )
   max-mondo-target# 0  ?do
      i mid-ok?  if
         2dup aligned l@ i >cpu-struct + token!
      then
   loop  2drop
;

headers

: per-cpu-defer: ( offset -- ) \ name
   create  align  l,  use-actions
;

inline-struct? on
h# 04 constant CPU-INIT		\ Starting INIT
h# 10 constant CPU-OBP-COLD	\ exec OBP, never started
h# 11 constant CPU-IDLING	\ prom idle, never started
h# 20 constant CPU-PARKED	\ Parked by mondo (bp)
h# 21 constant CPU-ENTERFORTH	\ Waiting to enter FORTH
h# 22 constant CPU-WAIT-RESTART	\ Waiting to restart
h# 30 constant CPU-OBP-WARM	\ CPU is running OBP (started)
h# 31 constant CPU-PROM-CIF	\ CPU is in the CIF
h# 32 constant CPU-ACCUMULATING	\ Waiting for cpus to arrive
h# 33 constant CPU-RELEASED	\ restarted.
h# 40 constant CPU-STARTED	\ cpu in client.
inline-struct? off

headers transient				\ transient

also assembler definitions			\ Assembler transient
hex

: get-cpu-struct ( up scr reg -- )
   >r r@			  get-mid  ( up scr )
   /cpu-struct over		  set	   ( up scr )	\ scr = /cpu-struct
   r@ over r@			  mulx	   ( up scr )	\ reg = offset
   >r ['] mp-cpu-state >user#  r@ set	   ( up )	\ scr = user#
   r@ r@			  nget	   ( )		\ scr = cpu-struct-base
   r> r@ r>			  add	   ( )
;  

: get-rp0 ( cpu-struct-ptr reg -- )
   0 >return-stack rs-size + over	set
   dup					add
;

: set-rp0 ( cpu-struct-ptr scr sc1 -- )
   >r 2dup				get-rp0
   0 >cpu-rp0 r@			set
   swap r>				stx
;

: get-sp0 ( cpu-struct-ptr reg -- )
   0 >data-stack ps-size + over		set
   dup					add
;

: set-sp0 ( cpu-struct-ptr scr sc1 -- )
   >r 2dup				get-sp0
   0 >cpu-sp0 r@			set
   swap r>				stx
;

: mark-cpu-state ( val scr sc1 state-ptr -- )
   >r >r						( val scr )
   tuck					set		( scr )
   r> 0 >cpu-status		over	set		( scr sc1 )
   r>					stx		(  )
;

resident previous definitions

\ The CPU structure's physical address, for trap code that runs before
\ (or without) OpenBoot's mappings: each place that needs it holds a
\ constant, seven instructions (or, sllx, or, sllx, or, sllx, or) whose
\ immediates are patched once the structure is allocated.
headerless
d# 64 constant #cpu-pa-sites
create cpu-pa-sites  #cpu-pa-sites /l* allot
variable #cpu-pa-site  0 #cpu-pa-site !

transient
\ (In the Forth vocabulary: in the assembler's, < and or are a condition
\ and an instruction.)
: note-cpu-pa-site  ( -- )
   #cpu-pa-site @ #cpu-pa-sites < 0= abort" Too many cpu-struct-pa sites"
   here origin-  cpu-pa-sites #cpu-pa-site @ la+ l!
   1 #cpu-pa-site +!
;
also assembler definitions
: cpu-struct-pa  ( reg -- )
   note-cpu-pa-site
   >r
   %g0 0 r@ or     r@ d# 12 r@ sllx
   r@  0 r@ or     r@ d# 10 r@ sllx
   r@  0 r@ or     r@ d# 10 r@ sllx
   r@  0 r@ or
   r> drop
;
resident  previous definitions

headerless
code iflush-word  ( adr -- )
   tos  %g0  iflush
   sp  tos  pop
c;
: patch-imm13  ( imm adr -- )
   dup l@ h# 1fff invert and  rot h# 1fff and or  over l!  iflush-word
;
: set-cpu-struct-pa  ( pa -- )
   dup d# 32 rshift          >r	\ hi (9 bits)
   dup d# 20 rshift h# fff and  >r
   dup d# 10 rshift h# 3ff and  >r
   h# 3ff and			( lo10 )  ( r: hi mid12 mid10 )
   r> r> r>			( lo10 mid10 mid12 hi )
   #cpu-pa-site @ 0 ?do
      cpu-pa-sites i la+ l@ origin+	( lo10 mid10 mid12 hi site )
      >r
      3 pick  r@ d# 24 + patch-imm13
      2 pick  r@ d# 16 + patch-imm13
      over    r@    8 + patch-imm13
      dup     r>        patch-imm13
   loop
   2drop 2drop
;

headerless
: init-cpu-state  ( -- )
   cpu-state >data-stack to pssave	(  )
   cpu-state >return-stack to rssave	(  )
   cpu-state >cpu-status @ 		( n )
   CPU-OBP-WARM tuck <	if		( s )
      drop  CPU-OBP-COLD		( s )
   then >cpu-status!			(  )
   0w					(  )
;

: use-fence? ( -- ? )	(cpu-state dup >stack-fence? @ state-valid @ 0 < and  ;
: cpu-rp0 ( -- n )	use-fence?  if  >cpu-rp0-fence  else  >cpu-rp0  then  ;
: cpu-sp0 ( -- n )	use-fence?  if  >cpu-sp0-fence  else  >cpu-sp0  then  ;

headerless
\
\ WATCHOUT!!
\ we come in on the (temporary) global stack and return on the per-cpu
\ ones.
\
code switch-to-private-stacks
   up   sc1		scr	get-cpu-struct
   CPU-OBP-COLD sc1 sc2	scr	mark-cpu-state
   scr  sc1 sc2			set-rp0
   scr	sc1 sc2			set-sp0

   'user .rp0		sc1	nget
   rp			sc2	move		\ Src
   scr			sc4	get-rp0
   sc1		sc2	sc3	subcc		\ depth
   0> if
      sc4	sc3	rp	sub		\ adjust stack ptr.
      sc4	sc3	sc4	sub
      begin
         sc2	%g0	sc5	ldx
         sc5	sc4	%g0	stx
         sc4	/n	sc4	add
         sc2	/n	sc2	add
         sc3	/n	sc3	subcc
      0=  until  nop
   then

   'user .sp0		sc1	nget
   sp			sc2	move		\ Src
   scr			sc4	get-sp0
   sc1		sc2	sc3	subcc		\ depth
   0> if
      sc4	sc3	sp	sub		\ adjust stack ptr.
      sc4	sc3	sc4	sub
      sc2	%g0	sc5	ldx
      begin
         sc5	sc4	%g0	stx
         sc4	/n	sc4	add
         sc2	/n	sc2	add
         sc3	/n	sc3	subcc
      0<  until
         sc2	%g0	sc5	ldx
   then
c;

stand-init: allocate cpu structs
   h# f008.0000 dup to mp-cpu-state
   >physical drop set-cpu-struct-pa
   ['] cpu-rp0  to  rp0
   ['] cpu-sp0  to  sp0
   switch-to-private-stacks
   CPU-OBP-COLD >cpu-status!
   -1 release-slaves? l!
;

: init-per-cpu-data ( -- )
   \ The address of the cpu struct, for the trap code.
   cpu-state >physical drop set-cpu-struct-pa
;

\ This routine will iterate over each cpu present in the system,
\ calling the acf with the mid on the top of the stack. The routine could
\ be called with additional args underneath the acf - the subroutine would
\ have to ensure the stack remained intact though.
: do-foreach-cpu ( acf -- )
   max-mondo-target# 0 ?do			( acf )
      i mid-ok? if				( acf )
         i mid-present? if			( acf )
            i swap >r r@ catch if drop  then	( )
            r>					( acf )
         then					( acf )
      then					( )
   loop  drop					( )
;

: (.cpu-state) ( mid -- )
   dup >cpu-struct >cpu-status @		( mid status )
   ." CPU: " over .x 
   ." Node: " swap >cpu-struct >cpu-node @ .x 
   case
      0                 of ." Not Present (offline)" endof
      CPU-INIT          of ." init completed" endof
      CPU-IDLING        of ." idling (not started)" endof
      CPU-OBP-COLD      of ." running OBP (not started)" endof
      CPU-PARKED        of ." parked" endof
      CPU-ENTERFORTH    of ." waiting to enterforth" endof
      CPU-WAIT-RESTART  of ." waiting to restart" endof
      CPU-OBP-WARM      of ." executing OBP (started)." endof
      CPU-PROM-CIF      of ." executing in the CIF" endof
      CPU-ACCUMULATING  of ." awaiting total release" endof
      CPU-RELEASED      of ." released to reenter client" endof
      CPU-STARTED       of ." cpu in client" endof
      ." Unknown! " dup .x
   endcase cr
;

headers

: .cpu-state ( -- )	['] (.cpu-state) do-foreach-cpu  ;

also magic-device-types definitions
: cpu ( ?? -- ?? )
   " reg" get-property if
[ifndef] RELEASE
      cmn-fatal[  " CPU node is missing reg property"  ]cmn-end
[then]
      exit
   then
   decode-int nip nip
   \ sun4u: the UPA port ID, from the port's address (1c0 + 2 x port ID)
   1 rshift h# 1f and				( cpu# )
   >cpu-struct >cpu-node			( adr' )
   current-device swap !			( )
;
previous definitions

headerless
