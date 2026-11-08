# Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
# OpenBoot release it is part of (see LICENSE).
#
# The release's own platform, the Sun Fire T2000 (Ontario, sun4v): built
# as a check of the tool chain. (Paths below obp/.)

NAME     = t2000
FLAGS    = -D SUN4V -D Ontario
RELEASE  = arch/sun4v/ontario/release
LOADFILE = arch/sun4v/niagara/loadprom.fth
RESET    = arch/sun4v/niagara/reset.fth
FLASH    =
INSTALL  =
