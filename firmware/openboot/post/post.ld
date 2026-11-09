/*
 * Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
 * OpenBoot release it is part of (see LICENSE).
 *
 * The power-on selftest's layout. It is placed in the flash PROM's second
 * half and starts there; once it has checked the memory it runs in, it
 * copies itself to 1 MiB and runs there with the MMU off (addresses are
 * physical, below 4 GiB). Its stack is below 1 MiB.
 */
OUTPUT_FORMAT("elf32-sparc")
OUTPUT_ARCH(sparc)
ENTRY(_start)
SECTIONS
{
	. = 0x100000;
	.text : { KEEP(*(.text.start)) *(.text*) }
	.rodata : { *(.rodata*) }
	.data : { *(.data*) }
	. = ALIGN(8);
	_image_end = .;
	.bss (NOLOAD) : { __bss_start = .; *(.bss*) *(COMMON) . = ALIGN(8); __bss_end = .; }
	/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
