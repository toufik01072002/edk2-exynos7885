/*
 * SM-A205W ARM64 handoff trampoline — fail-closed placeholder.
 *
 * The previous prototype made unverified assumptions about:
 * - x0 carrying the DTB after the vendor kernel's kexec relocation;
 * - the GIC distributor/CPU-interface MMIO addresses and GIC version;
 * - PSCI CPU_OFF being callable for CPUs other than the calling CPU;
 * - the fixed physical location 0x88000000, which collides with the DTB.
 *
 * Do not replace this guard with a guessed handoff sequence. The vendor
 * kernel's arch/arm64/kernel/relocate_kernel.S currently clears x0-x3
 * before branching to kimage_start. A handoff block must be passed through
 * an explicitly designed and tested kernel/TWRP contract.
 *
 * This stub is position-independent and intentionally never enters EDK2.
 */

.section .text, "ax"
.balign 4
.global _trampoline_entry
.type _trampoline_entry, %function

_trampoline_entry:
.Lhandoff_disabled:
    wfe
    b       .Lhandoff_disabled

.size _trampoline_entry, . - _trampoline_entry
