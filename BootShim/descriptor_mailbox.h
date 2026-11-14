/* SPDX-License-Identifier: MIT
 * BootShim descriptor mailbox consumer.
 *
 * The mailbox is an image-relative symbol exported by BootShim.S. This API
 * validates its bytes in place; it does not copy, dereference descriptor
 * physical addresses, reserve RAM, or transfer control to SEC.
 */
#ifndef BOOTSHIM_DESCRIPTOR_MAILBOX_H
#define BOOTSHIM_DESCRIPTOR_MAILBOX_H

#include <stdint.h>
#include "descriptor_consumer.h"

#define BOOTSHIM_DESCRIPTOR_MAILBOX_SIZE 256u

/* Validate the fixed in-image mailbox exported by BootShim.S. */
int bootshim_validate_embedded_handoff_descriptor(void);

#endif
