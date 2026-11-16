/*
 * uefi_loader.c - TWRP UEFI launch gate for Samsung SM-A205W.
 *
 * The runtime handoff is intentionally unavailable. This file contains no
 * kexec syscall path: enabling a compiler define cannot activate execution.
 * Reintroduce a launch implementation only with a reviewed physical-memory,
 * CPU/EL/MMU, GIC, DMA and cache contract plus hardware-backed validation.
 */

#include <stdio.h>
#include <string.h>

enum uefi_state {
    UEFI_STATE_NOT_FOUND = 0,
    UEFI_STATE_FOUND,
    UEFI_STATE_CHECKING,
    UEFI_STATE_COMPATIBLE,
    UEFI_STATE_INCOMPATIBLE,
    UEFI_STATE_READY,
    UEFI_STATE_BOOT_FAILED
};

static enum uefi_state g_state = UEFI_STATE_NOT_FOUND;
static char g_error_msg[256] = "EDK2 launch has not been evaluated";

static void (*ui_set_state)(enum uefi_state) = NULL;
static void (*ui_set_progress)(int) = NULL;
static void (*ui_set_message)(const char *) = NULL;

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
    return "";
}

int uefi_loader_main(void) {
    g_state = UEFI_STATE_INCOMPATIBLE;
    (void)snprintf(g_error_msg, sizeof(g_error_msg),
                   "%s",
                   "EDK2 launch blocked: ARM64 handoff is not implemented and validated");

    if (ui_set_state)
        ui_set_state(g_state);
    if (ui_set_progress)
        ui_set_progress(0);
    if (ui_set_message)
        ui_set_message(g_error_msg);

    return -1;
}

#ifdef UEFI_LOADER_STANDALONE
int main(int argc, char **argv) {
    int result;
    (void)argc;
    (void)argv;

    result = uefi_loader_main();
    if (result != 0)
        fprintf(stderr, "%s\n", uefi_loader_get_error());
    return result;
}
#endif
