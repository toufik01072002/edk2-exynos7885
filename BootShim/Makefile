ARCH              = arm64
TARGET            = aarch64-linux-gnu
CROSS_COMPILE     = $(TARGET)-
CC                = $(CROSS_COMPILE)gcc
OBJCOPY           = $(CROSS_COMPILE)objcopy

ENABLE_HANDOFF    ?= 0
UEFI_BASE         ?= 0x90020000
UEFI_SIZE         ?= 0x00200000
SEC_ENTRY         ?= 0x90028278
BOOTSHIM_BASE     ?= 0x80080000
FD_SOURCE         ?= 0x80400000

ASFLAGS           = -DUEFI_BASE=$(UEFI_BASE) -DUEFI_SIZE=$(UEFI_SIZE) -DSEC_ENTRY=$(SEC_ENTRY) -DBOOTSHIM_BASE=$(BOOTSHIM_BASE) -DFD_SOURCE=$(FD_SOURCE)

all: BootShim.elf BootShim.bin

BootShim.bin: BootShim.elf
	$(OBJCOPY) -O binary $< $@

BootShim.elf: BootShim.S
	$(CC) -c $< -o $@ $(ASFLAGS) -DENABLE_HANDOFF=$(ENABLE_HANDOFF)

# Compile the fixed-segment transfer candidate. It is staged separately from
# the legacy WFE-gated image and is only loaded with edk2-kexec --load-only.
handoff-candidate:
	$(CC) -c BootShim.S -o BootShim-handoff-candidate.elf $(ASFLAGS) -DENABLE_HANDOFF=1
	$(OBJCOPY) -O binary BootShim-handoff-candidate.elf BootShim-handoff-candidate.bin

BootShim.S:
