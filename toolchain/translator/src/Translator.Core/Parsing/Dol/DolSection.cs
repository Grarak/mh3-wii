using System;
using Translator.Core.Loading;

namespace Translator.Core.Parsing.Dol;

public enum SectionKind
{
    Text,
    Data,
    Bss
}

public sealed class DolSection
{
    public DolSection(int index, string name, SectionKind kind, uint fileOffset, uint virtualAddress, uint size, ReadOnlyMemory<byte> data)
    {
        Index = index;
        Name = name;
        Kind = kind;
        FileOffset = fileOffset;
        VirtualAddress = virtualAddress;
        Size = size;
        Data = data;
    }

    public int Index { get; }
    public string Name { get; }
    public SectionKind Kind { get; }
    public uint FileOffset { get; }
    public uint VirtualAddress { get; }
    public uint Size { get; }
    public AddressRange Range => AddressRange.FromStartAndSize(VirtualAddress, Size);
    public ReadOnlyMemory<byte> Data { get; }
    public bool HasData => Data.Length > 0;
    public bool IsExecutable => Kind == SectionKind.Text;
}
