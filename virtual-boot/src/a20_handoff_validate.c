/* SPDX-License-Identifier: MIT
 * Host-testable structural validator. It does not authorize or perform handoff.
 */
#include <stddef.h>
#include <stdint.h>
#include "a20_handoff.h"

#define SEGMENT_OFFSET 16u
#define SEGMENT_SIZE 48u
#define DESCRIPTOR_SIZE 256u
#define REQUIRED_PAYLOAD_FLAGS (A20_HANDOFF_F_KERNEL_VALID | \
    A20_HANDOFF_F_INITRAMFS_VALID | A20_HANDOFF_F_DTB_VALID)

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t read_le64(const uint8_t *p)
{
    return (uint64_t)read_le32(p) | ((uint64_t)read_le32(p + 4) << 32);
}

static int digest_is_zero(const uint8_t *p)
{
    size_t i;
    uint8_t value = 0;
    for (i = 0; i < 32; ++i)
        value |= p[i];
    return value == 0;
}

/* Returns 0 only for a structurally sound v1 descriptor. */
int a20_handoff_validate_v1(const uint8_t *data, size_t length)
{
    uint64_t starts[A20_HANDOFF_SEGMENT_COUNT];
    uint64_t ends[A20_HANDOFF_SEGMENT_COUNT];
    uint32_t flags;
    size_t i, j;

    if (data == NULL || length != DESCRIPTOR_SIZE)
        return -1;
    if (data[0] != 'A' || data[1] != '2' || data[2] != '0' ||
        data[3] != 'H' || data[4] != 'N' || data[5] != 'D' ||
        data[6] != '0' || data[7] != '1')
        return -2;
    if (read_le16(data + 8) != A20_HANDOFF_VERSION ||
        read_le16(data + 10) != A20_HANDOFF_HEADER_SIZE)
        return -3;

    flags = read_le32(data + 12);
    if ((flags & ~A20_HANDOFF_F_ALL) != 0 ||
        (flags & REQUIRED_PAYLOAD_FLAGS) != REQUIRED_PAYLOAD_FLAGS)
        return -4;

    for (i = 0; i < A20_HANDOFF_SEGMENT_COUNT; ++i) {
        const uint8_t *segment = data + SEGMENT_OFFSET + i * SEGMENT_SIZE;
        uint64_t address = read_le64(segment);
        uint64_t size = read_le64(segment + 8);

        if (address == 0 || size == 0 || (address & 3u) != 0)
            return -5;
        if (size > UINT64_MAX - address)
            return -6;
        if (digest_is_zero(segment + 16))
            return -7;
        starts[i] = address;
        ends[i] = address + size;
    }

    for (i = 0; i < A20_HANDOFF_SEGMENT_COUNT; ++i) {
        for (j = i + 1; j < A20_HANDOFF_SEGMENT_COUNT; ++j) {
            if (starts[i] < ends[j] && starts[j] < ends[i])
                return -8;
        }
    }
    return 0;
}
