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

    /* 2. Effacer l'écran en noir (720x1560x4 = 4 492 800 octets) */
    movz    x0, #0x0000
    movk    x0, #0xEC00, lsl #16
    ldr     x1, _FbTotalSize
    movz    x2, #0x0000
    movk    x2, #0xFF00, lsl #16
    orr     x2, x2, x2, lsl #32
_ClearBlack:
    str     x2, [x0], #8
    subs    x1, x1, #8
    b.gt    _ClearBlack

    /* 3. Dessiner le logo 256x256 BGRA centré à (X=232, Y=652) */
    movz    x0, #0x0000
    movk    x0, #0xEC00, lsl #16
    ldr     x1, _LogoFbOffset
    add     x0, x0, x1              /* x0 = destination ligne courante FB */
    adr     x2, _LogoData           /* x2 = source pixels BGRA */
    mov     x3, #256                /* x3 = compteur lignes (H = 256) */

_DrawLogoRow:
    mov     x7, x0                  /* x7 = curseur pixel dans la ligne FB */
    mov     x9, #256                /* x9 = compteur colonnes (W = 256) */
_DrawLogoPixel:
    ldr     w8, [x2], #4
    str     w8, [x7], #4
    subs    x9, x9, #1
    b.gt    _DrawLogoPixel

    add     x0, x0, #2880           /* Stride écran = 720 * 4 = 2880 octets */
    subs    x3, x3, #1
    b.gt    _DrawLogoRow

    /* 5. Barrières mémoire/instruction et saut vers UEFI (0x90000000) */
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
_FbTotalSize:
    .quad   4492800                 /* 720 * 1560 * 4 */
_LogoFbOffset:
    .quad   1878688                 /* ((1560 - 256)/2 * 720 + (720 - 256)/2) * 4 */

.balign 8
_LogoData:
    .incbin "logo.bin"

.balign 256
_ShimEnd:
.size _Head, . - _Head
