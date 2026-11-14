#ifndef A20_HANDOFF_RECORD_H
#define A20_HANDOFF_RECORD_H
#include <stdint.h>
#define A20_HANDOFF_MAILBOX_ADDRESS 0x90200000ULL
#define A20_HANDOFF_MAILBOX_SIZE    0x00001000U
#define A20_HANDOFF_MAGIC           0x31464F484F323041ULL /* "A20HOF1" */
#define A20_HANDOFF_VERSION         1U
#define A20_HANDOFF_HEADER_SIZE     56U
/* CRC32 covers record_size bytes with crc32 set to zero. Optional payload
 * addresses are zero when absent. Validate every bound and CRC before use. */
struct a20_handoff_record {
    uint64_t magic;
    uint32_t version;
    uint32_t header_size;
    uint32_t record_size;
    uint32_t flags;
    uint64_t dtb_address;
    uint64_t dtb_size;
    uint64_t cmdline_address;
    uint32_t cmdline_size;
    uint32_t crc32;
    uint8_t reserved[A20_HANDOFF_MAILBOX_SIZE - A20_HANDOFF_HEADER_SIZE];
};
#if defined(__cplusplus)
static_assert(sizeof(struct a20_handoff_record) == A20_HANDOFF_MAILBOX_SIZE, "mailbox must be 4 KiB");
#else
_Static_assert(sizeof(struct a20_handoff_record) == A20_HANDOFF_MAILBOX_SIZE, "mailbox must be 4 KiB");
#endif
#endif
