.section .text
.balign 8
.global _Head
.type _Head, %function

_Head:
    mov     x19, x0
    msr     daifset, #0xf

    /* 1. Copier EXYNOS7885PKG_UEFI.fd (_ShimEnd) vers 0x90000000 */
    adr     x2, _ShimEnd
    ldr     x4, _UefiBase
    ldr     x5, _UefiSize
    mov     x6, x4

_CopyUefi:
    ldr     x8, [x2], #8
    str     x8, [x6], #8
    subs    x5, x5, #8
    b.gt    _CopyUefi

    /* 2. Dessiner un rectangle blanc APRES la copie vers 0x90000000 */
    movz    x0, #0x0000
    movk    x0, #0xEC00, lsl #16
    mov     x1, #0x00200000
    mov     w2, #0xFFFFFFFF
_FillWhite:
    str     w2, [x0], #4
    subs    x1, x1, #4
    b.gt    _FillWhite

    /* 3. Sauter à 0x90000000 */
    dsb     sy
    ic      iallu
    dsb     sy
    isb
    mov     x0, x19
    br      x4

.balign 8
_UefiBase:
    .quad   UEFI_BASE
_UefiSize:
    .quad   UEFI_SIZE

.balign 256
_ShimEnd:
.size _Head, . - _Head
