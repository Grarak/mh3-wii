using Translator.Core.Loading;

namespace Translator.Core.Parsing.Rel;

public sealed class RelImage
{
    public RelImage(byte[] data, uint baseAddress)
    {
        Data = data;
        BaseAddress = baseAddress;
        Range = AddressRange.FromStartAndSize(baseAddress, (uint)data.Length);
    }

    public byte[] Data { get; }
    public uint BaseAddress { get; }
    public AddressRange Range { get; }
}
