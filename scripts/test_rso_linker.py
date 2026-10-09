#!/usr/bin/env python3
"""Small synthetic relocation/parser tests; no copyrighted test fixtures."""
import struct
import unittest
from link_rso import Rso, relocate, records, name, u32


class RelocationTests(unittest.TestCase):
    def test_address_halves_and_carry(self):
        for kind, expected in ((3, 0x8001), (4, 0x8001), (5, 0x8081), (6, 0x8082)):
            data = bytearray(2)
            relocate(data, 0, kind, 0x80818001, 0)
            self.assertEqual(bytes(data), struct.pack('>H', expected))
        data = bytearray(4)
        relocate(data, 0, 1, 0x81234567, 0)
        self.assertEqual(u32(data, 0), 0x81234567)

    def test_relative_branch_preserves_opcode_and_link(self):
        for delta in (4, -4, -0x2000000, 0x1FFFFFC):
            data = bytearray.fromhex('48000001')
            relocate(data, 0, 10, 0x80800000 + delta, 0x80800000)
            self.assertEqual(u32(data, 0), 0x48000001 | (delta & 0x3FFFFFC))

    def test_bad_relocations_fail(self):
        for offset, kind, value in ((4, 1, 0), (1, 4, 0), (0, 109, 0),
                                    (0, 10, 0x82800000), (0, 10, 0x80800002)):
            with self.assertRaises(ValueError):
                relocate(bytearray(4), offset, kind, value, 0x80800000)

    def test_table_bounds(self):
        for offset, size in ((0, 3), (4, 8), (-4, 4)):
            with self.assertRaises(ValueError):
                records(bytes(8), offset, size, 4)
        with self.assertRaises(ValueError):
            name(b'no terminator', 0)
        with self.assertRaises(ValueError):
            Rso(bytes(0x57))

    @staticmethod
    def fixture():
        data = bytearray(0xD0)
        def put(offset, *values):
            struct.pack_into('>' + 'I' * len(values), data, offset, *values)
        put(8, 3, 0x58)
        put(0x18, 1, 0x20)
        put(0x58, 0, 0, 0x80, 4, 0, 0x20)
        put(0x30, 0x90, 12, 0x9C, 12)
        put(0x90, 0x80, (2 << 8) | 1, 4)
        put(0x9C, 0x84, 1, 8)
        put(0x4C, 0xA8, 12, 0xB4)
        put(0xA8, 0, 0, 0)
        data[0xB4:0xB8] = b'foo\0'
        return data

    def test_link_bss_and_external(self):
        rso = Rso(self.fixture())
        linked, addresses = rso.link(0x80800000, {'foo': 0x80001000})
        self.assertEqual(addresses, [0, 0x80800080, 0x808000E0])
        self.assertEqual(u32(linked, 0x80), 0x808000E4)
        self.assertEqual(u32(linked, 0x84), 0x80001008)
        self.assertEqual(u32(rso.data, 0x80), 0)  # input remains pristine

    def test_invalid_link_inputs(self):
        rso = Rso(self.fixture())
        for base, imports in ((0x80800001, {'foo': 1}), (0x817FFFE0, {'foo': 1}),
                              (0x80800000, {})):
            with self.assertRaises(ValueError):
                rso.link(base, imports)
        data = self.fixture()
        struct.pack_into('>I', data, 0x1C, 0x40)
        with self.assertRaises(ValueError):
            Rso(data).link(0x80800000, {'foo': 1})


if __name__ == '__main__':
    unittest.main()
