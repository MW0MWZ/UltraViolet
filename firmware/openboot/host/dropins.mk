# Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
# OpenBoot release it builds (see LICENSE).
#
# GNU make rules for a platform's drop-ins, as the release's makefiles
# (obp/arch/common.mk, obp/forth.mk) have them: build.sh makes dropins.mk
# from the platform's dropins.src with didepend and runs this in the
# platform's build folder, with ROOT, OUT and FLAGS set.

BP		= $(ROOT)/obp
TOOLS		= $(OUT)/tools
QEMU		= qemu-sparc32plus-static
FORTH		= $(TOOLS)/forth
FFLAGS		= $(FLAGS)
MAKEDI		= $(QEMU) $(TOOLS)/makedi -c
FORTHDIC	= $(BP)/os/sun/sparc/forth.dic
FORTHTOOL	= $(QEMU) $(FORTH) -d $(FORTHDIC) $(FFLAGS) -s "$(FTHFLAGS)"
TOKENIZE	= $(QEMU) $(FORTH) -d $(BP)/pkg/tokenizr/sparc/tokenize.dic -D tokenizer? $(FFLAGS)
TOKFLAGS	= aout-header? off silent on
FTHFLAGS	=
export BP

# An FCode program, from the .tok file among its dependencies.
%.fc:
	@echo "== tokenizing $(filter %.tok,$^)"
	$(TOKENIZE) -s "$(FTHFLAGS)" -s "$(TOKFLAGS) tokenize $(filter %.tok,$^) $@" < /dev/null
	@test -s $@

include dropins.mk
