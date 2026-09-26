namespace Translator.Core.Parsing.Kamek;

public enum KamekCommandId : byte
{
    Addr32 = 1,
    WritePointer = 1,
    Addr16Lo = 4,
    Addr16Hi = 5,
    Addr16Ha = 6,
    Rel24 = 10,
    Write32 = 32,
    Write16 = 33,
    Write8 = 34,
    CondWritePointer = 35,
    CondWrite32 = 36,
    CondWrite16 = 37,
    CondWrite8 = 38,
    Branch = 64,
    BranchLink = 65
}
