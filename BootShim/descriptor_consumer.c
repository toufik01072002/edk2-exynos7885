/* SPDX-License-Identifier: MIT
 * BootShim-side consumer adapter; validation only.
 */
#include "descriptor_consumer.h"
#include "a20_handoff_validate.h"

int bootshim_validate_handoff_descriptor(const uint8_t *buffer, size_t length)
{
    if (buffer == NULL || length != 256u)
        return BOOTSHIM_DESCRIPTOR_INVALID;

    return a20_handoff_validate_v1(buffer, length) == 0
        ? BOOTSHIM_DESCRIPTOR_OK
        : BOOTSHIM_DESCRIPTOR_INVALID;
}
