\ Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
\ OpenBoot release it is part of (see LICENSE).
id: @(#)loadconfig.fth 1.1 26/11/09
purpose: The Ultra 5/10's configuration variables
copyright: Copyright 2026 Andy Taylor

\ The variables the Ultra 5/10 has, no more, each at
\ its place in an Ultra 10's NVRAM layout with an Ultra 10's encoding, length and default
\ (fixed-u10.fth), so that either firmware reads what the other wrote: the
\ console on the keyboard and screen, falling back to ttya. They are
\ defined in the reverse of the order an Ultra 10's printenv lists them, as
\ printenv lists the newest first.

fload ${BP}/pkg/confvar/loadcvar.fth                \ NVRAM device support
fload ${BP}/pkg/confvar/definitions/standard.fth    \ standard data types
fload ${BP}/arch/sun4u/ultra5_10/fixed-u10.fth        \ the Ultra 10's own types
\ (The keywords of the release's error-reset-recovery, which an Ultra 10 doesn't have:
\ loaded for cleanup.fth, which unlinks their vocabulary, the variable not.)
fload ${BP}/pkg/confvar/definitions/confvoc/recovery-types-voc.fth

transient
: at-u10 ( offset -- )  fixed-ptr !  ;
resident

headers

h# 001 at-u10  false		fixed-flag	diag-switch?
d# 7   h# 020	nodefault-u10-string		last-hardware-update
d# 15  h# 029	nodefault-u10-string		hardware-revision
h# 070 at-u10  false		fixed-flag	oem-banner?
d# 68  h# 071	nodefault-u10-string		oem-banner
h# 0b7 at-u10  false		fixed-flag	oem-logo?
d# 512 h# 0b8	nodefault-u10-bytes		oem-logo
h# 040 at-u10			nodefault-fixed-int	security-#badlogins
d# 8   h# 047	u10-password			security-password
h# 050				u10-security-mode	security-mode
u10-nvramrc					nvramrc
h# 2be at-u10  false		fixed-flag	use-nvramrc?
h# 2bf at-u10  false		fixed-flag	silent-mode?
h# 2c0 at-u10  d# 34		fixed-int	screen-#rows
h# 2c4 at-u10  d# 80		fixed-int	screen-#columns
h# 2c8 at-u10  true		fixed-flag	ansi-terminal?
h# 2c9 at-u10  false		fixed-flag	local-mac-address?
" disk net"	d# 256 h# 2ca	u10-string	boot-device
" "		d# 128 h# 3cc	u10-string	boot-file
" net"		d# 256 h# 44e	u10-string	diag-device
" "		d# 128 h# 550	u10-string	diag-file
h# 5d2 at-u10  false		fixed-flag	watchdog-reboot?
h# 5d3 at-u10  true		fixed-flag	auto-boot?
" boot"		d# 64  h# 5d4	u10-string	boot-command
h# 616 at-u10  default-load-base	fixed-int	load-base
" keyboard"	d# 32  h# 61a	u10-string	input-device
" screen"	d# 32  h# 63c	u10-string	output-device
h# 65e at-u10  false		fixed-flag	fcode-debug?
\ (an Ultra 10 keeps system-board-date's length in security-mode's byte: followed.)
d# 8   h# 050	nodefault-u10-string		system-board-date
d# 16  h# 05a	nodefault-u10-string		system-board-serial#
h# 008 at-u10			nodefault-fixed-int	#power-cycles

\ The level of the power-on tests and the manufacturing mode, with an Ultra 10's
\ keywords and bytes (mfg-mode's from the Ultra 10's own table).
resident  headers
vocabulary diag-level-types  also diag-level-types definitions
h# 00	byte-keyword	off
h# 20	byte-keyword	min
h# 40	byte-keyword	max
previous definitions
vocabulary mfg-mode-types  also mfg-mode-types definitions
h# 00	byte-keyword	off
h# 01	byte-keyword	on
h# 10	byte-keyword	menu-int
h# 11	byte-keyword	menu-ext
h# 20	byte-keyword	bft-int
h# 21	byte-keyword	bft-ext
h# 40	byte-keyword	sys-int
h# 41	byte-keyword	sys-ext
previous definitions

" max"	' diag-level-types	h# 002	u10-keyword	diag-level
" off"	' mfg-mode-types	h# 003	u10-keyword	mfg-mode
" 1,2,3"	d# 16  h# 65f	u10-string	pcib-probe-list
" 1,2,3,4"	d# 16  h# 671	u10-string	pcia-probe-list
" 9600,8,n,1,-"	d# 16  h# 683	u10-string	ttya-mode
" 9600,8,n,1,-"	d# 16  h# 695	u10-string	ttyb-mode
h# 6a7 at-u10  true		fixed-flag	ttya-ignore-cd
h# 6a8 at-u10  false		fixed-flag	ttya-rts-dtr-off
h# 6a9 at-u10  true		fixed-flag	ttyb-ignore-cd
h# 6aa at-u10  false		fixed-flag	ttyb-rts-dtr-off
" "		d# 386 h# 6ab	u10-bytes	keymap
h# 831 at-u10  false		fixed-flag	keyboard-click?
d# 7		h# 832		u10-char	scsi-initiator-id
h# 833 at-u10  true		fixed-flag	tpe-link-test?

\ The logo's size (arch/sun4u/config/banner.fth's, the variables being here).
d# 64 constant logo-width
d# 64 constant logo-height

headerless

stand-init: Track diagnostic-mode?
   ['] diag-switch? to diagnostic-mode?
;

headers
: ttya-console " ttya" ;
headerless
' ttya-console  is fallback-device

fload ${BP}/arch/sun4u/config/console.fth

\ The root's scsi-initiator-id property (arch/sun4u/config/scsi-id.fth's,
\ the variable being here).
: set-scsi-id-prop ( -- )
   scsi-initiator-id " scsi-initiator-id"  integer-property
;
stand-init: Setting scsi-initiator-id
   root-device
      set-scsi-id-prop
   device-end
;

\ Install hook for fcode-debug?
' fcode-debug?  to (fcode-debug?)

\ The routines that use the variables (pkg/confvar/interfaces/standard.fth's
\ list), with printenv's layout (ui-cvars-u10.fth) and nvramrc's (nvramrc-u10.fth)
\ as an Ultra 10's.
also nvdevice definitions
fload ${BP}/arch/sun4u/ultra5_10/ui-cvars-u10.fth
fload ${BP}/arch/sun4u/ultra5_10/nvramrc-u10.fth
fload ${BP}/pkg/confvar/interfaces/nvalias.fth
fload ${BP}/pkg/confvar/interfaces/security.fth
previous definitions

\ The NVRAM attached, as the release's install-nvram does, but with an Ultra 10's
\ test of it: its checksum. The hashed store (for nothing of an Ultra 10's) is
\ outside an Ultra 10's area and made new, silently, if it is not good. At a bad
\ checksum, as an Ultra 10 (its messages too): the defaults, oem-banner and oem-logo
\ emptied, diag-switch? true, and #power-cycles 0.
also nvdevice definitions
headerless
: u10-sum ( -- b )
   h# 17  h# 1cd8 h# c  do  i >fixed-buffer c@ xor  loop
;
: u10-ok? ( -- flag )  u10-sum  0 >fixed-buffer c@ =  ;
: u10-write-sum ( -- )
   u10-sum fixed-xfer-buffer c!  fixed-xfer-buffer 1 0 u10-write
;
previous definitions
also nvdevice
headerless
: install-nvram-u10 ( -- )
   nvram-package 0=  if  exit  then
   nvram-package phandle>devname			( dev$ )
   open-nvram 0=  if
      cmn-fatal[ " Unable to open NVRAM device" ]cmn-end  exit
   then
   init-nvmagic#  load-nvfixed-data
   nvtoken-region-ok?  if
      ['] load-nvtoken-data catch  if  init-nvtoken-region  then
   else
      init-nvtoken-region
   then
   u10-ok? 0=  if
      ." Incorrect configuration checksum; " cr
      ." Setting NVRAM parameters to default values." cr
      (set-defaults)
      " " ['] oem-banner set
      " " ['] oem-logo set
      true ['] diag-switch? set
      0 ['] #power-cycles set		\ (a blank NVRAM: none counted yet)
      ." Setting diag-switch? NVRAM parameter to true" cr
      u10-write-sum
      reset-config
   then
   ?secure
;
previous
