#!/usr/bin/env python3
"""Stage a validated v1 descriptor into the fixed BootShim image mailbox.

This is an offline bundle-staging operation. It does not discover RAM
reservations, create runtime addresses, invoke kexec, or enable SEC entry.
"""
import argparse
from pathlib import Path
import struct

IMAGE_SIZE = 0x10000
MAILBOX_OFFSET = 0xD000
MAILBOX_SIZE = 0x100
MAGIC = b"A20HND01"
KNOWN_FLAGS = 0x3F
REQUIRED_FLAGS = 0x07


def validate_descriptor(data):
    if len(data) != MAILBOX_SIZE:
        raise ValueError("descriptor must be exactly 256 bytes")
    if data[:8] != MAGIC:
        raise ValueError("invalid descriptor magic")
    version, header_size, flags = struct.unpack_from("<HHI", data, 8)
    if version != 1 or header_size != MAILBOX_SIZE:
        raise ValueError("unsupported descriptor version or size")
    if flags & ~KNOWN_FLAGS or (flags & REQUIRED_FLAGS) != REQUIRED_FLAGS:
        raise ValueError("invalid descriptor flags")

    ranges = []
    for index in range(5):
        offset = 16 + index * 48
        address, size = struct.unpack_from("<QQ", data, offset)
        digest = data[offset + 16:offset + 48]
        if not address or not size or address & 3:
            raise ValueError("invalid segment address or size: %d" % index)
        end = address + size
        if end > 0xFFFFFFFFFFFFFFFF or end <= address:
            raise ValueError("segment address overflow: %d" % index)
        if digest == bytes(32):
            raise ValueError("empty segment SHA-256: %d" % index)
        ranges.append((address, end, index))

    for i, (start, end, index) in enumerate(ranges):
        for other_start, other_end, other_index in ranges[i + 1:]:
            if start < other_end and other_start < end:
                raise ValueError("overlapping segments: %d and %d" % (index, other_index))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--image", required=True, help="64 KiB BootShim.bin")
    parser.add_argument("--descriptor", required=True, help="256-byte A20HND01 descriptor")
    parser.add_argument("--output", required=True, help="new staged BootShim image")
    args = parser.parse_args()

    image_path = Path(args.image)
    descriptor_path = Path(args.descriptor)
    output_path = Path(args.output)
    image = image_path.read_bytes()
    descriptor = descriptor_path.read_bytes()

    if len(image) != IMAGE_SIZE:
        raise SystemExit("BootShim image must be exactly 65536 bytes")
    validate_descriptor(descriptor)
    if output_path.resolve() in (image_path.resolve(), descriptor_path.resolve()):
        raise SystemExit("output must not overwrite an input")

    staged = bytearray(image)
    staged[MAILBOX_OFFSET:MAILBOX_OFFSET + MAILBOX_SIZE] = descriptor
    output_path.write_bytes(staged)

    check = output_path.read_bytes()
    if check[MAILBOX_OFFSET:MAILBOX_OFFSET + MAILBOX_SIZE] != descriptor:
        raise SystemExit("mailbox read-back verification failed")
    if check[:MAILBOX_OFFSET] != image[:MAILBOX_OFFSET] or check[MAILBOX_OFFSET + MAILBOX_SIZE:] != image[MAILBOX_OFFSET + MAILBOX_SIZE:]:
        raise SystemExit("unexpected modification outside mailbox")
    print("PASS: descriptor staged and read back at BootShim offset 0xD000")
    print("STATUS: offline image staging only; no runtime handoff or SEC entry")


if __name__ == "__main__":
    main()
