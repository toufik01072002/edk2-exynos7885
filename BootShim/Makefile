ARCH              = arm64
TARGET            = aarch64-linux-gnu
CROSS_COMPILE     = $(TARGET)-
CC                = $(CROSS_COMPILE)gcc
OBJCOPY           = $(CROSS_COMPILE)objcopy

# Compile the SEC gate for the target architecture, but keep it out of the
# active BootShim image until runtime evidence transport is implemented.
all: BootShim.elf BootShim.bin sec_entry_gate.o

BootShim.bin: BootShim.elf
	$(OBJCOPY) -O binary $< $@

BootShim.elf: BootShim.S
	$(CC) -c $< -o $@ -DUEFI_BASE=$(UEFI_BASE) -DUEFI_SIZE=$(UEFI_SIZE)

sec_entry_gate.o: sec_entry_gate.c sec_entry_gate.h
	$(CC) -std=c11 -Wall -Wextra -Werror -ffreestanding -fno-builtin \
		-fno-stack-protector -fno-pic -fno-pie -c sec_entry_gate.c -o $@

BootShim.S:
