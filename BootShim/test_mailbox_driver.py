#!/usr/bin/env python3
"""Static contract checks for the A20 mailbox kernel writer integration."""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
driver = (root / "virtual-boot/kernel/a20-handoff-mailbox.c").read_text()
dts = (root / "virtual-boot/dtb/exynos7885-a205w-virtual.dts").read_text()
workflow = (root / ".github/workflows/build-virtual-boot.yml").read_text()
helper = (root / "virtual-boot/initramfs/a20-handoff-publish.c").read_text()

required_driver = [
    "A20_MAILBOX_PHYS       0x90200000ULL",
    "A20_MAILBOX_SIZE       0x1000",
    'compatible = "samsung,a20-handoff-producer"',
    'mailbox->misc.name = "a20-handoff"',
    "crc32_le(~0U, (u8 *)record, record->record_size) ^ ~0U",
    "memcpy_toio(mailbox->base, &magic, sizeof(magic))",
    "record->crc32 != 0",
    "res.start != A20_MAILBOX_PHYS",
]
for item in required_driver:
    assert item in driver, f"missing driver contract: {item}"

assert 'a20_handoff_mailbox: handoff-mailbox@90200000' in dts
assert 'a20_handoff_producer: a20-handoff-producer' in dts
assert 'memory-region = <&a20_handoff_mailbox>;' in dts
assert 'compatible = "samsung,a20-handoff-producer";' in dts
assert "a20-handoff-mailbox.c" in workflow
assert "test_mailbox_driver.py" in workflow
assert "a20-handoff-publish.c" in workflow
assert "open(\"/dev/a20-handoff\"" in helper
assert "dtb_address > UINT64_MAX - dtb_size" in helper
assert "record.record_size = HEADER_SIZE" in helper
print("A20 mailbox driver source contract: PASS")
