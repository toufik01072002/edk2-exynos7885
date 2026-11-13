/*
 * trampoline.ld - Linker script for ARM64 trampoline
 *
 * The trampoline must be loaded at a known physical address by kexec.
 * We place it at 0x88000000 (in Bank 2, known free area).
 */

OUTPUT_ARCH(aarch64)
ENTRY(_trampoline_entry)

SECTIONS
{
    . = 0x88000000;

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

    /* Stack at the end */
    .bss.stack : {
        trampoline_stack = .;
        *(.bss.stack)
        trampoline_stack_top = .;
    }

    /DISCARD/ : {
        *(.note*)
        *(.comment*)
        *(.eh_frame*)
    }
}