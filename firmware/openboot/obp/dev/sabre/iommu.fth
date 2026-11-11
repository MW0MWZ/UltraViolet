\ Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
\ OpenBoot release it is part of (see LICENSE).
id: @(#)iommu.fth 1.2 26/11/11
purpose: The Sabre's IOMMU and the DMA methods of its children
copyright: Copyright 2026 Andy Taylor

\ The DMA methods the release's sun4v IOMMU code gives (obp/dev/sun4v-devices
\ /iommu), here with the IIi's IOMMU itself (IIi manual 19.3.2): a TSB of
\ 8-byte entries in memory, one per 8 KB page of DVMA space, each valid
\ (bit 63), cacheable (4), writable (1) with its physical page (40:13); the
\ IOMMU's own TLB of 16 entries flushed a page at a time.

h# 12 h# 8000.0000 lxjoin constant tte-valid	\ Valid, cacheable, writable
h# ffff.e000 h# 1ff lxjoin constant tte-pa-mask

\ The TSB is physically contiguous, mapped up to 64 KB at a time
\ (OpenBoot's virtual range is short of large pieces as it starts). The
\ pieces' addresses are kept as 64-bit words (FCode's cells are 32 bits:
\ @ would sign-extend them).
/tsb h# 1.0000 min constant /tsb-piece
/tsb /tsb-piece / constant #tsb-pieces
#tsb-pieces 8 * alloc-mem constant tsb-pieces
0 value tsb-pa

: >tsb-entry ( dvma -- adr )
   virtual-dma-base - low32 and  pageshift rshift 8 *	( offset )
   /tsb-piece /mod  8 * tsb-pieces + x@  +
;

: iommu-flush-page ( dvma -- )
   iommu-flush sabre-x!
   dma-write-sync sabre-x@ drop			\ Wait for it
;

\ The IOMMU's TLB emptied through its diagnostic access (with MMU_DE set
\ and translation off), the TSB allocated and cleared, then translation on.
: init-iommu ( -- )
   0 iommu-control sabre-x!
   2 iommu-control sabre-x!			\ MMU_DE
   d# 16 0 do
      0 i 8 * iommu-tag-diag + sabre-x!
      0 i 8 * iommu-data-diag + sabre-x!
   loop
   0 iommu-control sabre-x!
   /tsb dup phys-claim to tsb-pa
   #tsb-pieces 0 do
      tsb-pa i /tsb-piece * +  /tsb-piece (map-in)	( va )
      dup /tsb-piece erase
      i 8 * tsb-pieces + x!
   loop
   tsb-pa iommu-tsb-base sabre-x!
   tsb-size d# 16 << 1 or iommu-control sabre-x!	\ 8 KB pages, on
;

init-iommu

\ A page's DVMA address is tied to its virtual address, as an Ultra 10's
\ is: the 16 MB from C000.0000 the low 24 bits of OpenBoot's virtual
\ addresses (its memory is in the top 16 MB of its virtual range), so the
\ same page always has the same DVMA address and nothing is allocated.
: >dvma ( va -- dvma )  h# ff.ffff and  virtual-dma-base +  ;

: map-dvma-page ( pa dvma -- )
   swap tte-pa-mask and tte-valid or  over >tsb-entry x!	( dvma )
   iommu-flush-page
;

: unmap-dvma-page ( dvma -- )
   0 over >tsb-entry x!  iommu-flush-page
;

\ A range to map out: in OpenBoot's DVMA, each page mapped.
: dvma-mapped? ( dvma len -- flag )
   over virtual-dma-base u<  if  2drop false exit  then
   2dup +  virtual-dma-base /dvma-used + u>  if  2drop false exit  then
   true -rot  bounds ?do				( flag )
      i low32 and >tsb-entry x@ tte-valid and 0=  if  drop false  then
   pagesize +loop
;

external

: dma-alloc ( size -- vaddr )  memory-alloc  ;
: dma-free  ( vaddr size -- )  memory-free  ;

\ No streaming cache: DMA writes already in the APB complete to memory
\ before a read of the write synchronisation register does.
: dma-sync  ( virt-addr dev-addr size -- )
   3drop  dma-write-sync sabre-x@ drop
;

: dma-map-in ( virt size cache? -- dma-virt )
   drop  over pagesize 1- and +  pagesize round-up	( virt len )
   over page#mask and  swap bounds		( virt end start )
   \ (A loop index is a 32-bit FCode number, sign-extended: as an address
   \ it is taken to its low 32 bits.)
   2dup ?do  i low32 and c@ drop  pagesize +loop	\ Unmapped: a data access trap
   ?do  i low32 and  dup >physical drop  swap >dvma  map-dvma-page
   pagesize +loop
   >dvma						( dma-virt )
;

: dma-map-out ( virt dma-virt size -- )
   rot drop					( dma-virt size )
   over pagesize 1- and +  pagesize round-up	( dma-virt len )
   swap page#mask and swap			( dvma len )
   2dup dvma-mapped?  0=  if
      ." WARNING: attempt to map-out non dma-alloc'd range: "
      base @ >r hex  swap . .  r> base !  cr exit
   then
   bounds ?do  i low32 and unmap-dvma-page  pagesize +loop
;

headerless
