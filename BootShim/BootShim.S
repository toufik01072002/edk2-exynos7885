/*
 * SM-A205W - EDK2 BootShim
 *
 * Validation-only pre-SEC path. The descriptor mailbox is structurally
 * validated before any SEC transfer could be considered. Even a valid
 * descriptor is not sufficient: runtime placement, CPU state, DMA quiescence,
 * and secondary-core quiescence are not yet established. Therefore this
 * image deliberately halts after validation and never branches to SEC.
 *
 * Fixed image layout:
 *   0x0000        position-independent entry
 *   0x1000-0xCFFF freestanding descriptor validator code
 *   0xD000        256-byte handoff descriptor mailbox
 *   0xD100        patched SEC entry bundle offset (not executed)
 *   0xE000-0xFFFF private 8 KiB stack
 */
.section .text.entry,"ax"
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

    /* Validate the embedded v1 descriptor through the linked C consumer. */
    bl      bootshim_validate_embedded_handoff_descriptor
    cbnz    w0, _DescriptorFault

    /*
     * Structural validity is not runtime handoff authorization.
     * Stay fail-closed until an independent runtime evidence provider and
     * machine-state transition have been implemented and validated.
     */
_RuntimeEvidenceUnavailable:
    wfe
    b       _RuntimeEvidenceUnavailable

_DescriptorFault:
    wfe
    b       _DescriptorFault

_StackFault:
    wfe
    b       _StackFault

.section .mailbox,"aw",%progbits
.balign 16
.global _HandoffDescriptorMailbox
.type _HandoffDescriptorMailbox, %object
_HandoffDescriptorMailbox:
.space 0x100, 0
.size _HandoffDescriptorMailbox, . - _HandoffDescriptorMailbox

/* Patched with 0x10000 + actual PrePi SEC entry offset; not executed yet. */
.section .sec_offset,"a",%progbits
.balign 8
.global _SecEntryOffset
.type _SecEntryOffset, %object
_SecEntryOffset:
.quad 0
.size _SecEntryOffset, . - _SecEntryOffset

.section .stack,"aw",%nobits
.balign 16
.global _BootShimStackBottom
_BootShimStackBottom:
.space 0x2000
.global _BootShimStackTop
_BootShimStackTop:
