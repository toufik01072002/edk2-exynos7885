/* SPDX-License-Identifier: MIT
 * SM-A205W ARM64 handoff descriptor ABI v1.
 *
 * Layout only: this header does not enable or perform a handoff.
 * All multi-byte fields are little-endian. Producers must serialize explicitly
 * if they are not running on a little-endian CPU.
 */
#ifndef A20_HANDOFF_H
#define A20_HANDOFF_H

#include <stddef.h>
#include <stdint.h>

#define A20_HANDOFF_MAGIC "A20HND01"
#define A20_HANDOFF_VERSION 1u
#define A20_HANDOFF_HEADER_SIZE 256u
#define A20_HANDOFF_SEGMENT_COUNT 5u

#define A20_HANDOFF_F_KERNEL_VALID       (1u << 0)
#define A20_HANDOFF_F_INITRAMFS_VALID    (1u << 1)
#define A20_HANDOFF_F_DTB_VALID          (1u << 2)
#define A20_HANDOFF_F_RANGES_RESERVED    (1u << 3)
#define A20_HANDOFF_F_CACHE_STATE_KNOWN  (1u << 4)
#define A20_HANDOFF_F_EL_STATE_KNOWN     (1u << 5)
#define A20_HANDOFF_F_ALL                ((1u << 6) - 1u)

enum a20_handoff_segment_id {
    A20_HANDOFF_SEG_KERNEL = 0,
    A20_HANDOFF_SEG_INITRAMFS = 1,
    A20_HANDOFF_SEG_DTB = 2,
    A20_HANDOFF_SEG_BOOTSHIM = 3,
    A20_HANDOFF_SEG_UEFI_FD = 4
};

struct a20_handoff_segment_v1 {
    uint64_t physical_address;
    uint64_t size;
    uint8_t sha256[32];
} __attribute__((packed));

struct a20_handoff_descriptor_v1 {
    uint8_t magic[8];
    uint16_t version;
    uint16_t header_size;
    uint32_t flags;
    struct a20_handoff_segment_v1 segments[A20_HANDOFF_SEGMENT_COUNT];
} __attribute__((packed));

_Static_assert(sizeof(struct a20_handoff_segment_v1) == 48,
               "A20 handoff segment ABI size changed");
_Static_assert(offsetof(struct a20_handoff_descriptor_v1, segments) == 16,
               "A20 handoff segment offset changed");
_Static_assert(sizeof(struct a20_handoff_descriptor_v1) == A20_HANDOFF_HEADER_SIZE,
               "A20 handoff descriptor must remain exactly 256 bytes");
_Static_assert(offsetof(struct a20_handoff_descriptor_v1, flags) == 12,
               "A20 handoff flags offset changed");

#endif /* A20_HANDOFF_H */
