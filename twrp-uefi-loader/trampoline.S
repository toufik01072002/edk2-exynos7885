/*
 * trampoline.S - ARM64 trampoline for Linux → UEFI transition
 *
 * This code runs in kernel context (via kexec) and prepares the hardware
 * state for UEFI/PrePi entry at 0x90000000.
 *
 * Requirements:
 * - MMU must be OFF before branching to UEFI entry
 * - Caches clean + invalidated
 * - Secondary CPUs powered down (PSCI CPU_OFF)
 * - GIC interrupts masked
 * - x0 = DTB pointer (Linux boot protocol)
 * - x1-x3 = 0
 *
 * Build: aarch64-linux-gnu-gcc -c -nostdlib trampoline.S -o trampoline.o
 * Link:  aarch64-linux-gnu-ld -T trampoline.ld trampoline.o -o trampoline
 */

#define UEFI_ENTRY_ADDR     0x90000000
#define UEFI_FD_SIZE        0x00200000

/* System registers */
#define SCTLR_EL1           "sctlr_el1"
#define MAIR_EL1            "mair_el1"
#define TCR_EL1             "tcr_el1"
#define TTBR0_EL1           "ttbr0_el1"
#define TTBR1_EL1           "ttbr1_el1"
#define CPACR_EL1           "cpacr_el1"
#define HCR_EL2             "hcr_el2"
#define VBAR_EL1            "vbar_el1"
#define ESR_EL1             "esr_el1"
#define FAR_EL1             "far_el1"

#define SPSR_EL1            "spsr_el1"
#define ELR_EL1             "elr_el1"
#define SP_EL1              "sp_el1"
#define SP_EL0              "sp_el0"

#define SCTLR_M_BIT         (1 << 0)      /* MMU enable */
#define SCTLR_C_BIT         (1 << 2)      /* Cache enable */
#define SCTLR_I_BIT         (1 << 12)     /* Instruction cache */

/* GIC registers (GICv2/v3) */
#define GICD_BASE           0x02F00000    /* Exynos7885 GIC Distributor */
#define GICC_BASE           0x02F10000    /* Exynos7885 GIC CPU Interface */
#define GICD_CTLR           0x0000
#define GICD_IGROUPR        0x0080
#define GICD_ISENABLER      0x0100
#define GICD_ICENABLER      0x0180
#define GICD_ISPENDR        0x0200
#define GICD_ICPENDR        0x0280
#define GICD_ISACTIVER      0x0300
#define GICD_ICACTIVER      0x0380
#define GICD_IPRIORITYR     0x0400
#define GICD_ITARGETSR      0x0800
#define GICD_ICFGR          0x0C00

#define GICC_CTLR           0x0000
#define GICC_PMR            0x0004
#define GICC_BPR            0x0008
#define GICC_IAR            0x000C
#define GICC_EOIR           0x0010
#define GICC_RPR            0x0014
#define GICC_HPPIR          0x0018
#define GICC_ABPR           0x001C
#define GICC_AIAR           0x0020
#define GICC_AEOIR          0x0024
#define GICC_APR            0x00D0
#define GICC_NSAPR          0x00E0

/* PSCI function IDs */
#define PSCI_CPU_OFF        0x84000002
#define PSCI_CPU_ON         0x84000003
#define PSCI_SYSTEM_RESET   0x84000009

/* Cache maintenance */
#define DC_CIVAC            "dc civac, "
#define DC_CVAC             "dc cvac, "
#define IC_IVAU             "ic ivau, "
#define TLBI_VMALLE1IS      "tlbi vmalle1is"
#define DSB_SY              "dsb sy"
#define ISB_SY              "isb sy"
#define DMB_SY              "dmb sy"

/* Stack size for trampoline */
#define TRAMPOLINE_STACK_SIZE   0x2000  /* 8 KiB */

/* Entry point from kexec (x0 = DTB pointer) */
.text
.global _trampoline_entry
.type _trampoline_entry, %function

_trampoline_entry:
    /*
     * Entry from kexec: x0 = DTB pointer (physical address)
     * We are in EL1, MMU on, caches on, interrupts enabled
     * All secondary CPUs should already be stopped by kexec,
     * but we verify and force them off.
     */

    /* Save DTB pointer for later */
    mov     x19, x0                     /* x19 = DTB pointer */

    /* ------------------------------------------------------------
     * 1. Disable interrupts at GIC (mask all)
     * ------------------------------------------------------------*/
    bl      _gic_mask_all_interrupts

    /* ------------------------------------------------------------
     * 2. Power down secondary CPUs via PSCI
     * ------------------------------------------------------------*/
    bl      _psci_power_down_secondaries

    /* ------------------------------------------------------------
     * 3. Save critical system register state (for debugging/rollback)
     * ------------------------------------------------------------*/
    bl      _save_sysregs

    /* ------------------------------------------------------------
     * 4. Clean and invalidate caches (L1 + L2)
     * ------------------------------------------------------------*/
    bl      _clean_invalidate_caches

    /* ------------------------------------------------------------
     * 5. Disable MMU and caches (SCTLR_EL1)
     * ------------------------------------------------------------*/
    bl      _disable_mmu_caches

    /* ------------------------------------------------------------
     * 6. Invalidate TLBs
     * ------------------------------------------------------------*/
    bl      _invalidate_tlbs

    /* ------------------------------------------------------------
     * 7. Prepare register state for UEFI entry (Linux boot protocol)
     *    x0 = DTB pointer
     *    x1 = 0
     *    x2 = 0
     *    x3 = 0
     * ------------------------------------------------------------*/
    mov     x0, x19                     /* DTB pointer */
    mov     x1, xzr
    mov     x2, xzr
    mov     x3, xzr

    /* ------------------------------------------------------------
     * 8. Branch to UEFI entry point (BootShim at 0x90000000)
     *    This never returns.
     * ------------------------------------------------------------*/
    ldr     x4, =UEFI_ENTRY_ADDR
    br      x4

    /* Should never reach here */
    b       .


/* ============================================================
 * GIC: Mask all interrupts
 * ============================================================ */
_gic_mask_all_interrupts:
    /* GIC Distributor: disable all interrupts */
    ldr     x0, =GICD_BASE
    /* Disable Group 0 and Group 1 */
    mov     x1, #0
    str     x1, [x0, #GICD_CTLR]

    /* Clear all enable registers (32 interrupts per register, 1024 max) */
    mov     x2, #32                     /* 32 registers = 1024 interrupts */
    add     x3, x0, #GICD_ICENABLER
1:
    str     xzr, [x3], #4
    subs    x2, x2, #1
    b.ne    1b

    /* GIC CPU Interface: set priority mask to lowest (mask all) */
    ldr     x0, =GICC_BASE
    mov     x1, #0xFF                   /* Lowest priority = mask all */
    str     x1, [x0, #GICC_PMR]

    /* Disable GIC CPU interface */
    mov     x1, #0
    str     x1, [x0, #GICC_CTLR]

    dsb     sy
    isb     sy
    ret


/* ============================================================
 * PSCI: Power down all secondary CPUs
 * ============================================================ */
_psci_power_down_secondaries:
    /* Read MPIDR to identify current CPU */
    mrs     x0, mpidr_el1
    and     x0, x0, #0xFFFFFF           /* Affinity bits only */

    /* For each possible CPU (0-3 for Exynos7885 quad-core) */
    /* In practice, kexec should have stopped them, but we ensure */
    mov     x1, #0                      /* CPU index */
_psci_loop:
    cmp     x1, #4
    b.ge    _psci_done

    /* Skip current CPU */
    mov     x2, x1
    orr     x2, x2, #(1 << 24)          /* MPIDR format: Aff0=CPU, Aff1=Cluster */
    cmp     x0, x2
    beq     _psci_next

    /* Call PSCI CPU_OFF for this CPU */
    mov     x0, #PSCI_CPU_OFF
    mov     x1, x2                      /* target CPU MPIDR */
    mov     x2, xzr
    mov     x3, xzr
    hvc     #0                          /* PSCI call */

_psci_next:
    add     x1, x1, #1
    b       _psci_loop

_psci_done:
    ret


/* ============================================================
 * Save critical system registers (stored in trampoline_stack area)
 * ============================================================ */
_save_sysregs:
    /* We reserve space at the top of our stack for saved state */
    /* This is for debugging - not restored on this path */
    mrs     x1, SCTLR_EL1
    mrs     x2, MAIR_EL1
    mrs     x3, TCR_EL1
    mrs     x4, TTBR0_EL1
    mrs     x5, TTBR1_EL1
    mrs     x6, CPACR_EL1
    mrs     x7, HCR_EL2
    mrs     x8, VBAR_EL1
    mrs     x9, ESR_EL1
    mrs     x10, FAR_EL1
    mrs     x11, SPSR_EL1
    mrs     x12, ELR_EL1
    mrs     x13, SP_EL1
    /* Store to stack (we use SP which points to our trampoline stack) */
    stp     x1, x2, [sp, #-16]!
    stp     x3, x4, [sp, #-16]!
    stp     x5, x6, [sp, #-16]!
    stp     x7, x8, [sp, #-16]!
    stp     x9, x10, [sp, #-16]!
    stp     x11, x12, [sp, #-16]!
    str     x13, [sp, #-16]!
    ret


/* ============================================================
 * Clean and invalidate data cache + invalidate instruction cache
 * for the entire UEFI FD region (0x90000000 - 0x90200000)
 * ============================================================ */
_clean_invalidate_caches:
    ldr     x0, =UEFI_ENTRY_ADDR        /* Start address */
    ldr     x1, =UEFI_FD_SIZE           /* Size */
    add     x1, x1, x0                  /* End address */

    /* Align to cache line (64 bytes) */
    bic     x0, x0, #63
    add     x1, x1, #63
    bic     x1, x1, #63

_cache_loop:
    cmp     x0, x1
    b.ge    _cache_done

    /* Clean + invalidate D-cache line */
    dc      civac, x0
    /* Invalidate I-cache line */
    ic      ivau, x0

    add     x0, x0, #64
    b       _cache_loop

_cache_done:
    dsb     sy
    isb     sy
    ret


/* ============================================================
 * Disable MMU and caches via SCTLR_EL1
 * ============================================================ */
_disable_mmu_caches:
    mrs     x0, SCTLR_EL1
    /* Clear M (MMU), C (D-cache), I (I-cache) bits */
    bic     x0, x0, #(SCTLR_M_BIT | SCTLR_C_BIT | SCTLR_I_BIT)
    msr     SCTLR_EL1, x0
    isb     sy
    ret


/* ============================================================
 * Invalidate all TLBs (VMALLE1IS = all stage 1 entries, inner shareable)
 * ============================================================ */
_invalidate_tlbs:
    tlbi    vmalle1is
    dsb     sy
    isb     sy
    ret


/* ============================================================
 * Stack space for trampoline execution
 * ============================================================ */
.section .bss.stack, "aw", %nobits
.global trampoline_stack
.align 16
trampoline_stack:
    .skip   TRAMPOLINE_STACK_SIZE
trampoline_stack_top:


/* ============================================================
 * Symbol exports for linker script
 * ============================================================ */
.global _trampoline_entry
.global trampoline_stack
.global trampoline_stack_top