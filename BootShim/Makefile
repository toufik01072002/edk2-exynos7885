ARCH              = arm64
TARGET            = aarch64-linux-gnu
CROSS_COMPILE     = $(TARGET)-
CC                = $(CROSS_COMPILE)gcc
OBJCOPY           = $(CROSS_COMPILE)objcopy

# Compile the SEC gate for the target architecture, but keep it out of the
# active BootShim image until runtime evidence transport is implemented.
all: BootShim.elf BootShim.bin sec_entry_gate.o sec_handoff_adapter.o

BootShim.bin: BootShim.elf
	$(OBJCOPY) -O binary $< $@

BootShim.elf: BootShim.S
	$(CC) -c $< -o $@ -DUEFI_BASE=$(UEFI_BASE) -DUEFI_SIZE=$(UEFI_SIZE)

sec_entry_gate.o: sec_entry_gate.c sec_entry_gate.h
	$(CC) -std=c11 -Wall -Wextra -Werror -ffreestanding -fno-builtin \
		-fno-stack-protector -fno-pic -fno-pie -c sec_entry_gate.c -o $@

sec_handoff_adapter.o: sec_handoff_adapter.c sec_handoff_adapter.h sec_entry_gate.h ../virtual-boot/include/a20_handoff.h ../virtual-boot/include/a20_handoff_validate.h
	$(CC) -std=c11 -Wall -Wextra -Werror -ffreestanding -fno-builtin \
		-fno-stack-protector -fno-pic -fno-pie \
		-I. -I../virtual-boot/include -c sec_handoff_adapter.c -o $@

BootShim.S:
