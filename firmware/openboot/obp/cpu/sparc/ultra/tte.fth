\ Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
\ OpenBoot release it is part of (see LICENSE).
id: @(#)tte.fth 1.0 26/11/07
purpose: Translation table entries of the UltraSPARC I/II/IIi (sun4u)
copyright: Copyright 2026 Andy Taylor

\ The words cpu/sparc/ultra4v/tte.fth gives for sun4v, for the sun4u TTE:
\   63 V  62:61 size (8K, 64K, 512K, 4M)  60 NFO  59 IE  58:50 soft2
\   40:13 PA  12:7 soft (bit 7 marks OpenBoot's own mappings)
\   6 L  5 CP  4 CV  3 E  2 P  1 W  0 G

headerless
1 #pabits lshift 1-   constant pa-mask
h#  3                 constant tteshift
h#  8                 constant /tte

: >tte-writable ( tte -- tte' )  1 d#  1 lshift or  ;
: >tte-readonly ( tte -- tte' )  1 d#  1 lshift invert and  ;
: >tte-priv     ( tte -- tte' )  1 d#  2 lshift or  ;
: >tte-effect   ( tte -- tte' )  1 d#  3 lshift or  ;
: >tte-cv       ( tte -- tte' )  1 d#  4 lshift or  ;
: >tte-cp       ( tte -- tte' )  1 d#  5 lshift or  ;
: >tte-locked   ( tte -- tte' )  1 d#  6 lshift or  ;
: >tte-invert   ( tte -- tte' )  1 d# 59 lshift or  ;
: >tte-valid    ( tte -- tte' )  1 d# 63 lshift or  ;
: >tte-global   ( tte -- tte' )  1 or  ;
: >tte-8k       ( tte -- tte' )  3 d# 61 lshift invert and  ;
: >tte-64k      ( tte -- tte' )  >tte-8k  1 d# 61 lshift or  ;
: >tte-512k     ( tte -- tte' )  >tte-8k  2 d# 61 lshift or  ;
: >tte-4m       ( tte -- tte' )  >tte-8k  3 d# 61 lshift or  ;
: >tte-soft     ( tte -- tte' )  1 d# 7 lshift or  ;

0
>tte-writable
>tte-cv
>tte-effect
>tte-global
>tte-invert
constant tte-mode-mask

: mode>tte  ( mode tte  -- tte' )
   \ MODE == -1 is the default
   over true  =  if  nip exit  then          ( mode tte )
   tte-mode-mask invert and                  ( mode tte" )
   over h# 001 and  if  >tte-writable  then  ( mode tte" )
   over h# 020 and  if  >tte-cv        then  ( mode tte" )
   over h# 040 and  if  >tte-effect    then  ( mode tte" )
   over h# 080 and  if  >tte-global    then  ( mode tte" )
   over h# 100 and  if  >tte-invert    then  ( mode tte" )
   nip
;

: tte>mode  ( tte -- mode )
   0                                              ( tte mode" )
   over 0 >tte-writable and  if  h# 001 or  then  ( tte mode' )
   2 or \ Always readable                         ( tte mode' )
   4 or \ Always executable                       ( tte mode' )
   over 0 >tte-cv       and  if  h# 020 or  then  ( tte mode' )
   over 0 >tte-effect   and  if  h# 040 or  then  ( tte mode' )
   over 0 >tte-global   and  if  h# 080 or  then  ( tte mode' )
   over 0 >tte-invert   and  if  h# 100 or  then  ( tte mode )
   nip
;

: valid-tte? ( tte -- flag )  0<  ;
: tte>size ( tte -- size )  d# 61 rshift 3 and  3 *  d# 13 +  1 swap lshift  ;

\ Set this as the memory high water mark.
variable physmax physmax off

headers
: memory? ( pa.lo pa.hi -- flag ) drop physmax @ < ;

: tte> ( tte -- pa.lo pa.hi )
   pa-mask and page#mask and  0 ( pa.lo pa.hi )
;

: >tte ( pa.lo pa.hi -- tte )
   2dup memory?  if                    ( pa.lo pa.hi )
      drop 0 >tte-cp >tte-cv           ( pa tte )
   else                                ( pa.lo pa.hi )
      drop 0 >tte-effect               ( pa tte )
   then                                ( pa tte )
   swap  pa-mask and page#mask and or  ( tte )
   >tte-valid >tte-writable >tte-priv  ( tte )
;
headerless
