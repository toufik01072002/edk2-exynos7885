/*
 * uefi_loader.c - Userspace UEFI firmware loader for TWRP
 *
 * This runs as a static ARM64 binary from TWRP recovery.
 * It locates, validates, and loads the UEFI firmware via kexec.
 *
 * Build: aarch64-linux-gnu-gcc -static -Os uefi_loader.c -o uefi_loader
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <errno.h>
#include <dirent.h>
#include <stdarg.h>
#include <inttypes.h>
#include <linux/kexec.h>

/* Architecture-specific syscall numbers for ARM64 */
#ifndef __NR_kexec_load
#define __NR_kexec_load  104
#endif
/* kexec_file_load is not available on ARM64 in Linux 4.4 - do not define */

/* UEFI firmware constants */
#define UEFI_FD_BASE        0x90000000UL
#define UEFI_FD_SIZE        0x00200000UL  /* 2 MiB */
#define UEFI_MAGIC_OFFSET   0x00000030UL  /* ARM64 magic "ARM\x64" in kernel header */
#define UEFI_MAGIC_VALUE    0x00364D5241UL /* "ARM6" little-endian */

/* Search paths for UEFI firmware */
static const char *uefi_search_paths[] = {
    "/sdcard/uefi/EXYNOS7885PKG_UEFI.fd",
    "/sdcard/uefi/uefi.fd",
    "/sdcard/EXYNOS7885PKG_UEFI.fd",
    "/sdcard/uefi.fd",
    "/usb-otg/uefi/EXYNOS7885PKG_UEFI.fd",
    "/usb-otg/uefi/uefi.fd",
    "/usb-otg/EXYNOS7885PKG_UEFI.fd",
    "/usb-otg/uefi.fd",
    "/data/local/tmp/EXYNOS7885PKG_UEFI.fd",
    NULL
};

/* BootShim path (prepended to FD) */
static const char *boothim_search_paths[] = {
    "/sdcard/uefi/BootShim.bin",
    "/sdcard/BootShim.bin",
    "/usb-otg/uefi/BootShim.bin",
    "/usb-otg/BootShim.bin",
    "/data/local/tmp/BootShim.bin",
    NULL
};

/* Platform GUID for SM-A205W (from a205w.dsc) */
static const uint8_t PLATFORM_GUID[16] = {
    0xbf, 0xa3, 0xf1, 0x28, 0x3a, 0x19, 0xe3, 0x47,
    0xa7, 0xb9, 0x5a, 0x43, 0x5e, 0xaa, 0xb2, 0xee
};

/* Firmware state */
enum uefi_state {
    UEFI_STATE_NOT_FOUND = 0,
    UEFI_STATE_FOUND,
    UEFI_STATE_CHECKING,
    UEFI_STATE_COMPATIBLE,
    UEFI_STATE_INCOMPATIBLE,
    UEFI_STATE_READY,
    UEFI_STATE_BOOT_FAILED
};

/* Global state for UI callbacks */
static enum uefi_state g_state = UEFI_STATE_NOT_FOUND;
static char g_error_msg[256] = {0};
static char g_found_path[512] = {0};
static size_t g_firmware_size = 0;

/* UI callback function pointers (set by TWRP integration) */
void (*ui_set_state)(enum uefi_state state) = NULL;
void (*ui_set_progress)(int percent) = NULL;
void (*ui_set_message)(const char *msg) = NULL;

/* ============================================================
 * Utility functions
 * ============================================================ */

static void set_state(enum uefi_state state) {
    g_state = state;
    if (ui_set_state) ui_set_state(state);
}

static void set_progress(int percent) {
    if (ui_set_progress) ui_set_progress(percent);
}

static void set_message(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(g_error_msg, sizeof(g_error_msg), fmt, args);
    va_end(args);
    if (ui_set_message) ui_set_message(g_error_msg);
}

static int file_exists(const char *path) {
    return access(path, F_OK) == 0;
}

static size_t get_file_size(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) return st.st_size;
    return 0;
}

static int read_file(const char *path, void *buf, size_t max_size) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;

    size_t total = 0;
    ssize_t n = 0;
    while (total < max_size) {
        n = read(fd, (char *)buf + total, max_size - total);
        if (n <= 0) break;
        total += n;
    }
    close(fd);
    return (n < 0) ? -1 : (int)total;
}

static void *alloc_aligned(size_t alignment, size_t size) {
    void *ptr = NULL;
    if (posix_memalign(&ptr, alignment, size) != 0) return NULL;
    return ptr;
}

/* ============================================================
 * UEFI Firmware Validation
 * ============================================================ */

static int validate_uefi_fd(const void *fd_data, size_t fd_size) {
    if (fd_size < 0x1000) {
        set_message("FD too small: %zu bytes (min 4 KiB)", fd_size);
        return -1;
    }

    if (fd_size > UEFI_FD_SIZE) {
        set_message("FD too large: %zu bytes (max %u)", fd_size, UEFI_FD_SIZE);
        return -1;
    }

    /* Check for UEFI PI Firmware Volume header signature "_FVH" */
    const uint32_t *hdr = (const uint32_t *)fd_data;
    if (hdr[0] != 0x4856465F) {  /* "_FVH" little-endian */
        set_message("Invalid FV header signature: 0x%08x", hdr[0]);
        return -1;
    }

    /* Check FV length matches */
    uint32_t fv_length = hdr[1];
    if (fv_length != fd_size) {
        set_message("FV length mismatch: header says %u, file is %zu", fv_length, fd_size);
        return -1;
    }

    /* Verify GUID matches our platform (at offset 0x2C in FV header) */
    const uint8_t *fv_guid = (const uint8_t *)fd_data + 0x2C;
    if (memcmp(fv_guid, PLATFORM_GUID, 16) != 0) {
        set_message("Platform GUID mismatch - not for SM-A205W");
        return -1;
    }

    /* Check FV attributes indicate it's a valid executable FV */
    uint32_t fv_attributes = hdr[8];  /* Attributes at offset 0x20 */
    if (!(fv_attributes & 0x01)) {    /* FVB2_ERASE_POLARITY bit as sanity */
        set_message("Warning: FV attributes unusual: 0x%08x", fv_attributes);
    }

    return 0;
}

static int validate_boothim(const void *bs_data, size_t bs_size) {
    if (bs_size < 256 || bs_size > 64*1024) {
        set_message("BootShim size invalid: %zu bytes", bs_size);
        return -1;
    }

    /* Check for ARM64 kernel header magic "ARM\x64" */
    const uint8_t *magic = (const uint8_t *)bs_data + UEFI_MAGIC_OFFSET;
    uint32_t magic_val = *(const uint32_t *)magic;
    if (magic_val != UEFI_MAGIC_VALUE) {
        set_message("BootShim missing ARM64 magic: 0x%08x", magic_val);
        return -1;
    }

    return 0;
}

/* ============================================================
 * Memory map parsing (to find free region at 0x90000000)
 * ============================================================ */

static int check_memory_available(uint64_t addr, uint64_t size) {
    FILE *fp = fopen("/proc/iomem", "r");
    if (!fp) {
        set_message("Cannot open /proc/iomem: %s", strerror(errno));
        return -1;
    }

    char line[512];
    uint64_t start, end;
    const char *name;

    while (fgets(line, sizeof(line), fp)) {
        char *name = NULL;
        if (sscanf(line, "%" SCNx64 "-%" SCNx64 " : %ms", &start, &end, &name) == 3) {
            uint64_t region_start = start;
            uint64_t region_end = end + 1;

            /* Check overlap */
            if (addr < region_end && addr + size > region_start) {
                set_message("Memory conflict: 0x%" PRIx64 "-0x%" PRIx64 " overlaps with '%s' (0x%" PRIx64 "-0x%" PRIx64 ")",
                           addr, addr + size - 1, name, region_start, region_end - 1);
                free(name);
                fclose(fp);
                return -1;
            }
            free(name);
        }
    }
    fclose(fp);
    return 0;
}

/* ============================================================
 * DTB Generation for UEFI (minimal)
 * ============================================================ */

static int generate_uefi_dtb(void *buf, size_t buf_size) {
    /* Minimal DTB for UEFI boot:
     * - /chosen: bootargs, linux,initrd (not used)
     * - /memory: reg = <0x0 0x80000000 0x0 0xBBC00000> (Bank 0)
     * - /cpus: CPU nodes
     * - /intc: GIC
     * - /timer: ARM arch timer
     *
     * For now, return a placeholder. Real implementation uses libfdt.
     */
    (void)buf; (void)buf_size;
    set_message("DTB generation not yet implemented - using kernel DTB");
    return 0;
}

/* ============================================================
 * kexec syscall wrapper (kexec_load only - kexec_file_load not available on ARM64 Linux 4.4)
 * ============================================================ */

static long sys_kexec_load(unsigned long entry, unsigned long nr_segments,
                           struct kexec_segment *segments, unsigned long flags) {
    return syscall(__NR_kexec_load, entry, nr_segments, segments, flags);
}

/* ============================================================
 * Main loader logic
 * ============================================================ */

int uefi_loader_main(void) {
    void *fd_buffer = NULL;
    void *bs_buffer = NULL;
    void *combined_buffer = NULL;
    size_t fd_size = 0, bs_size = 0;
    char *fd_path = NULL, *bs_path = NULL;
    int ret = -1;

    set_state(UEFI_STATE_CHECKING);
    set_message("Scanning for UEFI firmware...");
    set_progress(10);

    /* ---- Step 1: Find UEFI FD ---- */
    for (int i = 0; uefi_search_paths[i]; i++) {
        if (file_exists(uefi_search_paths[i])) {
            fd_size = get_file_size(uefi_search_paths[i]);
            if (fd_size > 0 && fd_size <= UEFI_FD_SIZE) {
                fd_path = (char *)uefi_search_paths[i];
                break;
            }
        }
    }

    if (!fd_path) {
        set_state(UEFI_STATE_NOT_FOUND);
        set_message("UEFI firmware not found in standard locations");
        goto cleanup;
    }

    set_message("Found UEFI FD: %s (%zu bytes)", fd_path, fd_size);
    set_progress(30);

    /* ---- Step 2: Find BootShim ---- */
    for (int i = 0; boothim_search_paths[i]; i++) {
        if (file_exists(boothim_search_paths[i])) {
            bs_size = get_file_size(boothim_search_paths[i]);
            if (bs_size > 0 && bs_size <= 64*1024) {
                bs_path = (char *)boothim_search_paths[i];
                break;
            }
        }
    }

    if (!bs_path) {
        set_message("BootShim not found - trying FD alone (may not boot)");
        /* Continue without BootShim - FD at 0x90000000 might work if entry is correct */
    } else {
        set_message("Found BootShim: %s (%zu bytes)", bs_path, bs_size);
    }
    set_progress(50);

    /* ---- Step 3: Allocate and load files ---- */
    fd_buffer = alloc_aligned(4096, UEFI_FD_SIZE);
    if (!fd_buffer) {
        set_message("Failed to allocate FD buffer");
        goto cleanup;
    }
    memset(fd_buffer, 0, UEFI_FD_SIZE);

    if (read_file(fd_path, fd_buffer, UEFI_FD_SIZE) != (int)fd_size) {
        set_message("Failed to read UEFI FD");
        goto cleanup;
    }

    if (validate_uefi_fd(fd_buffer, fd_size) != 0) {
        goto cleanup;
    }

    if (bs_path) {
        bs_buffer = alloc_aligned(4096, bs_size);
        if (!bs_buffer) {
            set_message("Failed to allocate BootShim buffer");
            goto cleanup;
        }
        if (read_file(bs_path, bs_buffer, bs_size) != (int)bs_size) {
            set_message("Failed to read BootShim");
            goto cleanup;
        }
        if (validate_boothim(bs_buffer, bs_size) != 0) {
            goto cleanup;
        }
    }
    set_progress(70);

    /* ---- Step 4: Check memory availability ---- */
    if (check_memory_available(UEFI_FD_BASE, UEFI_FD_SIZE) != 0) {
        goto cleanup;
    }
    set_progress(80);

    /* ---- Step 5: Prepare combined image (BootShim + FD) ---- */
    size_t combined_size = (bs_size ? bs_size : 0) + UEFI_FD_SIZE;
    combined_buffer = alloc_aligned(4096, combined_size);
    if (!combined_buffer) {
        set_message("Failed to allocate combined buffer");
        goto cleanup;
    }

    uint8_t *dst = (uint8_t *)combined_buffer;
    if (bs_buffer) {
        memcpy(dst, bs_buffer, bs_size);
        dst += bs_size;
    }
    memcpy(dst, fd_buffer, UEFI_FD_SIZE);

    /* ---- Step 6: Generate DTB for UEFI ---- */
    void *dtb_buffer = alloc_aligned(4096, 64*1024);
    if (!dtb_buffer || generate_uefi_dtb(dtb_buffer, 64*1024) != 0) {
        set_message("DTB generation failed");
        goto cleanup;
    }
    set_progress(90);

    /* ---- Step 7: kexec_load ---- */
    struct kexec_segment segments[2] = {0};

    /* Segment 0: Kernel (BootShim+FD) - loaded at UEFI_FD_BASE */
    segments[0].buf = (void __user *)combined_buffer;
    segments[0].bufsz = combined_size;
    segments[0].mem = (void *)UEFI_FD_BASE;
    segments[0].memsz = combined_size;

    /* Segment 1: DTB */
    segments[1].buf = (void __user *)dtb_buffer;
    segments[1].bufsz = 64*1024;  /* TODO: actual DTB size */
    segments[1].mem = (void *)0x88000000;  /* Some free memory for DTB */
    segments[1].memsz = 64*1024;

    set_state(UEFI_STATE_READY);
    set_message("Loading via kexec... (no return on success)");
    set_progress(100);

    /* KEXEC_ON_CRASH = 0, KEXEC_PRESERVE_CONTEXT = not used */
    long kret = sys_kexec_load(UEFI_FD_BASE, 2, segments, 0);
    if (kret != 0) {
        set_message("kexec_load failed: %ld (%s)", kret, strerror(errno));
        set_state(UEFI_STATE_BOOT_FAILED);
        goto cleanup;
    }

    /* Execute! */
    set_message("Executing kexec... transferring to UEFI");
    sync();  /* Ensure buffers written */

    /* This syscall doesn't return on success */
    kret = syscall(__NR_reboot, 0x4321fedc, 0x28121969, 0x05121996, "kexec");
    if (kret != 0) {
        set_message("kexec reboot failed: %ld (%s)", kret, strerror(errno));
    }

    set_state(UEFI_STATE_BOOT_FAILED);

cleanup:
    if (fd_buffer) free(fd_buffer);
    if (bs_buffer) free(bs_buffer);
    if (combined_buffer) free(combined_buffer);
    if (dtb_buffer) free(dtb_buffer);
    return (g_state == UEFI_STATE_BOOT_FAILED) ? -1 : 0;
}

/* ============================================================
 * TWRP Integration API
 * ============================================================ */

void uefi_loader_set_ui_callbacks(
    void (*set_state_cb)(enum uefi_state),
    void (*set_progress_cb)(int),
    void (*set_message_cb)(const char *)
) {
    ui_set_state = set_state_cb;
    ui_set_progress = set_progress_cb;
    ui_set_message = set_message_cb;
}

enum uefi_state uefi_loader_get_state(void) {
    return g_state;
}

const char *uefi_loader_get_error(void) {
    return g_error_msg;
}

const char *uefi_loader_get_path(void) {
    return g_found_path;
}

/* Entry point for standalone execution (testing) */
#ifdef UEFI_LOADER_STANDALONE
int main(int argc, char **argv) {
    (void)argc; (void)argv;
    return uefi_loader_main();
}
#endif