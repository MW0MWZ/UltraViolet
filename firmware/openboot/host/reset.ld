/* Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
 * OpenBoot release it builds (see LICENSE).
 *
 * The reset code's layout, as obp/arch/sun4u/openboot.map gives it for
 * Solaris's linker: the code and data from address 0 (the PROM), the bss at
 * 0x40000 (memory). */
ENTRY(reset)
SECTIONS {
    . = 0x0;
    .text : { *(.text) *(.text.*) *(.rodata) *(.rodata.*) *(.data) *(.data1) *(.data.*) }
    . = 0x40000;
    .bss (NOLOAD) : { *(.bss) *(.bss.*) *(COMMON) }
    /DISCARD/ : { *(.comment) *(.note*) *(.debug*) *(.stab*) *(.eh_frame) }
}
