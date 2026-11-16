/*
 * Fixed-segment ARM64 kexec loader for the SM-A205W EDK2 handoff.
 *
 * --load-only stages two fixed kexec segments without executing them.
 * --execute invokes the kernel's normal KEXEC reboot path for an image that
 * has already been staged. Execution is always an explicit user action.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/kexec.h>
#include <linux/reboot.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef __aarch64__
#error "edk2-kexec must be built for AArch64"
#endif

#define BOOTSHIM_ADDR  UINT64_C(0x80080000)
#define BOOTSHIM_SIZE  UINT64_C(0x00010000)
#define FD_STAGE_ADDR  UINT64_C(0x80400000)
#define FD_SIZE        UINT64_C(0x00200000)
#define FD_BASE        UINT64_C(0x90020000)
#define FD_END         UINT64_C(0x90220000)
#define RAM0_START     UINT64_C(0x80000000)
#define RAM0_END       UINT64_C(0x90000000)
#define RAM_BANK0_END  UINT64_C(0xBBC00000)
#define FVH_OFFSET     0x28

static void usage(const char *name)
{
    fprintf(stderr,
        "Usage:\n"
        "  %s --load-only [BootShim.bin UEFI.fd]\n"
        "  %s --execute\n"
        "  %s --unload\n"
        "\n"
        "--load-only stages fixed physical segments and returns.\n"
        "--execute requests the kernel's KEXEC reboot for the staged image.\n"
        "Execution is non-returning on success and transfers to BootShim.\n",
        name, name, name);
}

static int read_exact(const char *path, size_t expected, unsigned char **out)
{
    struct stat st;
    unsigned char *buf;
    size_t done = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC);

    if (fd < 0) {
        fprintf(stderr, "open %s: %s\n", path, strerror(errno));
        return -1;
    }
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_size < 0 || (uint64_t)st.st_size != expected) {
        fprintf(stderr, "%s: expected exactly %zu bytes\n", path, expected);
        close(fd);
        return -1;
    }
    buf = malloc(expected);
    if (!buf) {
        fprintf(stderr, "malloc %zu: %s\n", expected, strerror(errno));
        close(fd);
        return -1;
    }
    while (done < expected) {
        ssize_t n = read(fd, buf + done, expected - done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            fprintf(stderr, "short read from %s\n", path);
            free(buf);
            close(fd);
            return -1;
        }
        done += (size_t)n;
    }
    close(fd);
    *out = buf;
    return 0;
}

static int execute_image(void)
{
    long rc;

    if (geteuid() != 0) {
        fputs("ERROR: root privileges are required for kexec execution.\n",
              stderr);
        return 1;
    }

    /*
     * The kernel's kernel_kexec() path performs restart preparation,
     * migrates to the reboot CPU, calls machine_shutdown() (which disables
     * non-boot CPUs on this ARM64 kernel), then enters machine_kexec().
     * Do not attempt userspace CPU-offline or peripheral shutdown here.
     */
    sync();
    rc = syscall(__NR_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
                 LINUX_REBOOT_CMD_KEXEC, NULL);
    fprintf(stderr, "KEXEC reboot returned: %s\n", strerror(errno));
    return rc == 0 ? 0 : 1;
}

static int unload_image(void)
{
    long rc = syscall(__NR_kexec_load, 0UL, 0UL, NULL,
                      (unsigned long)KEXEC_ARCH_AARCH64);
    if (rc != 0) {
        fprintf(stderr, "kexec_load(unload): %s\n", strerror(errno));
        return 1;
    }
    puts("KEXEC image unloaded; no reboot requested.");
    return 0;
}

int main(int argc, char **argv)
{
    const char *shim_path = "/edk2/BootShim.bin";
    const char *fd_path = "/edk2/UEFI.fd";
    unsigned char *shim = NULL, *fd = NULL;
    struct kexec_segment segments[2];
    long rc;
    int result = 1;

    if (argc == 2 && strcmp(argv[1], "--unload") == 0)
        return unload_image();
    if (argc == 2 && strcmp(argv[1], "--execute") == 0)
        return execute_image();

    if ((argc != 2 && argc != 4) || strcmp(argv[1], "--load-only") != 0) {
        usage(argv[0]);
        return 2;
    }
    if (argc == 4) {
        shim_path = argv[2];
        fd_path = argv[3];
    }
    if (geteuid() != 0) {
        fputs("ERROR: root privileges are required for kexec_load.\n", stderr);
        return 1;
    }

    if (BOOTSHIM_ADDR < RAM0_START ||
        BOOTSHIM_ADDR + BOOTSHIM_SIZE > RAM0_END ||
        FD_STAGE_ADDR < RAM0_START ||
        FD_STAGE_ADDR + FD_SIZE > RAM0_END ||
        FD_BASE != UINT64_C(0x90020000) || FD_END != UINT64_C(0x90220000) ||
        FD_BASE < RAM0_START || FD_END > RAM_BANK0_END ||
        BOOTSHIM_ADDR + BOOTSHIM_SIZE > FD_STAGE_ADDR ||
        FD_STAGE_ADDR + FD_SIZE > FD_BASE ||
        FD_BASE + FD_SIZE != FD_END) {
        fputs("ERROR: fixed segment layout violates the SM-A205W RAM map.\n",
              stderr);
        return 1;
    }

    if (read_exact(shim_path, (size_t)BOOTSHIM_SIZE, &shim) != 0)
        goto out;
    if (read_exact(fd_path, (size_t)FD_SIZE, &fd) != 0)
        goto out;
    if (memcmp(fd + FVH_OFFSET, "_FVH", 4) != 0) {
        fputs("ERROR: UEFI.fd has no EFI firmware-volume signature.\n", stderr);
        goto out;
    }

    memset(segments, 0, sizeof(segments));
    segments[0].buf = shim;
    segments[0].bufsz = (size_t)BOOTSHIM_SIZE;
    segments[0].mem = (const void *)(uintptr_t)BOOTSHIM_ADDR;
    segments[0].memsz = (size_t)BOOTSHIM_SIZE;

    segments[1].buf = fd;
    segments[1].bufsz = (size_t)FD_SIZE;
    segments[1].mem = (const void *)(uintptr_t)FD_STAGE_ADDR;
    segments[1].memsz = (size_t)FD_SIZE;

    rc = syscall(__NR_kexec_load, (unsigned long)BOOTSHIM_ADDR, 2UL,
                 segments, (unsigned long)KEXEC_ARCH_AARCH64);
    if (rc != 0) {
        fprintf(stderr, "kexec_load: %s\n", strerror(errno));
        goto out;
    }

    puts("PASS: kexec_load accepted the two fixed physical segments.");
    printf("  BootShim: 0x%08llx-0x%08llx (entry 0x%08llx)\n",
           (unsigned long long)BOOTSHIM_ADDR,
           (unsigned long long)(BOOTSHIM_ADDR + BOOTSHIM_SIZE),
           (unsigned long long)BOOTSHIM_ADDR);
    printf("  FD stage: 0x%08llx-0x%08llx\n",
           (unsigned long long)FD_STAGE_ADDR,
           (unsigned long long)(FD_STAGE_ADDR + FD_SIZE));
    printf("  FD target after BootShim copy: 0x%08llx-0x%08llx\n",
           (unsigned long long)FD_BASE, (unsigned long long)FD_END);
    puts("LOAD-ONLY: image is staged in the kernel; no reboot or execution occurred.");
    puts("To request the non-returning transfer explicitly, run: /sbin/edk2-kexec --execute");
    result = 0;

out:
    free(shim);
    free(fd);
    return result;
}
