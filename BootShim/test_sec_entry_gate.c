/* SPDX-License-Identifier: MIT
 * Host-only tests. All addresses below are synthetic fixtures, not device
 * reservations or recommended runtime placements.
 */
#include "sec_entry_gate.h"

#include <string.h>

static void valid_fixture(struct bootshim_sec_entry_evidence *e)
{
    memset(e, 0, sizeof(*e));
    e->bootshim_base = UINT64_C(0x8FFF0000);
    e->fd_base = BOOTSHIM_FD_LINK_BASE;
    e->fd_size = BOOTSHIM_FD_SIZE;
    e->sec_entry = BOOTSHIM_FD_LINK_BASE + UINT64_C(0x80000);
    e->stack_base = e->bootshim_base + BOOTSHIM_STACK_OFFSET;
    e->stack_size = BOOTSHIM_STACK_SIZE;
    e->stack_pointer = e->stack_base + e->stack_size;
    e->ranges_reserved = true;
    e->stack_writable = true;
    e->fd_executable = true;
    e->cpu_state_verified = true;
    e->dma_quiescent = true;
    e->secondary_cores_quiescent = true;
}

#define CHECK(expr, code) do { if (!(expr)) return (code); } while (0)

int main(void)
{
    struct bootshim_sec_entry_evidence e;
    valid_fixture(&e);
    CHECK(bootshim_validate_sec_entry_evidence(&e) == BOOTSHIM_SEC_GATE_OK, 1);
    CHECK(bootshim_validate_sec_entry_evidence(NULL) == BOOTSHIM_SEC_GATE_NULL, 2);

    e.fd_base++;
    CHECK(bootshim_validate_sec_entry_evidence(&e) == BOOTSHIM_SEC_GATE_LAYOUT, 3);
    valid_fixture(&e);
    e.stack_pointer--;
    CHECK(bootshim_validate_sec_entry_evidence(&e) == BOOTSHIM_SEC_GATE_STACK, 4);
    valid_fixture(&e);
    e.sec_entry = e.fd_base + e.fd_size;
    CHECK(bootshim_validate_sec_entry_evidence(&e) == BOOTSHIM_SEC_GATE_ENTRY, 5);
    valid_fixture(&e);
    e.bootshim_base = UINT64_MAX - UINT64_C(0x7FFF);
    CHECK(bootshim_validate_sec_entry_evidence(&e) == BOOTSHIM_SEC_GATE_RANGE, 6);

    valid_fixture(&e); e.ranges_reserved = false;
    CHECK(bootshim_validate_sec_entry_evidence(&e) == BOOTSHIM_SEC_GATE_EVIDENCE, 7);
    valid_fixture(&e); e.stack_writable = false;
    CHECK(bootshim_validate_sec_entry_evidence(&e) == BOOTSHIM_SEC_GATE_EVIDENCE, 8);
    valid_fixture(&e); e.fd_executable = false;
    CHECK(bootshim_validate_sec_entry_evidence(&e) == BOOTSHIM_SEC_GATE_EVIDENCE, 9);
    valid_fixture(&e); e.cpu_state_verified = false;
    CHECK(bootshim_validate_sec_entry_evidence(&e) == BOOTSHIM_SEC_GATE_EVIDENCE, 10);
    valid_fixture(&e); e.dma_quiescent = false;
    CHECK(bootshim_validate_sec_entry_evidence(&e) == BOOTSHIM_SEC_GATE_EVIDENCE, 11);
    valid_fixture(&e); e.secondary_cores_quiescent = false;
    CHECK(bootshim_validate_sec_entry_evidence(&e) == BOOTSHIM_SEC_GATE_EVIDENCE, 12);

    return 0;
}
