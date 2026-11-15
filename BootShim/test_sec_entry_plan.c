/* SPDX-License-Identifier: MIT
 * Host-only synthetic plan tests; addresses are not device memory.
 */
#include <stdint.h>
#include <string.h>
#include "a20_handoff.h"
#include "sec_entry_plan.h"

static void put_le64(uint8_t *p, uint64_t v)
{
    unsigned int i;
    for (i = 0; i < 8; ++i)
        p[i] = (uint8_t)(v >> (i * 8));
}

static void make_descriptor(uint8_t *d)
{
    static const uint64_t addresses[5] = {
        0x10000000, 0x12000000, 0x14000000, 0x8fff0000, 0x90020000
    };
    unsigned int i, j;

    memset(d, 0, A20_HANDOFF_HEADER_SIZE);
    memcpy(d, A20_HANDOFF_MAGIC, 8);
    d[8] = 1;
    d[11] = 1;
    d[12] = 0x3f;
    for (i = 0; i < A20_HANDOFF_SEGMENT_COUNT; ++i) {
        uint8_t *s = d + 16 + i * 48;
        put_le64(s, addresses[i]);
        put_le64(s + 8, i == A20_HANDOFF_SEG_BOOTSHIM ? 0x10000 :
                        i == A20_HANDOFF_SEG_UEFI_FD ? 0x200000 : 0x1000);
        for (j = 0; j < 32; ++j)
            s[16 + j] = (uint8_t)(i + j + 1);
    }
}

static struct bootshim_sec_entry_evidence make_evidence(void)
{
    struct bootshim_sec_entry_evidence e = {
        .bootshim_base = 0x8fff0000,
        .fd_base = 0x90000000,
        .fd_size = 0x200000,
        .sec_entry = 0x900A0000,
        .stack_base = 0x8fff0000 + BOOTSHIM_STACK_OFFSET,
        .stack_size = BOOTSHIM_STACK_SIZE,
        .stack_pointer = 0x8fff0000 + BOOTSHIM_IMAGE_SIZE,
        .ranges_reserved = true,
        .stack_writable = true,
        .fd_executable = true,
        .cpu_state_verified = true,
        .dma_quiescent = true,
        .secondary_cores_quiescent = true
    };
    return e;
}

int main(void)
{
    uint8_t descriptor[A20_HANDOFF_HEADER_SIZE];
    struct bootshim_sec_entry_evidence e = make_evidence();
    struct bootshim_sec_entry_plan p;

    make_descriptor(descriptor);
    memset(&p, 0xa5, sizeof(p));
    if (bootshim_prepare_sec_entry_plan(descriptor, sizeof(descriptor),
                                        &e, &p) != 0)
        return 1;
    if (p.sec_entry != e.sec_entry || p.stack_pointer != e.stack_pointer ||
        p.fd_base != e.fd_base || p.fd_size != e.fd_size)
        return 2;

    e.dma_quiescent = false;
    memset(&p, 0xa5, sizeof(p));
    if (bootshim_prepare_sec_entry_plan(descriptor, sizeof(descriptor),
                                        &e, &p) == 0)
        return 3;
    if (p.sec_entry || p.stack_pointer || p.fd_base || p.fd_size)
        return 4;

    if (bootshim_prepare_sec_entry_plan(descriptor, sizeof(descriptor),
                                        &e, NULL) !=
        BOOTSHIM_SEC_HANDOFF_DESCRIPTOR)
        return 5;
    return 0;
}
