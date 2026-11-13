/*
 * Linker script for the fail-closed, position-independent trampoline stub.
 *
 * No physical VMA is assigned here. The old 0x88000000 address collided
 * with the manifest's DTB load address. A runtime loader must not be added
 * until a non-overlapping reserved-memory contract has been established.
 */

OUTPUT_ARCH(aarch64)
ENTRY(_trampoline_entry)

SECTIONS
{
    .text : {
        *(.text*)
        *(.rodata*)
    }

    .data : {
        *(.data*)
    }

    .bss : {
        *(.bss*)
        *(COMMON)
    }

    /DISCARD/ : {
        *(.note*)
        *(.comment*)
        *(.eh_frame*)
    }
}
