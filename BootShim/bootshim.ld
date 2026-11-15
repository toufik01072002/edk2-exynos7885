/* SPDX-License-Identifier: MIT
 * Fixed 64 KiB position-independent BootShim image layout.
 * Validation code lives in the reserved 0x1000-0xCFFF code window.
 */
ENTRY(_Head)
SECTIONS
{
    . = 0x00000000;
    .text.entry : { KEEP(*(.text.entry)) }

    . = 0x00001000;
    .text.validation : {
        *(.text)
        *(.text.*)
        *(.rodata)
        *(.rodata.*)
    }

    . = 0x0000D000;
    .mailbox : { KEEP(*(.mailbox)) }

    . = 0x0000D100;
    .sec_offset : { KEEP(*(.sec_offset)) }

    . = 0x0000E000;
    .stack (NOLOAD) : { KEEP(*(.stack)) }

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
        *(.data*)
        *(.bss*)
    }
}
