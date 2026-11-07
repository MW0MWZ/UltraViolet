\ Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
\ OpenBoot release it is part of (see LICENSE).
id: @(#)tlbasm.fth 1.0 26/11/07
purpose: Locked TLB entries for the UltraSPARC I/II/IIi (sun4u)
copyright: Copyright 2026 Andy Taylor

\ cpu/sparc/ultra4v/tlbasm.fth's routines for sun4u: an entry made from
\ its parts and loaded, locked, through the tag access and data-in
\ registers, in context 0.
\
\ %o0 = VA
\ %o1 = PA
\ %o2 = Size 0 = 8K, 1 = 64K, 2 = 512K, 3 = 4M
\ %o3 = TTE Mode bits
\ %o4, %o5 trashed

label setup-itlb-entry
   %o1 d# 64 #pabits -  %o5  sllx		\ I/O space (PA bit 40)?
   %o5 %g0    %g0  subcc
   0<  if
      %o1  0 >tte-priv >tte-cv >tte-cp >tte-writable  %o5  or	\ P,CP,CV,W
      %o1  0 >tte-priv >tte-effect >tte-writable  %o5  or	\ P,E,W
   then
   %o5  %o3          %o5  or			\ Other bits
   %o5  0 >tte-locked  %o5  or			\ L
   %o2  d# 61        %o4  sllx
   %o5  %o4          %o5  or			\ Size
   %g0  1            %o4  or
   %o4  d# 63        %o4  sllx			\ V
   %o4  %o5          %o5  or			\ %o5 = TTE
   %g0  h# 30        %o4  add
   %o0  %o4  %g0  h# 50  stxa			\ tag access: VA, context 0
   #Sync membar
   %o5  %g0  %g0  h# 54  stxa			\ data in
   #Sync membar
   retl
   nop
end-code

label setup-dtlb-entry
   %o1 d# 64 #pabits -  %o5  sllx
   %o5 %g0    %g0  subcc
   0<  if
      %o1  0 >tte-priv >tte-cv >tte-cp >tte-writable  %o5  or	\ P,CP,CV,W
      %o1  0 >tte-priv >tte-effect >tte-writable  %o5  or	\ P,E,W
   then
   %o5  %o3          %o5  or
   %o5  0 >tte-locked  %o5  or
   %o2  d# 61        %o4  sllx
   %o5  %o4          %o5  or
   %g0  1            %o4  or
   %o4  d# 63        %o4  sllx
   %o4  %o5          %o5  or
   %g0  h# 30        %o4  add
   %o0  %o4  %g0  h# 58  stxa
   #Sync membar
   %o5  %g0  %g0  h# 5c  stxa
   #Sync membar
   retl
   nop
end-code
