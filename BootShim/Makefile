ARCH              = arm64
TARGET            = aarch64-linux-gnu
CROSS_COMPILE     = $(TARGET)-
CC                = $(CROSS_COMPILE)gcc
OBJCOPY           = $(CROSS_COMPILE)objcopy

# Compile the complete validation/plan path as freestanding ARM64 objects.
# Keep it out of the active image until descriptor transport is real.
all: BootShim.elf BootShim.bin descriptor_mailbox.o sec_entry_gate.o sec_handoff_adapter.o sec_entry_plan.o

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

sec_entry_plan.o: sec_entry_plan.c sec_entry_plan.h sec_handoff_adapter.h sec_entry_gate.h
	$(CC) -std=c11 -Wall -Wextra -Werror -ffreestanding -fno-builtin \
		-fno-stack-protector -fno-pic -fno-pie \
		-I. -I../virtual-boot/include -c sec_entry_plan.c -o $@

BootShim.S:

descriptor_mailbox.o: descriptor_mailbox.c descriptor_mailbox.h descriptor_consumer.h
	$(CC) -std=c11 -Wall -Wextra -Werror -ffreestanding -fno-builtin \
		-fno-stack-protector -fno-pic -fno-pie -c descriptor_mailbox.c -o $@
