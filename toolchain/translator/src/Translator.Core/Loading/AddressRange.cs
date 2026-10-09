using System;

namespace Translator.Core.Loading;

public readonly record struct AddressRange(uint Start, uint End)
{
    public uint Length => checked(End - Start);

    public bool Contains(uint address) => address >= Start && address < End;

    public static AddressRange FromStartAndSize(uint start, uint size) => new(start, checked(start + size));

    public static AddressRange Union(AddressRange a, AddressRange b) => new(
        Math.Min(a.Start, b.Start),
        Math.Max(a.End, b.End));
}
