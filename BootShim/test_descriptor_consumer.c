/* SPDX-License-Identifier: MIT
 * Host-only consumer boundary tests. Fixture addresses are synthetic.
 */
#include <stdint.h>
#include <string.h>
#include "a20_handoff.h"
#include "descriptor_consumer.h"

static void put_le64(uint8_t *p, uint64_t value)
{
    unsigned int i;
    for (i = 0; i < 8; ++i)
        p[i] = (uint8_t)(value >> (i * 8));
}

static void make_fixture(uint8_t *buffer)
{
    static const uint64_t addresses[5] = {
        0x10000000u, 0x12000000u, 0x14000000u, 0x16000000u, 0x18000000u
    };
    unsigned int i, j;

    memset(buffer, 0, A20_HANDOFF_HEADER_SIZE);
    memcpy(buffer, A20_HANDOFF_MAGIC, 8);
    buffer[8] = 1;
    buffer[10] = 0;
    buffer[11] = 1;
    buffer[12] = 7; /* payload metadata only; no hardware-ready flags */
    for (i = 0; i < A20_HANDOFF_SEGMENT_COUNT; ++i) {
        uint8_t *segment = buffer + 16 + i * 48;
        put_le64(segment, addresses[i]);
        put_le64(segment + 8, 0x1000);
        for (j = 0; j < 32; ++j)
            segment[16 + j] = (uint8_t)(i + j + 1);
    }
}

int main(void)
{
    uint8_t descriptor[A20_HANDOFF_HEADER_SIZE];

    make_fixture(descriptor);
    if (bootshim_validate_handoff_descriptor(
            descriptor, sizeof(descriptor)) != BOOTSHIM_DESCRIPTOR_OK)
        return 1;
    if (bootshim_validate_handoff_descriptor(
            descriptor, sizeof(descriptor) - 1) != BOOTSHIM_DESCRIPTOR_INVALID)
        return 2;

    descriptor[0] ^= 0xff;
    if (bootshim_validate_handoff_descriptor(
            descriptor, sizeof(descriptor)) != BOOTSHIM_DESCRIPTOR_INVALID)
        return 3;
    if (bootshim_validate_handoff_descriptor(NULL, sizeof(descriptor)) !=
        BOOTSHIM_DESCRIPTOR_INVALID)
        return 4;
    return 0;
}
