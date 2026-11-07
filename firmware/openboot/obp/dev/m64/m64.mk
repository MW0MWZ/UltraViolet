# Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
# OpenBoot release it is part of (see LICENSE).
#
# The Ultra 5/10's on-board ATI Rage Pro (PGX24): UltraViolet's open source
# FCode for it (firmware/rage), its FCode program taken from the PCI
# expansion ROM image built there (the ROM header's word at 2 gives the
# program's offset, the program's header its length).

M64ROM = ${BP}/../../rage/m64.rom

m64.di: ${M64ROM}
	off=$$(od -An -tu2 -j2 -N2 --endian=little ${M64ROM} | tr -d ' '); \
	len=$$(od -An -tu4 -j$$(($$off + 4)) -N4 --endian=big ${M64ROM} | tr -d ' '); \
	dd if=${M64ROM} of=m64.fc bs=1 skip=$$off count=$$len status=none
	${MAKEDI} m64.fc ati-m64
