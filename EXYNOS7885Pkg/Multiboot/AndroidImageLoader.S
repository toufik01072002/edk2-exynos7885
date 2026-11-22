.text
.align 4
.global Arm64LinuxHandoff
.type Arm64LinuxHandoff, %function

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

    /* Disable the MMU and caches at the current exception level. */
    mrs     x9, CurrentEL
    cmp     x9, #0x8
    b.eq    .Lel2
    cmp     x9, #0x4
    b.eq    .Lel1
    b       .Lhalt

.Lel2:
    mrs     x9, hcr_el2
    orr     x9, x9, #(1 << 31)
    msr     hcr_el2, x9

    mov     x9, #3
    msr     cnthctl_el2, x9
    msr     cntvoff_el2, xzr
    msr     cptr_el2, xzr

    mrs     x9, sctlr_el2
    bic     x9, x9, #1
    bic     x9, x9, #4
    bic     x9, x9, #0x1000
    msr     sctlr_el2, x9
    dsb     sy
    tlbi    alle2
    tlbi    alle1
    dsb     sy
    isb
    b       .Lbranch

.Lel1:
    mrs     x9, sctlr_el1
    bic     x9, x9, #1
    bic     x9, x9, #4
    bic     x9, x9, #0x1000
    msr     sctlr_el1, x9
    dsb     sy
    tlbi    vmalle1
    dsb     sy
    isb

.Lbranch:
    ic      iallu
    dsb     sy
    isb

    /* Linux arm64 boot protocol: x0 = DTB, x1-x3 = 0. */
    mov     x0, x23
    mov     x1, xzr
    mov     x2, xzr
    mov     x3, xzr
    br      x19

.Lhalt:
    wfe
    b       .Lhalt

/* Clean [x0, x0+x1) using the data-cache line size from CTR_EL0. */
.Lclean_range:
    cbz     x1, .Lclean_done
    mrs     x9, ctr_el0
    ubfx    x10, x9, #16, #4
    mov     x11, #4
    lsl     x11, x11, x10
    sub     x12, x11, #1
    bic     x0, x0, x12
    add     x12, x0, x1
.Lclean_loop:
    dc      cvac, x0
    add     x0, x0, x11
    cmp     x0, x12
    b.lo    .Lclean_loop
.Lclean_done:
    ret

.size Arm64LinuxHandoff, . - Arm64LinuxHandoff
