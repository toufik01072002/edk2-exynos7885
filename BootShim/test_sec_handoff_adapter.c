/* SPDX-License-Identifier: MIT
 * Host-only synthetic tests. No fixture address represents device RAM.
 */
#include <stdint.h>
#include <string.h>

#include "a20_handoff.h"
#include "sec_handoff_adapter.h"

static void put_le64(uint8_t *p, uint64_t value)
{
    unsigned int i;
    for (i = 0; i < 8; ++i)
        p[i] = (uint8_t)(value >> (i * 8));
}

static void fixture(uint8_t *d)
{
    unsigned int i, j;
    static const uint64_t addr[5] = {
        0x10000000, 0x12000000, 0x14000000, 0x8fff0000, 0x90020000
    };

    memset(d, 0, A20_HANDOFF_HEADER_SIZE);
    memcpy(d, A20_HANDOFF_MAGIC, 8);
    d[8] = 1;
    d[10] = 0;
    d[11] = 1;
    d[12] = 0x3f; /* all payload and hardware-evidence flags */
    for (i = 0; i < A20_HANDOFF_SEGMENT_COUNT; ++i) {
        uint8_t *s = d + 16 + i * 48;
        put_le64(s, addr[i]);
        put_le64(s + 8, i == A20_HANDOFF_SEG_BOOTSHIM ? 0x10000 :
                        i == A20_HANDOFF_SEG_UEFI_FD ? 0x200000 : 0x1000);
        for (j = 0; j < 32; ++j)
            s[16 + j] = (uint8_t)(i + j + 1);
    }
}

static struct bootshim_sec_entry_evidence evidence(void)
{
    struct bootshim_sec_entry_evidence e = {
        .bootshim_base = 0x8fff0000,
        .fd_base = 0x90020000,
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
    uint8_t d[A20_HANDOFF_HEADER_SIZE];
    struct bootshim_sec_entry_evidence e = evidence();

    fixture(d);
    if (bootshim_validate_sec_handoff(d, sizeof(d), &e) !=
        BOOTSHIM_SEC_HANDOFF_OK)
        return 1;

    d[12] = 0x07; /* no reservation/cache/EL proof */
    if (bootshim_validate_sec_handoff(d, sizeof(d), &e) !=
        BOOTSHIM_SEC_HANDOFF_FLAGS)
        return 2;

    fixture(d);
    put_le64(d + 16 + A20_HANDOFF_SEG_UEFI_FD * 48, 0x91000000);
    if (bootshim_validate_sec_handoff(d, sizeof(d), &e) !=
        BOOTSHIM_SEC_HANDOFF_SEGMENTS)
        return 3;

    fixture(d);
    e.dma_quiescent = false;
    if (bootshim_validate_sec_handoff(d, sizeof(d), &e) !=
        BOOTSHIM_SEC_HANDOFF_GATE)
        return 4;

    fixture(d);
    if (bootshim_validate_sec_handoff(NULL, sizeof(d), &e) !=
        BOOTSHIM_SEC_HANDOFF_DESCRIPTOR)
        return 5;
    return 0;
}
