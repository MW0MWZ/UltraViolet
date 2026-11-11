\ Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
\ OpenBoot release it is part of (see LICENSE).
id: @(#)usb-hungary.fth 1.0 26/11/11
purpose: Hungarian (ISO 8859-2) keyboard table (Sun Type 5 layout 33, HID country 12)
copyright: Copyright 2026 Andy Taylor

\ As the Ultra 10's Type 5 keyboard gives it, by USB usage, from the USA's:
\ plain, shifted and AltGraph characters, in ISO 8859 codes (oops: a key
\ with no character).

decimal
12 keyboard: hungary

\  normal  	shifted      altg      usage#
\  -------      -------      -------   -----    ------------
                ascii '                    30     sk		\ key 1e
                ascii "                    31     sk		\ key 1f
                ascii +      ascii #       32     sak		\ key 20
                ascii !      ascii $       33     sak		\ key 21
                ascii /      ascii ^       35     sak		\ key 23
                ascii =      ascii {       36     sak		\ key 24
                ascii (      ascii [       37     sak		\ key 25
                ascii )      ascii ]       38     sak		\ key 26
   h# f6        h# d6        ascii }       39     allk		\ key 27
   h# fc        h# dc        ascii \       45     allk		\ key 28
   h# f3        h# d3        ascii ~       46     allk		\ key 29
   ascii 0      h# a7                      53     nsk		\ key 2a
                             ascii @       20     ak		\ key 36
   ascii z      ascii Z                    28     nsk		\ key 3b
   h# f5        h# d5                      47     nsk		\ key 40
   h# fa        h# da                      48     nsk		\ key 41
                             h# df         22     ak		\ key 4e
                             ascii &       14     ak		\ key 54
   h# e9        h# c9        ascii ;       51     allk		\ key 56
   h# e1        h# c1        h# e4         52     allk		\ key 57
   h# fb        h# db                      49     nsk		\ key 58
   ascii y      ascii Y      ascii <       29     allk		\ key 64
                             ascii >       27     ak		\ key 65
                             ascii `        6     ak		\ key 66
                ascii ?      ascii *       54     sak		\ key 6b
                ascii :                    55     sk		\ key 6c
   ascii -      ascii _                    56     nsk		\ key 6d
kend
