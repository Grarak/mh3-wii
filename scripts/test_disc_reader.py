#!/usr/bin/env python3
"""Exercise the native partition reader against independent bytes and real assets."""
import struct
import subprocess
import tempfile
from pathlib import Path
from prepare_partition import prepare

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / 'build/native/MH3'


def read(root, offset, size, output, succeeds=True):
    result = subprocess.run([str(BINARY), '--disc-read', str(root), hex(offset), str(size), str(output)],
                            capture_output=True, text=True)
    if (result.returncode == 0) != succeeds:
        raise AssertionError(result.stderr)
    return output.read_bytes() if succeeds else None


with tempfile.TemporaryDirectory(prefix='mh3-disc-test-') as directory:
    root = Path(directory)
    (root / 'sys').mkdir()
    (root / 'files').mkdir()
    boot = bytearray(0x440)
    struct.pack_into('>I', boot, 0x18, 0x5D1C9EA3)
    fst = struct.pack('>IIIIIIIII', 0x01000000, 0, 3, 0, 0x3100 // 4, 4, 2, 0x3104 // 4, 4) + b'a\0b\0'
    struct.pack_into('>III', boot, 0x420, 0x2800 // 4, 0x3000 // 4, len(fst) // 4)
    for name, data in [('boot.bin', boot), ('bi2.bin', bytes(0x2000)), ('apploader.img', b'APPL'),
                       ('main.dol', b'DOL!'), ('fst.bin', fst)]:
        (root / 'sys' / name).write_bytes(data)
    (root / 'files/a').write_bytes(b'ABCD')
    (root / 'files/b').write_bytes(b'EFGH')
    output = root / 'output'
    assert read(root, 0x3102, 6, output) == b'CDEFGH'
    assert read(root, 0x43E, 4, output) == b'\0' * 4
    read(root, 0x3108, 1, output, False)
    read(root, 0x3000 + len(fst), 1, output, False)
    # Preserve actual nonzero gap bytes and verify cross-extent reads.
    partition = bytearray(b'\xA5' * 0xF800)
    partition[:len(boot)] = boot
    partition[0x3000:0x3000 + len(fst)] = fst
    # Two decrypted physical sectors: the 0x400-byte hash blocks must disappear.
    iso = bytearray(0x80000)
    struct.pack_into('>II', iso, 0x40000, 1, 0x40020 // 4)
    struct.pack_into('>II', iso, 0x40020, 0x50000 // 4, 0)
    struct.pack_into('>II', iso, 0x502B8, 0x20000 // 4, 0x10000 // 4)
    for i in range(2):
        start = 0x70000 + i * 0x8000
        iso[start:start + 0x400] = b'H' * 0x400
        iso[start + 0x400:start + 0x8000] = partition[i * 0x7C00:(i + 1) * 0x7C00]
    iso_path = root / 'source.iso'
    iso_path.write_bytes(iso)
    prepare(iso_path, root)
    assert (root / 'partition.bin').read_bytes() == partition
    assert read(root, 0x3106, 6, output) == b'GH' + b'\xA5' * 4
    assert read(root, 0x7BFE, 4, output) == b'\xA5' * 4
    read(root, len(partition) - 1, 2, output, False)
    (root / 'partition.bin').write_bytes(b'bad')
    read(root, 0x3100, 1, output, False)
    partition[0] ^= 1
    (root / 'partition.bin').write_bytes(partition)
    read(root, 0x3100, 1, output, False)
    (root / 'partition.bin').unlink()
    (root / 'files/a').write_bytes(b'bad')
    read(root, 0x3100, 1, output, False)

    # Sample real assets using an independent FST walk and compare exact bytes.
    disc = ROOT / 'Assets/DATA'
    fst = (disc / 'sys/fst.bin').read_bytes()
    count = struct.unpack_from('>I', fst, 8)[0]
    stack = [(count, disc / 'files')]
    samples = []
    for index in range(1, count):
        while index >= stack[-1][0]:
            stack.pop()
        kind_name, offset, size = struct.unpack_from('>III', fst, index * 12)
        name_at = count * 12 + (kind_name & 0xFFFFFF)
        name = fst[name_at:fst.index(0, name_at)].decode()
        path = stack[-1][1] / name
        if kind_name >> 24:
            stack.append((size, path))
        elif size and (len(samples) < 3 or index % 500 == 0):
            samples.append((offset * 4, size, path))
    for offset, size, path in samples:
        amount = min(size, 128)
        with path.open('rb') as file:
            file.seek(size - amount)
            expected = file.read(amount)
        assert read(disc, offset + size - amount, amount, output) == expected, path
    assert read(disc, 0, 32, output) == (disc / 'sys/boot.bin').read_bytes()[:32]
    print(f'Disc reader passed synthetic boundary/error tests and {len(samples)} real asset comparisons')
