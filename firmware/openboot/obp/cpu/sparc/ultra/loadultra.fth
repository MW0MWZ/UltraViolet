\ Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
\ OpenBoot release it is part of (see LICENSE).
id: @(#)loadultra.fth 1.0 26/11/07
purpose: The UltraSPARC I/II/IIi (sun4u) processor support
copyright: Copyright 2026 Andy Taylor

\ As cpu/sparc/ultra4v/loadultra.fth loads sun4v's, without a hypervisor.

[ifndef] assembler?  transient  [then]
fload ${BP}/cpu/sparc/ultra/implasm.fth
[ifndef] assembler?  resident  [then]

headers
fload ${BP}/cpu/sparc/ultra/impldis.fth
fload ${BP}/cpu/sparc/ultra/map.fth
fload ${BP}/cpu/sparc/ultra/mmuregs.fth
fload ${BP}/cpu/sparc/ultra/tte.fth
fload ${BP}/cpu/sparc/ultra/tlb.fth

alias cache-off   noop immediate
alias cache-on    noop immediate
alias clear-cache noop immediate
alias flush-cache-page drop

fload ${BP}/cpu/sparc/ultra/mmu.fth
fload ${BP}/cpu/sparc/ultra/tte-lookup.fth

headerless
