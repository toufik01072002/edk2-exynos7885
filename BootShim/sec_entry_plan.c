/* SPDX-License-Identifier: MIT
 * Join descriptor validation and runtime evidence into one fail-closed plan.
 */
#include "sec_entry_plan.h"
#include "sec_handoff_adapter.h"

int bootshim_prepare_sec_entry_plan(
    const uint8_t *descriptor, size_t length,
    const struct bootshim_sec_entry_evidence *e,
    struct bootshim_sec_entry_plan *plan)
{
    int status;

    if (plan == NULL)
        return BOOTSHIM_SEC_HANDOFF_DESCRIPTOR;

    plan->sec_entry = 0;
    plan->stack_pointer = 0;
    plan->fd_base = 0;
    plan->fd_size = 0;

    status = bootshim_validate_sec_handoff(descriptor, length, e);
    if (status != BOOTSHIM_SEC_HANDOFF_OK)
        return status;

    plan->sec_entry = e->sec_entry;
    plan->stack_pointer = e->stack_pointer;
    plan->fd_base = e->fd_base;
    plan->fd_size = e->fd_size;
    return BOOTSHIM_SEC_HANDOFF_OK;
}
