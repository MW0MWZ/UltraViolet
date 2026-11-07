\ Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
\ OpenBoot release it is part of (see LICENSE).
id: @(#)mapdi.fth 1.0 26/11/07
purpose: Mapping the flash PROM's drop-ins
copyright: Copyright 2026 Andy Taylor

\ The drop-ins are in the flash PROM (1 MiB at 1ff.f000.0000, the RIO
\ EBus's BAR0); arch/sun4v/mapdi.fth's words map it while they are read.

headerless
h# 1ff.f000.0000 constant prom-pa
: map-drop-in   ( -- va )  prom-pa 0 ROMsize memmap  ;
: unmap-drop-in ( va -- )  ROMsize 2dup mmu-unmap mmu-release  ;
headers
