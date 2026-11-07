\ Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
\ OpenBoot release it is part of (see LICENSE).
id: @(#)map.fth 1.0 26/11/07
purpose: Address-space parameters of the UltraSPARC I/II/IIi (sun4u)
copyright: Copyright 2026 Andy Taylor

\ As cpu/sparc/ultra4v/map.fth gives them for sun4v, for the sun4u
\ processors: 44 bits of virtual address, 41 of physical (UltraSPARC IIi).

headers
d# 44 constant #vabits
d# 41 constant #pabits

h# 14 constant memory-asi	\ ASI_PHYS_USE_EC
h# 15 constant bypass-asi	\ ASI_PHYS_BYPASS_EC_WITH_EBIT
h# 1d constant bypass-le-asi	\ ASI_PHYS_BYPASS_EC_WITH_EBIT_LITTLE

h# 0d			constant pageshift     \ 8K
1 pageshift lshift	constant pagesize
pagesize 1-		constant pagemask
pagesize negate		constant page#mask

headerless
h# 12			constant segmentshift     \ 256K
1 segmentshift lshift	constant segmentsize
segmentsize 1-		constant segmentmask
segmentsize negate	constant segment#mask
headers
