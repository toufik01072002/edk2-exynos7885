// SPDX-License-Identifier: GPL-2.0
/*
 * Build one A20 handoff record and submit it to the kernel mailbox writer.
 * Physical addresses are supplied by the caller only after their lifetime
 * and reserved-memory ownership have been established.
 */
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAILBOX_SIZE 4096U
#define RECORD_MAGIC UINT64_C(0x31464F484F323041)
#define RECORD_VERSION 1U
#define HEADER_SIZE 56U
#define MAX_DTB_SIZE (2U * 1024U * 1024U)
#define MAX_CMDLINE_SIZE 4096U

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
    uint8_t reserved[MAILBOX_SIZE - HEADER_SIZE];
};

_Static_assert(sizeof(struct a20_handoff_record) == MAILBOX_SIZE,
               "mailbox ABI must be exactly 4096 bytes");

static int parse_u64(const char *text, uint64_t *value)
{
    char *end = NULL;
    unsigned long long parsed;

    errno = 0;
    parsed = strtoull(text, &end, 0);
    if (errno || !text[0] || !end || *end)
        return -1;
    *value = (uint64_t)parsed;
    return 0;
}

int main(int argc, char **argv)
{
    struct a20_handoff_record record;
    uint64_t dtb_address, dtb_size, cmdline_address, cmdline_size;
    int fd;
    ssize_t written;

    if (argc != 5) {
        fprintf(stderr,
                "usage: %s <dtb-phys> <dtb-size> <cmdline-phys> <cmdline-size>\n",
                argv[0]);
        return 2;
    }

    if (parse_u64(argv[1], &dtb_address) ||
        parse_u64(argv[2], &dtb_size) ||
        parse_u64(argv[3], &cmdline_address) ||
        parse_u64(argv[4], &cmdline_size)) {
        fprintf(stderr, "invalid numeric argument\n");
        return 2;
    }

    if ((dtb_address == 0) != (dtb_size == 0) ||
        (cmdline_address == 0) != (cmdline_size == 0) ||
        dtb_size > MAX_DTB_SIZE || cmdline_size > MAX_CMDLINE_SIZE ||
        dtb_address > UINT64_MAX - dtb_size ||
        cmdline_address > UINT64_MAX - cmdline_size ||
        cmdline_size > UINT32_MAX) {
        fprintf(stderr, "invalid address/size pair or payload bound\n");
        return 2;
    }

    memset(&record, 0, sizeof(record));
    record.magic = RECORD_MAGIC;
    record.version = RECORD_VERSION;
    record.header_size = HEADER_SIZE;
    record.record_size = HEADER_SIZE;
    record.dtb_address = dtb_address;
    record.dtb_size = dtb_size;
    record.cmdline_address = cmdline_address;
    record.cmdline_size = (uint32_t)cmdline_size;

    fd = open("/dev/a20-handoff", O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        perror("open /dev/a20-handoff");
        return 1;
    }

    written = write(fd, &record, sizeof(record));
    if (written != (ssize_t)sizeof(record)) {
        if (written < 0)
            perror("write /dev/a20-handoff");
        else
            fprintf(stderr, "short mailbox write: %zd/%u\n",
                    written, MAILBOX_SIZE);
        close(fd);
        return 1;
    }

    close(fd);
    puts("A20 handoff record submitted; CRC32 calculated by kernel driver.");
    return 0;
}
