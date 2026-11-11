\ Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
\ OpenBoot release it is part of (see LICENSE).
id: @(#)usb-czech.fth 1.0 26/11/11
purpose: Czech (ISO 8859-2) keyboard table (Sun Type 5 layout 35, HID country 5)
copyright: Copyright 2026 Andy Taylor

\ As the Ultra 10's Type 5 keyboard gives it, by USB usage, from the USA's:
\ plain, shifted and AltGraph characters, in ISO 8859 codes (oops: a key
\ with no character).

decimal
5 keyboard: czech

\  normal  	shifted      altg      usage#
\  -------      -------      -------   -----    ------------
   ascii +      ascii 1                    30     nsk		\ key 1e
   h# ec        ascii 2      ascii @       31     allk		\ key 1f
   h# b9        ascii 3      ascii #       32     allk		\ key 20
   h# e8        ascii 4      ascii $       33     allk		\ key 21
   h# f8        ascii 5      ascii %       34     allk		\ key 22
   h# be        ascii 6      ascii ^       35     allk		\ key 23
   h# fd        ascii 7      ascii &       36     allk		\ key 24
   h# e1        ascii 8      ascii *       37     allk		\ key 25
   h# ed        ascii 9      ascii {       38     allk		\ key 26
   h# e9        ascii 0      ascii }       39     allk		\ key 27
   ascii =      ascii %                    45     nsk		\ key 28
   oops         oops                       46     nsk		\ key 29
   ascii z      ascii Z                    28     nsk		\ key 3b
   h# fa        ascii /      ascii [       47     allk		\ key 40
   ascii )      ascii (      ascii ]       48     allk		\ key 41
                             h# df         22     ak		\ key 4e
   h# f9        ascii "      ascii ;       51     allk		\ key 56
   h# a7        ascii !      ascii '       52     allk		\ key 57
   oops         ascii \                    49     nsk		\ key 58
   ascii y      ascii Y                    29     nsk		\ key 64
                ascii ?      ascii <       54     sak		\ key 6b
                ascii :      ascii >       55     sak		\ key 6c
   ascii -      ascii _                    56     nsk		\ key 6d
kend
