namespace Translator.Core.Parsing.Kamek;

public sealed record KamekCommand(
    long StreamOffset,
    uint CommandWord,
    KamekCommandId Id,
    bool AddressIsAbsolute,
    uint Address,
    IReadOnlyList<uint> Arguments)
{
    public bool AddressIsRelative => !AddressIsAbsolute;
}
