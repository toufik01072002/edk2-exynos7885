/* SPDX-License-Identifier: MIT
 * Synthetic host fixtures only; addresses are not hardware reservations.
 */
#include <stdint.h>
#include <string.h>
#include "a20_handoff.h"
#include "a20_handoff_validate.h"

static void put_le64(uint8_t *p, uint64_t v)
{
    unsigned int i;
    for (i = 0; i < 8; ++i) p[i] = (uint8_t)(v >> (i * 8));
}

static void make_fixture(uint8_t *b)
{
    unsigned int i, j;
    const uint64_t addresses[5] = {
        0x10000000u, 0x12000000u, 0x14000000u, 0x16000000u, 0x18000000u
    };
    memset(b, 0, 256);
    memcpy(b, A20_HANDOFF_MAGIC, 8);
    b[8] = 1; b[10] = 0; b[11] = 1;
    b[12] = 7; /* kernel, initramfs, DTB valid; hardware gates remain clear */
    for (i = 0; i < 5; ++i) {
        uint8_t *s = b + 16 + i * 48;
        put_le64(s, addresses[i]);
        put_le64(s + 8, 0x1000);
        for (j = 0; j < 32; ++j) s[16 + j] = (uint8_t)(i + j + 1);
    }
}

int main(void)
{
    uint8_t b[256];
    make_fixture(b);
    if (a20_handoff_validate_v1(b, sizeof(b)) != 0) return 1;
    if (a20_handoff_validate_v1(b, sizeof(b) - 1) == 0) return 2;
    b[8] = 2;
    if (a20_handoff_validate_v1(b, sizeof(b)) == 0) return 3;
    make_fixture(b);
    put_le64(b + 16 + 48, 0x10000800u); /* overlap with segment 0 */
    if (a20_handoff_validate_v1(b, sizeof(b)) == 0) return 4;
    make_fixture(b);
    put_le64(b + 16, UINT64_MAX - 0x7ffu);
    put_le64(b + 24, 0x1000u); /* address + size overflow */
    if (a20_handoff_validate_v1(b, sizeof(b)) == 0) return 5;
    make_fixture(b);
    b[12] = 0; /* required payload-valid flags missing */
    if (a20_handoff_validate_v1(b, sizeof(b)) == 0) return 6;
    return 0;
}
