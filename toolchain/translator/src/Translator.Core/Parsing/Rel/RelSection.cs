using Translator.Core.Loading;

namespace Translator.Core.Parsing.Rel;

public sealed class RelSection
{
    public RelSection(int index, uint fileOffset, uint size, bool executable)
    {
        Index = index;
        FileOffset = fileOffset;
        Size = size;
        Executable = executable;
        Range = AddressRange.FromStartAndSize(fileOffset, size);
    }

    public int Index { get; }
    public uint FileOffset { get; }
    public uint Size { get; }
    public bool Executable { get; }
    public AddressRange Range { get; }
}
