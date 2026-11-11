\ Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
\ OpenBoot release it is part of (see LICENSE).
id: @(#)usb-canadian-french.fth 1.0 26/11/11
purpose: Canadian French keyboard table (Sun Type 5 layout 32, HID country 4)
copyright: Copyright 2026 Andy Taylor

\ As the Ultra 10's Type 5 keyboard gives it, by USB usage, from the USA's:
\ plain, shifted and AltGraph characters, in ISO 8859 codes (oops: a key
\ with no character).

decimal
4 keyboard: canadian-french

\  normal  	shifted      altg      usage#
\  -------      -------      -------   -----    ------------
                             h# b1         30     ak		\ key 1e
                             h# a3         32     ak		\ key 20
                             h# a2         33     ak		\ key 21
                             h# a4         34     ak		\ key 22
                ascii ?                    35     sk		\ key 23
                             ascii [       38     ak		\ key 26
                             ascii ]       39     ak		\ key 27
                             h# ac         46     ak		\ key 29
   ascii /      ascii \      ascii |       53     allk		\ key 2a
                             h# b6         19     ak		\ key 3f
   oops         oops                       47     nsk		\ key 40
   h# e7        h# c7        ascii ~       48     allk		\ key 41
                             h# b0         51     ak		\ key 56
   h# e8        h# c8                      52     nsk		\ key 57
   h# e0        h# c0                      50     nsk		\ key 58
                             h# ab         29     ak		\ key 64
                             h# bb         27     ak		\ key 65
                             h# b5         16     ak		\ key 6a
                ascii '      ascii <       54     sak		\ key 6b
                ascii "      ascii >       55     sak		\ key 6c
   h# e9        h# c9        ascii `       56     allk		\ key 6d
   h# f9        h# d9                     100     nsk		\ key 7c
kend
