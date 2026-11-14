/*
 * SM-A205W - EDK2 BootShim
 *
 * SAFE PREPARATION MODE:
 * - Position-independent; no fixed physical address.
 * - Reserves a 256-byte descriptor mailbox inside the image.
 * - Establishes a private stack inside its own 64 KiB image.
 * - Does not populate the mailbox, copy the FD, or branch to SEC.
 * - Remains blocked in WFE after stack setup.
 *
 * Image layout:
 *   0x0000-0xCFFF  executable shim code / reserved image space
 *   0xD000-0xD0FF  handoff descriptor mailbox (256 bytes)
 *   0xD100-0xDFFF  reserved
 *   0xE000-0xFFFF  private 8 KiB stack
 *   0x10000        FD begins in the bundle
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
 * Fixed image-relative mailbox. A future producer may populate these bytes
 * before the BootShim image is handed to kexec. The consumer must locate it
 * PC-relatively; no x0-x3 argument register is assumed to survive kexec.
 * All-zero contents mean "no descriptor" and must fail validation.
 */
.org 0xD000, 0
.balign 16
.global _HandoffDescriptorMailbox
.type _HandoffDescriptorMailbox, %object
_HandoffDescriptorMailbox:
.space 0x100, 0
.size _HandoffDescriptorMailbox, . - _HandoffDescriptorMailbox

/* Reserve an 8 KiB writable stack at image offsets 0xE000-0xFFFF. */
.org 0xE000, 0
.global _BootShimStackBottom
_BootShimStackBottom:
.space 0x2000, 0
.global _BootShimStackTop
_BootShimStackTop:

/* Fixed bundle contract: BootShim is exactly 64 KiB; FD starts at 0x10000. */
.org 0x10000, 0

.size _Head, . - _Head
