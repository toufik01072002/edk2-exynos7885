/* SPDX-License-Identifier: MIT
 * BootShim descriptor-consumer boundary.
 *
 * This is a validation-only interface. It does not establish descriptor
 * transport, reserve memory, change CPU state, or enter EDK2 SEC.
 */
#ifndef BOOTSHIM_DESCRIPTOR_CONSUMER_H
#define BOOTSHIM_DESCRIPTOR_CONSUMER_H

#include <stddef.h>
#include <stdint.h>

enum bootshim_descriptor_status {
    BOOTSHIM_DESCRIPTOR_OK = 0,
    BOOTSHIM_DESCRIPTOR_INVALID = -1
};

/* Accepts an externally supplied byte buffer; never dereferences a physical
 * address from the descriptor and never performs a control-flow transition.
 */
int bootshim_validate_handoff_descriptor(const uint8_t *buffer, size_t length);

#endif
