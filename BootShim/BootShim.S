/*
 * SM-A205W - EDK2 BootShim
 *
 * SAFE PREPARATION MODE:
 * - Position-independent; no fixed physical address.
 * - Establishes a private stack inside its own 64 KiB image.
 * - Does not copy the FD or branch to SEC.
 * - Remains blocked in WFE after stack setup.
 *
 * The stack occupies [image + 0xE000, image + 0x10000).
 * The FD starts at image + 0x10000, so the stack grows down
 * inside the BootShim reservation and cannot overwrite the FD.
 */

.section .text
.balign 16
.global _Head
.type _Head, %function

_Head:
    b       _PrepareStack

_PrepareStack:
    /* ADR keeps the stack position-independent with the loaded image. */
    adr     x9, _BootShimStackTop
    mov     sp, x9

    /* Fail closed if the loaded image base did not preserve 16-byte alignment. */
    and     x10, x9, #0xF
    cbnz    x10, _StackFault

_Disabled:
    wfe
    b       _Disabled

_StackFault:
    wfe
    b       _StackFault

/*
 * Reserve an 8 KiB writable stack in the tail of the 64 KiB BootShim.
 * Exact reservation size: 0x2000 bytes (from 0xE000 through 0xFFFF).
 * Stack grows downward from _BootShimStackTop. The FD begins immediately
 * after the top boundary at bundle offset 0x10000.
 */
.org 0xE000, 0
.global _BootShimStackBottom
_BootShimStackBottom:
.space 0x2000, 0
.global _BootShimStackTop
_BootShimStackTop:

/* Fixed bundle contract: BootShim is exactly 64 KiB; FD starts at 0x10000. */
.org 0x10000, 0

.size _Head, . - _Head
