\ Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
\ OpenBoot release it is part of (see LICENSE).
id: @(#)usb-poland.fth 1.0 26/11/11
purpose: Polish (ISO 8859-2) keyboard table (Sun Type 5 layout 34, HID country 21)
copyright: Copyright 2026 Andy Taylor

\ As the Ultra 10's Type 5 keyboard gives it, by USB usage, from the USA's:
\ plain, shifted and AltGraph characters, in ISO 8859 codes (oops: a key
\ with no character).

decimal
21 keyboard: poland

\  normal  	shifted      altg      usage#
\  -------      -------      -------   -----    ------------
                ascii "      ascii @       31     sak		\ key 1f
                ascii :      ascii #       32     sak		\ key 20
                ascii ?      ascii $       33     sak		\ key 21
                ascii +      ascii ^       35     sak		\ key 23
                ascii _      ascii &       36     sak		\ key 24
                ascii (      ascii *       37     sak		\ key 25
                ascii )                    38     sk		\ key 26
                ascii =                    39     sk		\ key 27
   h# bf        h# ad                      45     nsk		\ key 28
   oops         oops                       46     nsk		\ key 29
                             h# b0         53     ak		\ key 2a
   h# f3        h# ac        ascii [       47     allk		\ key 40
   ascii /      h# a7        ascii ]       48     allk		\ key 41
                             h# df         22     ak		\ key 4e
                             ascii &       14     ak		\ key 54
   h# b3        h# a3                      51     nsk		\ key 56
   h# b1        h# ca        ascii '       52     allk		\ key 57
                ascii ;      h# a4         49     sak		\ key 58
                h# b6        ascii <       54     sak		\ key 6b
                h# f1        ascii >       55     sak		\ key 6c
   ascii -      h# e6                      56     nsk		\ key 6d
kend
