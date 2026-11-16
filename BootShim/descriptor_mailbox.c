/* SPDX-License-Identifier: MIT */
#include "descriptor_mailbox.h"

/* Resolved by the BootShim.S image-relative mailbox symbol. */
extern const uint8_t _HandoffDescriptorMailbox[BOOTSHIM_DESCRIPTOR_MAILBOX_SIZE];

int bootshim_validate_embedded_handoff_descriptor(void)
{
    return bootshim_validate_handoff_descriptor(
        _HandoffDescriptorMailbox, BOOTSHIM_DESCRIPTOR_MAILBOX_SIZE);
}
