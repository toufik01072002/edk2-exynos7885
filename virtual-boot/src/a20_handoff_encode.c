/* SPDX-License-Identifier: MIT */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "a20_handoff_encode.h"
#include "a20_handoff_validate.h"

static void write_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}
static void write_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void write_le64(uint8_t *p, uint64_t v)
{
    write_le32(p, (uint32_t)v);
    write_le32(p + 4, (uint32_t)(v >> 32));
}
int a20_handoff_encode_v1(uint8_t *out, size_t capacity, uint32_t flags,
                          const struct a20_handoff_segment_input segments[A20_HANDOFF_SEGMENT_COUNT])
{
    size_t i, j;
    if (out == NULL) return -1;
    if (capacity < A20_HANDOFF_HEADER_SIZE || segments == NULL) {
        if (capacity >= A20_HANDOFF_HEADER_SIZE)
            memset(out, 0, A20_HANDOFF_HEADER_SIZE);
        return -1;
    }
    memset(out, 0, A20_HANDOFF_HEADER_SIZE);
    if ((flags & ~A20_HANDOFF_F_ALL) != 0) return -1;
    memcpy(out, A20_HANDOFF_MAGIC, 8);
    write_le16(out + 8, A20_HANDOFF_VERSION);
    write_le16(out + 10, A20_HANDOFF_HEADER_SIZE);
    write_le32(out + 12, flags);
    for (i = 0; i < A20_HANDOFF_SEGMENT_COUNT; ++i) {
        uint8_t digest = 0;
        uint8_t *dst = out + 16 + i * 48;
        if (segments[i].physical_address == 0 || segments[i].size == 0 ||
            (segments[i].physical_address & 3u) != 0 ||
            segments[i].size > UINT64_MAX - segments[i].physical_address) {
            memset(out, 0, A20_HANDOFF_HEADER_SIZE);
            return -1;
        }
        for (j = 0; j < 32; ++j) digest |= segments[i].sha256[j];
        if (digest == 0) {
            memset(out, 0, A20_HANDOFF_HEADER_SIZE);
            return -1;
        }
        write_le64(dst, segments[i].physical_address);
        write_le64(dst + 8, segments[i].size);
        memcpy(dst + 16, segments[i].sha256, 32);
    }
    if (a20_handoff_validate_v1(out, A20_HANDOFF_HEADER_SIZE) != 0) {
        memset(out, 0, A20_HANDOFF_HEADER_SIZE);
        return -1;
    }
    return 0;
}
