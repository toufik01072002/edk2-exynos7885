#!/usr/bin/env python3
import importlib.util
import struct
import tempfile
from pathlib import Path

script = Path(__file__).with_name("patch_descriptor_mailbox.py")
spec = importlib.util.spec_from_file_location("patcher", script)
patcher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(patcher)


def make_descriptor():
    data = bytearray(256)
    data[:8] = b"A20HND01"
    struct.pack_into("<HHI", data, 8, 1, 256, 0x07)
    # Five disjoint, aligned ranges with non-zero SHA-256 values.
    for i in range(5):
        offset = 16 + i * 48
        struct.pack_into("<QQ", data, offset, 0x100000 + i * 0x10000, 0x1000)
        data[offset + 16:offset + 48] = bytes([i + 1]) * 32
    return bytes(data)


descriptor = make_descriptor()
patcher.validate_descriptor(descriptor)
for bad in (descriptor[:-1], bytes(256), descriptor[:12] + b"\xff" * 4 + descriptor[16:]):
    try:
        patcher.validate_descriptor(bad)
    except ValueError:
        pass
    else:
        raise AssertionError("invalid descriptor was accepted")

with tempfile.TemporaryDirectory() as tmp:
    root = Path(tmp)
    image = bytes((i % 251 for i in range(65536)))
    (root / "BootShim.bin").write_bytes(image)
    (root / "descriptor.bin").write_bytes(descriptor)
    import subprocess
    import sys
    subprocess.run([
        sys.executable, str(script),
        "--image", str(root / "BootShim.bin"),
        "--descriptor", str(root / "descriptor.bin"),
        "--output", str(root / "staged.bin"),
    ], check=True)
    staged = (root / "staged.bin").read_bytes()
    assert len(staged) == len(image)
    assert staged[:0xD000] == image[:0xD000]
    assert staged[0xD000:0xD100] == descriptor
    assert staged[0xD100:] == image[0xD100:]
print("PASS: mailbox staging, read-back, boundary preservation, and invalid-input rejection")
