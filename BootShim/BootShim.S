/*
 * SM-A205W - EDK2 BootShim
 * Position-independent entry: establish a private stack, then branch to the
 * PrePi SEC entry offset embedded by the post-EDK2 bundle build.
 * The combined UEFI bundle is also carried in the Virtual Boot initramfs.
 *
 * 0x0000-0xCFFF code; 0xD000 descriptor mailbox; 0xD100 SEC offset;
 * 0xE000-0xFFFF private 8 KiB stack; FD begins at bundle offset 0x10000.
 */
.section .text
.balign 16
.global _Head
.type _Head, %function
_Head:
    b       _PrepareStack

_PrepareStack:
    adr     x9, _BootShimStackTop
    mov     sp, x9
    and     x10, x9, #0xF
    cbnz    x10, _StackFault

    /* Resolve the real PrePi SEC entry relative to the loaded bundle base. */
    adr     x16, _Head
    adr     x17, _SecEntryOffset
    ldr     x17, [x17]
    add     x16, x16, x17
    br      x16

_StackFault:
    wfe
    b       _StackFault

.org 0xD000, 0
.balign 16
.global _HandoffDescriptorMailbox
.type _HandoffDescriptorMailbox, %object
_HandoffDescriptorMailbox:
.space 0x100, 0
.size _HandoffDescriptorMailbox, . - _HandoffDescriptorMailbox

/* Patched with 0x10000 + the actual SEC entry offset inside the FD. */
.org 0xD100, 0
.balign 8
.global _SecEntryOffset
.type _SecEntryOffset, %object
_SecEntryOffset:
.quad 0
.size _SecEntryOffset, . - _SecEntryOffset

.org 0xE000, 0
.global _BootShimStackBottom
_BootShimStackBottom:
.space 0x2000, 0
.global _BootShimStackTop
_BootShimStackTop:

.org 0x10000, 0
.size _Head, . - _Head
