#!/usr/bin/env python3
"""Synthetic regression for indirect function discovery."""
import struct
import unittest
from generate_symbols import discover_indirect_pointer_seeds, discover_vtable_pointer_seeds

class FunctionSeedTests(unittest.TestCase):
    def test_return_boundaries_only(self):
        # Two leaf functions follow returns; the third pointer enters a body.
        code = [0x4E800020, 0x38600001, 0x4E800020,
                0x4E800420, 0x38600000, 0x60000000, 0x4E800020]
        pointers = [0x80001004, 0x80001010, 0x80001014,
                    0x80001000, 0x80001005, 0x90001004, 0x80001004]
        blob = b''.join(struct.pack('>I', x) for x in code + pointers)
        sections = [dict(kind='text', address='0x80001000', file_offset=0, bytes=len(code)*4),
                    dict(kind='data', address='0x80002000', file_offset=len(code)*4, bytes=len(pointers)*4)]
        self.assertEqual(discover_indirect_pointer_seeds(blob, sections),
                         {0x80001004, 0x80001010})

    def test_this_adjustment_thunks(self):
        code = [0x60000000, 0x3863FFF0, 0x4BFFFFF8,
                0x3863FFF0, 0x48000005,  # linked call is not a tail thunk
                0x3863FFF0, 0x48001000,  # destination outside executable text
                0x3884FFF0, 0x4BFFFFE0] # adjusting r4 is not a this thunk
        pointers = [0x80001004, 0x8000100C, 0x80001014, 0x8000101C]
        blob = b''.join(struct.pack('>I', x) for x in code + pointers)
        sections = [dict(kind='text', address='0x80001000', file_offset=0, bytes=len(code)*4),
                    dict(kind='data', address='0x80002000', file_offset=len(code)*4, bytes=len(pointers)*4)]
        self.assertEqual(discover_indirect_pointer_seeds(blob, sections), {0x80001004})

    def test_vtable_header_and_recognized_first_entry(self):
        code = [0x60000000] * 12
        words = [0, 0, 0x80001000, 0x80001004, 0x80001008, 7,
                 0, 0, 0x80001010, 0x80001014, 8, # unknown first method
                 0, 1, 0x80001000, 0x80001018, 9, # wrong header
                 0, 0, 0x8000101C, 10,           # only one pointer
                 0, 0, 0x80001000, 0x80001005, 0x80001020] # unaligned terminates
        blob = b''.join(struct.pack('>I', x) for x in code + words)
        sections = [dict(kind='text', address='0x80001000', file_offset=0, bytes=len(code)*4),
                    dict(kind='data', address='0x80002000', file_offset=len(code)*4, bytes=len(words)*4)]
        self.assertEqual(discover_vtable_pointer_seeds(blob, sections, {0x80001000, 0x8000101C}),
                         {0x80001000, 0x80001004, 0x80001008})

if __name__ == '__main__':
    unittest.main()
