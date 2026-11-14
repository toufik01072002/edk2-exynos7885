/* SPDX-License-Identifier: MIT
 * Offline BootShim mailbox stager for the Virtual Boot initramfs.
 *
 * This utility only creates a staged image file. It does not write physical
 * memory, reserve RAM, invoke kexec, or authorize SEC execution.
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define IMAGE_SIZE 0x10000u
#define MAILBOX_OFFSET 0xD000u
#define MAILBOX_SIZE 0x100u
#define SEGMENTS 5u
#define MAGIC "A20HND01"

static uint16_t le16(const uint8_t *p) {
    return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8);
}
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t le64(const uint8_t *p) {
    return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32);
}
static int read_exact(const char *path, uint8_t *buf, size_t size) {
    int fd = open(path, O_RDONLY);
    size_t done = 0;
    struct stat st;
    if (fd < 0) return -1;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size != (off_t)size) {
        close(fd); errno = EINVAL; return -1;
    }
    while (done < size) {
        ssize_t n = read(fd, buf + done, size - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { close(fd); errno = EIO; return -1; }
        done += (size_t)n;
    }
    if (close(fd)) return -1;
    return 0;
}
static int write_exact(int fd, const uint8_t *buf, size_t size) {
    size_t done = 0;
    while (done < size) {
        ssize_t n = write(fd, buf + done, size - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        done += (size_t)n;
    }
    return 0;
}
static int validate(const uint8_t *d) {
    uint32_t flags;
    uint64_t starts[SEGMENTS], ends[SEGMENTS];
    unsigned i, j;
    if (memcmp(d, MAGIC, 8) || le16(d + 8) != 1 ||
        le16(d + 10) != MAILBOX_SIZE) return -1;
    flags = le32(d + 12);
    if ((flags & ~0x3fu) || (flags & 0x07u) != 0x07u) return -1;
    for (i = 0; i < SEGMENTS; ++i) {
        const uint8_t *p = d + 16 + i * 48;
        uint8_t digest = 0;
        starts[i] = le64(p);
        uint64_t size = le64(p + 8);
        if (!starts[i] || !size || (starts[i] & 3u) ||
            size > UINT64_MAX - starts[i]) return -1;
        ends[i] = starts[i] + size;
        for (j = 0; j < 32; ++j) digest |= p[16 + j];
        if (!digest) return -1;
    }
    for (i = 0; i < SEGMENTS; ++i)
        for (j = i + 1; j < SEGMENTS; ++j)
            if (starts[i] < ends[j] && starts[j] < ends[i]) return -1;
    return 0;
}
int main(int argc, char **argv) {
    uint8_t image[IMAGE_SIZE], descriptor[MAILBOX_SIZE], verify[IMAGE_SIZE];
    struct stat in_st, desc_st;
    int out;
    if (argc != 4) {
        fprintf(stderr, "usage: handoff-stage BootShim.bin descriptor.bin output.bin\n");
        return 2;
    }
    if (stat(argv[1], &in_st) || stat(argv[2], &desc_st) ||
        (in_st.st_dev == desc_st.st_dev && in_st.st_ino == desc_st.st_ino)) {
        perror("input files"); return 2;
    }
    if (read_exact(argv[1], image, sizeof(image)) ||
        read_exact(argv[2], descriptor, sizeof(descriptor))) {
        perror("read input"); return 2;
    }
    if (validate(descriptor)) {
        fprintf(stderr, "invalid A20HND01 descriptor\n"); return 2;
    }
    memcpy(image + MAILBOX_OFFSET, descriptor, MAILBOX_SIZE);
    out = open(argv[3], O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (out < 0) { perror("create output (must not already exist)"); return 2; }
    if (write_exact(out, image, sizeof(image)) || fsync(out) || close(out)) {
        perror("write output"); unlink(argv[3]); return 2;
    }
    if (read_exact(argv[3], verify, sizeof(verify)) ||
        memcmp(verify, image, sizeof(image)) ||
        memcmp(verify + MAILBOX_OFFSET, descriptor, MAILBOX_SIZE)) {
        fprintf(stderr, "staged image read-back mismatch\n");
        unlink(argv[3]); return 2;
    }
    puts("PASS: descriptor staged and read back at image offset 0xD000");
    puts("STATUS: file staging only; no physical-memory write or SEC transfer");
    return 0;
}
