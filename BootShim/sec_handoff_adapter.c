/* SPDX-License-Identifier: MIT
 * Bind the v1 descriptor metadata to independently supplied SEC evidence.
 */
#include "sec_handoff_adapter.h"

#include "a20_handoff.h"
#include "a20_handoff_validate.h"

static uint64_t read_le64(const uint8_t *p)
{
    uint64_t value = 0;
    unsigned int i;

    for (i = 0; i < 8; ++i)
        value |= (uint64_t)p[i] << (i * 8);
    return value;
}

int bootshim_validate_sec_handoff(const uint8_t *descriptor, size_t length,
                                  const struct bootshim_sec_entry_evidence *e)
{
    const uint8_t *shim;
    const uint8_t *fd;
    uint32_t flags;

    if (descriptor == NULL || e == NULL ||
        length != A20_HANDOFF_HEADER_SIZE ||
        a20_handoff_validate_v1(descriptor, length) != 0)
        return BOOTSHIM_SEC_HANDOFF_DESCRIPTOR;

    flags = (uint32_t)descriptor[12] |
            ((uint32_t)descriptor[13] << 8) |
            ((uint32_t)descriptor[14] << 16) |
            ((uint32_t)descriptor[15] << 24);
    if ((flags & (A20_HANDOFF_F_RANGES_RESERVED |
                  A20_HANDOFF_F_CACHE_STATE_KNOWN |
                  A20_HANDOFF_F_EL_STATE_KNOWN)) !=
        (A20_HANDOFF_F_RANGES_RESERVED |
         A20_HANDOFF_F_CACHE_STATE_KNOWN |
         A20_HANDOFF_F_EL_STATE_KNOWN))
        return BOOTSHIM_SEC_HANDOFF_FLAGS;

    shim = descriptor + 16 + A20_HANDOFF_SEG_BOOTSHIM * 48;
    fd = descriptor + 16 + A20_HANDOFF_SEG_UEFI_FD * 48;

    if (read_le64(shim) != e->bootshim_base ||
        read_le64(shim + 8) != BOOTSHIM_IMAGE_SIZE ||
        read_le64(fd) != e->fd_base ||
        read_le64(fd + 8) != e->fd_size)
        return BOOTSHIM_SEC_HANDOFF_SEGMENTS;

    if (bootshim_validate_sec_entry_evidence(e) != BOOTSHIM_SEC_GATE_OK)
        return BOOTSHIM_SEC_HANDOFF_GATE;

    return BOOTSHIM_SEC_HANDOFF_OK;
}
