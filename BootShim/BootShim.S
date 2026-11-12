/*
 * ARM64 Linux Image wrapper for the SM-A205W Virtual Boot path.
 *
 * The Linux Image header is fixed at offsets 0..63. The UEFI FD is an
 * appended payload; its physical destination is NOT an executable entry
 * point. Until the SEC PE/COFF entry point is extracted and validated,
 * this shim deliberately stops after copying the FD instead of branching
 * blindly to the beginning of the firmware volume.
 */
        .section .text, "ax"
        .align  3
        .global _Head
_Head:
        b       _Start
        .long   0
        .quad   TEXT_OFFSET
        .quad   IMAGE_SIZE
        .quad   0
        .quad   0
        .quad   0
        .quad   0
        .ascii  "ARM\\x64"
        .long   0

_Start:
        /* Preserve x0: ARM64 Linux Image entry supplies the FDT address. */
        adr     x1, _Payload
        ldr     x5, =UEFI_BASE
        ldr     x6, =UEFI_SIZE

_CopyLoop:
        ldp     x2, x3, [x1], #16
        stp     x2, x3, [x5], #16
        subs    x6, x6, #16
        b.ne    _CopyLoop

_HandoffPending:
        /* UEFI_BASE is the FV header, not a proven SEC entry point. */
        dsb     sy
        isb
_Stop:
        wfe
        b       _Stop

        .align  4
_Payload:
        /* build_bootshim.sh appends the firmware FD immediately here. */
