\ Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
\ OpenBoot release it is part of (see LICENSE).
id: @(#)tlb.fth 1.0 26/11/07
purpose: TLB access for the UltraSPARC I/II/IIi (sun4u)
copyright: Copyright 2026 Andy Taylor

\ The words cpu/sparc/ultra4v/tlb.fth gives for sun4v, done through the
\ processor's own MMU registers: the tag access register (VA 30 of the
\ IMMU's and DMMU's ASIs, 50 and 58), the data-in registers (54, 5c) that
\ load an entry where the TLB chooses, the data access registers (55, 5d)
\ that load one at an index, and the demap ASIs (57, 5f). Each TLB has 64
\ entries.

headerless
h# 50 constant immu-asi
h# 58 constant dmmu-asi
h# 30 constant tag-access

: va>va,ctx ( vadr -- vadr ctx# )
   dup page#mask and swap  pagemask and
;

\ Demap a page in the primary context (OpenBoot's own: context 0).
code demap-itlb  ( addr -- )
   page#mask  sc2  sc1  setx
   tos  sc1  tos  and
   %g0  tos  %g0  h# 57  stxa
   #Sync  membar
   tos  %g0  iflush
   sp  tos  pop
c;
code demap-dtlb  ( addr -- )
   page#mask  sc2  sc1  setx
   tos  sc1  tos  and
   %g0  tos  %g0  h# 5f  stxa
   #Sync  membar
   sp  tos  pop
c;
: demap-tlb  ( addr -- )  dup demap-itlb demap-dtlb  ;

: (.tte-soft1) ( tte -- )  7 rshift h# 3f and .x  ;
: (.tte-soft2) ( tte -- )  d# 50 rshift h# 1ff and .x  ;

\ An entry loaded where the TLB chooses (tag access, then data-in).
code itlb-tar-dir! ( tte vadr -- )
   %g0  h# 30  sc1  add
   tos  sc1  %g0  h# 50  stxa		\ tag access: VA and context
   #Sync  membar
   sp  tos  pop
   tos  %g0  %g0  h# 54  stxa		\ data in
   #Sync  membar
   sp  tos  pop
c;
code dtlb-tar-dir! ( tte vadr -- )
   %g0  h# 30  sc1  add
   tos  sc1  %g0  h# 58  stxa
   #Sync  membar
   sp  tos  pop
   tos  %g0  %g0  h# 5c  stxa
   #Sync  membar
   sp  tos  pop
c;

\ An entry loaded at an index (tag access, then data access).
code itlb-tar-data! ( tte index tlb# vadr -- )
   %g0  h# 30  sc1  add
   tos  sc1  %g0  h# 50  stxa		\ tag access
   #Sync  membar
   sp  1 /n*  sc2  nget			\ index
   sc2  3  sc2  sllx
   sp  2 /n*  sc3  nget			\ tte
   sc3  sc2  %g0  h# 55  stxa		\ data access
   #Sync  membar
   sp  3 /n*  sp  add
   sp  tos  pop
c;
code dtlb-tar-data! ( tte index tlb# vadr -- )
   %g0  h# 30  sc1  add
   tos  sc1  %g0  h# 58  stxa
   #Sync  membar
   sp  1 /n*  sc2  nget
   sc2  3  sc2  sllx
   sp  2 /n*  sc3  nget
   sc3  sc2  %g0  h# 5d  stxa
   #Sync  membar
   sp  3 /n*  sp  add
   sp  tos  pop
c;

\ An entry read back: its data and its tag.
: itlb-data@ ( index -- tte )  3 lshift h# 55 spacex@  ;
: dtlb-data@ ( index -- tte )  3 lshift h# 5d spacex@  ;
: itlb-tag@  ( index -- tag )  3 lshift h# 56 spacex@  ;
: dtlb-tag@  ( index -- tag )  3 lshift h# 5e spacex@  ;

headers
defer .soft1 ( tte -- ) ' (.tte-soft1) is .soft1
defer .soft2 ( tte -- ) ' (.tte-soft2) is .soft2

: flush-tlb-page    ( vadr -- )
   pagesize round-down  demap-tlb
;

: flush-tlb-range ( vadr size -- )
   over +  pagesize round-up  swap pagesize round-down
   ?do  i demap-tlb  pagesize +loop
;

\ Demap the primary context, in both TLBs (type 1: context; locked
\ entries stay).
: flush-tlb-context ( -- )
   0 h# 40 h# 57 spacex!  0 h# 40 h# 5f spacex!
;

: .tlb ( tlbdata -- )
   >r r@
   ." G:"          1bits .
   ." W:"          1bits .
   ." P:"          1bits .
   ." E:"          1bits .
   ." CV:"         1bits .
   ." CP:"         1bits .
   ." L:"          1bits .
   ." Soft:"       6 bits drop r@ .soft1
   ." PA[40:13]:"  d# 28 bits dup .x
   ." PA:"         pageshift lshift .x cr
   ( diag )        9 bits drop
   ." Soft2:"      9 bits drop r@ .soft2
   ." IE:"         1bits .
   ." NFO:"        1bits .
   ." Size:"       2 bits .x
   ." V:"          1bits .
   r> 2drop
;

headerless
: is-ultra3+? ( ver -- flag )  drop false  ;
: is-ultra4? ( ver -- flag )  drop false  ;
: is-dtlb#3? ( ver -- flag )  drop false  ;
: is-cmp? ( ver -- flag)  drop false  ;
: #dtlb-entries ( tlb# -- #tlb-entries )  drop d# 64  ;
: #itlb-entries ( tlb# -- #tlb-entries )  drop d# 64  ;
headers
