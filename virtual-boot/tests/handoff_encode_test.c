/* SPDX-License-Identifier: MIT */
#include <stdint.h>
#include <string.h>
#include "a20_handoff_encode.h"
#include "a20_handoff_validate.h"

int main(void)
{
    struct a20_handoff_segment_input s[A20_HANDOFF_SEGMENT_COUNT];
    uint8_t out[A20_HANDOFF_HEADER_SIZE];
    unsigned int i, j;
    memset(s, 0, sizeof(s));
    for (i = 0; i < A20_HANDOFF_SEGMENT_COUNT; ++i) {
        s[i].physical_address = 0x10000000ULL + (uint64_t)i * 0x20000ULL;
        s[i].size = 0x10000;
        for (j = 0; j < 32; ++j) s[i].sha256[j] = (uint8_t)(i + j + 1);
    }
    if (a20_handoff_encode_v1(out, sizeof(out),
          A20_HANDOFF_F_KERNEL_VALID | A20_HANDOFF_F_INITRAMFS_VALID |
          A20_HANDOFF_F_DTB_VALID, s) != 0) return 1;
    if (out[8] != 1 || out[9] != 0 || out[10] != 0 || out[11] != 1) return 2;
    if (a20_handoff_validate_v1(out, sizeof(out)) != 0) return 3;

    s[2].physical_address = 0;
    if (a20_handoff_encode_v1(out, sizeof(out),
          A20_HANDOFF_F_KERNEL_VALID | A20_HANDOFF_F_INITRAMFS_VALID |
          A20_HANDOFF_F_DTB_VALID, s) == 0) return 4;
    for (i = 0; i < sizeof(out); ++i) if (out[i] != 0) return 5;
    return 0;
}
