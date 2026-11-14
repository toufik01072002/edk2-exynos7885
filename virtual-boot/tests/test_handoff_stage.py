#!/usr/bin/env python3
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

tool = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory() as tmp:
    root = Path(tmp)
    original = bytes((i % 239 for i in range(65536)))
    descriptor = bytearray(256)
    descriptor[:8] = b"A20HND01"
    struct.pack_into("<HHI", descriptor, 8, 1, 256, 7)
    for i in range(5):
        off = 16 + i * 48
        struct.pack_into("<QQ", descriptor, off, 0x100000 + i * 0x10000, 0x1000)
        descriptor[off + 16:off + 48] = bytes([i + 1]) * 32
    (root / "BootShim.bin").write_bytes(original)
    (root / "descriptor.bin").write_bytes(descriptor)
    output = root / "staged.bin"
    subprocess.run([str(tool), str(root / "BootShim.bin"),
                    str(root / "descriptor.bin"), str(output)], check=True)
    result = output.read_bytes()
    assert len(result) == 65536
    assert result[:0xD000] == original[:0xD000]
    assert result[0xD000:0xD100] == descriptor
    assert result[0xD100:] == original[0xD100:]
    # Output is exclusive: a second invocation must not overwrite it.
    denied = subprocess.run([str(tool), str(root / "BootShim.bin"),
                             str(root / "descriptor.bin"), str(output)],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    assert denied.returncode != 0
    bad = bytearray(descriptor)
    bad[0] = 0
    (root / "bad.bin").write_bytes(bad)
    rejected = subprocess.run([str(tool), str(root / "BootShim.bin"),
                               str(root / "bad.bin"), str(root / "bad-out.bin")],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    assert rejected.returncode != 0
    assert not (root / "bad-out.bin").exists()
print("PASS: ARM64 initramfs stager accepts valid descriptor and rejects invalid/overwrite")
