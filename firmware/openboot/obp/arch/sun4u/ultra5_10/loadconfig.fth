\ Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
\ OpenBoot release it is part of (see LICENSE).
id: @(#)loadconfig.fth 1.0 26/11/07
purpose: The Ultra 5/10's configuration variables
copyright: Copyright 2026 Andy Taylor

\ As arch/sun4v/niagara/loadconfig.fth defines sun4v's, the variables the
\ Ultra 5/10's own OpenBoot (3.x) has, with its defaults: the console on
\ the keyboard and screen, falling back to ttya.

fload ${BP}/pkg/confvar/loadcvar.fth                \ NVRAM device support
fload ${BP}/pkg/confvar/definitions/standard.fth    \ standard data types

fload ${BP}/arch/sun4u/config/reset-recovery.fth

fload ${BP}/pkg/confvar/definitions/confvoc/verbosity-types-voc.fth
: verbosity-default ( -- adr,len ) " min" ;

\ The fixed part of the NVRAM (arch/sun4v/niagara/fixed.fth's layout).
1			fixed-ptr !				\ 0x01
false			fixed-flag diag-switch?			\ 0x01
h# e			fixed-ptr !
verbosity-default	' verbosity-types fixed-vocab-variable verbosity \ 0x0e
h# 20			constant security-start
security-start		fixed-ptr !				\ 0x20
nodefault-fixed-int				security-#badlogins
d# 08			config-password:	security-password
config-security:				security-mode
stand-init: Track diagnostic-mode?
   ['] diag-switch? to diagnostic-mode?
;

fload ${BP}/arch/sun4u/config/nvramrc.fth

" disk net"	d# 256	config-string	boot-device
" "		d# 128	config-string	boot-file
" net"		d# 256	config-string	diag-device
" "		d# 128	config-string	diag-file
" boot"		d# 64	config-string	boot-command
" "		d# 512	config-long-string network-boot-arguments
true		config-flag	auto-boot?
false		config-flag	auto-boot-on-error?
default-load-base config-int	load-base

" keyboard"	d# 32	config-string	input-device
" screen"	d# 32	config-string	output-device

headers
: ttya-console " ttya" ;
headerless
' ttya-console  is fallback-device

fload ${BP}/arch/sun4u/config/console.fth

" 9600,8,n,1,-" d# 16 config-string ttya-mode
" 9600,8,n,1,-" d# 16 config-string ttyb-mode
true		config-flag	ttya-ignore-cd
true		config-flag	ttyb-ignore-cd
false		config-flag	ttya-rts-dtr-off
false		config-flag	ttyb-rts-dtr-off

fload ${BP}/arch/sun4u/config/termemu.fth
fload ${BP}/arch/sun4u/config/banner.fth
fload ${BP}/arch/sun4u/config/scsi-id.fth

false	config-flag	fcode-debug?
false	config-flag	local-mac-address?

\ Install hook for fcode-debug?
' fcode-debug?  to (fcode-debug?)

fload ${BP}/pkg/confvar/interfaces/standard.fth     \ support consumers
