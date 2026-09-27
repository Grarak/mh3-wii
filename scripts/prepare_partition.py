#!/usr/bin/env python3
"""Preserve exact decrypted partition bytes, including padding lost by extraction.

Input must be an ISO produced by `tools/nodtool convert --decrypt INPUT OUTPUT`.
The original RVZ and all generated disc bytes remain private ignored assets.
"""
import argparse
import os
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def prepare(iso, root):
    def read_at(file, offset, length):
        file.seek(offset)
        data = file.read(length)
        if len(data) != length:
            raise ValueError('Truncated decrypted ISO')
        return data

    with iso.open('rb') as source:
        groups = struct.unpack('>8I', read_at(source, 0x40000, 32))
        partitions = []
        for count, table in zip(groups[::2], groups[1::2]):
            if count > 256:
                raise ValueError('Invalid partition table count')
            for i in range(count):
                offset, kind = struct.unpack('>II', read_at(source, table * 4 + i * 8, 8))
                if kind == 0:
                    partitions.append(offset * 4)
        if len(partitions) != 1:
            raise ValueError('Expected exactly one data partition')
        partition = partitions[0]
        offset, size = struct.unpack('>II', read_at(source, partition + 0x2B8, 8))
        start, size = partition + offset * 4, size * 4
        if not size or size % 0x8000 or start + size > os.fstat(source.fileno()).st_size:
            raise ValueError('Invalid partition data extent')
        boot = (root / 'sys/boot.bin').read_bytes()
        if len(boot) != 0x440 or read_at(source, start + 0x400, len(boot)) != boot:
            raise ValueError('ISO must be decrypted and match the extracted partition')
        destination = root / 'partition.bin'
        temporary = root / 'partition.bin.tmp'
        try:
            with temporary.open('wb') as out:
                source.seek(start)
                remaining = size
                while remaining:
                    amount = min(remaining, 0x8000 * 128)
                    block = source.read(amount)
                    if len(block) != amount:
                        raise ValueError('Truncated partition')
                    for sector in range(0, amount, 0x8000):
                        out.write(block[sector + 0x400:sector + 0x8000])
                    remaining -= amount
            # Check the FST as well as the disc ID/header before publishing.
            with temporary.open('rb') as out:
                fst = (root / 'sys/fst.bin').read_bytes()
                fst_offset = struct.unpack_from('>I', boot, 0x424)[0] * 4
                if read_at(out, fst_offset, len(fst)) != fst:
                    raise ValueError('Decrypted partition FST differs from extracted files')
            temporary.replace(destination)
        finally:
            temporary.unlink(missing_ok=True)
        print(f'Preserved {destination.stat().st_size} original partition bytes in {destination}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('decrypted_iso', type=Path)
    parser.add_argument('--root', type=Path, default=ROOT / 'Assets/DATA')
    args = parser.parse_args()
    prepare(args.decrypted_iso, args.root)
