ARCH              = arm64
CROSS_COMPILE     ?= aarch64-linux-gnu-
CC                = $(CROSS_COMPILE)gcc
OBJCOPY           = $(CROSS_COMPILE)objcopy
NM                = $(CROSS_COMPILE)nm

CFLAGS := -std=c11 -Wall -Wextra -Werror -ffreestanding -fno-builtin \
          -fno-stack-protector -fno-pic -fno-pie \
          -fno-unwind-tables -fno-asynchronous-unwind-tables -O2
INCLUDES := -I. -I../virtual-boot/include

OBJS := descriptor_mailbox.o descriptor_consumer.o a20_handoff_validate.o

.PHONY: all clean verify
all: BootShim.bin verify

BootShim.elf: BootShim.S bootshim.ld $(OBJS)
	$(CC) -c -ffreestanding -fno-pic -fno-pie BootShim.S -o BootShim-entry.o
	$(CC) -nostdlib -static -no-pie -Wl,--build-id=none -Wl,--no-undefined \
		-Wl,-T,bootshim.ld BootShim-entry.o $(OBJS) -o $@

BootShim.bin: BootShim.elf
	$(OBJCOPY) -O binary --gap-fill=0 --pad-to=0x10000 $< $@

descriptor_mailbox.o: descriptor_mailbox.c descriptor_mailbox.h descriptor_consumer.h
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

descriptor_consumer.o: descriptor_consumer.c descriptor_consumer.h ../virtual-boot/include/a20_handoff_validate.h
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

a20_handoff_validate.o: ../virtual-boot/src/a20_handoff_validate.c ../virtual-boot/include/a20_handoff.h
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

sec_entry_gate.o: sec_entry_gate.c sec_entry_gate.h
	$(CC) $(CFLAGS) -c $< -o $@

sec_handoff_adapter.o: sec_handoff_adapter.c sec_handoff_adapter.h sec_entry_gate.h ../virtual-boot/include/a20_handoff.h ../virtual-boot/include/a20_handoff_validate.h
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

sec_entry_plan.o: sec_entry_plan.c sec_entry_plan.h sec_handoff_adapter.h sec_entry_gate.h
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

verify: BootShim.bin BootShim.elf
	test "$$(wc -c < BootShim.bin)" -eq 65536
	$(NM) -n BootShim.elf | grep -Eq ' 0000000000000000 T _Head$$'
	$(NM) -n BootShim.elf | grep -Eq ' 000000000000d000 D _HandoffDescriptorMailbox$$'
	$(NM) -n BootShim.elf | grep -Eq ' 000000000000d100 R _SecEntryOffset$$'
	$(NM) -n BootShim.elf | grep -q 'bootshim_validate_embedded_handoff_descriptor'
	$(NM) -n BootShim.elf | grep -Eq ' 0000000000010000 B _BootShimStackTop$$'
	python3 -c 'from pathlib import Path; b=Path("BootShim.bin").read_bytes(); assert len(b)==0x10000; assert b[0xD000:0xD100]==bytes(0x100); assert b[0xD100:0xD108]==bytes(8); print("PASS: fixed 64 KiB BootShim, mailbox, SEC slot and stack layout")'

clean:
	rm -f BootShim-entry.o BootShim.elf BootShim.bin $(OBJS)
