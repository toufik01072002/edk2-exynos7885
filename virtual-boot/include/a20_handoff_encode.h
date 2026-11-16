/* SPDX-License-Identifier: MIT
 * Explicit little-endian serializer for the SM-A205W handoff ABI v1.
 * Callers must provide verified physical reservations and computed SHA-256.
 */
#ifndef A20_HANDOFF_ENCODE_H
#define A20_HANDOFF_ENCODE_H
#include <stddef.h>
#include <stdint.h>
#include "a20_handoff.h"

struct a20_handoff_segment_input {
    uint64_t physical_address;
    uint64_t size;
    uint8_t sha256[32];
};

/* Returns 0 on success; output is zeroed and -1 returned on any invalid input.
 * This serializes metadata only; it does not reserve memory or enable handoff.
 */
int a20_handoff_encode_v1(uint8_t *output, size_t capacity, uint32_t flags,
                          const struct a20_handoff_segment_input segments[A20_HANDOFF_SEGMENT_COUNT]);
#endif
