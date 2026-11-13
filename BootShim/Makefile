ARCH              = arm64
TARGET            = aarch64-linux-gnu
CROSS_COMPILE     = $(TARGET)-
CC                = $(CROSS_COMPILE)gcc
OBJCOPY           = $(CROSS_COMPILE)objcopy

ENABLE_HANDOFF    ?= 0
UEFI_BASE         ?= 0x90000000
UEFI_SIZE         ?= 0x00200000
SEC_ENTRY         ?= 0x90008280

ASFLAGS           = -DUEFI_BASE=$(UEFI_BASE) -DUEFI_SIZE=$(UEFI_SIZE) -DSEC_ENTRY=$(SEC_ENTRY)

all: BootShim.elf BootShim.bin

BootShim.bin: BootShim.elf
	$(OBJCOPY) -O binary $< $@

BootShim.elf: BootShim.S
	$(CC) -c $< -o $@ $(ASFLAGS) -DENABLE_HANDOFF=$(ENABLE_HANDOFF)

# Compile the transfer path separately for syntax/layout review. This binary
# is not concatenated into workspace/UEFI and is never packaged for launch.
handoff-candidate:
	$(CC) -c BootShim.S -o BootShim-handoff-candidate.elf $(ASFLAGS) -DENABLE_HANDOFF=1
	$(OBJCOPY) -O binary BootShim-handoff-candidate.elf BootShim-handoff-candidate.bin

BootShim.S:
