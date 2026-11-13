LOCAL_PATH := $(call my-dir)

# ============================================================
# trampoline - ARM64 assembly trampoline for Linux -> UEFI
# ============================================================
include $(CLEAR_VARS)
LOCAL_MODULE := trampoline
LOCAL_MODULE_CLASS := EXECUTABLES
LOCAL_MODULE_TAGS := optional
LOCAL_SRC_FILES := trampoline.S
LOCAL_LD_SCRIPT := trampoline.ld
LOCAL_CFLAGS := -nostdlib -ffreestanding
LOCAL_LDFLAGS := -nostdlib -Wl,--gc-sections
LOCAL_FORCE_STATIC_EXECUTABLE := true
include $(BUILD_EXECUTABLE)

# ============================================================
# uefi_loader - Userspace UEFI firmware loader
# ============================================================
include $(CLEAR_VARS)
LOCAL_MODULE := uefi_loader
LOCAL_MODULE_CLASS := EXECUTABLES
LOCAL_MODULE_TAGS := optional
LOCAL_SRC_FILES := uefi_loader.c
LOCAL_CFLAGS := -Wall -Wextra -O2 -D_GNU_SOURCE
LOCAL_STATIC_LIBRARIES := libc liblog libcutils
LOCAL_FORCE_STATIC_EXECUTABLE := true
LOCAL_LDFLAGS := -static -Wl,--gc-sections
include $(BUILD_EXECUTABLE)