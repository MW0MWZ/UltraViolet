\ Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
\ OpenBoot release it is part of (see LICENSE).
id: @(#)regs.fth 1.0 26/11/07
purpose: The Happy Meal's registers, its DMA memory and its MII interface
copyright: Copyright 2026 Andy Taylor

\ One memory BAR (offset 10) of 32-bit little-endian registers: the global
\ ones (SEB) at 0, transmit DMA (ETX) at 2000, receive DMA (ERX) at 4000,
\ the MAC (BigMAC) at 6000, the MII interface (MIF) at 7000. (The PCI
\ probe has rl@ and rl! read and write PCI's byte order, as these drivers
\ are loaded.)

headerless

0 instance value reg-base
0 instance value cpu-dma-base		\ The DMA block, as the CPU sees it
0 instance value io-dma-base		\ and as the device does
h# 1.0000 constant /dma-blk

: map-in  ( phys.lo phys.mid phys.hi size -- va )  " map-in" $call-parent  ;
: map-out ( va size -- )  " map-out" $call-parent  ;
: dma-alloc ( size -- va )  " dma-alloc" $call-parent  ;
: dma-free ( va size -- )  " dma-free" $call-parent  ;
: dma-map-in ( va n cache? -- devadr )  " dma-map-in" $call-parent  ;
: dma-map-out ( va devadr n -- )  " dma-map-out" $call-parent  ;

: reg@ ( offset -- l )  reg-base + rl@  ;
: reg! ( l offset -- )  reg-base + rl!  ;

\ Global
h# 0000 constant seb-reset	h# 0004 constant seb-cfg
h# 0100 constant seb-stat	h# 0104 constant seb-imask
\ ETX
h# 2000 constant etx-pending	h# 2004 constant etx-cfg
h# 2008 constant etx-ring	h# 202c constant etx-rsize
\ ERX
h# 4000 constant erx-cfg	h# 4004 constant erx-ring
\ BigMAC
h# 6000 constant bmac-xif	h# 6208 constant bmac-tx-reset
h# 620c constant bmac-tx-cfg	h# 6230 constant bmac-tx-max
h# 6308 constant bmac-rx-reset	h# 630c constant bmac-rx-cfg
h# 6310 constant bmac-rx-max	h# 6318 constant bmac-madd2
h# 631c constant bmac-madd1	h# 6320 constant bmac-madd0
h# 6328 constant bmac-len-errs	h# 632c constant bmac-align-errs
h# 6330 constant bmac-crc-errs
h# 6340 constant bmac-hash3	\ (hash2 344, hash1 348, hash0 34c)
\ MIF
h# 700c constant mif-frame	h# 7010 constant mif-cfg
h# 7014 constant mif-imask

: map-regs ( -- )
   0 0 my-space h# 0200.0010 or  h# 8000 map-in to reg-base
   4 my-w@ h# 146 or 4 my-w!		\ Memory, master, parity, SERR#
;
: unmap-regs ( -- )
   reg-base h# 8000 map-out  0 to reg-base
   4 my-w@ 6 invert and 4 my-w!
;

: map-buffers ( -- )
   /dma-blk dma-alloc to cpu-dma-base
   cpu-dma-base /dma-blk false dma-map-in to io-dma-base
;
: unmap-buffers ( -- )
   cpu-dma-base io-dma-base /dma-blk dma-map-out
   cpu-dma-base /dma-blk dma-free
   0 to cpu-dma-base  0 to io-dma-base
;

: cpu>io ( va -- devadr )  cpu-dma-base - io-dma-base +  ;

\ Descriptors are two little-endian words in memory.
: le-l@ ( adr -- l )  l@ lbflip  ;
: le-l! ( l adr -- )  >r lbflip r> l!  ;

\ The MII: management frames through the MIF's frame register, to the
\ internal PHY (a DP83840) at address 1 on MDIO0; a frame is done when
\ its bit 16 (the turnaround's) reads 1.
1 instance value phy-adr

: mif-wait ( -- ok? )
   d# 1000 0 do  mif-frame reg@ h# 1.0000 and if  unloop true exit  then  loop
   false
;
: phy-frame ( op reg -- frame )
   d# 18 lshift  phy-adr d# 23 lshift or  or  h# 2.0000 or
;
: phy@ ( reg -- data )
   h# 6000.0000 swap phy-frame mif-frame reg!
   mif-wait drop  mif-frame reg@ h# ffff and
;
: phy! ( data reg -- )
   h# 5000.0000 swap phy-frame  swap h# ffff and or  mif-frame reg!
   mif-wait drop
;

\ (10 and 100 Mbit/s only: no gigabit PHY registers)
: gmii-phy? ( -- false )  false  ;
