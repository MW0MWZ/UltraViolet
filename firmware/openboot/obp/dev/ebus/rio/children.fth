\ Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
\ OpenBoot release it is part of (see LICENSE).
id: @(#)children.fth 1.0 26/11/07
purpose: The EBus's devices
copyright: Copyright 2026 Andy Taylor

\ The Ultra 5/10's EBus devices, with the properties the machine's own
\ firmware gives them (as Solaris and other systems find them). Those that
\ OpenBoot itself drives (the serial ports, keyboard and mouse) have their
\ drivers among the drop-ins (in /packages/SUNW,builtin-drivers), each
\ loaded into its node.

: child ( unit$ -- )  new-device  0 0 2swap set-args  ;

\ A register in the second window (offset 14): reg's three cells
: reg+ ( xdr,len offset size -- xdr',len' )  >r >r h# 14 en+ r> en+ r> en+  ;

\ The virtual address of a register, mapped for good ("address" entries:
\ Solaris uses some before it maps the devices itself).
: va+ ( xdr,len offset size -- xdr',len' )
   >r h# 14 r> " map-in" $call-parent en+
;

: interrupts-property ( n -- )  " interrupts" integer-property  ;

\ A drop-in driver, if there is one, loaded into the node being made.
: builtin-driver ( name$ -- )
   " SUNW,builtin-drivers" find-package if	( name$ phandle )
      find-method if  execute  then		( )
   else						( name$ )
      2drop					( )
   then
;

\ (Each node is made inside a definition: a driver loaded into a node is
\ an FCode program of its own, and FCode interpreted after it at the top
\ level here would be read with its tokens, not ours.)

\ The auxiliary I/O registers (the LED, the floppy's terminal count, ...)
: make-auxio ( -- )
   " 14,726000" child
      " auxio" name
      0 0 encode-bytes
         h# 726000 4 reg+  h# 728000 4 reg+  h# 72a000 4 reg+
         h# 72c000 4 reg+  h# 72f000 4 reg+
      " reg" property
      0 0 encode-bytes
         h# 726000 4 va+  h# 728000 4 va+  h# 72a000 4 va+
         h# 72c000 4 va+  h# 72f000 4 va+
      " address" property
   finish-device
;

\ The power controller: the soft power button's interrupt
: make-power ( -- )
   " 14,724000" child
      " power" name
      0 0 encode-bytes  h# 724000 4 reg+  " reg" property
      0 0 encode-bytes  h# 724000 4 va+  " address" property
      1 interrupts-property
      " button" boolean-property
   finish-device
;

\ The processor clock's synthesiser
: make-pll ( -- )
   " 14,504000" child
      " SUNW,pll" name
      0 0 encode-bytes  h# 504000 3 reg+  " reg" property
      " MC12430" encode-string " freq-syn" property
   finish-device
;

\ The SAB82532: ttya and ttyb
: make-se ( -- )
   " 14,400000" child
      " ebus-se" builtin-driver
   finish-device
;

\ The SuperIO's UARTs: the keyboard, then the mouse
: make-su ( unit$ prop$ driver$ -- )
   2rot child
      2swap boolean-property
      builtin-driver
   finish-device
;

\ The SuperIO's parallel port
: make-ecpp ( -- )
   " 14,3043bc" child
      " ecpp" name
      0 0 encode-bytes
         h# 3043bc h# 10 reg+  h# 30015c 2 reg+  h# 700000 h# 10 reg+
      " reg" property
      1 interrupts-property
   finish-device
;

\ The SuperIO's floppy controller
: make-fdthree ( -- )
   " 14,3023f0" child
      " fdthree" name
      0 0 encode-bytes
         h# 3023f0 8 reg+  h# 706000 h# 10 reg+  h# 720000 4 reg+
      " reg" property
      1 interrupts-property
      " block" device-type
      0 0 encode-bytes
         h# 3023f0 8 va+  h# 706000 h# 10 va+  h# 720000 4 va+
      " address" property
      \ (unit and manual, or status, from the driver's probe)
      " ebus-fdthree" builtin-driver
   finish-device
;

\ The M48T59: NVRAM, IDPROM and clock (OpenBoot reads its NVRAM through
\ /packages/nvram)
: make-eeprom ( -- )
   " 14,0" child
      " eeprom" name
      " mk48t59" model
      0 0 encode-bytes  0 h# 2000 reg+  " reg" property
      0 0 encode-bytes  0 h# 2000 va+  " address" property
      " ebus-eeprom" builtin-driver
   finish-device
;

\ The flash PROM, in the first window, with OpenBoot's version
: make-flashprom ( -- )
   " 10,0" child
      " flashprom" name
      " SUNW,370-3697" model
      h# 10 encode-int 0 en+ h# 10.0000 en+  " reg" property
      " /openprom" find-package if
         " version" rot get-package-property 0= if  " version" property  then
      then
      " ebus-flashprom" builtin-driver
   finish-device
;

\ The audio codec
: make-cs4231 ( -- )
   " 14,200000" child
      " SUNW,CS4231" name
      " SUNW,CS4231h" model
      0 0 encode-bytes
         h# 200000 h# 100 reg+  h# 702000 h# 10 reg+
         h# 704000 h# 10 reg+  h# 722000 4 reg+
      " reg" property
      1 encode-int 2 en+ " interrupts" property
      " eb2dma" encode-string " dma-model" property
      " cdrom" encode-string " aux1" property
      " none" encode-string " aux2" property
   finish-device
;

: make-children ( -- )
   make-auxio  make-power  make-pll  make-se
   " 14,3083f8" " keyboard" " ebus-su-kbd" make-su
   " 14,3062f8" " mouse" " ebus-su" make-su
   make-ecpp  make-fdthree  make-eeprom  make-flashprom  make-cs4231
;
