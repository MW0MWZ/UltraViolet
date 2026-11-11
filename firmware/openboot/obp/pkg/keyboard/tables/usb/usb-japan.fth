\ Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
\ OpenBoot release it is part of (see LICENSE).
id: @(#)usb-japan.fth 1.0 26/11/11
purpose: Japanese keyboard table (Sun Type 5 layout 31, HID country 15)
copyright: Copyright 2026 Andy Taylor

\ As the Ultra 10's Type 5 keyboard gives it, by USB usage, from the USA's:
\ plain, shifted and AltGraph characters, in ISO 8859 codes (oops: a key
\ with no character).

decimal
15 keyboard: japan

\  normal  	shifted      altg      usage#
\  -------      -------      -------   -----    ------------
                ascii "                    31     sk		\ key 1f
                ascii &                    35     sk		\ key 23
                ascii '                    36     sk		\ key 24
                ascii (                    37     sk		\ key 25
                ascii )                    38     sk		\ key 26
                oops                       39     sk		\ key 27
                ascii =                    45     sk		\ key 28
   ascii ^      ascii ~                    46     nsk		\ key 29
   oops         ascii |                    53     nsk		\ key 2a
   ascii @      ascii `                    47     nsk		\ key 40
   ascii [      ascii {                    48     nsk		\ key 41
                ascii +                    51     sk		\ key 56
   ascii :      ascii *                    52     nsk		\ key 57
   ascii ]      ascii }                    49     nsk		\ key 58
kend
