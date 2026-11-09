\ Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
\ OpenBoot release it is part of (see LICENSE).
id: @(#)ramforth.fth 1.0 26/11/07
purpose: OpenBoot's memory, and leaving the reset code's TLB entries
copyright: Copyright 2026 Andy Taylor

\ What arch/sun4v/ramforth.fth does for sun4v, on the Ultra 5/10: the reset
\ code has put OpenBoot in the 1 MiB at the top of memory (mem-base on, as
\ an Ultra 10 has it) and mapped it with locked TLB entries (the image, in
\ the top 512 KiB, at monvirtbase; its RAM, the 64 KiB below, at RAMbase;
\ and itself at 0). Here OpenBoot's page tables take over those
\ mappings, and the reset code's entries are taken out, so that the MMU
\ miss handlers map the rest from then on.

headers
false value forth-in-ram?
: (romforth) ( -- )  ; immediate

: mem-base ( -- pa.lo pa.hi )
   hi-memory-base @  pageshift lshift ( base )
   /obp-memory round-down  obmem      ( pa.lo pa.hi )
;
: ROMphys ( -- pa.lo pa.hi )  mem-base >r  /obp-image + r>  ;
: RAMphys ( -- pa.lo pa.hi )  ROMphys >r  RAMsize-start - r>  ;

headerless
\ A locked entry for a virtual address taken out of a TLB: each entry whose
\ tag's VA matches is invalidated (demap leaves locked entries).
: (unlock-tlb ( va tag@ data! -- )
   rot page#mask and  d# 64 0 do			( tag@ data! va )
      i 3 pick execute  page#mask and  over =  if	( tag@ data! va )
         0 i 3 pick execute				( tag@ data! va )
      then						( tag@ data! va )
   loop  3drop						(  )
;
: itlb-data! ( tte index -- )  3 lshift h# 55 spacex!  ;
: dtlb-data! ( tte index -- )  3 lshift h# 5d spacex!  ;
: (flush-tlb-entry) ( va i&d? -- )
   if  dup ['] itlb-tag@ ['] itlb-data! (unlock-tlb  then
   ['] dtlb-tag@ ['] dtlb-data! (unlock-tlb
;

\ The reset code's own entry (0, in both TLBs) and its entry for RAMbase
\ are taken out; the image's 512 KiB at monvirtbase stays locked in both, as
\ sun4u needs it to (the trap table and the MMU miss handlers are there,
\ and run when nothing else is mapped).
: flush-temporary-mappings ( -- )
   0 true (flush-tlb-entry)
   RAMbase false (flush-tlb-entry)
;

headers
: (cacheforth)  ( -- )
   true to forth-in-ram?
   \ The page tables' root, at the top of OpenBoot's RAM.
   RAMphys drop pagesize - setup-root-vpt
   \ Their entries for RAMbase, RAMsize and the image (512 KiB).
   RAMphys RAMbase RAMsize-start map-pages
   ROMphys monvirtbase /obp-image map-pages
   enable-map-flushing
   flush-temporary-mappings
;
: cacheforth ( -- )
   forth-in-ram?  if  exit  then
   (cacheforth)
;

headerless
stand-init: Copying ROM to RAM
   cacheforth
;

fload ${BP}/arch/sun/dynamic-user.fth	\ S
headers
