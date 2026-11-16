/* SPDX-License-Identifier: MIT
 * Static pre-SEC evidence gate. Validation only; never transfers control.
 */
#ifndef BOOTSHIM_SEC_ENTRY_GATE_H
#define BOOTSHIM_SEC_ENTRY_GATE_H

#include <stdbool.h>
#include <stdint.h>

#define BOOTSHIM_IMAGE_SIZE       UINT64_C(0x10000)
#define BOOTSHIM_STACK_OFFSET     UINT64_C(0xE000)
#define BOOTSHIM_STACK_SIZE       UINT64_C(0x2000)
#define BOOTSHIM_FD_LINK_BASE     UINT64_C(0x90020000)
#define BOOTSHIM_FD_SIZE          UINT64_C(0x200000)

enum bootshim_sec_gate_status {
    BOOTSHIM_SEC_GATE_OK = 0,
    BOOTSHIM_SEC_GATE_NULL = -1,
    BOOTSHIM_SEC_GATE_RANGE = -2,
    BOOTSHIM_SEC_GATE_LAYOUT = -3,
    BOOTSHIM_SEC_GATE_STACK = -4,
    BOOTSHIM_SEC_GATE_ENTRY = -5,
    BOOTSHIM_SEC_GATE_EVIDENCE = -6
};

/* All addresses are supplied by a future runtime loader. Test fixtures are
 * synthetic. Boolean evidence must come from independent runtime checks.
 */
struct bootshim_sec_entry_evidence {
    uint64_t bootshim_base;
    uint64_t fd_base;
    uint64_t fd_size;
    uint64_t sec_entry;
    uint64_t stack_base;
    uint64_t stack_size;
    uint64_t stack_pointer;
    bool ranges_reserved;
    bool stack_writable;
    bool fd_executable;
    bool cpu_state_verified;
    bool dma_quiescent;
    bool secondary_cores_quiescent;
};

/* Returns OK only when geometry and every runtime-evidence gate pass.
 * This function does not itself establish any evidence or branch to SEC.
 */
int bootshim_validate_sec_entry_evidence(
    const struct bootshim_sec_entry_evidence *evidence);

#endif
