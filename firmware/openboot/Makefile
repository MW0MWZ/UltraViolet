# Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
# OpenBoot release it is part of (see LICENSE).
#
# make            the tool chain and every system (systems/*.mk)
# make NAME       one system (after the tool chain)
# make toolchain  the host tools and the Forth kernel
# (build.sh does the work, in Docker.)

SYSTEMS := $(basename $(notdir $(wildcard systems/*.mk)))

.PHONY: all toolchain $(SYSTEMS)
all: toolchain $(SYSTEMS)
toolchain:
	./build.sh tools kernel
$(SYSTEMS):
	./build.sh $@
