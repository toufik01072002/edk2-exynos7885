#!/usr/bin/env python3
import struct
import unittest
from patch_sec_entry import ENTRY_SLOT_OFFSET, patch_bootshim

class PatchSecEntryTests(unittest.TestCase):
    def test_bundle_relative_offset(self):
        image = bytes(0x10000)
        result = patch_bootshim(image, 0x81234)
        self.assertEqual(struct.unpack_from("<Q", result, ENTRY_SLOT_OFFSET)[0], 0x181234)
        self.assertEqual(result[:ENTRY_SLOT_OFFSET], image[:ENTRY_SLOT_OFFSET])
        self.assertEqual(result[ENTRY_SLOT_OFFSET + 8:], image[ENTRY_SLOT_OFFSET + 8:])

    def test_idempotent(self):
        first = patch_bootshim(bytes(0x10000), 0x80000)
        self.assertEqual(patch_bootshim(first, 0x80000), first)

    def test_rejects_unaligned_offset(self):
        with self.assertRaises(ValueError):
            patch_bootshim(bytes(0x10000), 0x80002)

    def test_rejects_out_of_range_offset(self):
        with self.assertRaises(ValueError):
            patch_bootshim(bytes(0x10000), 0x200000)

    def test_rejects_conflicting_slot(self):
        image = bytearray(0x10000)
        struct.pack_into("<Q", image, ENTRY_SLOT_OFFSET, 0x1234)
        with self.assertRaises(ValueError):
            patch_bootshim(bytes(image), 0x80000)

if __name__ == "__main__":
    unittest.main()
