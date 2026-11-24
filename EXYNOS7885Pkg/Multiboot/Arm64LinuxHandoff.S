.text
.align 4
.global Arm64LinuxHandoff
.type Arm64LinuxHandoff, %function

/*
 * Store a stage code in the diagnostic mailbox without calling firmware.
 * Layout: magic, version, stage, inverse, payload addresses, counter.
 */
.macro HANDOFF_DIAG stage
    ldr     x10, =0x8d2ff000
    movz    w9, #\stage
    str     w9, [x10, #8]
    mvn     w11, w9
    str     w11, [x10, #12]
    ldr     w11, [x10, #40]
    add     w11, w11, #1
    str     w11, [x10, #40]
    dc      cvac, x10
    dsb     sy
.endm


/*
 * x0 = kernel entry
 * x1 = kernel image size
 * x2 = initrd physical address
 * x3 = initrd size
 * x4 = DTB physical address
 * x5 = DTB size
 *
 * Called only after ExitBootServices(). Does not return.
 */
Arm64LinuxHandoff:
    mov     x19, x0
    mov     x20, x1
    mov     x21, x2
    mov     x22, x3
    mov     x23, x4
    mov     x24, x5

    HANDOFF_DIAG 0xA001
    msr     daifset, #0xf

    /* Clean all payload ranges to the point of coherency. */
    mov     x0, x19
    mov     x1, x20
    bl      .Lclean_range

    mov     x0, x21
    mov     x1, x22
    bl      .Lclean_range

    mov     x0, x23
    mov     x1, x24
    bl      .Lclean_range

    dsb     sy
    HANDOFF_DIAG 0xA002

    /* Invalidate stale instructions while firmware mappings are active. */
    ic      iallu
    dsb     sy
    isb

    /* Disable the MMU and caches at the current exception level. */
    mrs     x9, CurrentEL
    HANDOFF_DIAG 0xA003
    cmp     x9, #0x8
    b.eq    .Lel2
    cmp     x9, #0x4
    b.eq    .Lel1
    b       .Lhalt

.Lel2:
    /* Enter Linux as non-VHE AArch64 EL2: RW=1, all other HCR controls clear. */
    movz    x9, #0x8000, lsl #16
    msr     hcr_el2, x9
    isb

    mov     x9, #3
    msr     cnthctl_el2, x9
    msr     cntvoff_el2, xzr
    msr     cptr_el2, xzr

    HANDOFF_DIAG 0xA004
    mrs     x9, sctlr_el2
    bic     x9, x9, #1
    bic     x9, x9, #4
    bic     x9, x9, #0x1000
    dsb     sy
    msr     sctlr_el2, x9
    isb
    tlbi    alle2
    tlbi    alle1
    dsb     sy
    isb
    b       .Lbranch

.Lel1:
    HANDOFF_DIAG 0xA004
    mrs     x9, sctlr_el1
    bic     x9, x9, #1
    bic     x9, x9, #4
    bic     x9, x9, #0x1000
    dsb     sy
    msr     sctlr_el1, x9
    isb
    tlbi    vmalle1
    dsb     sy
    isb

.Lbranch:
    /* Linux arm64 boot protocol: x0 = DTB, x1-x3 = 0. */
    mov     x0, x23
    mov     x1, xzr
    mov     x2, xzr
    mov     x3, xzr
    br      x19

.Lhalt:
    HANDOFF_DIAG 0xA0FF
    wfe
    b       .Lhalt

/* Clean [x0, x0+x1) using the data-cache line size from CTR_EL0. */
.Lclean_range:
    cbz     x1, .Lclean_done
    add     x12, x0, x1
    mrs     x9, ctr_el0
    ubfx    x10, x9, #16, #4
    mov     x11, #4
    lsl     x11, x11, x10
    sub     x13, x11, #1
    add     x12, x12, x13
    bic     x12, x12, x13
    bic     x0, x0, x13
.Lclean_loop:
    dc      cvac, x0
    add     x0, x0, x11
    cmp     x0, x12
    b.lo    .Lclean_loop
.Lclean_done:
    ret

.size Arm64LinuxHandoff, . - Arm64LinuxHandoff
