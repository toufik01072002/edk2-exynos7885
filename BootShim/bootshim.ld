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

    ASSERT(SIZEOF(.text.entry) <= 0x1000, "BootShim entry exceeds its code window")
    ASSERT(ADDR(.text.validation) + SIZEOF(.text.validation) <= 0xD000, "validation code overlaps descriptor mailbox")
    ASSERT(SIZEOF(.mailbox) == 0x100, "descriptor mailbox must be 256 bytes")
    ASSERT(SIZEOF(.sec_offset) == 8, "SEC offset slot must be 8 bytes")
    ASSERT(ADDR(.stack) + SIZEOF(.stack) == 0x10000, "private stack must end at 64 KiB")

    /DISCARD/ :
        *(.comment)
        *(.note*)
        *(.eh_frame*)
        *(.data*)
        *(.bss*)
    }
}
