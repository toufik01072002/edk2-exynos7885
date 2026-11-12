ARCH          := arm64
TARGET        := aarch64-linux-gnu
CROSS_COMPILE := $(TARGET)-
CC            := $(CROSS_COMPILE)gcc
OBJCOPY       := $(CROSS_COMPILE)objcopy

UEFI_BASE   ?= 0x90000000
UEFI_SIZE   ?= 0x00200000
TEXT_OFFSET ?= 0x00080000
IMAGE_SIZE  ?= 0

.PHONY: all clean
all: BootShim.elf BootShim.bin

BootShim.elf: BootShim.S
	@test -n "$(UEFI_BASE)" -a -n "$(UEFI_SIZE)"
	$(CC) -c $< -o $@ \
		-DUEFI_BASE=$(UEFI_BASE) -DUEFI_SIZE=$(UEFI_SIZE) \
		-DTEXT_OFFSET=$(TEXT_OFFSET) -DIMAGE_SIZE=$(IMAGE_SIZE)

BootShim.bin: BootShim.elf
	$(OBJCOPY) -O binary $< $@

clean:
	rm -f BootShim.elf BootShim.bin
