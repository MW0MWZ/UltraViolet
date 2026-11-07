\ Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
\ OpenBoot release it is part of (see LICENSE).
id: @(#)fastfill.fth 1.0 26/11/07
purpose: Touching and clearing physical memory
copyright: Copyright 2026 Andy Taylor

\ arch/sun4v/fastfill.fth's words, with memory cleared by the processor's
\ own stores (sun4v asks the hypervisor to scrub it).

headers
code touch-memory ( pa.lo pa.hi size -- )
   sp				sc1	pop		\ pa.hi in sc1
   sp				scr	pop		\ pa.lo in scr
   begin
      tos  /x			tos	subcc
      scr  tos memory-asi	sc2	ldxa
   0= until
      sc2  scr tos memory-asi		stxa
   sp  tos				pop
   #Sync				membar
c;

headerless
code (clear-memory ( pa.lo pa.hi size -- )
   sp				sc1	pop
   sp				scr	pop
   tos  h# 40			tos	subcc
   %g0  memory-asi			wrasi
   begin
      scr  tos			sc1	add
      %g0  sc1 h#  0 %asi		stxa
      %g0  sc1 h#  8 %asi		stxa
      %g0  sc1 h# 10 %asi		stxa
      %g0  sc1 h# 18 %asi		stxa
      %g0  sc1 h# 20 %asi		stxa
      %g0  sc1 h# 28 %asi		stxa
      %g0  sc1 h# 30 %asi		stxa
      %g0  sc1 h# 38 %asi		stxa
   0<= until
      tos  h# 40		tos	subcc
  sp				tos	pop
c;

headers
defer clear-memory
alias bclear-memory-4MB clear-memory
' (clear-memory  to clear-memory
' (clear-memory  to clear-mem
headerless
