\ Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
\ OpenBoot release it is part of (see LICENSE).
id: @(#)iommu.fth 1.0 26/11/07
purpose: The Sabre's IOMMU and the DMA methods of its children
copyright: Copyright 2026 Andy Taylor

\ The DMA methods the release's sun4v IOMMU code gives (obp/dev/sun4v-devices
\ /iommu), here with the IIi's IOMMU itself (IIi manual 19.3.2): a TSB of
\ 8-byte entries in memory, one per 8 KB page of DVMA space, each valid
\ (bit 63), cacheable (4), writable (1) with its physical page (40:13); the
\ IOMMU's own TLB of 16 entries flushed a page at a time.

h# 12 h# 8000.0000 lxjoin constant tte-valid	\ Valid, cacheable, writable
h# ffff.e000 h# 1ff lxjoin constant tte-pa-mask

\ The TSB is physically contiguous (1 MB), mapped a 64 KB piece at a time
\ (OpenBoot's virtual range is short of large pieces as it starts). The
\ pieces' addresses are kept as 64-bit words (FCode's cells are 32 bits:
\ @ would sign-extend them).
h# 1.0000 constant /tsb-piece
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
   /tsb pagesize phys-claim to tsb-pa
   #tsb-pieces 0 do
      tsb-pa i /tsb-piece * +  /tsb-piece (map-in)	( va )
      dup /tsb-piece erase
      i 8 * tsb-pieces + x!
   loop
   tsb-pa iommu-tsb-base sabre-x!
   tsb-size d# 16 << 1 or iommu-control sabre-x!	\ 8 KB pages, on
;

init-iommu

variable dma-list	dma-list off
virtual-dma-base virtual-dma-size dma-list free-memrange

: map-dvma-page ( pa dvma -- )
   swap tte-pa-mask and tte-valid or  over >tsb-entry x!	( dvma )
   iommu-flush-page
;

: unmap-dvma-page ( dvma -- )
   0 over >tsb-entry x!  iommu-flush-page
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
   drop						( virt size )
   over pagesize 1- and  tuck +			( virt offset size' )
   pagesize round-up >r				( virt offset ) ( r: len )
   pagesize r@ dma-list allocate-memrange	( virt offset dvma false | .. true )
   if  cmn-error[ " Out of DVMA space" ]cmn-end abort  then	( virt offset dvma )
   rot page#mask and  r>  0 ?do			( offset dvma va )
      dup >physical drop  2 pick i +  map-dvma-page
      pagesize +
   pagesize +loop  drop				( offset dvma )
   +						( dma-virt )
;

: dma-map-out ( virt dma-virt size -- )
   rot drop					( dma-virt size )
   over pagesize 1- and +  pagesize round-up	( dma-virt len )
   swap page#mask and swap			( dvma len )
   2dup bounds ?do  i unmap-dvma-page  pagesize +loop
   dma-list free-memrange
;

headerless
