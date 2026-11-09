# Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
# OpenBoot release it is part of (see LICENSE).
#
# The Sun Ultra 5 and Ultra 10 (UltraSPARC IIi, its PCI bus module and the
# APB): a flash PROM image, built into UltraViolet as its default firmware.
# (Paths below obp/.)

NAME     = ultra5_10
FLAGS    = -D SUN4U -D Ultra5_10
RELEASE  = arch/sun4u/ultra5_10/release
LOADFILE = arch/sun4u/ultra5_10/loadprom.fth
RESET    = arch/sun4u/ultra5_10/reset.fth
# The flash PROM: 1 MiB, the image followed by erased (ff) bytes
FLASH    = 1048576
# The whole 1 MiB flash image copied beside build.sh, for UltraViolet
INSTALL  = ultra5_10.prom
# The power-on selftest, its own program, at the flash PROM's second half
# (as an Ultra 10 has it); built always, installed once POST_ON is yes
POST     = post
POST_AT  = 524288
POST_ON  = yes
