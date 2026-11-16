/* SPDX-License-Identifier: MIT
 * Validated SEC transfer parameters. Preparation only; no branch occurs.
 */
#ifndef BOOTSHIM_SEC_ENTRY_PLAN_H
#define BOOTSHIM_SEC_ENTRY_PLAN_H

#include <stddef.h>
#include <stdint.h>
#include "sec_handoff_adapter.h"

struct bootshim_sec_entry_plan {
    uint64_t sec_entry;
    uint64_t stack_pointer;
    uint64_t fd_base;
    uint64_t fd_size;
};

/* Clears plan on every failure. A successful plan is metadata only and must
 * not be executed until a real transport and machine-state transition exist.
 */
int bootshim_prepare_sec_entry_plan(
    const uint8_t *descriptor, size_t length,
    const struct bootshim_sec_entry_evidence *evidence,
    struct bootshim_sec_entry_plan *plan);

#endif
