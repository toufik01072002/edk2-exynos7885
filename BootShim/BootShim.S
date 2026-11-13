/*
 * SM-A205W - EDK2 BootShim
 *
 * SAFE MODE:
 * - No Linux Image header imitation.
 * - No memory copy.
 * - No fixed physical address.
 * - No branch to the firmware.
 *
 * Execution remains blocked until the actual
 * bootloader handoff protocol is implemented.
 */

.section .text
.balign 4
.global _Head
.type _Head, %function

_Head:
    b       _Disabled

.balign 4
_Disabled:
    wfe
    b       _Disabled

/*
 * Keep a minimum 256-byte artifact for bundle validation.
 */
.space 244, 0

/* Fixed bundle contract: FD begins at offset 0x10000. */
.org 0x10000, 0

.size _Head, . - _Head
