\ Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
\ OpenBoot release it is part of (see LICENSE).
id: @(#)obd-pci.fth 1.0 26/11/14
purpose: obdiag's tests of the RIO: its EBus's PCI header and DMA, its Ethernet
copyright: Copyright 2026 Andy Taylor

headerless

\ --- PCI/Cheerio (all_pci/cheerio_test), in the EBus node ------------------
\
\ Configuration accesses to the EBus function; a failure's address is the
\ configuration address used.

: cfg-adr ( offset -- adr )  my-space +  ;

: vendor-id-test ( -- )
   " vendor_id_test" subtest  " e2-v-id" msg
   0 cfg-adr  h# 108e  0 cfg-w@  ?fail-at
;
: device-id-test ( -- )
   " device_id_test" subtest  " e2-dev-id" msg
   2 cfg-adr  h# 1000  2 cfg-w@  ?fail-at
;
\ Each byte and half of the first word, read narrow, as the word read whole
\ has it
: mixmode-read ( -- )
   " mixmode_read" subtest  " pci config" msg
   0 cfg-l@					( l )
   4 0 do
      i cfg-adr  over i 8 * rshift h# ff and  i cfg-b@  ?fail-at
   loop
   0 cfg-adr  over h# ffff and  0 cfg-w@  ?fail-at
   2 cfg-adr  swap d# 16 rshift h# ffff and  2 cfg-w@  ?fail-at
;
: e2-class-test ( -- )
   " e2_class_test" subtest  " e2-class" msg
   9 cfg-adr  0  9 cfg-b@  ?fail-at
   h# a cfg-adr  h# 80  h# a cfg-b@  ?fail-at
   h# b cfg-adr  6  h# b cfg-b@  ?fail-at
;
\ The status register: all its error bits cleared (written with ones), it
\ reads fast back-to-back capable and medium DEVSEL alone
: status-reg-walk1 ( -- )
   " status_reg_walk1" subtest  " e2-stat" msg
   h# 3ff 6 cfg-w!
   6 cfg-adr  h# 280  6 cfg-w@  ?fail-at
;
\ A byte register's bits 0-6 walked, then the register 0. (These three set
\ no message of their own: the status test's is in force.)
: walk7 ( offset -- )
   7 0 do
      dup cfg-adr  1 i lshift			( offset adr v )
      dup 3 pick cfg-b!
      2 pick cfg-b@ h# 7f and  ?fail-at
   loop
   0 swap cfg-b!
;
: line-size-walk1 ( -- )  " line_size_walk1" subtest  h# c walk7  ;
: latency-walk1 ( -- )  " latency_walk1" subtest  h# d walk7  ;
: line-walk1 ( -- )  " line_walk1" subtest  h# 3c walk7  ;
\ The interrupt pin: A (1) on the first revision (revision-id's low four
\ bits 0), none (0) after
: pin-test ( -- )
   " pin_test" subtest  " e2-int-pin" msg
   " revision-id" get-my-property if
      " Could not find revision-id property" fail-msg
   then  decode-int nip nip			( rev )
   h# f and 0= if  1  else  0  then		( exp )
   h# 3d cfg-adr  swap  h# 3d cfg-b@  ?fail-at
;

\ (The test as an Ultra 10's obdiag opens the EBus node: the latency timer
\ to 28.)
: cheerio-open ( -- )  h# 28 h# d cfg-b!  ;

\ The EBus's cache line size, latency timer and interrupt line are left
\ 0, as an Ultra 10's test leaves them.
: (cheerio-test) ( -- )
   cheerio-open
   vendor-id-test  device-id-test  mixmode-read  e2-class-test
   status-reg-walk1  line-size-walk1  latency-walk1  line-walk1  pin-test
;
: cheerio-test ( -- )  ebus-path ['] (cheerio-test) with-node  ;

\ The sub-test menu's entries: one sub-test each
0 value cheerio-sub
: (cheerio-one) ( -- )  cheerio-open  cheerio-sub execute  ;
: cheerio-one ( -- )  ebus-path ['] (cheerio-one) with-node  ;

\ --- EBUS DMA/TCR Registers (all_dma/ebus_test), in the EBus node --------
\
\ The four EBus DMA channels (at 70.0000, 70.2000, 70.4000, 70.6000 in
\ the second window): their registers walked and their states, then each
\ moving 128 bytes through its FIFO in its diagnostic mode.

0 value dma-regs			\ The four channels, mapped
0 value dbuf-va   0 value dbuf-io	\ Two 8 KB DVMA buffers: send at 0,
h# 4000 constant /dbufs		\ receive at 2000
: send-buf ( -- va )  dbuf-va  ;
: recv-buf ( -- va )  dbuf-va h# 2000 +  ;

\ (In the EBus node's own instance: its map-in.)
: ebus-self-map ( offset size -- va )  ebus-space swap " map-in" my-self $call-method  ;
: ebus-self-unmap ( va size -- )  " map-out" my-self $call-method  ;

: dma-open ( -- )
   cheerio-open
   h# 70.0000 h# 8000 ebus-self-map to dma-regs
   /dbufs dvma-alloc to dbuf-io to dbuf-va
;
: dma-close ( -- )
   dma-regs ?dup if  h# 8000 ebus-self-unmap  0 to dma-regs  then
   dbuf-va ?dup if  dbuf-io /dbufs dvma-free  0 to dbuf-va  then
;

: channel ( n -- base )  1- h# 2000 * dma-regs +  ;

\ The registers' names, as the messages give them (the CSRs of channels 2
\ to 4 "dccr", as an Ultra 10's are)
: dacr$ ( n -- $ )
   case  1 of " dacr1" endof  2 of " dacr2" endof  3 of " dacr3" endof
   >r " dacr4" r>  endcase
;
: dbcr$ ( n -- $ )
   case  1 of " dbcr1" endof  2 of " dbcr2" endof  3 of " dbcr3" endof
   >r " dbcr4" r>  endcase
;
: dcsr$ ( n -- $ )
   case  1 of " dcsr1" endof  2 of " dccr2" endof  3 of " dccr3" endof
   >r " dccr4" r>  endcase
;

\ A register walked: for each bit, mask AND the bit written, and read
\ back through the mask; then 0.
: walk-le ( adr mask #bits -- )
   0 do						( adr mask )
      over  over 1 i lshift and			( adr mask adr v )
      2dup swap le-rl!				( adr mask adr v )
      over le-rl@ 3 pick and  ?fail-at		( adr mask )
   loop  drop  0 swap le-rl!
;

h# 0400.0000 constant a-loaded
h# 0800.0000 constant na-loaded

: dma-reg-channel ( n -- )
   dup dacr$ msg  dup channel 4 +  h# ffff.ffff  d# 32 walk-le
   dup channel dma-reset
   dup dbcr$ msg  dup channel 8 +  h# ff.ffff  d# 24 walk-le
   dup channel dma-reset
   dup dcsr$ msg  dup channel  h# 018d.2110  d# 25 walk-le
   dup channel dma-reset
   channel					( base )
   " Id wrong in CSR" msg
   dup  h# c000.0000  over dma-csr@ h# f000.0000 and  ?fail-at
   " A_loaded didn't get set" msg
   h# 10 over dma-acr!
   dup  a-loaded  over dma-csr@ a-loaded and  ?fail-at
   h# 0100.2000 over dma-csr!  h# 10 over dma-acr!
   " A_loaded got cleared" msg
   dup  a-loaded  over dma-csr@ a-loaded and  ?fail-at
   " NA_loaded didn't get set" msg
   dup  na-loaded  over dma-csr@ na-loaded and  ?fail-at
   dup dma-reset
   " Reset failed to clear csr register" msg
   dup  0  over dma-csr@ h# 0fff.ffff and  ?fail-at
   drop
;
: dma-reg-test ( -- )
   " dma_reg_test" subtest
   5 1 do  i dma-reg-channel  loop
;

\ The diagnostic mode: DIAG_EN, burst size 3, CSR drains off; a read
\ fills the channel's FIFO from memory (DIAG_RD_DONE), a write (WRITE as
\ well) empties it to memory (DIAG_WR_DONE). The byte count is not used.
0 value poll-base
: rd-done? ( -- flag )  poll-base dma-csr@ h# 800 and 0<>  ;
: wr-done? ( -- flag )  poll-base dma-csr@ h# 1000 and 0<>  ;

: dma-func-channel ( n -- )
   channel to poll-base
   h# 80 0 do  i send-buf i + c!  loop
   h# 80 0 do  h# dead.beef recv-buf i + l!  4 +loop
   poll-base dma-reset
   dbuf-io poll-base dma-acr!
   h# 001d.0000 poll-base dma-csr!
   d# 130 ms
   " DMA Read Timeout" msg
   ['] rd-done? d# 20 poll 0= if  fail  then
   poll-base dma-reset
   dbuf-io h# 2000 + poll-base dma-acr!
   h# 001d.0100 poll-base dma-csr!
   d# 130 ms
   " DMA Write Timeout" msg
   ['] wr-done? d# 20 poll 0= if  fail  then
   recv-buf dbuf-io h# 2000 + h# 80 dvma-sync
   \ (A wrong byte: its index, with the message last set.)
   h# 80 0 do  i  send-buf i + c@  recv-buf i + c@  ?fail-at  loop
;
: dma-func-test ( -- )
   " dma_func_test" subtest
   5 1 do  i dma-func-channel  loop
;

\ (A failure ends the test; the channels and buffers are given back
\ whatever the end.)
: dma-run ( xt -- )
   ['] dma-open catch ?dup if  dma-close throw  then
   catch  dma-close  throw
;
: (dma-test) ( -- )  ['] dma-reg-test dma-run  ['] dma-func-test dma-run  ;
: dma-test ( -- )  ebus-path ['] (dma-test) with-node  ;
: (dma-reg-only) ( -- )  ['] dma-reg-test dma-run  ;
: (dma-func-only) ( -- )  ['] dma-func-test dma-run  ;
: dma-reg-only ( -- )  ebus-path ['] (dma-reg-only) with-node  ;
: dma-func-only ( -- )  ebus-path ['] (dma-func-only) with-node  ;

\ --- Ethernet (ethernet_test), in the network node -------------------------
\
\ The Happy Meal's registers (little-endian words in its memory BAR:
\ global at 0, ETX at 2000, ERX at 4000, BigMAC at 6000, MIF at 7000)
\ walked, then a frame looped back four ways: through the MAC, and through
\ the internal transceiver at 10 Mbit/s, at 100 Mbit/s and through its
\ twisted-pair interface.

0 value hme			\ The registers, mapped
0 value hme-cmd			\ The PCI command register, as it was
0 value eth-va   0 value eth-io	\ The DVMA area: transmit descriptors at
h# 40 h# 700 * h# 2000 + constant /eth-area	\ 0, receive at 800, the
: txd ( -- va )  eth-va  ;			\ transmit buffer at 1000, the
: rxd ( n -- va )  8 * eth-va h# 800 + +  ;	\ receive buffers at 2000 (700
: tx-buf ( -- va )  eth-va h# 1000 +  ;	\ each)
: rx-buf ( n -- va )  h# 700 * eth-va h# 2000 + +  ;
: >eth-io ( va -- devadr )  eth-va - eth-io +  ;

: hme@ ( offset -- l )  hme + le-rl@  ;
: hme! ( l offset -- )  hme + le-rl!  ;
: hme-set ( bits offset -- )  tuck hme@ or swap hme!  ;
: hme-clear ( bits offset -- )  tuck hme@ swap invert and swap hme!  ;

\ Descriptors: two little-endian words, the flags and the buffer's address
: desc! ( flags adr va -- )  tuck 4 + >r lbflip r> l!  >r lbflip r> l!  ;
: desc-flags ( va -- l )  l@ lbflip  ;

h# 6000 constant bmac
h# 7000 constant mif

\ The set-up before the sub-tests (none of them names it)
: eth-open ( -- )
   0 0 my-space h# 0200.0010 or  h# 8000 " map-in" $call-parent to hme
   4 cfg-w@ dup to hme-cmd  h# 146 or 4 cfg-w!
   /eth-area dvma-alloc to eth-io to eth-va
   eth-va /eth-area erase
;
: eth-close ( -- )
   eth-va ?dup if  eth-io /eth-area dvma-free  0 to eth-va  then
   hme ?dup if  h# 8000 " map-out" $call-parent  0 to hme  hme-cmd 4 cfg-w!  then
;

\ The channels' reset: ETX and ERX, until the bits read 0 (100 ms)
: reset-done? ( -- flag )  0 hme@ 0=  ;
: channel-reset ( -- ok? )  3 0 hme!  ['] reset-done? d# 100 poll  ;
: my-channel-reset ( -- )
   " Cannot reset Ethernet channel!" msg
   channel-reset 0= if  fail  then
;

\ Registers walked: (offset mask) pairs from the block's base
: walk-regs ( base table #regs -- )
   0 do						( base table )
      2dup @ + hme +  over na1+ @  d# 32 walk-le	( base table )
      2 na+
   loop  2drop
;
create global1-regs  0 , 0 ,  4 , 7 ,
create global2-regs  h# 100 , 0 ,  h# 104 , h# ff7f.ffff ,
create xif-regs      0 , 3 ,
create bmac-tx-regs
   h# 208 , 0 ,      h# 20c , h# 7e0 ,  h# 210 , h# ff ,   h# 214 , h# ff ,
   h# 218 , h# ff ,  h# 21c , h# ff ,   h# 220 , h# ff ,   h# 224 , h# ff ,
   h# 228 , h# ff ,  h# 22c , h# ff ,   h# 230 , h# 1fff , h# 234 , h# ff ,
   h# 238 , 0 ,      h# 23c , h# ffff , h# 240 , h# ffff , h# 244 , h# ffff ,
   h# 248 , h# ff ,  h# 24c , h# ff ,   h# 250 , h# 3ff ,  h# 254 , 0 ,
create mif-regs
   0 , 1 ,  4 , 1 ,  8 , 1 ,  h# c , h# ffff ,  h# 10 , h# ff ,  h# 14 , h# ffff ,

: hme-reg-test ( -- )
   " hme_reg_test" subtest  " hme walk ones register test" msg
   " global_reg1_test" subtest   0 global1-regs 2 walk-regs
   " global_reg2_test" subtest   0 global2-regs 2 walk-regs
   " bmac_xif_reg_test" subtest  bmac xif-regs 1 walk-regs
   " bmac_tx_reg_test" subtest   bmac bmac-tx-regs d# 20 walk-regs
   " mif_reg_test" subtest       mif mif-regs 6 walk-regs
;

\ The internal transceiver's registers (a DP83840 at MII address 1), by
\ the MIF's bit-bang lines (clock 0, data 4, output enable 8; the data in
\ on MIF configuration bit 8, MDIO0, in bit-bang mode, bit 2).
: mif-cfg@ ( -- l )  mif h# 10 + hme@  ;
: mif-cfg! ( l -- )  mif h# 10 + hme!  ;
: bb-clock ( -- )  0 mif hme!  1 mif hme!  ;
: bb-bit ( bit -- )  1 and mif 4 + hme!  bb-clock  ;
: bb-bits ( value #bits -- )
   0 swap 1- do  dup i rshift bb-bit  -1 +loop  drop
;
: bb-in ( -- bit )  bb-clock  mif-cfg@ 8 rshift 1 and  ;
: bb-start ( op reg -- )			\ op: 1 write, 2 read
   mif-cfg@ 4 or mif-cfg!  1 mif 8 + hme!
   h# ffff.ffff d# 32 bb-bits  1 2 bb-bits
   swap 2 bb-bits  1 5 bb-bits  5 bb-bits
;
: bb-end ( -- )  0 mif 8 + hme!  mif-cfg@ 4 invert and mif-cfg!  ;
: mii! ( value reg -- )
   1 swap bb-start  2 2 bb-bits  d# 16 bb-bits  bb-end
;
\ A read: the turnaround's 0 (after the line's released one, if any),
\ then sixteen bits
: mii@ ( reg -- value )
   2 swap bb-start  0 mif 8 + hme!
   bb-in if  bb-in drop  then
   0  d# 16 0 do  2*  bb-in or  loop
   bb-end
;
: mii-set ( bits reg -- )  tuck mii@ or swap mii!  ;
: mii-clear ( bits reg -- )  tuck mii@ swap invert and swap mii!  ;

\ The internal transceiver selected; it must be there (MDIO0)
: select-internal ( -- )
   " Internal transceiver not selected" msg
   mif-cfg@ 1 invert and mif-cfg!  0 mif 8 + hme!
   mif-cfg@ h# 100 and 0= if  fail  then
;
: phy-reset-done? ( -- flag )  0 mii@ h# 8000 and 0=  ;
: reset-transceiver ( -- )
   " Can not reset-transceiver" msg
   h# 8000 0 mii!
   ['] phy-reset-done? d# 50 poll 0= if
      ." Cannot reset transceiver!" cr  fail
   then
   h# 100 0 mii-clear  h# 2000 0 mii-clear  2 ms
   2 mii@ d# 16 lshift  3 mii@ or  h# 2000.5c00 = if
      h# 20 h# 17 mii-set  2 ms
   then
;
: txmac-off? ( -- flag )  bmac h# 20c + hme@ 1 and 0=  ;
: rxmac-off? ( -- flag )  bmac h# 30c + hme@ 1 and 0=  ;
: macs-off ( -- )
   " Txmac did not reset" msg
   1 bmac h# 20c + hme-clear  ['] txmac-off? d# 20 poll 0= if  fail  then
   " Rxmac did not reset" msg
   1 bmac h# 30c + hme-clear  ['] rxmac-off? d# 20 poll 0= if  fail  then
;

\ The station address, the machine's; no hash or address filter
6 buffer: eth-mac
: get-mac ( -- )
   " mac-address" my-self ihandle>phandle find-method if
      execute  eth-mac swap 6 min move
   else  eth-mac 6 erase  then
;
: mac-half ( n -- w )  eth-mac + dup c@ 8 lshift swap 1+ c@ or  ;
: set-station ( -- )
   get-mac
   0 mac-half bmac h# 320 + hme!
   2 mac-half bmac h# 31c + hme!
   4 mac-half bmac h# 318 + hme!
   h# 360 h# 340 do  0 bmac i + hme!  4 +loop
;

\ The rings and the MACs on: 16 transmit descriptors, 64 receive (each
\ given to the chip), the receive MAC promiscuous and keeping the CRC
: start-rings ( -- )
   0 4 hme!  h# ffff.ffff h# 104 hme!
   1 h# 2004 hme-set
   d# 16 0 do  0  tx-buf >eth-io  txd i 8 * +  desc!  loop
   h# 40 bmac h# 234 + hme!  0 bmac h# 250 + hme!
   txd >eth-io h# 2008 hme!  0 h# 202c hme!
   1 h# 4000 hme-set
   h# 40 0 do  h# 700 d# 16 lshift  i rx-buf >eth-io  i rxd desc!  loop
   h# 40 bmac h# 314 + hme!
   0 rxd >eth-io h# 4004 hme!
   h# 141 bmac h# 30c + hme!
   h# 20 bmac h# 220 + hme!
   h# 40 0 do
      h# 8000.0000 h# 700 d# 16 lshift or  i rx-buf >eth-io  i rxd desc!
   loop
   h# 401 bmac h# 20c + hme-set
   1 bmac h# 30c + hme-set
;

\ The global status: its bits clear as they are read, so they are kept
\ here as they come
0 value eth-stat
: stat@ ( -- l )  h# 100 hme@ eth-stat or dup to eth-stat  ;
0 value stat-bit
: stat-bit? ( -- flag )  stat@ stat-bit and 0<>  ;
: wait-stat ( bit ms -- ok? )  swap to stat-bit  ['] stat-bit? swap poll  ;

\ The frame: to and from the station, then 48 bytes of patterns
create eth-pattern
   h# ff c, 0 c, 1 c, 2 c, 4 c, 8 c, h# 10 c, h# 20 c, h# 40 c, h# 80 c,
   h# fe c, h# fd c, h# fb c, h# f7 c, h# ef c, h# df c, h# bf c, h# 7f c, h# 55 c, h# aa c,
   h# ff c, 0 c, 1 c, 2 c, 4 c, 8 c, h# 10 c, h# 20 c, h# 40 c, h# 80 c,
   h# fe c, h# fd c, h# fb c, h# f7 c, h# ef c, h# df c, h# bf c, h# 7f c, h# 55 c, h# aa c,
   1 c, 2 c, 4 c, 8 c, h# 10 c, h# 20 c, h# 40 c, h# 80 c,
d# 48 constant /pattern
d# 60 constant /frame

: send-frame ( -- )
   eth-mac tx-buf 6 move  eth-mac tx-buf 6 + 6 move
   eth-pattern tx-buf d# 12 + /pattern move
   0 to eth-stat  stat@ drop  0 to eth-stat
   h# e000.0000 /frame or  tx-buf >eth-io  txd desc!
   1 h# 2000 hme!
   " packet did not xmit " msg
   h# 0100.0000 d# 5000 wait-stat 0= if  fail  then
   h# 100 d# 5000 wait-stat 0= if  fail  then
   eth-stat h# fc3c.0000 and if  fail  then
;

\ A dump of 48 bytes, rows of 10 (the last 8), each in three columns
: .bytes ( adr -- )
   /pattern 0 do
      dup i + c@ push-hex 3 u.r pop-base
      i d# 10 mod 9 =  i /pattern 1- =  or if  cr  then
   loop  drop
;

\ Receiving: the next descriptor back from the chip within 9 seconds, no
\ receive error; a frame starting with a byte with bit 7 set passed over.
0 value rx#
: receive-frame ( -- )
   " Did not receive expected loopback packet " msg
   get-msecs d# 9000 +				( when )
   begin
      rx# rxd desc-flags h# 8000.0000 and 0= if
         eth-stat h# 003c.00fc and if  fail  then
         rx# rx-buf c@ h# 80 and 0= if  drop exit  then
         rx# 1+ h# 3f and to rx#
      then
      stat@ drop
      dup obd-timed-out?
   until  drop fail
;
: check-frame ( -- )
   rx# rxd desc-flags d# 16 rshift h# 3fff and		( len )
   dup d# 64 <> if
      ." Wrong packet length; expected " d# 64 .d ." , observed " .d cr
      fail
   then  drop
   rx# rx-buf d# 12 + eth-pattern /pattern comp if
      ." Received packet contained incorrect data.  Expected: " cr
      eth-pattern .bytes
      ." Observed:" cr
      rx# rx-buf d# 12 + .bytes
      fail
   then
;

\ The loopback modes: the transceiver's registers, and the XIF's
: mac-internal-mode ( -- )  h# 800 h# 18 mii!  h# aa bmac hme!  ;
: 10mb-xcvr-mode ( -- )
   h# 4000 0 mii!  h# 800 h# 18 mii!  h# 8050 h# 17 mii!  h# a1 bmac hme-set
;
: 100mb-phy-mode ( -- )  h# 6000 0 mii!  h# 8050 h# 17 mii!  h# a1 bmac hme-set  ;
: 100mb-twister-mode ( -- )  h# 2000 0 mii!  h# 8100 h# 18 mii!  h# a1 bmac hme-set  ;

: loopback ( mode-xt name$ -- )
   subtest
   channel-reset drop
   select-internal
   reset-transceiver
   d# 100 ms  channel-reset drop
   macs-off
   set-station
   execute
   0 to rx#  start-rings
   send-frame  receive-frame  check-frame
   channel-reset drop
;
: mac-internal-test ( -- )
   ['] mac-internal-mode " mac_internal_loopback_test" loopback
;
: 10mb-xcvr-test ( -- )  ['] 10mb-xcvr-mode " 10mb_xcvr_loopback_test" loopback  ;
: 100mb-phy-test ( -- )  ['] 100mb-phy-mode " 100mb_phy_loopback_test" loopback  ;
: 100mb-twister-test ( -- )
   ['] 100mb-twister-mode " 100mb_twister_loopback_test" loopback
;

: (eth-all) ( -- )
   hme-reg-test  mac-internal-test  10mb-xcvr-test  100mb-phy-test  100mb-twister-test
;
\ (The whole test names the two resets my_channel_reset, as an Ultra 10's
\ does; the sub-test menu's entries do not.)
: eth-body ( xt -- )
   dup ['] (eth-all) =  if  " my_channel_reset" subtest  then
   my-channel-reset my-channel-reset  execute
;
\ (Whatever the end, the set-up given back: an Ultra 10's obdiag leaves it
\ after a failure of the second global group.)
: eth-run ( xt -- )
   ['] eth-open catch ?dup if  eth-close throw  then
   ['] eth-body catch  dup if  nip  then  eth-close  throw
;

\ The node opened with its driver (its link set up), as an Ultra 10's
\ obdiag opens it; if the driver will not open (no link), the test runs
\ in an instance without it, as it runs on an Ultra 10 with no cable.
: with-net ( xt -- )
   net-path open-dev ?dup if				( xt ih )
      dup >r ['] call-package catch  dup if  nip nip  then
      r> close-dev  throw
   else
      net-path rot with-node
   then
;
: (eth-test) ( -- )  ['] (eth-all) eth-run  ;
: eth-test ( -- )  ['] (eth-test) with-net  ;

\ The sub-test menu's entries: each with the set-up (which an Ultra 10's
\ entries leave out, so that they trap there)
: (eth-reg-only) ( -- )  ['] hme-reg-test eth-run  ;
: (eth-mac-only) ( -- )  ['] mac-internal-test eth-run  ;
: (eth-10-only) ( -- )  ['] 10mb-xcvr-test eth-run  ;
: (eth-100-only) ( -- )  ['] 100mb-phy-test eth-run  ;
: (eth-tw-only) ( -- )  ['] 100mb-twister-test eth-run  ;
: eth-reg-only ( -- )  ['] (eth-reg-only) with-net  ;
: eth-mac-only ( -- )  ['] (eth-mac-only) with-net  ;
: eth-10-only ( -- )  ['] (eth-10-only) with-net  ;
: eth-100-only ( -- )  ['] (eth-100-only) with-net  ;
: eth-tw-only ( -- )  ['] (eth-tw-only) with-net  ;
headers
