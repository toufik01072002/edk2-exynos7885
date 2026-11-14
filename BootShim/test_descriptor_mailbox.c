/* SPDX-License-Identifier: MIT
 * Host test for the BootShim image-relative mailbox validation path.
 * The symbol is supplied here as a synthetic fixture, not physical memory.
 */
#include <stdint.h>
#include <string.h>
#include "a20_handoff.h"
#include "descriptor_mailbox.h"

uint8_t _HandoffDescriptorMailbox[BOOTSHIM_DESCRIPTOR_MAILBOX_SIZE];

static void put_le64(uint8_t *p, uint64_t value)
{
    unsigned int i;
    for (i = 0; i < 8; ++i)
        p[i] = (uint8_t)(value >> (i * 8));
}

static void make_fixture(void)
{
    static const uint64_t addresses[5] = {
        0x10000000u, 0x12000000u, 0x14000000u, 0x16000000u, 0x18000000u
    };
    unsigned int i, j;

    memset(_HandoffDescriptorMailbox, 0, sizeof(_HandoffDescriptorMailbox));
    memcpy(_HandoffDescriptorMailbox, A20_HANDOFF_MAGIC, 8);
    _HandoffDescriptorMailbox[8] = 1;
    _HandoffDescriptorMailbox[10] = 0;
    _HandoffDescriptorMailbox[11] = 1;
    _HandoffDescriptorMailbox[12] = 7;
    for (i = 0; i < A20_HANDOFF_SEGMENT_COUNT; ++i) {
        uint8_t *segment = _HandoffDescriptorMailbox + 16 + i * 48;
        put_le64(segment, addresses[i]);
        put_le64(segment + 8, 0x1000);
        for (j = 0; j < 32; ++j)
            segment[16 + j] = (uint8_t)(i + j + 1);
    }
}

int main(void)
{
    make_fixture();
    if (bootshim_validate_embedded_handoff_descriptor() != BOOTSHIM_DESCRIPTOR_OK)
        return 1;

    memset(_HandoffDescriptorMailbox, 0, sizeof(_HandoffDescriptorMailbox));
    if (bootshim_validate_embedded_handoff_descriptor() != BOOTSHIM_DESCRIPTOR_INVALID)
        return 2;

    make_fixture();
    _HandoffDescriptorMailbox[0] ^= 0xff;
    if (bootshim_validate_embedded_handoff_descriptor() != BOOTSHIM_DESCRIPTOR_INVALID)
        return 3;
    return 0;
}
