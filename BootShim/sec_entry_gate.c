/* SPDX-License-Identifier: MIT
 * Fail-closed SEC entry evidence validator; no memory access or branch.
 */
#include "sec_entry_gate.h"

#include <stddef.h>

static bool add_overflows(uint64_t base, uint64_t size)
{
    return size > UINT64_MAX - base;
}

static bool ranges_overlap(uint64_t a_start, uint64_t a_end,
                           uint64_t b_start, uint64_t b_end)
{
    return a_start < b_end && b_start < a_end;
}

int bootshim_validate_sec_entry_evidence(
    const struct bootshim_sec_entry_evidence *e)
{
    uint64_t shim_end, fd_end, stack_end;

    if (e == NULL)
        return BOOTSHIM_SEC_GATE_NULL;

    if (e->bootshim_base == 0 || e->fd_base == 0 ||
        e->fd_size == 0 || e->stack_base == 0)
        return BOOTSHIM_SEC_GATE_RANGE;

    if (add_overflows(e->bootshim_base, BOOTSHIM_IMAGE_SIZE) ||
        add_overflows(e->fd_base, e->fd_size) ||
        add_overflows(e->stack_base, e->stack_size))
        return BOOTSHIM_SEC_GATE_RANGE;

    shim_end = e->bootshim_base + BOOTSHIM_IMAGE_SIZE;
    fd_end = e->fd_base + e->fd_size;
    stack_end = e->stack_base + e->stack_size;

    /* The ECT window is 0x90000000-0x90020000. The fixed FD starts
     * immediately after it; the position-independent BootShim is staged
     * separately in RAM and must not overlap the FD destination.
     */
    if (e->fd_base != BOOTSHIM_FD_LINK_BASE ||
        e->fd_size != BOOTSHIM_FD_SIZE ||
        ranges_overlap(e->bootshim_base, shim_end, e->fd_base, fd_end))
        return BOOTSHIM_SEC_GATE_LAYOUT;

    if (e->stack_base != e->bootshim_base + BOOTSHIM_STACK_OFFSET ||
        e->stack_size != BOOTSHIM_STACK_SIZE ||
        stack_end != shim_end ||
        (e->stack_pointer & UINT64_C(0xF)) != 0 ||
        e->stack_pointer <= e->stack_base ||
        e->stack_pointer > stack_end)
        return BOOTSHIM_SEC_GATE_STACK;

    if (e->sec_entry < e->fd_base || e->sec_entry >= fd_end ||
        (e->sec_entry & UINT64_C(3)) != 0)
        return BOOTSHIM_SEC_GATE_ENTRY;

    if (!e->ranges_reserved || !e->stack_writable || !e->fd_executable ||
        !e->cpu_state_verified || !e->dma_quiescent ||
        !e->secondary_cores_quiescent)
        return BOOTSHIM_SEC_GATE_EVIDENCE;

    return BOOTSHIM_SEC_GATE_OK;
}
