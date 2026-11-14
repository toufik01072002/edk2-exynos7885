/* SPDX-License-Identifier: MIT
 * Descriptor-to-SEC evidence adapter. Validation only; never branches.
 */
#ifndef BOOTSHIM_SEC_HANDOFF_ADAPTER_H
#define BOOTSHIM_SEC_HANDOFF_ADAPTER_H

#include <stddef.h>
#include <stdint.h>
#include "sec_entry_gate.h"

enum bootshim_sec_handoff_status {
    BOOTSHIM_SEC_HANDOFF_OK = 0,
    BOOTSHIM_SEC_HANDOFF_DESCRIPTOR = -10,
    BOOTSHIM_SEC_HANDOFF_FLAGS = -11,
    BOOTSHIM_SEC_HANDOFF_SEGMENTS = -12,
    BOOTSHIM_SEC_HANDOFF_GATE = -13
};

/* The descriptor buffer must already have been safely transported into
 * readable memory. Segment addresses are compared as metadata only.
 * No physical address is dereferenced and no execution transition occurs.
 */
int bootshim_validate_sec_handoff(const uint8_t *descriptor, size_t length,
                                  const struct bootshim_sec_entry_evidence *evidence);

#endif
